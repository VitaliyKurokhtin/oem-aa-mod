import Foundation

enum FCParseResult: Equatable, Sendable {
    case tire(TireReading)
    case sessionReset
    case ignored
}

/// Port of `a4.c.a` / `c4.a.l` from `com.icarvietnam.tpms.bluetooth`.
enum FCParser {
    static func bytesToFCLine(_ data: Data) -> String {
        data.map { byte in
            String(format: "%02X", byte)
        }.joined(separator: " ")
    }

    static func parseLine(_ line: String) -> FCParseResult {
        let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
        let compact = trimmed.replacingOccurrences(of: "\\s", with: "", options: .regularExpression)
        let tokens = trimmed.split(separator: " ").map(String.init)

        guard tokens.first == "FC", tokens.count >= 4 else { return .ignored }

        let length = hexTokenInt(tokens[1])
        guard length == tokens.count - 2 else { return .ignored }

        var xorValue = 0
        for i in 0 ... length {
            xorValue ^= hexTokenInt(tokens[i])
        }
        let checksumIndex = length + 1
        guard checksumIndex < tokens.count, xorValue == hexTokenInt(tokens[checksumIndex]) else {
            return .ignored
        }

        let opcode = tokens[1]
        switch opcode {
        case "02", "04":
            if compact.hasPrefix("FC02D52B") || compact.hasPrefix("FC04E5") {
                return .sessionReset
            }
            return .ignored
        case "08":
            guard tokens.count >= 8 else { return .ignored }
            let wheel = Int(tokens[3]) ?? 0
            let alarm = hexTokenInt(tokens[4])
            let pressure = Float(hexTokenInt(tokens[5])) / 10.0
            let t6 = hexTokenInt(tokens[6])
            let tempC: Int
            if t6 == 0 {
                tempC = hexTokenInt(tokens[7])
            } else {
                tempC = t6
            }
            let reading = TireReading(wheel: wheel, pressureBar: pressure, tempC: tempC, alarm: alarm)
            return .tire(reading)
        case "09":
            guard tokens.count >= 8 else { return .ignored }
            let wheel = Int(tokens[3]) ?? 0
            let alarm = hexTokenInt(tokens[4])
            let pressureRaw = hexTokenInt(tokens[5] + tokens[6])
            let pressure = Float(pressureRaw) / 145.0
            let tempC = hexTokenInt(tokens[7])
            let reading = TireReading(wheel: wheel, pressureBar: pressure, tempC: tempC, alarm: alarm)
            return .tire(reading)
        default:
            return .ignored
        }
    }

    /// Little-endian style hex string → int (`c4.a.l`).
    static func hexTokenInt(_ token: String) -> Int {
        guard !token.isEmpty else { return 0 }
        var value = 0
        let upper = token.uppercased()
        for (i, char) in upper.reversed().enumerated() {
            let nybble: Int
            if let d = char.wholeNumberValue {
                nybble = d
            } else if let ascii = char.asciiValue, ascii >= 65, ascii <= 70 {
                nybble = Int(ascii) - 55
            } else {
                return 0
            }
            value += (1 << (i * 4)) * nybble
        }
        return value
    }
}