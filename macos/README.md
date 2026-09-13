# Grid0 Relay for macOS

The current desktop application is the [Qt app in ../desktop](../desktop/README.md). The SwiftUI package below is the earlier prototype, retained for reference. Use the root README's Qt build and packaging commands for new work.

`Grid0Relay` is a native SwiftUI frontend for the existing CMake relay. Open [Grid0RelayApp/Package.swift](Grid0RelayApp/Package.swift) in Xcode, then run the `Grid0Relay` scheme.

The app discovers local adapters, prefills `en0` as the Wi-Fi side and a ZeroTier-looking adapter such as `feth1082` as the overlay side, and lets the user change both. It also exposes the game subnet, fake gateway, automatic local-Switch discovery, relay-binary path, traffic-diagnostics toggle, relay log, and a future Switch-discovery status area.

Pressing **Start relay** invokes macOS's standard administrator credential dialog. The app does not receive, store, or send the password. It starts the selected relay in the background and writes its output to `/var/tmp/grid0-relay.log`.

For development, build the relay from the repository root first:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

Then choose `build/src/grid0-relay` in the app if it does not find it automatically. The Swift package needs macOS 14 or later and Xcode 26 or a compatible Swift toolchain.

The package is intentionally separate from the CMake target. A later packaging pass will add a bundled relay binary and signed privileged-launch design for distribution as a standalone `.app`.
