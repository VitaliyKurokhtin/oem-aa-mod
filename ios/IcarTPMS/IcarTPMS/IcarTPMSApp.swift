import SwiftUI

@main
struct IcarTPMSApp: App {
    @StateObject private var central = TPMSCentral()
    @StateObject private var liveActivity = LiveActivityManager()
    @StateObject private var carPlayMonitor = CarPlayMonitor()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(central)
                .environmentObject(liveActivity)
                .environmentObject(carPlayMonitor)
                .onAppear {
                    central.onTPMSReady = { name in
                        Task { @MainActor in
                            liveActivity.startIfNeeded(
                                vehicleName: name,
                                snapshot: central.snapshot,
                                usePSI: central.usePSI
                            )
                        }
                    }
                    central.onSnapshotChange = { snapshot in
                        Task { @MainActor in
                            liveActivity.update(from: snapshot, usePSI: central.usePSI)
                        }
                    }
                    central.autoConnectIfPossible()

                    carPlayMonitor.onDisconnect = {
                        terminateApp()
                    }
                }
                .onOpenURL { url in
                    handleIncomingURL(url)
                }
        }
    }

    private func handleIncomingURL(_ url: URL) {
        let action = (url.host ?? url.path).lowercased()
        if action.contains("close") || action.contains("disconnect") || action.contains("exit") || action.contains("stop") {
            terminateApp()
        }
    }

    private func terminateApp() {
        liveActivity.end()
        central.disconnect()
        // Wait 0.5s for ActivityKit IPC dismissal before quitting
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            exit(0)
        }
    }
}