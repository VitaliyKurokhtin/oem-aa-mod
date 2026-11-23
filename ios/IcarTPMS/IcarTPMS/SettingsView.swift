import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var central: TPMSCentral
    @EnvironmentObject private var liveActivity: LiveActivityManager
    @EnvironmentObject private var carPlayMonitor: CarPlayMonitor

    @State private var showingForgetAlert = false
    @State private var showingAutomationGuide = false

    var body: some View {
        NavigationStack {
            List {
                // Section 1: Paired Receiver
                pairedDeviceSection

                // Section 2: BLE Scanner
                scannerSection

                // Section 3: Discovered Devices
                discoveredDevicesSection

                // Section 4: Display & Units
                preferencesSection

                // Section 5: Automation Guide
                automationSection

                // Section 6: Diagnostics & Raw Packet
                diagnosticsSection
            }
            .navigationTitle("Cài Đặt & Kết Nối")
            .navigationBarTitleDisplayMode(.inline)
            .alert("Quên bộ thu TPMS?", isPresented: $showingForgetAlert) {
                Button("Huỷ", role: .cancel) {}
                Button("Quên thiết bị", role: .destructive) {
                    central.forgetSavedDevice()
                }
            } message: {
                Text("App sẽ không còn tự động kết nối với bộ thu này mỗi khi khởi động nữa. Bạn có thể quét và ghép đôi lại bất cứ lúc nào.")
            }
        }
    }

    // MARK: - Sections

    private var pairedDeviceSection: some View {
        Section(header: Text("Bộ thu đã ghép đôi")) {
            if let savedName = central.savedDeviceName, let uuid = central.savedDeviceUUID {
                VStack(alignment: .leading, spacing: 10) {
                    HStack(spacing: 12) {
                        Image(systemName: "antenna.radiowaves.left.and.right")
                            .font(.system(size: 22))
                            .foregroundStyle(central.connectedDeviceID != nil ? .green : .orange)
                            .frame(width: 36, height: 36)
                            .background(
                                (central.connectedDeviceID != nil ? Color.green : Color.orange).opacity(0.12)
                            )
                            .clipShape(Circle())

                        VStack(alignment: .leading, spacing: 2) {
                            Text(savedName)
                                .font(.headline)
                            Text(uuid.uuidString)
                                .font(.system(size: 11, design: .monospaced))
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                        }
                    }

                    HStack(spacing: 6) {
                        Circle()
                            .fill(central.connectedDeviceID != nil ? Color.green : Color.secondary)
                            .frame(width: 8, height: 8)
                        Text(central.connectedDeviceID != nil ? "Đang kết nối (Hoạt động)" : "Chưa kết nối (Tự kết nối khi xe nổ máy)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }

                    HStack(spacing: 12) {
                        if central.connectedDeviceID != nil {
                            Button("Ngắt kết nối") {
                                central.disconnect()
                            }
                            .buttonStyle(.bordered)
                            .tint(.secondary)
                        } else {
                            Button("Kết nối lại") {
                                central.autoConnectIfPossible()
                            }
                            .buttonStyle(.borderedProminent)
                        }

                        Spacer()

                        Button("Quên thiết bị") {
                            showingForgetAlert = true
                        }
                        .buttonStyle(.bordered)
                        .tint(.red)
                    }
                    .padding(.top, 4)
                }
                .padding(.vertical, 4)
            } else {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Chưa ghép đôi thiết bị nào")
                        .font(.subheadline.weight(.medium))
                        .foregroundStyle(.primary)
                    Text("Nhấn nút Quét bên dưới để tìm và chọn bộ thu iCar C33 của bạn. Thiết bị sẽ được lưu lại vĩnh viễn.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .padding(.vertical, 4)
            }
        }
    }

    private var scannerSection: some View {
        Section(header: Text("Tìm kiếm thiết bị BLE")) {
            HStack(spacing: 10) {
                Button {
                    if central.isScanning {
                        central.stopScanning()
                    } else {
                        central.startScanning()
                    }
                } label: {
                    HStack(spacing: 8) {
                        if central.isScanning {
                            ProgressView()
                                .controlSize(.small)
                            Text("Đang quét… (Bấm để dừng)")
                        } else {
                            Image(systemName: "magnifyingglass")
                            Text("Quét tìm thiết bị")
                        }
                    }
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
            }

            Toggle("Quét tất cả BLE (không chỉ FFE0)", isOn: $central.scanAllBLE)
                .font(.subheadline)
        }
    }

    private var discoveredDevicesSection: some View {
        Section(header: Text("Thiết bị tìm thấy (\(central.discoveredDevices.count))")) {
            if central.discoveredDevices.isEmpty {
                Text(central.isScanning ? "Đang tìm kiếm bộ thu xung quanh…" : "Không có thiết bị. Bấm \"Quét tìm thiết bị\" ở trên.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                ForEach(central.discoveredDevices) { device in
                    Button {
                        central.connect(to: device)
                    } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 3) {
                                HStack(spacing: 6) {
                                    Text(device.name)
                                        .font(.body.weight(.semibold))
                                        .foregroundStyle(.primary)
                                    if central.savedDeviceUUID == device.id {
                                        Text("Đã lưu")
                                            .font(.system(size: 9, weight: .bold))
                                            .padding(.horizontal, 4)
                                            .padding(.vertical, 1)
                                            .background(Color.blue.opacity(0.15))
                                            .foregroundStyle(.blue)
                                            .clipShape(Capsule())
                                    }
                                }
                                Text(device.id.uuidString)
                                    .font(.caption2.monospaced())
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                            }

                            Spacer()

                            Text("\(device.rssi) dBm")
                                .font(.caption.monospaced())
                                .foregroundStyle(.secondary)

                            if central.connectedDeviceID == device.id {
                                Image(systemName: "checkmark.circle.fill")
                                    .foregroundStyle(.green)
                            } else {
                                Image(systemName: "chevron.right")
                                    .font(.caption2)
                                    .foregroundStyle(.tertiary)
                            }
                        }
                    }
                }
            }
        }
    }

    private var preferencesSection: some View {
        Section(header: Text("Tuỳ chọn đơn vị & Hoạt động")) {
            Picker("Đơn vị áp suất", selection: $central.usePSI) {
                Text("Bar (kg/cm²)").tag(false)
                Text("PSI (lbs/in²)").tag(true)
            }

            Toggle("Tự tắt app khi ngắt CarPlay", isOn: $carPlayMonitor.autoCloseOnDisconnect)
                .font(.subheadline)

            VStack(alignment: .leading, spacing: 6) {
                Text("Ngưỡng an toàn xe ô tô con:")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)

                HStack {
                    Text("• Áp suất tiêu chuẩn:")
                    Spacer()
                    Text("2.1 - 2.5 bar (30 - 36 PSI)")
                        .fontWeight(.medium)
                }
                .font(.caption)

                HStack {
                    Text("• Cảnh báo áp suất thấp:")
                    Spacer()
                    Text("< 1.8 bar (< 26 PSI)")
                        .foregroundStyle(.orange)
                }
                .font(.caption)

                HStack {
                    Text("• Cảnh báo nhiệt độ lốp cao:")
                    Spacer()
                    Text("> 65 °C")
                        .foregroundStyle(.red)
                }
                .font(.caption)
            }
            .padding(.vertical, 2)
        }
    }

    private var automationSection: some View {
        Section(header: Text("Tự động hoá khi lên xe & Xuống xe")) {
            DisclosureGroup(
                isExpanded: $showingAutomationGuide,
                content: {
                    VStack(alignment: .leading, spacing: 10) {
                        Text("1. Tự động bật khi lên xe:")
                            .font(.caption.weight(.bold))
                            .foregroundStyle(.primary)

                        numberedStep(
                            num: 1,
                            title: "Tạo mục Tự động hoá trong Phím tắt (Shortcuts)",
                            desc: "Điều kiện: Khi CarPlay kết nối (hoặc khi kết nối Bluetooth xe)."
                        )

                        numberedStep(
                            num: 2,
                            title: "Tác vụ: Mở ứng dụng (Open App)",
                            desc: "Chọn app \"iCar TPMS\" và bật \"Chạy ngay lập tức\"."
                        )

                        Divider().padding(.vertical, 2)

                        Text("2. Tự động tắt khi xuống xe:")
                            .font(.caption.weight(.bold))
                            .foregroundStyle(.primary)

                        Text("• Mặc định: App sẽ tự động phát hiện khi CarPlay ngắt kết nối để huỷ Live Activity và tắt app.")
                            .font(.caption2)
                            .foregroundStyle(.secondary)

                        Text("• Hoặc thêm Tự động hoá khi ngắt CarPlay: Chọn tác vụ \"Mở URL\" với đường dẫn icartpms://close để tắt ngay lập tức.")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                    .padding(.vertical, 4)
                },
                label: {
                    Label("Hướng dẫn tự động hoá CarPlay", systemImage: "sparkles")
                        .font(.subheadline.weight(.medium))
                }
            )
        }
    }

    private var diagnosticsSection: some View {
        Section(header: Text("Chẩn đoán & Dữ liệu Raw")) {
            HStack {
                Text("Trạng thái Bluetooth:")
                Spacer()
                Text(bluetoothStateString)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(central.bluetoothState == .poweredOn ? .green : .red)
            }

            if liveActivity.isActive {
                HStack {
                    Text("Live Activity:")
                    Spacer()
                    Text("Đang chạy")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.green)
                }
            }

            if !central.lastRawLine.isEmpty {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Gói tin hex cuối (FC Parser):")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                    Text(central.lastRawLine)
                        .font(.system(size: 11, design: .monospaced))
                        .padding(6)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color(.tertiarySystemBackground))
                        .clipShape(RoundedRectangle(cornerRadius: 6))
                }
                .padding(.vertical, 2)
            }

            HStack {
                Text("Trạng thái link:")
                Spacer()
                Text(central.statusMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.trailing)
            }
        }
    }

    private func numberedStep(num: Int, title: String, desc: String) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Text("\(num)")
                .font(.system(size: 11, weight: .bold))
                .foregroundStyle(.white)
                .frame(width: 18, height: 18)
                .background(Color.blue)
                .clipShape(Circle())

            VStack(alignment: .leading, spacing: 1) {
                Text(title)
                    .font(.caption.weight(.semibold))
                Text(desc)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var bluetoothStateString: String {
        switch central.bluetoothState {
        case .poweredOn: return "Bật (poweredOn)"
        case .poweredOff: return "Tắt (poweredOff)"
        case .unauthorized: return "Chưa cấp quyền (unauthorized)"
        case .resetting: return "Khởi động lại (resetting)"
        case .unsupported: return "Không hỗ trợ (unsupported)"
        default: return "Chưa rõ (unknown)"
        }
    }
}
