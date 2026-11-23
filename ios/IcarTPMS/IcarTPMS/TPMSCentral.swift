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
    @Published private(set) var savedDeviceUUID: UUID?
    @Published private(set) var savedDeviceName: String?
    @Published var usePSI = true
    /// When false, only peripherals advertising FFE0 (or name hint) are listed.
    @Published var scanAllBLE = false

    private enum StorageKeys {
        static let savedDeviceUUID = "IcarTPMS_savedDeviceUUID"
        static let savedDeviceName = "IcarTPMS_savedDeviceName"
    }

    private var central: CBCentralManager?
    private var peripheral: CBPeripheral?
    private var writeCharacteristic: CBCharacteristic?
    private var peripheralsByID: [UUID: CBPeripheral] = [:]
    private let serviceShortUUIDs: [CBUUID] = [CBUUID(string: "FFE0")]
    private var scanStopWorkItem: DispatchWorkItem?
    private var reconnectWorkItem: DispatchWorkItem?
    private var shouldAutoReconnect = false

    var onSnapshotChange: ((TireSnapshot) -> Void)?
    /// Fired when GATT notify is armed (TPMS link ready).
    var onTPMSReady: ((String) -> Void)?

    override init() {
        super.init()
        if let uuidString = UserDefaults.standard.string(forKey: StorageKeys.savedDeviceUUID),
           let uuid = UUID(uuidString: uuidString) {
            savedDeviceUUID = uuid
            let name = UserDefaults.standard.string(forKey: StorageKeys.savedDeviceName) ?? "TPMS"
            savedDeviceName = name
            snapshot.connectedDeviceName = name
            statusMessage = "Paired with \(name)"
        }
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

    func autoConnectIfPossible() {
        guard let savedUUID = savedDeviceUUID else { return }
        guard connectedDeviceID == nil else { return }
        let manager = ensureCentral()
        guard manager.state == .poweredOn else { return }

        let name = savedDeviceName ?? "TPMS"
        shouldAutoReconnect = true

        // 1. Try retrieving directly via CoreBluetooth known peripheral cache
        let peripherals = manager.retrievePeripherals(withIdentifiers: [savedUUID])
        if let targetPeripheral = peripherals.first {
            peripheral = targetPeripheral
            targetPeripheral.delegate = self
            connectedDeviceID = savedUUID
            snapshot.connectedDeviceName = name
            statusMessage = "Auto-connecting to \(name)…"
            manager.connect(targetPeripheral, options: nil)
            return
        }

        // 2. If not yet cached, scan to find it
        statusMessage = "Searching for \(name)…"
        scanStopWorkItem?.cancel()
        isScanning = true
        let services: [CBUUID]? = scanAllBLE ? nil : serviceShortUUIDs
        manager.scanForPeripherals(
            withServices: services,
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]
        )

        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            if self.connectedDeviceID == nil {
                self.stopScanning()
                self.statusMessage = "\(name) not found — tap Scan or wake receiver"
            }
        }
        scanStopWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 15, execute: work)
    }

    func saveDevice(id: UUID, name: String) {
        savedDeviceUUID = id
        savedDeviceName = name
        UserDefaults.standard.set(id.uuidString, forKey: StorageKeys.savedDeviceUUID)
        UserDefaults.standard.set(name, forKey: StorageKeys.savedDeviceName)
    }

    func forgetSavedDevice() {
        disconnect()
        savedDeviceUUID = nil
        savedDeviceName = nil
        UserDefaults.standard.removeObject(forKey: StorageKeys.savedDeviceUUID)
        UserDefaults.standard.removeObject(forKey: StorageKeys.savedDeviceName)
        statusMessage = "Device forgotten — tap Scan to pair"
    }

    func connect(to device: DiscoveredDevice) {
        saveDevice(id: device.id, name: device.name)
        let manager = ensureCentral()
        stopScanning()
        reconnectWorkItem?.cancel()
        reconnectWorkItem = nil
        shouldAutoReconnect = true
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
        shouldAutoReconnect = false
        reconnectWorkItem?.cancel()
        reconnectWorkItem = nil
        stopScanning()
        if let peripheral, let central {
            central.cancelPeripheralConnection(peripheral)
        }
        connectedDeviceID = nil
        self.peripheral = nil
        writeCharacteristic = nil
        statusMessage = "Disconnected"
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

    private func scheduleReconnect(manager: CBCentralManager, peripheral: CBPeripheral) {
        reconnectWorkItem?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.shouldAutoReconnect, self.peripheral === peripheral else { return }
            self.statusMessage = "Reconnecting to \(self.snapshot.connectedDeviceName ?? "TPMS")…"
            manager.connect(peripheral, options: nil)
        }
        reconnectWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0, execute: work)
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
                if shouldAutoReconnect, let periph = peripheral, let central = self.central {
                    statusMessage = "Bluetooth ready — reconnecting…"
                    scheduleReconnect(manager: central, peripheral: periph)
                } else if savedDeviceUUID != nil && connectedDeviceID == nil {
                    autoConnectIfPossible()
                } else if statusMessage.contains("Bluetooth") || statusMessage.contains("Turn on") {
                    statusMessage = "Bluetooth ready — tap Scan"
                }
            case .unauthorized:
                reconnectWorkItem?.cancel()
                statusMessage = "Allow Bluetooth for this app in Settings"
            case .poweredOff:
                reconnectWorkItem?.cancel()
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
            let name = Self.displayName(peripheral: peripheral, advertisementData: advertisementData)

            // Auto-connect if this matches our saved device!
            if let saved = savedDeviceUUID, saved == peripheral.identifier, connectedDeviceID == nil {
                let displayName = name != "Unknown" ? name : (savedDeviceName ?? "TPMS")
                connect(to: DiscoveredDevice(id: peripheral.identifier, name: displayName, rssi: RSSI.intValue, peripheral: peripheral))
                return
            }

            if scanAllBLE {
                let upper = name.uppercased()
                let services = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
                let hasFFE0 = services.contains { $0.uuidString.uppercased().contains("FFE0") }
                let nameHint = ["TPMS", "ICAR", "C33", "I3C", "TAIYA", "BLE"].contains { upper.contains($0) }
                // In "all BLE" mode still prefer likely TPMS; show others only if named.
                if !hasFFE0 && !nameHint && upper == "UNKNOWN" {
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
            reconnectWorkItem?.cancel()
            reconnectWorkItem = nil
            statusMessage = "Connected — discovering services…"
            peripheral.discoverServices(nil)
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            if shouldAutoReconnect, let manager = self.central, let periph = self.peripheral {
                let reason = error?.localizedDescription ?? "connect failed"
                statusMessage = "Connect failed (\(reason)). Retrying…"
                scheduleReconnect(manager: manager, peripheral: periph)
            } else {
                statusMessage = error?.localizedDescription ?? "Connect failed"
                connectedDeviceID = nil
                self.peripheral = nil
            }
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            writeCharacteristic = nil
            if shouldAutoReconnect, let manager = self.central, let periph = self.peripheral {
                let reason = error?.localizedDescription ?? "link timeout"
                statusMessage = "Disconnected (\(reason)). Auto-reconnecting…"
                scheduleReconnect(manager: manager, peripheral: periph)
            } else {
                if connectedDeviceID == peripheral.identifier {
                    connectedDeviceID = nil
                }
                self.peripheral = nil
                statusMessage = error?.localizedDescription ?? "Disconnected"
            }
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
            guard error == nil else {
                statusMessage = error?.localizedDescription ?? "Characteristic discovery failed"
                return
            }
            var didSubscribe = false
            for characteristic in service.characteristics ?? [] {
                let uuid = characteristic.uuid.uuidString.uppercased()
                let isNotify = characteristic.properties.contains(.notify)
                    || characteristic.properties.contains(.indicate)
                    || uuid.contains("FFE4")
                    || uuid.contains("FFE2")
                if isNotify && !uuid.contains("FFE9") {
                    peripheral.setNotifyValue(true, for: characteristic)
                    didSubscribe = true
                }
                if uuid.contains("FFE9") || characteristic.properties.contains(.write) || characteristic.properties.contains(.writeWithoutResponse) {
                    writeCharacteristic = characteristic
                }
            }
            if didSubscribe || writeCharacteristic != nil {
                statusMessage = "Listening for TPMS frames"
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