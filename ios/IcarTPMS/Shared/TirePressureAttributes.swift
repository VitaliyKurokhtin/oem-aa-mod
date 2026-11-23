import ActivityKit
import Foundation

struct TirePressureAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        // Pressures
        var flPressure: String
        var frPressure: String
        var rlPressure: String
        var rrPressure: String
        var unit: String

        // Temperatures
        var flTemp: String
        var frTemp: String
        var rlTemp: String
        var rrTemp: String

        // Faults / Warnings
        var flFault: Bool
        var frFault: Bool
        var rlFault: Bool
        var rrFault: Bool

        var hasWarning: Bool
        var updatedAt: Date
    }

    var vehicleName: String
}