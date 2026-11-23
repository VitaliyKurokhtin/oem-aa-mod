import ActivityKit
import Foundation

struct TirePressureAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        var flPSI: String
        var frPSI: String
        var rlPSI: String
        var rrPSI: String
        var flFault: Bool
        var frFault: Bool
        var rlFault: Bool
        var rrFault: Bool
        var updatedAt: Date
    }

    var vehicleName: String
}