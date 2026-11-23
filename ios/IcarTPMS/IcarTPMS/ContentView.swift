import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var central: TPMSCentral
    @EnvironmentObject private var liveActivity: LiveActivityManager

    @State private var selectedTab = 0

    var body: some View {
        TabView(selection: $selectedTab) {
            TireDashboardView()
                .tabItem {
                    Label("Áp suất lốp", systemImage: "car.fill")
                }
                .tag(0)

            SettingsView()
                .tabItem {
                    Label("Cài đặt", systemImage: "gearshape.fill")
                }
                .tag(1)
        }
    }
}
