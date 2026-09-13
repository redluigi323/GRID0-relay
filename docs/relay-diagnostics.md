# Relay diagnostics

Run from the project directory:

```sh
sudo ./build/src/grid0-relay --netif en0 --zerotier-if feth1082 --diagnostics --capture-prefix ./lobby-test-01
```

Replace adapter names with the selected local interfaces. Use a new capture prefix for each run; existing files are never overwritten. The parent directory must exist. Ctrl-C flushes and closes the captures.

## What the traces prove

RX means the relay observed a frame on the selected adapter. TX means the relay attempted pcap injection; send errors are logged separately. Neither TX nor a successful pcap call proves receipt by a console or acceptance by the game. No current header-only log identifies a PIA validation failure.

UDP diagnostics give both Ethernet frame length and UDP payload length, plus a non-cryptographic payload fingerprint. A 662-byte Ethernet frame with 20-byte IPv4 and 8-byte UDP headers contains **620 bytes of game payload**: 662 - 14 - 20 - 8 = 620. This matches the old working sys-zerotier log's `len 620`; it was not evidence of a different update or room format. Fingerprints help correlate copies, while packet captures allow exact byte comparison.

ICMP diagnostics report type/code and decode the quoted UDP tuple in destination-unreachable/time-exceeded messages. `proto=1` alone never established a game handshake. The second captured test contained 22 incoming UDP/35000 unicast packets with 620-byte payloads. All were submitted to Wi-Fi unchanged with valid checksums; macOS also emitted eight ICMP type 3/code 3 port-unreachable errors quoting this traffic. This establishes a host UDP endpoint problem, but does not prove why the game rejected or missed the replies.

## Packet capture files

The optional prefix creates five standard Ethernet PCAP files:

- `lobby-test-01-wifi-rx.pcap`: observed local traffic.
- `lobby-test-01-wifi-tx.pcap`: frames submitted for local transmission.
- `lobby-test-01-zerotier-rx.pcap`: observed remote traffic.
- `lobby-test-01-zerotier-tx.pcap`: frames submitted for overlay transmission.
- `lobby-test-01-host.pcap`: observed ICMP with either selected adapter's own source MAC. These packets are logged but never forwarded.

Captures include game payloads. Normal text diagnostics include sizes, endpoints, and fingerprints rather than full payloads. Timestamps reflect relay processing time, using a shared monotonic clock anchored to wall-clock seconds at startup; they are not radio delivery timestamps. Wi-Fi discovery ARP probes are counted separately and are not included in the TX capture. The host file can include observed copies of relay-injected ICMP as well as OS-generated ICMP; compare the TX files before attributing a packet to the OS.

These captures work entirely on the Mac; a new modded-console log is not required to collect them. For a later peer test, keep host and client attempts in separate runs, so packet direction can be compared to the selected role.

## Comparison with upstream switch-lan-play

Upstream `src/ipv4/ipv4.c` forwards the console's IPv4 packet into its relay client. `src/lan-client.c` wraps it in a separate UDP transport and reconstructs Ethernet delivery on receipt, using learned local MAC addresses. It does not continuously manufacture game packets or rewrite PIA session data. Its periodic keepalive maintains its own relay transport. It also uses Ethernet unicast fanout for some received broadcasts.

This project instead carries IPv4 directly on the native ZeroTier adapter and maps the console's source address to the adapter's managed address. Incoming broadcasts preserve the ZeroTier subnet broadcast. The older forced translation to `10.255.255.255` has been removed. Outgoing legacy broadcast translation remains for compatibility, but UDP/35000 at a mismatched broadcast prints a warning: it cannot repair PIA authentication. This exposes traffic to the host OS stack, unlike upstream's inner game packets. macOS generated ICMP port-unreachable for unbound game UDP ports in the second captured test. The filter exposes host-source ICMP instead of hiding it.

The relay now binds UDP/35000 and UDP/49152–49155 on the dynamically detected managed IPv4 address before startup completes. Additional Switch source ports are reserved before their first outgoing datagram, up to 64 total ports per run. These sockets drain the host's duplicate copies; pcap remains the only forwarding path. A conflicting application causes a clear startup error for the initial ports, or an error and dropped outgoing datagram for a newly observed port. Sockets are released on shutdown. No firewall, global ICMP suppression, or routing settings are changed. The shutdown counter `Host UDP socket copies drained` confirms how many packets reached those sockets; it does not measure console delivery.

Do not assume removing ICMP errors guarantees a lobby: the inspected sys-zerotier `VNet::onIcmp` handles echo requests and ignores other ICMP types. The peer's installed revision and game acceptance are not established by this Mac capture.

## Local IPv4 delivery probe

With diagnostics enabled, the first incoming unicast UDP/35000 reply also triggers one local ICMP echo from the fake gateway to the detected Switch's IP and MAC. A matching checksum-valid echo response with the per-run payload prints `Local IPv4 delivery confirmed`. The relay consumes that response locally; it never sends it into ZeroTier. No extra game packets or periodic game traffic are generated.

A matching echo establishes that this local unicast IPv4 exchange worked, not that Nintendo accepted the UDP reply or its payload. If there is no match, shutdown says delivery remains unconfirmed. A console can ignore ICMP, so absence is not proof of an access-point failure. The probe is recorded in Wi-Fi TX/RX captures and identified separately in the console log.

Overlay neighbors are now learned from incoming IPv4/ARP in a separate cache. Unicast replies use the learned remote Ethernet MAC; unknown destinations get an ARP request plus a broadcast fallback to avoid dropping the first packet. Local and overlay ARP tables cannot overwrite each other's entries. The unsolicited ARP-before-every-packet experiment was removed; ordinary Wi-Fi proxy ARP replies remain.

## Address setup and limitations

The relay reads the selected ZeroTier adapter's IPv4 and netmask. For Splatoon testing, use both the managed IP and its exact mask on the stock Switch. Copy the fake gateway printed at startup as well. Reconnect and restart LAN mode after changing settings. These settings align the game's address and broadcast with the overlay; they are not proof of lobby discovery.

Test 03 confirmed local IPv4 echo delivery and removal of host port-unreachable errors, but no room appeared. Offline AES-GCM verification of all five captured search challenges succeeded using `10.255.255.255` in the nonce and failed using the ZeroTier broadcast `10.147.17.255`. The [documented PIA challenge format](https://github.com/kinnay/NintendoClients/wiki/LAN-Protocol#crypto-challenge) includes the broadcast address in the nonce. Therefore preserving UDP payload bytes while translating between these broadcasts is insufficient. For this capture's adapter, the Switch needs mask `255.255.255.0`; the old `/8` broadcast must disappear. The CLI now prints the detected settings and warns about the mismatch, and the UI displays the selected adapter's mask. The relay does not modify encrypted game payloads or include game keys.

Automatic discovery scans the configured /24 game range using ARP; an observed address is a **Switch candidate**, not verified device identity. `--no-discover-switch` disables the scan.

The user confirmed successful Splatoon 3 room discovery and hosting in tests 04/05 after aligning the mask and broadcast. Both captures contain session UDP in both directions. This verifies the tested setup rather than all games, routers or platforms. Offline tests exercise unfragmented discovery/reply forwarding, checksums, exact payload preservation, separate neighbor caches, unknown-peer fallback, capture output, host-frame loop prevention, and diagnostic echo matching. Loopback socket tests exercise endpoint binding, conflict handling, datagram draining, and release/rebind. Running the loopback tests requires permission to bind local UDP sockets.

```sh
python3 tests/run_native_tests.py
```

Build the project first. An alternate CMake build directory can be supplied as the script's first argument.

## Avoid overlapping relay instances on macOS

The first captured test was contaminated by an older GUI-launched relay that had remained running as root since the previous day. Closing a terminal or rebuilding the executable does not stop such a detached process. Both instances used the same adapters and sent different versions of received packets onto Wi-Fi.

On macOS, the CLI now refuses startup if another executable named `grid0-relay` is running, including legacy versions, and new instances also hold a process-lifetime lock at `/var/run/grid0-relay.lock`. The lock file is retained after exit; its presence alone does not mean a relay is running. Do not delete the file to bypass the lock. The current implementation permits one native relay per Mac.

The current Qt launcher waits for `Relay started (PID ...)` after full initialization and reports early startup errors. Its supervisor sends SIGINT and reaps its relay child on Stop or app disconnection. The older SwiftUI prototype could leave a detached process; the CLI's existing-process message identifies that PID if it blocks a new test.
