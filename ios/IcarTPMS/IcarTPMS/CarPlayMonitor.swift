import AVFoundation
import Combine
import SwiftUI
import UIKit

@MainActor
final class CarPlayMonitor: ObservableObject {
    @Published private(set) var isCarPlayActive = false
    @AppStorage("autoCloseOnCarPlayDisconnect") var autoCloseOnDisconnect = true

    var onDisconnect: (() -> Void)?

    private var cancellables = Set<AnyCancellable>()

    init() {
        checkCarPlayState()

        // 1. Audio session route change (CarPlay audio port: .carAudio)
        NotificationCenter.default.publisher(for: AVAudioSession.routeChangeNotification)
            .receive(on: RunLoop.main)
            .sink { [weak self] notification in
                self?.handleAudioRouteChange(notification)
            }
            .store(in: &cancellables)

        // 2. UIScreen disconnect (CarPlay external display)
        NotificationCenter.default.publisher(for: UIScreen.didDisconnectNotification)
            .receive(on: RunLoop.main)
            .sink { [weak self] notification in
                self?.handleScreenDisconnect(notification)
            }
            .store(in: &cancellables)

        // 3. UIScreen connect (CarPlay external display connected)
        NotificationCenter.default.publisher(for: UIScreen.didConnectNotification)
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in
                self?.isCarPlayActive = true
            }
            .store(in: &cancellables)
    }

    private func checkCarPlayState() {
        let session = AVAudioSession.sharedInstance()
        let hasCarAudio = session.currentRoute.outputs.contains { $0.portType == .carAudio }
        let hasCarScreen = UIScreen.screens.contains { $0.traitCollection.userInterfaceIdiom == .carPlay }
        isCarPlayActive = hasCarAudio || hasCarScreen
    }

    private func handleAudioRouteChange(_ notification: Notification) {
        let session = AVAudioSession.sharedInstance()
        let hasCarAudio = session.currentRoute.outputs.contains { $0.portType == .carAudio }

        if isCarPlayActive && !hasCarAudio {
            isCarPlayActive = false
            triggerDisconnect()
        } else if !isCarPlayActive && hasCarAudio {
            isCarPlayActive = true
        }
    }

    private func handleScreenDisconnect(_ notification: Notification) {
        if let screen = notification.object as? UIScreen, screen.traitCollection.userInterfaceIdiom == .carPlay {
            isCarPlayActive = false
            triggerDisconnect()
        }
    }

    func triggerDisconnect() {
        guard autoCloseOnDisconnect else { return }
        onDisconnect?()
    }
}
