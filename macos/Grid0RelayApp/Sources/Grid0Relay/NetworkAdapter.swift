import Foundation

struct NetworkAdapter: Identifiable, Hashable {
    let name: String
    let addresses: [String]
    let macAddress: String?
    let subnetMasks: [String: String]

    var id: String { name }

    var detail: String {
        let address = addresses.isEmpty ? "No IPv4 address" : addresses.joined(separator: ", ")
        return macAddress.map { "\(address)  •  \($0)" } ?? address
    }

    var isLikelyWiFi: Bool { name == "en0" }

    var isLikelyZeroTier: Bool {
        name.hasPrefix("zt") || name.hasPrefix("feth") || addresses.contains { $0.hasPrefix("10.147.") }
    }
}

enum InterfaceDiscovery {
    static func adapters() -> [NetworkAdapter] {
        let names = command("/sbin/ifconfig", ["-l"])
            .split(whereSeparator: { $0 == " " || $0 == "\n" || $0 == "\t" })
            .map(String.init)

        return names.compactMap { name in
            let output = command("/sbin/ifconfig", [name])
            guard !output.isEmpty else { return nil }
            let addresses = matches("(?m)^\\s*inet\\s+([0-9.]+)", in: output)
            let mac = matches("(?m)^\\s*ether\\s+([0-9a-f:]+)", in: output).first
            var subnetMasks: [String: String] = [:]
            for line in output.split(separator: "\n") {
                let fields = line.split(whereSeparator: { $0.isWhitespace }).map(String.init)
                guard fields.count >= 4, fields[0] == "inet", fields[2] == "netmask" else { continue }
                if fields[3].hasPrefix("0x"), let mask = UInt32(fields[3].dropFirst(2), radix: 16) {
                    subnetMasks[fields[1]] = [24, 16, 8, 0].map { String((mask >> $0) & 255) }.joined(separator: ".")
                } else if fields[3].split(separator: ".").count == 4 {
                    subnetMasks[fields[1]] = fields[3]
                }
            }
            return NetworkAdapter(name: name, addresses: addresses, macAddress: mac, subnetMasks: subnetMasks)
        }
        .sorted { lhs, rhs in
            if lhs.isLikelyWiFi != rhs.isLikelyWiFi { return lhs.isLikelyWiFi }
            if lhs.isLikelyZeroTier != rhs.isLikelyZeroTier { return lhs.isLikelyZeroTier }
            return lhs.name < rhs.name
        }
    }

    private static func matches(_ pattern: String, in text: String) -> [String] {
        guard let expression = try? NSRegularExpression(pattern: pattern) else { return [] }
        let range = NSRange(text.startIndex..., in: text)
        return expression.matches(in: text, range: range).compactMap { match in
            guard let range = Range(match.range(at: 1), in: text) else { return nil }
            return String(text[range])
        }
    }

    private static func command(_ executable: String, _ arguments: [String]) -> String {
        let process = Process()
        let output = Pipe()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        process.standardOutput = output
        process.standardError = Pipe()
        guard (try? process.run()) != nil else { return "" }
        process.waitUntilExit()
        return String(data: output.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
    }
}
