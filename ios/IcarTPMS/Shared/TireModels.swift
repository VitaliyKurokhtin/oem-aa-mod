import Foundation

enum TireHealthStatus: Equatable, Sendable {
    case normal
    case lowPressure
    case highPressure
    case highTemperature
    case fault

    var title: String {
        switch self {
        case .normal: return "Bình thường"
        case .lowPressure: return "Áp suất thấp"
        case .highPressure: return "Áp suất cao"
        case .highTemperature: return "Nhiệt độ cao"
        case .fault: return "Báo động"
        }
    }
}

struct TireReading: Equatable, Sendable {
    var wheel: Int
    var pressureBar: Float
    var tempC: Int
    var alarm: Int

    var fault: Bool { alarm == 4 }

    func pressurePSI() -> Float { pressureBar * 14.5 }

    var healthStatus: TireHealthStatus {
        if fault { return .fault }
        if pressureBar < 1.8 { return .lowPressure }
        if pressureBar > 3.0 { return .highPressure }
        if tempC > 65 { return .highTemperature }
        return .normal
    }
}

struct TireSnapshot: Equatable, Sendable {
    private(set) var wheels: [TireReading?] = Array(repeating: nil, count: 4)
    var lastUpdate: Date?
    var connectedDeviceName: String?

    mutating func apply(_ reading: TireReading) {
        let index = reading.wheel - 1
        guard (0 ..< 4).contains(index) else { return }
        wheels[index] = reading
        lastUpdate = Date()
    }

    func wheelLabel(_ index: Int) -> String {
        switch index {
        case 0: return "FL"
        case 1: return "FR"
        case 2: return "RL"
        default: return "RR"
        }
    }

    func vietnameseWheelName(_ index: Int) -> String {
        switch index {
        case 0: return "Trước Trái"
        case 1: return "Trước Phải"
        case 2: return "Sau Trái"
        default: return "Sau Phải"
        }
    }

    var averagePressureBar: Float? {
        let valid = wheels.compactMap { $0?.pressureBar }
        guard !valid.isEmpty else { return nil }
        return valid.reduce(0, +) / Float(valid.count)
    }

    var maxTempC: Int? {
        let valid = wheels.compactMap { $0?.tempC }
        return valid.max()
    }

    var hasAnyWarning: Bool {
        wheels.compactMap { $0 }.contains { $0.healthStatus != .normal }
    }
}