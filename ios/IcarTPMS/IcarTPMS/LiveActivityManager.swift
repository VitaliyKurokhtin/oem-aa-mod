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
        func text(_ index: Int) -> String {
            guard let r = snapshot.wheels[index] else { return "—" }
            if usePSI {
                return String(format: "%.0f", r.pressurePSI())
            }
            return String(format: "%.1f", r.pressureBar)
        }
        func fault(_ index: Int) -> Bool {
            snapshot.wheels[index]?.fault ?? false
        }
        return TirePressureAttributes.ContentState(
            flPSI: text(0),
            frPSI: text(1),
            rlPSI: text(2),
            rrPSI: text(3),
            flFault: fault(0),
            frFault: fault(1),
            rlFault: fault(2),
            rrFault: fault(3),
            updatedAt: snapshot.lastUpdate ?? Date()
        )
    }
}