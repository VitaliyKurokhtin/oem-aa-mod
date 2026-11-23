import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var central: TPMSCentral
    @EnvironmentObject private var liveActivity: LiveActivityManager

    var body: some View {
        NavigationStack {
            VStack(spacing: 12) {
                Text(central.statusMessage)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: .infinity)

                if liveActivity.isActive {
                    Label("Live Activity on", systemImage: "platter.filled.top.and.arrow.up.iphone")
                        .font(.caption)
                        .foregroundStyle(.green)
                }

                tireGrid

                Toggle("Show PSI", isOn: $central.usePSI)
                Toggle("Scan all BLE (not only FFE0)", isOn: $central.scanAllBLE)

                HStack {
                    Button(central.isScanning ? "Scanning…" : "Scan") {
                        central.startScanning()
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(central.isScanning)

                    Button("Stop") {
                        central.stopScanning()
                        central.disconnect()
                    }
                    .buttonStyle(.bordered)
                }

                deviceList

                if !central.lastRawLine.isEmpty {
                    Text(central.lastRawLine)
                        .font(.caption.monospaced())
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                        .background(Color(.secondarySystemBackground))
                        .cornerRadius(8)
                }

                Spacer(minLength: 0)
            }
            .padding()
            .navigationTitle("iCar TPMS")
        }
    }

    private var deviceList: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Devices")
                .font(.headline)
            if central.discoveredDevices.isEmpty {
                Text(central.isScanning ? "Looking for receivers…" : "No devices yet — tap Scan")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                List {
                    ForEach(central.discoveredDevices) { device in
                        Button {
                            central.connect(to: device)
                        } label: {
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(device.name)
                                        .font(.body.weight(.semibold))
                                        .foregroundStyle(.primary)
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
                                }
                            }
                        }
                    }
                }
                .listStyle(.plain)
                .frame(minHeight: 120, maxHeight: 220)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var tireGrid: some View {
        LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 12) {
            ForEach(0 ..< 4, id: \.self) { index in
                tireCard(index: index)
            }
        }
    }

    @ViewBuilder
    private func tireCard(index: Int) -> some View {
        let label = central.snapshot.wheelLabel(index)
        let reading = central.snapshot.wheels[index]
        VStack(alignment: .leading, spacing: 4) {
            Text(label)
                .font(.headline)
            if let reading {
                let pressure = central.usePSI
                    ? String(format: "%.0f PSI", reading.pressurePSI())
                    : String(format: "%.1f bar", reading.pressureBar)
                Text(pressure)
                    .font(.title2.bold())
                Text("\(reading.tempC) °C")
                    .font(.caption)
                if reading.fault {
                    Text("Alarm")
                        .font(.caption)
                        .foregroundStyle(.red)
                }
            } else {
                Text("—")
                    .font(.title2)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding()
        .background(reading?.fault == true ? Color.red.opacity(0.12) : Color(.tertiarySystemBackground))
        .cornerRadius(12)
    }
}