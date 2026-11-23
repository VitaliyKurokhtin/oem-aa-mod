import Foundation

struct TireReading: Equatable, Sendable {
    var wheel: Int
    var pressureBar: Float
    var tempC: Int
    var alarm: Int

    var fault: Bool { alarm == 4 }

    func pressurePSI() -> Float { pressureBar * 14.5 }
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
}