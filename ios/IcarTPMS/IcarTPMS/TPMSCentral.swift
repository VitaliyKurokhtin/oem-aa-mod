import Combine
import CoreBluetooth
import Foundation

struct DiscoveredDevice: Identifiable, Equatable {
    let id: UUID
    let name: String
    var rssi: Int
    let peripheral: CBPeripheral

    static func == (lhs: DiscoveredDevice, rhs: DiscoveredDevice) -> Bool {
        lhs.id == rhs.id && lhs.rssi == rhs.rssi && lhs.name == rhs.name
    }
}

@MainActor
final class TPMSCentral: NSObject, ObservableObject {
    @Published private(set) var snapshot = TireSnapshot()
    @Published private(set) var bluetoothState: CBManagerState = .unknown
    @Published private(set) var statusMessage = "Tap Scan, then pick a device"
    @Published private(set) var lastRawLine = ""
    @Published private(set) var discoveredDevices: [DiscoveredDevice] = []
    @Published private(set) var isScanning = false
    @Published private(set) var connectedDeviceID: UUID?
    @Published var usePSI = true
    /// When false, only peripherals advertising FFE0 (or name hint) are listed.
    @Published var scanAllBLE = false

    private var central: CBCentralManager?
    private var peripheral: CBPeripheral?
    private var writeCharacteristic: CBCharacteristic?
    private var peripheralsByID: [UUID: CBPeripheral] = [:]
    private let serviceShortUUIDs: [CBUUID] = [CBUUID(string: "FFE0")]
    private var scanStopWorkItem: DispatchWorkItem?

    var onSnapshotChange: ((TireSnapshot) -> Void)?
    /// Fired when GATT notify is armed and handshake sent (TPMS link ready).
    var onTPMSReady: ((String) -> Void)?

    override init() {
        super.init()
    }

    private func ensureCentral() -> CBCentralManager {
        if let central {
            return central
        }
        let manager = CBCentralManager(delegate: self, queue: nil, options: nil)
        central = manager
        return manager
    }

    func startScanning() {
        let manager = ensureCentral()
        guard manager.state == .poweredOn else {
            statusMessage = manager.state == .unauthorized
                ? "Allow Bluetooth for this app in Settings"
                : "Turn on Bluetooth (state \(manager.state.rawValue))"
            return
        }

        scanStopWorkItem?.cancel()
        discoveredDevices = []
        peripheralsByID = [:]
        isScanning = true
        statusMessage = scanAllBLE ? "Scanning all BLE…" : "Scanning FFE0 / TPMS…"

        let services: [CBUUID]? = scanAllBLE ? nil : serviceShortUUIDs
        manager.scanForPeripherals(
            withServices: services,
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]
        )

        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.stopScanning()
            if self.connectedDeviceID == nil {
                self.statusMessage = self.discoveredDevices.isEmpty
                    ? "No devices — try Scan all BLE"
                    : "Select a device below"
            }
        }
        scanStopWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 20, execute: work)
    }

    func stopScanning() {
        scanStopWorkItem?.cancel()
        scanStopWorkItem = nil
        central?.stopScan()
        isScanning = false
    }

    func connect(to device: DiscoveredDevice) {
        guard let manager = central else { return }
        stopScanning()
        if let current = peripheral, current.identifier != device.id {
            manager.cancelPeripheralConnection(current)
        }
        let peripheral = peripheralsByID[device.id] ?? device.peripheral
        self.peripheral = peripheral
        peripheral.delegate = self
        writeCharacteristic = nil
        connectedDeviceID = device.id
        snapshot.connectedDeviceName = device.name
        statusMessage = "Connecting to \(device.name)…"
        manager.connect(peripheral, options: nil)
    }

    func disconnect() {
        stopScanning()
        if let peripheral, let central {
            central.cancelPeripheralConnection(peripheral)
        }
        connectedDeviceID = nil
    }

    private func upsertDiscovery(_ peripheral: CBPeripheral, rssi: Int, advertisementData: [String: Any]) {
        let name = Self.displayName(peripheral: peripheral, advertisementData: advertisementData)
        peripheralsByID[peripheral.identifier] = peripheral
        if let idx = discoveredDevices.firstIndex(where: { $0.id == peripheral.identifier }) {
            discoveredDevices[idx].rssi = rssi
            if discoveredDevices[idx].name != name, name != "Unknown" {
                discoveredDevices[idx] = DiscoveredDevice(
                    id: peripheral.identifier,
                    name: name,
                    rssi: rssi,
                    peripheral: peripheral
                )
            }
        } else {
            discoveredDevices.append(
                DiscoveredDevice(id: peripheral.identifier, name: name, rssi: rssi, peripheral: peripheral)
            )
        }
        discoveredDevices.sort { $0.rssi > $1.rssi }
    }

    private static func displayName(peripheral: CBPeripheral, advertisementData: [String: Any]) -> String {
        if let local = advertisementData[CBAdvertisementDataLocalNameKey] as? String, !local.isEmpty {
            return local
        }
        if let name = peripheral.name, !name.isEmpty {
            return name
        }
        return "Unknown"
    }

    private func sendHandshake() {
        guard let peripheral, let writeCharacteristic else { return }
        let payloads: [Data] = [
            Data([0xFC, 0x02, 0xD5, 0x2B]),
            Data([0xFC, 0x04, 0xE5]),
        ]
        for data in payloads {
            let writeType: CBCharacteristicWriteType = writeCharacteristic.properties.contains(.writeWithoutResponse)
                ? .withoutResponse
                : .withResponse
            peripheral.writeValue(data, for: writeCharacteristic, type: writeType)
        }
    }

    private func handleNotifyData(_ data: Data) {
        let line = FCParser.bytesToFCLine(data)
        lastRawLine = line
        let result = FCParser.parseLine(line)
        switch result {
        case .tire(let reading):
            snapshot.apply(reading)
            onSnapshotChange?(snapshot)
            statusMessage = "Receiving — wheel \(reading.wheel)"
        case .sessionReset:
            statusMessage = "Session reset from receiver"
        case .ignored:
            break
        }
    }
}

extension TPMSCentral: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { @MainActor in
            bluetoothState = central.state
            switch central.state {
            case .poweredOn:
                if statusMessage.contains("Bluetooth") || statusMessage.contains("Turn on") {
                    statusMessage = "Bluetooth ready — tap Scan"
                }
            case .unauthorized:
                statusMessage = "Allow Bluetooth for this app in Settings"
            case .poweredOff:
                statusMessage = "Turn on Bluetooth"
            default:
                break
            }
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        Task { @MainActor in
            if scanAllBLE {
                let name = Self.displayName(peripheral: peripheral, advertisementData: advertisementData).uppercased()
                let services = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
                let hasFFE0 = services.contains { $0.uuidString.uppercased().contains("FFE0") }
                let nameHint = ["TPMS", "ICAR", "C33", "I3C", "TAIYA", "BLE"].contains { name.contains($0) }
                // In "all BLE" mode still prefer likely TPMS; show others only if named.
                if !hasFFE0 && !nameHint && name == "UNKNOWN" {
                    return
                }
            }
            upsertDiscovery(peripheral, rssi: RSSI.intValue, advertisementData: advertisementData)
            if isScanning {
                statusMessage = "Found \(discoveredDevices.count) — tap to connect"
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        Task { @MainActor in
            statusMessage = "Discovering services…"
            peripheral.discoverServices(nil)
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            statusMessage = error?.localizedDescription ?? "Connect failed"
            connectedDeviceID = nil
            self.peripheral = nil
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            if connectedDeviceID == peripheral.identifier {
                connectedDeviceID = nil
            }
            self.peripheral = nil
            writeCharacteristic = nil
            statusMessage = error?.localizedDescription ?? "Disconnected"
        }
    }
}

extension TPMSCentral: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        Task { @MainActor in
            if let error {
                statusMessage = error.localizedDescription
                return
            }
            for service in peripheral.services ?? [] {
                peripheral.discoverCharacteristics(nil, for: service)
            }
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        Task { @MainActor in
            guard error == nil else { return }
            for characteristic in service.characteristics ?? [] {
                let uuid = characteristic.uuid.uuidString.uppercased()
                let isNotify = ["FFE4", "FFE5", "FFE9"].contains { uuid.contains($0) }
                if isNotify {
                    peripheral.setNotifyValue(true, for: characteristic)
                }
                if uuid.contains("FFE9"), writeCharacteristic == nil {
                    writeCharacteristic = characteristic
                }
            }
            if writeCharacteristic != nil {
                sendHandshake()
                statusMessage = "Subscribed — waiting for FC frames"
                let name = snapshot.connectedDeviceName ?? peripheral.name ?? "TPMS"
                onTPMSReady?(name)
            }
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard error == nil, let data = characteristic.value, !data.isEmpty else { return }
        Task { @MainActor in
            handleNotifyData(data)
        }
    }
}