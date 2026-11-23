import ActivityKit
import SwiftUI
import WidgetKit

struct TirePressureLiveActivityWidget: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: TirePressureAttributes.self) { context in
            lockScreenView(context: context)
                .activityBackgroundTint(Color(white: 0.10))
                .activitySystemActionForegroundColor(.white)
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Image(systemName: "car.fill")
                        .font(.system(size: 14))
                        .foregroundStyle(.blue)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    Text(context.attributes.vehicleName)
                        .font(.caption2.weight(.medium))
                        .foregroundStyle(.secondary)
                }
            } compactLeading: {
                Image(systemName: "car.fill")
                    .font(.system(size: 11))
                    .foregroundStyle(.blue)
            } compactTrailing: {
                // No tire info on dynamic island as requested
                EmptyView()
            } minimal: {
                Image(systemName: "car.fill")
                    .font(.system(size: 10))
                    .foregroundStyle(.blue)
            }
        }
    }

    @ViewBuilder
    private func lockScreenView(context: ActivityViewContext<TirePressureAttributes>) -> some View {
        VStack(spacing: 8) {
            // Header Bar
            HStack {
                HStack(spacing: 5) {
                    Image(systemName: "car.fill")
                        .font(.system(size: 12))
                        .foregroundStyle(.blue)
                    Text(context.attributes.vehicleName)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.white)
                }

                Spacer()

                if context.state.hasWarning {
                    HStack(spacing: 3) {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .font(.system(size: 8))
                        Text("CẢNH BÁO LỐP")
                            .font(.system(size: 8, weight: .bold))
                    }
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(Color.red.opacity(0.3))
                    .foregroundStyle(.red)
                    .clipShape(Capsule())
                } else {
                    HStack(spacing: 3) {
                        Circle().fill(Color.green).frame(width: 5, height: 5)
                        Text("4 LỐP AN TOÀN")
                            .font(.system(size: 8, weight: .bold))
                    }
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(Color.green.opacity(0.2))
                    .foregroundStyle(.green)
                    .clipShape(Capsule())
                }

                Text(context.state.updatedAt, style: .time)
                    .font(.system(size: 10))
                    .foregroundStyle(Color.white.opacity(0.5))
            }

            // Real 4-Wheel Position Visualizer
            HStack(alignment: .center, spacing: 6) {
                // Left Column: FL (top) & RL (bottom)
                VStack(spacing: 6) {
                    LiveTireCell(
                        code: "FL",
                        pressure: context.state.flPressure,
                        unit: context.state.unit,
                        temp: context.state.flTemp,
                        isFault: context.state.flFault,
                        isLeft: true
                    )
                    LiveTireCell(
                        code: "RL",
                        pressure: context.state.rlPressure,
                        unit: context.state.unit,
                        temp: context.state.rlTemp,
                        isFault: context.state.rlFault,
                        isLeft: true
                    )
                }
                .frame(maxWidth: .infinity)

                // Center Column: Mini Car Silhouette
                LiveActivityCarView(
                    flFault: context.state.flFault,
                    frFault: context.state.frFault,
                    rlFault: context.state.rlFault,
                    rrFault: context.state.rrFault
                )
                .frame(width: 54)

                // Right Column: FR (top) & RR (bottom)
                VStack(spacing: 6) {
                    LiveTireCell(
                        code: "FR",
                        pressure: context.state.frPressure,
                        unit: context.state.unit,
                        temp: context.state.frTemp,
                        isFault: context.state.frFault,
                        isLeft: false
                    )
                    LiveTireCell(
                        code: "RR",
                        pressure: context.state.rrPressure,
                        unit: context.state.unit,
                        temp: context.state.rrTemp,
                        isFault: context.state.rrFault,
                        isLeft: false
                    )
                }
                .frame(maxWidth: .infinity)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
    }
}

private struct LiveTireCell: View {
    let code: String
    let pressure: String
    let unit: String
    let temp: String
    let isFault: Bool
    let isLeft: Bool

    var body: some View {
        VStack(alignment: isLeft ? .trailing : .leading, spacing: 1) {
            HStack(spacing: 3) {
                if !isLeft {
                    Text(code)
                        .font(.system(size: 9, weight: .bold, design: .rounded))
                        .foregroundStyle(isFault ? .red : .cyan)
                    Spacer(minLength: 0)
                } else {
                    Spacer(minLength: 0)
                    Text(code)
                        .font(.system(size: 9, weight: .bold, design: .rounded))
                        .foregroundStyle(isFault ? .red : .cyan)
                }
            }

            HStack(alignment: .firstTextBaseline, spacing: 2) {
                if isLeft { Spacer(minLength: 0) }
                Text(pressure)
                    .font(.system(size: 16, weight: .bold, design: .rounded))
                    .foregroundStyle(.white)
                Text(unit)
                    .font(.system(size: 9, weight: .medium, design: .rounded))
                    .foregroundStyle(Color.white.opacity(0.65))
                if !isLeft { Spacer(minLength: 0) }
            }

            HStack(spacing: 2) {
                if isLeft { Spacer(minLength: 0) }
                Image(systemName: "thermometer.medium")
                    .font(.system(size: 8))
                    .foregroundStyle(isFault ? .red : Color.white.opacity(0.55))
                Text(temp)
                    .font(.system(size: 10, weight: .medium, design: .rounded))
                    .foregroundStyle(isFault ? .red : Color.white.opacity(0.85))
                if !isLeft { Spacer(minLength: 0) }
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .frame(maxWidth: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(isFault ? Color.red.opacity(0.25) : Color.white.opacity(0.08))
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(isFault ? Color.red.opacity(0.6) : Color.white.opacity(0.12), lineWidth: 1)
                )
        )
    }
}

private struct LiveActivityCarView: View {
    let flFault: Bool
    let frFault: Bool
    let rlFault: Bool
    let rrFault: Bool

    var body: some View {
        ZStack {
            // Vehicle body
            RoundedRectangle(cornerRadius: 12)
                .fill(
                    LinearGradient(
                        colors: [Color(white: 0.28), Color(white: 0.16)],
                        startPoint: .top,
                        endPoint: .bottom
                    )
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 12)
                        .stroke(Color.white.opacity(0.2), lineWidth: 1)
                )
                .frame(width: 42, height: 86)

            // Windshield
            Capsule()
                .fill(Color.cyan.opacity(0.35))
                .frame(width: 26, height: 10)
                .offset(y: -16)

            // Sunroof
            RoundedRectangle(cornerRadius: 2)
                .fill(Color.white.opacity(0.08))
                .frame(width: 22, height: 16)
                .offset(y: 2)

            // Rear glass
            Capsule()
                .fill(Color.cyan.opacity(0.25))
                .frame(width: 24, height: 8)
                .offset(y: 20)

            // 4 mini tire pads
            VStack {
                HStack {
                    miniPad(flFault)
                    Spacer()
                    miniPad(frFault)
                }
                Spacer()
                HStack {
                    miniPad(rlFault)
                    Spacer()
                    miniPad(rrFault)
                }
            }
            .frame(width: 50, height: 68)
        }
        .frame(width: 54, height: 90)
    }

    private func miniPad(_ fault: Bool) -> some View {
        RoundedRectangle(cornerRadius: 2)
            .fill(fault ? Color.red : Color.green)
            .frame(width: 5, height: 13)
            .shadow(color: (fault ? Color.red : Color.green).opacity(0.8), radius: 2)
    }
}
