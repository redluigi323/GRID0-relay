# Qt desktop application

Qt 6 Widgets supplies the native style, fonts, palette, dialogs and controls. The UI intentionally has no replacement stylesheet. On macOS, regular tabs use QMacStyle's rounded controls; document tabs and fixed oversized button heights are avoided. System appearance changes are left to Qt.

## Daily use

1. Connect the computer and stock Switch to the same local network. Connect the native ZeroTier client.
2. In Settings → Connection, select the local and ZeroTier interfaces once. The app remembers the names and does not silently replace a saved, missing adapter. Refresh after a network change.
3. Copy the IP, exact mask, and fake gateway shown on Play into the Switch. Reconnect/restart LAN mode after any change. The current desktop setup supports one Switch on a `/24` overlay.
4. Start the relay and approve the macOS administrator dialog. The app reports Running only after the relay's readiness message. No password enters the Qt app.
5. Stop the relay or close the app when finished. Cmd-Q also waits for shutdown. Cmd-comma opens Settings.

A detected device is a console candidate inferred from local traffic, not vendor-authenticated hardware identity. Runtime errors and a broadcast-mask mismatch are shown on Play; detailed logs remain under Advanced.

Some access points forward a console's unicast traffic with a different Ethernet source address. The relay prefers an ARP claim or LAN broadcast when choosing the console's return address, then keeps that address stable for the session. Alternate sources for the same IP are still forwarded and counted in diagnostics. If you change consoles or the console's IP, stop and restart the relay. A conflicting address can also mean a duplicate IP, so keep only one local console on the displayed address.

IPv6 can remain enabled on the console and PC. GRID0 forwards the game's IPv4 LAN traffic; it does not tunnel IPv6 or use IPv6 packets to select the console's IPv4 return address.

After stopping, the log includes capture health for each adapter MAC: packets delivered to the relay, read/injection errors, and driver buffer/interface drop counters. Driver counter meanings vary by platform; a zero may mean unavailable and does not prove delivery to another console. Full batches count Windows capture reads that reached the 128-packet batch limit, not dropped packets. Windows requests a 4 MiB capture buffer per adapter to absorb brief stalls while keeping immediate capture enabled. See [Npcap's counter definitions](https://npcap.com/guide/wpcap/pcap_stats.html).

Packet recording batches disk flushes after 32 records or on traffic after 100 ms. Normal shutdown flushes every remaining record. A forced kill or crash can lose the final buffered records (up to 31 per capture file), so stop the relay normally before exporting a report.

The custom relay field in Advanced is optional. Clearing it restores the bundled relay. Default paths are resolved at launch, so moving the app does not break that setting.

## Diagnostics and reports

Verbose diagnostics and packet capture default off. Every launch records a text log and session metadata in the app's local data reports folder (Application Support on macOS, Local AppData on Windows). Captures, when enabled, are created alongside them with a unique session prefix. The report directory is private to the user; root-created PCAPs are readable inside it. Nothing is uploaded automatically. Stop first, then use Export report to copy the completed session into a chosen folder. The original stays available through Open reports folder.

The visible log retains 2,000 lines; the on-disk text log is capped around 16 MiB. Packet captures are not size-limited, so enable them for a reproduction session rather than leaving them on indefinitely. Reports contain local/overlay IPs and may contain game payloads.

## macOS and Linux launch boundary

The app runs as the logged-in user. A quoted `osascript` command on macOS, or `pkexec` on Linux, requests authorization for the bundled `grid0-relay-supervisor`, which connects to the app's private Unix socket and starts one relay child with explicit arguments. Only the authorized root peer is accepted by the app: macOS reads it with `getpeereid`, Linux with `SO_PEERCRED`. The supervisor verifies the GUI peer's UID, forwards child output, and accepts only a stop message. It executes no commands from that connection and never handles a password. Linux passes the arguments to `pkexec` directly rather than through a shell.

Where there is no polkit agent to ask, Start reports that and prints the equivalent `sudo` command instead of failing silently.

Running from an AppImage adds one step. Its contents live on a FUSE mount that only the user who started it can read, so root could not execute the launcher or relay from there. The app copies both into its own private data directory, mode 0700, and runs those copies. Root therefore executes a binary from a user-owned path: acceptable for this development authorization design on a single-user machine, and the reason a system install is preferable for shared machines. The relay and launcher link libstdc++ and libgcc statically for this reason, so neither needs anything from inside the AppImage once `pkexec` has cleared the environment.

The supervisor owns the child PID and waits for it. Stop, socket disconnection, or termination signals request SIGINT; a child that fails to exit is killed after five seconds and reaped. There is no installed privileged daemon, global firewall change, or detached relay left deliberately running after app closure. The relay's existing singleton guard also rejects legacy/background duplicates.

This is a development authorization design. Public distribution still needs a reviewed signing/notarization workflow; packaging below produces an ad-hoc signed local app.

## Build and package

Build from the project root using the README's `ZLL_BUILD_GUI=ON` command. The CLI alone still builds with this option off and does not require Qt.

```sh
python3 desktop/package-macos.py --build build --output dist/qt-macos --macdeployqt "$(brew --prefix qtbase)/bin/macdeployqt"
```

Use a fresh output folder; the script refuses to replace an existing app. It copies the relay and supervisor, bundles Qt frameworks/plugins, creates the app icon, applies/verifies a development signature, and creates a ZIP. The resulting `.app` includes Qt; users still need the native ZeroTier client. This Mac build targets Apple Silicon and its configured SDK/deployment target. It is not a universal binary.

## Verification

```sh
ctest --test-dir build/desktop --output-on-failure
python3 tests/test_desktop_supervisor.py build/desktop/grid0-relay-supervisor
python3 tests/run_native_tests.py
```

The tests cover settings persistence, subnet derivation, invalid/missing adapters, gateway validation, shell/AppleScript quoting, relay forwarding, and actual supervisor child cleanup on Stop/disconnection. The supervisor tests use unprivileged mock children and local IPC. They do not automatically approve or exercise the macOS password dialog.

On Linux the same checks apply, using `build-linux/desktop` as the build directory. `tests.cpp` also covers the launcher's behaviour with no polkit agent present.

Window-only UI previews do not start a relay or save preferences:

```sh
build/desktop/Grid0Relay.app/Contents/MacOS/Grid0Relay --preview --screenshot /tmp/play.png
build/desktop/Grid0Relay.app/Contents/MacOS/Grid0Relay --preview --settings --advanced --screenshot /tmp/advanced.png
```

## Platform boundary

The Qt views, settings, validation and adapter discovery use portable APIs. macOS and Linux use the privileged supervisor described above. Windows has a MinGW x64 build, Npcap runtime loading, friendly adapter selection and event-based Start/Stop; this preview elevates the app at launch. The Windows style is Qt Widgets' `windows11` plugin. See [Windows setup, cross-compilation and packaging](../docs/windows.md).

Windows binaries and mock tests have been cross-compiled, but Windows execution and gameplay still require a Windows machine or CI runner. The Linux app ships as an AppImage carrying Qt; it expects the host's libpcap and ZeroTier, and its relay path has not been tested against a console yet. Cross-platform source still requires a compiler and Qt SDK for each target.
