import ActivityKit
import SwiftUI
import WidgetKit

struct TirePressureLiveActivityWidget: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: TirePressureAttributes.self) { context in
            lockScreenView(context: context)
                .activityBackgroundTint(Color.black.opacity(0.85))
                .activitySystemActionForegroundColor(.white)
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Text("TPMS")
                }
                DynamicIslandExpandedRegion(.trailing) {
                    Text(context.state.flPSI)
                }
                DynamicIslandExpandedRegion(.bottom) {
                    HStack {
                        wheelPill("FL", context.state.flPSI, context.state.flFault)
                        wheelPill("FR", context.state.frPSI, context.state.frFault)
                        wheelPill("RL", context.state.rlPSI, context.state.rlFault)
                        wheelPill("RR", context.state.rrPSI, context.state.rrFault)
                    }
                }
            } compactLeading: {
                Text("TPMS")
            } compactTrailing: {
                Text(context.state.flPSI)
            } minimal: {
                Text("◎")
            }
        }
    }

    @ViewBuilder
    private func lockScreenView(context: ActivityViewContext<TirePressureAttributes>) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(context.attributes.vehicleName)
                .font(.headline)
            HStack {
                wheelPill("FL", context.state.flPSI, context.state.flFault)
                wheelPill("FR", context.state.frPSI, context.state.frFault)
                wheelPill("RL", context.state.rlPSI, context.state.rlFault)
                wheelPill("RR", context.state.rrPSI, context.state.rrFault)
            }
        }
        .padding()
    }

    private func wheelPill(_ label: String, _ value: String, _ fault: Bool) -> some View {
        VStack(spacing: 2) {
            Text(label).font(.caption2)
            Text(value).font(.caption.bold())
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
        .background(fault ? Color.red.opacity(0.35) : Color.white.opacity(0.15))
        .cornerRadius(6)
    }
}