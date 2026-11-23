import SwiftUI

struct TireCardView: View {
    let index: Int
    let reading: TireReading?
    let usePSI: Bool

    private var label: String {
        switch index {
        case 0: return "Trước Trái"
        case 1: return "Trước Phải"
        case 2: return "Sau Trái"
        default: return "Sau Phải"
        }
    }

    private var code: String {
        switch index {
        case 0: return "FL"
        case 1: return "FR"
        case 2: return "RL"
        default: return "RR"
        }
    }

    private var statusColor: Color {
        guard let reading else { return Color.secondary }
        switch reading.healthStatus {
        case .normal: return Color.green
        case .lowPressure, .highPressure, .highTemperature: return Color.orange
        case .fault: return Color.red
        }
    }

    private var isLeft: Bool {
        index % 2 == 0
    }

    var body: some View {
        VStack(alignment: isLeft ? .trailing : .leading, spacing: 5) {
            // Header: Code + Name
            HStack(spacing: 4) {
                if !isLeft {
                    badgeView
                    Text(label)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 0)
                } else {
                    Spacer(minLength: 0)
                    Text(label)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(.secondary)
                    badgeView
                }
            }

            // Main Pressure Value
            if let reading {
                HStack(alignment: .firstTextBaseline, spacing: 3) {
                    if isLeft { Spacer(minLength: 0) }
                    Text(usePSI ? String(format: "%.0f", reading.pressurePSI()) : String(format: "%.1f", reading.pressureBar))
                        .font(.system(size: 26, weight: .bold, design: .rounded))
                        .foregroundStyle(.primary)
                    Text(usePSI ? "PSI" : "bar")
                        .font(.system(size: 12, weight: .semibold, design: .rounded))
                        .foregroundStyle(.secondary)
                    if !isLeft { Spacer(minLength: 0) }
                }

                // Temperature + Status Warning
                HStack(spacing: 5) {
                    if isLeft { Spacer(minLength: 0) }
                    HStack(spacing: 3) {
                        Image(systemName: "thermometer.medium")
                            .font(.system(size: 10))
                        Text("\(reading.tempC)°C")
                            .font(.system(size: 11, weight: .medium, design: .rounded))
                    }
                    .foregroundStyle(reading.tempC > 65 ? .red : .secondary)

                    if reading.fault {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .font(.system(size: 10))
                            .foregroundStyle(.red)
                    } else if reading.healthStatus != .normal {
                        Text(reading.healthStatus.title)
                            .font(.system(size: 9, weight: .semibold))
                            .padding(.horizontal, 4)
                            .padding(.vertical, 1)
                            .background(statusColor.opacity(0.18))
                            .foregroundStyle(statusColor)
                            .clipShape(Capsule())
                    }
                    if !isLeft { Spacer(minLength: 0) }
                }
            } else {
                HStack {
                    if isLeft { Spacer(minLength: 0) }
                    Text("— —")
                        .font(.system(size: 24, weight: .bold, design: .rounded))
                        .foregroundStyle(.tertiary)
                    if !isLeft { Spacer(minLength: 0) }
                }
                HStack {
                    if isLeft { Spacer(minLength: 0) }
                    Text("Chờ dữ liệu")
                        .font(.system(size: 11))
                        .foregroundStyle(.tertiary)
                    if !isLeft { Spacer(minLength: 0) }
                }
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 10)
        .background(
            RoundedRectangle(cornerRadius: 14)
                .fill(Color(.secondarySystemBackground))
                .overlay(
                    RoundedRectangle(cornerRadius: 14)
                        .stroke(
                            reading?.fault == true ? Color.red.opacity(0.7) :
                            (reading?.healthStatus != .normal && reading != nil ? Color.orange.opacity(0.6) : Color.white.opacity(0.08)),
                            lineWidth: reading?.healthStatus == .normal || reading == nil ? 0.8 : 1.5
                        )
                )
        )
        .shadow(color: Color.black.opacity(0.06), radius: 5, x: 0, y: 2)
    }

    private var badgeView: some View {
        Text(code)
            .font(.system(size: 10, weight: .bold, design: .rounded))
            .padding(.horizontal, 5)
            .padding(.vertical, 2)
            .background(statusColor.opacity(0.16))
            .foregroundStyle(statusColor)
            .clipShape(Capsule())
    }
}

struct VehicleStatusCard: View {
    let snapshot: TireSnapshot
    let usePSI: Bool

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 12) {
                ZStack {
                    Circle()
                        .fill(snapshot.hasAnyWarning ? Color.red.opacity(0.15) : Color.green.opacity(0.15))
                        .frame(width: 38, height: 38)
                    Image(systemName: snapshot.hasAnyWarning ? "exclamationmark.shield.fill" : "checkmark.shield.fill")
                        .foregroundStyle(snapshot.hasAnyWarning ? .red : .green)
                        .font(.system(size: 20))
                }

                VStack(alignment: .leading, spacing: 2) {
                    Text(snapshot.hasAnyWarning ? "Cần kiểm tra lốp" : "Áp suất 4 lốp an toàn")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.primary)
                    Text(statusDescription)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }

            Divider()

            HStack {
                HStack(spacing: 4) {
                    Text("Áp suất TB:")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    if let avg = snapshot.averagePressureBar {
                        let text = usePSI ? String(format: "%.0f PSI", avg * 14.5) : String(format: "%.1f bar", avg)
                        Text(text)
                            .font(.caption.weight(.semibold))
                    } else {
                        Text("—")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer()
                HStack(spacing: 4) {
                    Text("Nhiệt độ max:")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    if let maxT = snapshot.maxTempC {
                        Text("\(maxT) °C")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(maxT > 65 ? .red : .primary)
                    } else {
                        Text("—")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
        .padding(12)
        .background(Color(.secondarySystemBackground))
        .clipShape(RoundedRectangle(cornerRadius: 14))
    }

    private var statusDescription: String {
        let count = snapshot.wheels.compactMap { $0 }.count
        if count == 0 {
            return "Chưa nhận được tín hiệu cảm biến"
        } else if count < 4 {
            return "Đã nhận dữ liệu \(count)/4 bánh xe"
        } else if snapshot.hasAnyWarning {
            return "Phát hiện bánh xe có áp suất hoặc nhiệt độ bất thường"
        } else {
            return "Tất cả thông số đều nằm trong ngưỡng an toàn"
        }
    }
}

struct TireDashboardView: View {
    @EnvironmentObject private var central: TPMSCentral
    @EnvironmentObject private var liveActivity: LiveActivityManager

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    // Connection Status & Live Activity Bar
                    headerStatusBar

                    // 4-Wheel Real Position Visualizer
                    carVisualizerSection
                        .padding(.vertical, 8)

                    // Vehicle Safety Overview Card
                    VehicleStatusCard(snapshot: central.snapshot, usePSI: central.usePSI)

                    Spacer(minLength: 20)
                }
                .padding(.horizontal, 16)
                .padding(.top, 8)
            }
            .navigationTitle("Áp Suất Lốp")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Picker("Đơn vị", selection: $central.usePSI) {
                        Text("Bar").tag(false)
                        Text("PSI").tag(true)
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 110)
                }
            }
        }
    }

    private var headerStatusBar: some View {
        HStack(spacing: 10) {
            // Connection Dot & Device
            HStack(spacing: 6) {
                Circle()
                    .fill(connectionColor)
                    .frame(width: 8, height: 8)
                Text(connectionLabel)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.primary)
                    .lineLimit(1)
            }

            Spacer()

            // Live Activity Badge
            if liveActivity.isActive {
                HStack(spacing: 4) {
                    Image(systemName: "platter.filled.top.and.arrow.up.iphone")
                        .font(.system(size: 10))
                    Text("Live Activity")
                        .font(.caption2.weight(.medium))
                }
                .foregroundStyle(.green)
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(Color.green.opacity(0.12))
                .clipShape(Capsule())
            } else if let last = central.snapshot.lastUpdate {
                Text(last, style: .time)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Color(.secondarySystemBackground))
        .clipShape(Capsule())
    }

    private var carVisualizerSection: some View {
        HStack(alignment: .center, spacing: 8) {
            // Left Column: Front Left (top) & Rear Left (bottom)
            VStack(spacing: 24) {
                TireCardView(index: 0, reading: central.snapshot.wheels[0], usePSI: central.usePSI)
                TireCardView(index: 2, reading: central.snapshot.wheels[2], usePSI: central.usePSI)
            }
            .frame(maxWidth: .infinity)

            // Center Column: Top-Down Car Silhouette
            CarTopDownView(wheels: central.snapshot.wheels)
                .frame(width: 126)

            // Right Column: Front Right (top) & Rear Right (bottom)
            VStack(spacing: 24) {
                TireCardView(index: 1, reading: central.snapshot.wheels[1], usePSI: central.usePSI)
                TireCardView(index: 3, reading: central.snapshot.wheels[3], usePSI: central.usePSI)
            }
            .frame(maxWidth: .infinity)
        }
    }

    private var connectionColor: Color {
        if central.connectedDeviceID != nil {
            return .green
        }
        if central.isScanning || central.statusMessage.contains("Reconnecting") || central.statusMessage.contains("Auto-connecting") {
            return .orange
        }
        return .gray
    }

    private var connectionLabel: String {
        if let name = central.snapshot.connectedDeviceName, central.connectedDeviceID != nil {
            return name
        }
        if let saved = central.savedDeviceName {
            return "\(saved) (Đang kết nối...)"
        }
        return central.statusMessage
    }
}
