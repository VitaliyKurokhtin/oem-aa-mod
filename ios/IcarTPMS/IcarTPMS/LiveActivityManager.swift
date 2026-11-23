import ActivityKit
import Combine
import Foundation

@MainActor
final class LiveActivityManager: ObservableObject {
    @Published private(set) var isActive = false
    private var activity: Activity<TirePressureAttributes>?

    func startIfNeeded(vehicleName: String, snapshot: TireSnapshot? = nil, usePSI: Bool = true) {
        guard ActivityAuthorizationInfo().areActivitiesEnabled else {
            NSLog("Live Activities disabled in Settings")
            return
        }

        if activity == nil, let existing = Activity<TirePressureAttributes>.activities.first {
            activity = existing
            isActive = true
        }

        let state = contentState(from: snapshot ?? TireSnapshot(), usePSI: usePSI)

        if let activity {
            Task {
                await activity.update(.init(state: state, staleDate: Date().addingTimeInterval(120)))
            }
            isActive = true
            return
        }

        let attributes = TirePressureAttributes(vehicleName: vehicleName)
        do {
            activity = try Activity.request(
                attributes: attributes,
                content: .init(state: state, staleDate: Date().addingTimeInterval(120))
            )
            isActive = true
        } catch {
            NSLog("Live Activity start failed: \(error)")
            isActive = false
        }
    }

    func update(from snapshot: TireSnapshot, usePSI: Bool) {
        // Auto-start on first TPMS sample if not running yet.
        if activity == nil {
            startIfNeeded(
                vehicleName: snapshot.connectedDeviceName ?? "TPMS",
                snapshot: snapshot,
                usePSI: usePSI
            )
            return
        }
        let state = contentState(from: snapshot, usePSI: usePSI)
        Task {
            await activity?.update(.init(state: state, staleDate: Date().addingTimeInterval(120)))
        }
    }

    func end() {
        guard let activity else { return }
        Task {
            await activity.end(nil, dismissalPolicy: .immediate)
        }
        self.activity = nil
        isActive = false
    }

    private func contentState(from snapshot: TireSnapshot, usePSI: Bool) -> TirePressureAttributes.ContentState {
        func pressureText(_ index: Int) -> String {
            guard let r = snapshot.wheels[index] else { return "—" }
            if usePSI {
                return String(format: "%.0f", r.pressurePSI())
            }
            return String(format: "%.1f", r.pressureBar)
        }

        func tempText(_ index: Int) -> String {
            guard let r = snapshot.wheels[index] else { return "—" }
            return "\(r.tempC)°C"
        }

        func isFault(_ index: Int) -> Bool {
            guard let r = snapshot.wheels[index] else { return false }
            return r.fault || r.pressureBar < 1.8 || r.pressureBar > 3.0 || r.tempC > 65
        }

        return TirePressureAttributes.ContentState(
            flPressure: pressureText(0),
            frPressure: pressureText(1),
            rlPressure: pressureText(2),
            rrPressure: pressureText(3),
            unit: usePSI ? "PSI" : "bar",
            flTemp: tempText(0),
            frTemp: tempText(1),
            rlTemp: tempText(2),
            rrTemp: tempText(3),
            flFault: isFault(0),
            frFault: isFault(1),
            rlFault: isFault(2),
            rrFault: isFault(3),
            hasWarning: snapshot.hasAnyWarning,
            updatedAt: snapshot.lastUpdate ?? Date()
        )
    }
}