import SwiftUI

@main
struct IcarTPMSApp: App {
    @StateObject private var central = TPMSCentral()
    @StateObject private var liveActivity = LiveActivityManager()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(central)
                .environmentObject(liveActivity)
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
                }
        }
    }
}