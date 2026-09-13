import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var relay: RelayModel
    @State private var showLog = false

    var body: some View {
        VStack(spacing: 0) {
            header
            Form {
                Section("Adapters") {
                    Picker("Switch Wi-Fi adapter", selection: $relay.wifiInterface) {
                        Text("Choose an adapter").tag("")
                        ForEach(relay.adapters) { adapter in
                            Text("\(adapter.name) — \(adapter.detail)").tag(adapter.name)
                        }
                    }
                    Picker("ZeroTier adapter", selection: $relay.zeroTierInterface) {
                        Text("Choose an adapter").tag("")
                        ForEach(relay.adapters) { adapter in
                            Text("\(adapter.name) — \(adapter.detail)").tag(adapter.name)
                        }
                    }
                    Button("Refresh adapters", systemImage: "arrow.clockwise") {
                        relay.refreshAdapters()
                    }
                }

                Section("LAN play network") {
                    TextField("Subnet", text: $relay.subnet, prompt: Text("10.147.17.0/24"))
                    TextField("Fake gateway", text: $relay.gateway, prompt: Text("10.147.17.1"))
                    Toggle("Find the local Switch automatically", isOn: $relay.discoverSwitch)
                    if let address = relay.selectedZeroTierAddress {
                        LabeledContent("Switch address") {
                            Text(address).textSelection(.enabled)
                        }
                        LabeledContent("Switch subnet mask") {
                            Text(relay.selectedZeroTierMask ?? "Unavailable — check the relay startup log").textSelection(.enabled)
                        }
                        Text("Use this address, this exact subnet mask, and the fake gateway on the Switch. After changing them, reconnect and restart LAN mode. Matching masks are required for Splatoon's LAN authentication.")
                    } else {
                        Text("Select the ZeroTier adapter to see the address for the stock Switch.")
                    }
                    Text("The address is shared across separate Wi-Fi and ZeroTier interfaces; it does not replace the Mac's Wi-Fi address.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Section("Relay executable") {
                    LabeledContent("Path") {
                        Text(relay.relayPath.isEmpty ? "No executable selected" : relay.relayPath)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    }
                    Button("Choose relay executable…") { relay.chooseRelay() }
                }

                Section("Relay") {
                    Toggle("Enable traffic diagnostics", isOn: $relay.diagnostics)
                    LabeledContent("Status") {
                        Text(relay.status)
                            .foregroundStyle(relay.relayPID == nil ? Color.secondary : Color.green)
                    }
                    HStack {
                        Button("Start relay", systemImage: "play.fill") { relay.start() }
                            .buttonStyle(.borderedProminent)
                            .disabled(!relay.canStart || relay.isStarting || relay.relayPID != nil)
                        Button("Stop", systemImage: "stop.fill") { relay.stop() }
                            .disabled(relay.relayPID == nil || relay.isStarting)
                        Button("Show log") { showLog = true }
                    }
                }

                Section("Switch") {
                    Text("Switch detection will appear here after packet-level discovery is connected to the relay status channel.")
                        .foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
        }
        .sheet(isPresented: $showLog) {
            RelayLogView(log: relay.relayLog())
        }
    }

    private var header: some View {
        HStack(spacing: 14) {
            Image(systemName: "network")
                .font(.system(size: 30))
                .foregroundStyle(.tint)
            VStack(alignment: .leading, spacing: 2) {
                Text("GRID0 Relay")
                    .font(.title2.weight(.semibold))
                Text("Connect native Switch LAN play through ZeroTier")
                    .foregroundStyle(.secondary)
            }
            Spacer()
        }
        .padding(20)
        .background(.bar)
    }
}

private struct RelayLogView: View {
    let log: String
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading) {
            HStack {
                Text("Relay log").font(.headline)
                Spacer()
                Button("Done") { dismiss() }
            }
            ScrollView {
                Text(log.isEmpty ? "No relay output yet." : log)
                    .font(.system(.body, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
                    .padding(.vertical, 4)
            }
        }
        .padding(20)
        .frame(width: 680, height: 440)
    }
}
