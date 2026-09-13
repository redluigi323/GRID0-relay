import AppKit
import Combine
import Foundation

@MainActor
final class RelayModel: ObservableObject {
    private enum AdministratorResult {
        case success(Int32)
        case failure(String)
    }

    @Published private(set) var adapters: [NetworkAdapter] = []
    @Published var wifiInterface = ""
    @Published var zeroTierInterface = ""
    @Published var subnet = "10.147.17.0/24"
    @Published var gateway = "10.147.17.1"
    @Published var diagnostics = true
    @Published var discoverSwitch = true
    @Published var relayPath = RelayModel.defaultRelayPath()
    @Published private(set) var status = "Not running"
    @Published private(set) var isStarting = false
    @Published private(set) var relayPID: Int32?

    private let logURL = URL(fileURLWithPath: "/var/tmp/grid0-relay.log")

    init() {
        refreshAdapters()
    }

    var canStart: Bool {
        relayPID == nil && !isStarting &&
        !wifiInterface.isEmpty && !zeroTierInterface.isEmpty &&
        wifiInterface != zeroTierInterface && !subnet.isEmpty &&
        !gateway.isEmpty && FileManager.default.isExecutableFile(atPath: relayPath)
    }

    var selectedZeroTierAddress: String? {
        adapters.first(where: { $0.name == zeroTierInterface })?.addresses.first
    }

    var selectedZeroTierMask: String? {
        guard let adapter = adapters.first(where: { $0.name == zeroTierInterface }),
              let address = adapter.addresses.first else { return nil }
        return adapter.subnetMasks[address]
    }

    func refreshAdapters() {
        adapters = InterfaceDiscovery.adapters()
        if !adapters.contains(where: { $0.name == wifiInterface }) {
            wifiInterface = adapters.first(where: \.isLikelyWiFi)?.name ?? adapters.first?.name ?? ""
        }
        if !adapters.contains(where: { $0.name == zeroTierInterface }) {
            zeroTierInterface = adapters.first(where: \.isLikelyZeroTier)?.name ?? ""
        }
    }

    func chooseRelay() {
        let panel = NSOpenPanel()
        panel.title = "Choose GRID0 Relay"
        panel.message = "Select the GRID0 Relay executable built by CMake."
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            relayPath = url.path
        }
    }

    func start() {
        guard canStart else {
            status = "Choose two different adapters and a valid relay executable."
            return
        }
        isStarting = true
        status = "Requesting administrator access…"
        let command = relayCommand()
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let result = Self.runAsAdministrator(command: command, expectsPID: true)
            DispatchQueue.main.async {
                guard let self else { return }
                self.isStarting = false
                switch result {
                case .success(let pid):
                    self.relayPID = pid
                    self.status = "Running as administrator (PID \(pid))."
                case .failure(let message):
                    self.status = message
                }
            }
        }
    }

    func stop() {
        guard let relayPID else { return }
        isStarting = true
        status = "Stopping relay…"
        let command = "kill -INT \(relayPID)"
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let result = Self.runAsAdministrator(command: command, expectsPID: false)
            DispatchQueue.main.async {
                guard let self else { return }
                self.isStarting = false
                switch result {
                case .success:
                    self.relayPID = nil
                    self.status = "Stopped"
                case .failure(let message):
                    self.status = message
                }
            }
        }
    }

    func relayLog() -> String {
        (try? String(contentsOf: logURL, encoding: .utf8)) ?? "The relay has not written a log yet."
    }

    private func relayCommand() -> String {
        var arguments = [relayPath, "--netif", wifiInterface, "--zerotier-if", zeroTierInterface,
                         "--subnet", subnet, "--gateway", gateway]
        if diagnostics { arguments.append("--diagnostics") }
        if !discoverSwitch { arguments.append("--no-discover-switch") }
        let executable = arguments.map(Self.shellQuote).joined(separator: " ")
        let log = Self.shellQuote(logURL.path)
        // A shell PID is not proof that initialization succeeded. Surface
        // duplicate-instance and adapter failures instead of saying Running.
        return """
        \(executable) > \(log) 2>&1 &
        relay_pid=$!
        attempts=0
        while [ "$attempts" -lt 100 ]; do
            if ! kill -0 "$relay_pid" 2>/dev/null; then
                cat \(log) >&2
                exit 1
            fi
            if /usr/bin/grep -q "^Relay started (PID $relay_pid)$" \(log); then
                echo "$relay_pid"
                exit 0
            fi
            sleep 0.1
            attempts=$((attempts + 1))
        done
        kill -INT "$relay_pid" 2>/dev/null
        echo "Relay did not finish initialization. Check the relay log." >&2
        exit 1
        """
    }

    nonisolated private static func runAsAdministrator(command: String, expectsPID: Bool) -> AdministratorResult {
        let appleScript = "do shell script \(appleScriptQuote(command)) with administrator privileges"
        let process = Process()
        let output = Pipe()
        let errors = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", appleScript]
        process.standardOutput = output
        process.standardError = errors
        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return .failure("Could not open the macOS administrator prompt: \(error.localizedDescription)")
        }
        let errorText = String(data: errors.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard process.terminationStatus == 0 else {
            return .failure(errorText.isEmpty ? "Administrator authorization was cancelled or failed." : errorText)
        }
        if !expectsPID {
            return .success(0)
        }
        let outputText = String(data: output.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard let pid = Int32(outputText) else {
            return .failure("The relay started but did not return a process ID.")
        }
        return .success(pid)
    }

    nonisolated private static func shellQuote(_ value: String) -> String {
        "'\(value.replacingOccurrences(of: "'", with: "'\\\"'\\\"'"))'"
    }

    nonisolated private static func appleScriptQuote(_ value: String) -> String {
        "\"\(value.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\""))\""
    }

    private static func defaultRelayPath() -> String {
        let fileManager = FileManager.default
        var directory = URL(fileURLWithPath: fileManager.currentDirectoryPath, isDirectory: true)
        for _ in 0..<8 {
            let candidate = directory.appending(path: "build/src/grid0-relay").path
            if fileManager.isExecutableFile(atPath: candidate) { return candidate }
            directory.deleteLastPathComponent()
        }
        return ""
    }
}
