# GRID0 Relay

GRID0 Relay is a GPLv3 fork of switch-lan-play intended to connect a stock Nintendo Switch in native LAN mode with a sys-zerotier Switch. It retains upstream's pcap Wi-Fi handling, ARP mediation, fake-gateway/lwIP code, and cross-platform adapter discovery.

The custom UDP relay path has been replaced in the native launch path by a separate ZeroTier capture/injection handle. The relay sends proxy ARP and IPv4 frames through it and reconstructs frames for the local Wi-Fi interface. It does not require ZeroTier's Allow Bridging option because pcap injects with the relay's own adapter MAC on each link.

Run native mode with an explicit Wi-Fi and ZeroTier capture interface. The default gaming subnet is `10.147.17.0/24` and its local fake-gateway address is `10.147.17.1`; both are runtime settings.

```sh
sudo ./build/src/grid0-relay --netif en0 --zerotier-if ZEROTIER_INTERFACE
```

Use `--list-if` to find capture-interface names. For Splatoon/sys-zerotier, copy the **IP and exact subnet mask of the selected ZeroTier adapter** to the Switch, along with the fake gateway. The relay prints all three at startup; the Qt app displays them on Play. Do not pick a different Switch IP or reuse the legacy `255.0.0.0` mask for a `/24` ZeroTier network. Reconnect the Switch and restart LAN mode after changing these settings. To use another range, pass matching values to the relay:

```sh
sudo ./build/src/grid0-relay --netif en0 --zerotier-if ZEROTIER_INTERFACE \\
  --subnet 10.147.23.0/24 --gateway 10.147.23.1
```

The relay only captures ARP and IPv4 after opening the two named adapters. It never asks ZeroTier to bridge foreign Ethernet MAC addresses.

The subnet mask matters to the game itself: PIA's authenticated LAN challenge includes the subnet broadcast address in its encryption nonce. Changing only the packet's destination cannot repair a challenge made for a different broadcast address. See the [LAN protocol research](https://github.com/kinnay/NintendoClients/wiki/LAN-Protocol#crypto-challenge). Incoming broadcasts now preserve the ZeroTier subnet broadcast; outgoing UDP/35000 with a conflicting legacy broadcast produces an explicit warning.

Do not use the remaining legacy `--relay-server-addr` option for the intended sys-zerotier path.

Read [FORK_NOTICE.md](FORK_NOTICE.md) for upstream attribution and licensing. The full GPLv3 text is in [LICENSE.txt](LICENSE.txt).

## Build on macOS

The fork vendors the upstream-pinned libuv and uvw submodules. With Xcode Command Line Tools and CMake installed:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
./build/src/grid0-relay --version
```

The compatibility setting is needed by the historical, pinned libuv CMake files. Their compiler warnings are upstream dependency warnings.

## Qt desktop app

The desktop UI now lives in [desktop](desktop), using Qt 6 Widgets with the system style and palette. On macOS, the Play page gives the relay summary and Switch-settings panel a restrained native Liquid Glass treatment through the vendored [qt-liquid-glass](https://github.com/fsalinas26/qt-liquid-glass) library. macOS 26 uses `NSGlassEffectView`; earlier supported macOS versions use the library's visual-effect fallback. The earlier SwiftUI prototype is retained under `macos/Grid0RelayApp` as reference, but is not part of the current desktop build.

On macOS:

```sh
brew install qtbase cmake
cmake -S . -B build -DZLL_BUILD_GUI=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build build --parallel 4
open build/desktop/Grid0Relay.app
```

**Play** provides Start/Stop, console detection, and the exact Switch network settings. **Settings → Connection** remembers adapter selection and the fake gateway. **Settings → Advanced** contains diagnostics, packet captures, automatic discovery, a custom relay executable, and report export. On macOS, the UI remains unprivileged and requests administrator authorization only when starting the relay. The first Windows preview requests UAC elevation when the app opens. Stopping or closing the window shuts down the child relay. See [desktop/README.md](desktop/README.md) for packaging, testing and platform status.

The user reported successful Splatoon 3 room discovery and hosting after matching the Switch mask to ZeroTier and preserving the broadcast address. Captures 04/05 contain session UDP in both directions. This validates that tested setup, not every game/router/platform combination.

For a failed lobby test, use `--diagnostics` on the CLI or enable detailed diagnostics in Advanced. Optional packet captures contain game payloads and network addresses and stay local until exported. [docs/relay-diagnostics.md](docs/relay-diagnostics.md) explains the traffic entries. Lightweight `--status-events` reports console candidates without enabling verbose packet logging.

## Windows x64 preview

The Qt desktop app and native Npcap relay now cross-compile for Windows x64. The Windows package includes Qt's Windows 11 style and its runtime DLLs. Players install **Npcap** and **ZeroTier One** separately; the SDK is only needed at build time. Automatic hotspot/ICS/dependency installation is not included.

See [Windows setup and build instructions](docs/windows.md). On this Mac, with MinGW-w64, CMake, Ninja and Qt 6.11.2 host tools installed:

```sh
python3 scripts/fetch-windows-deps.py --with-qt
python3 scripts/build-windows.py --host-qt "$(brew --prefix qtbase)"
```

The Windows executables are compiled and their packaged DLL dependencies checked. Windows gameplay and administrator launch remain unverified until tested on a Windows PC. Native Windows CI is configured for compilation and mock launcher tests.
