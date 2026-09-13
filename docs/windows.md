# Windows preview — Grid0 Relay

This is a Windows x64 build of the Qt desktop app and the native ZeroTier relay. The macOS version has been tested successfully with Splatoon 3 hosting and room discovery. This Windows port has been cross-compiled and its packaged DLL imports audited; Windows gameplay, adapter injection and UAC behavior still need hardware testing. It is an unsigned development build.

## Set up and play

1. Extract the **whole ZIP** to a folder, then open **Grid0Relay.exe**. Approve Windows' administrator prompt.
2. If ZeroTier One or Npcap is missing, Grid0 Relay offers the official installer in **Settings → Connection**. It downloads each installer over HTTPS, asks Windows to validate its Authenticode signature, installs ZeroTier, and opens Npcap's official installer so you can approve its driver terms. The Npcap SDK is only for developers; players do not need it.
3. If WinPcap is detected, use the app's **Open Apps & Features** button to remove it, then reopen Grid0 Relay before installing Npcap.
4. Join and authorize the same ZeroTier network as your friend. Give each ZeroTier member its own managed IPv4 address; the desktop UI currently supports a `/24` network (`255.255.255.0`).
5. Connect the PC and Switch to the same local network. In **Settings → Connection**, choose the PC's local Wi-Fi/Ethernet adapter and the ZeroTier adapter. Friendly names and IPs are shown; choices are saved. If an adapter is missing, connect ZeroTier and refresh.
6. Enter the **exact IP, subnet mask and fake gateway shown on Play** into the Switch's manual network settings. The Switch uses this PC's ZeroTier managed IP on its separate physical LAN. Reconnect the Switch and restart the game after changes. Enter Splatoon 3's native LAN mode, not local wireless mode.
6. Click **Start relay**. Stop it or close the app when finished. Your friend can use sys-zerotier on the same network, as in the working macOS setup.

Npcap's normal Ethernet-compatible capture is used. Raw 802.11/monitor mode and WinPcap compatibility mode are not required. There is no separate relay server and no requirement to enable ZeroTier Ethernet bridging. A connection-test pass alone does not prove lobby discovery works.

This preview uses the existing local-network method. It does not configure Mobile Hotspot, ICS, routes, firewall rules, or a fake DHCP server. You can try a manually configured hotspot by selecting its actual Switch-facing adapter, but this topology has not been validated on Windows. Router/client isolation can prevent the PC and Switch from exchanging traffic.

## Troubleshooting

- **Npcap could not be loaded:** install/repair current Npcap, then reopen the app. The relay loads `%SystemRoot%\System32\Npcap\wpcap.dll` explicitly; it does not use an old WinPcap DLL from another directory.
- **ZeroTier adapter unavailable:** install/connect ZeroTier, join/authorize the network, and refresh adapters. The selected interface needs a managed IPv4 address.
- **Cannot open capture / access denied:** run the app as administrator, confirm Npcap's driver is installed, and check the selected adapter. Raw Wi-Fi injection remains dependent on the driver and access point.
- **No room:** verify identical game versions, native LAN mode, exact mask/IP settings, and a friend actually hosting a room. Enable **Settings → Advanced → Detailed traffic diagnostics**. Packet captures are optional and include game payloads and addresses.
- **Ports already in use:** stop another relay instance or the application using the reported UDP port. The relay reserves local game ports to prevent the host network stack from rejecting incoming packets.

Reports stay under the current Windows account's local application data, normally `%LOCALAPPDATA%\Grid0 Relay\Grid0Relay\reports`. Use **Open reports folder** to find the exact path, then stop the relay and **Export report**. No reports are uploaded automatically.

The CLI is included as `grid0-relay.exe`. From an administrator PowerShell in the extracted folder:

```powershell
.\grid0-relay.exe --list-if
.\grid0-relay.exe --netif '\Device\NPF_{LOCAL-GUID}' --zerotier-if '\Device\NPF_{ZEROTIER-GUID}' --diagnostics
```

Use the names printed by `--list-if`, including braces. The GUI translates Windows adapter GUIDs to Npcap names automatically. Press Ctrl-C to stop the CLI.

## Build Windows from macOS

You need a Windows compiler and **two Qt installations**: Windows libraries and matching macOS tools (moc/rcc). A macOS Qt installation alone cannot link a Windows executable. The checked-in scripts pin Qt 6.11.2 and Npcap SDK 1.16 with SHA-256 checks. Downloads are build dependencies, not automatic installation on a player's PC.

```sh
brew install mingw-w64 qtbase cmake ninja
python3 scripts/fetch-windows-deps.py --with-qt
python3 scripts/build-windows.py --host-qt "$(brew --prefix qtbase)"
```

The host Qt tools must be **6.11.2**, matching the pinned Windows SDK. If Homebrew has moved on, use a matching Qt installation and pass its prefix to `--host-qt`; do not combine different moc/header versions. This build was compiled with Homebrew MinGW GCC 16.2.0 and the official Qt 6.11.2 MinGW SDK. Compiler runtime DLLs are taken from the selected compiler, and bundled imports/exports are checked during packaging. Runtime testing is still required for this cross-toolchain combination.

Output: `dist/windows-x64/Grid0-Relay-Windows-x64.zip`. The package includes Qt's Windows 11 style, compiler DLLs, license notices, this project's buildable source, and the matching Qt source archive. Npcap and ZeroTier are downloaded only after an explicit user request; neither is included in the package. Choose a fresh `--output` directory when repackaging; existing packages are not overwritten. Use `--no-package` for a build only. SDK downloads may be cached; no private captures are added to the source archive.

On Linux, the same scripts work with `mingw-w64`, Ninja, matching host Qt tools and `7zz`/`7z` to extract the Qt archive. Linux-host cross compilation has not been tested here.

## Build natively on Windows

Install Python 3.10+, CMake, Ninja, and a MinGW-w64 x64 compiler on PATH. Use one MinGW toolchain consistently; the supplied Windows Qt SDK is a MinGW build, not an MSVC SDK. The MSVC build path is not validated.

```powershell
python scripts/fetch-windows-deps.py --with-qt
python scripts/build-windows.py --compiler g++
```

For native launcher tests, package into a separate fresh folder with `--include-tests`, then run the packaged `zll-desktop-tests.exe`. These tests use a mock relay, need no Npcap or Switch, and cover Start, Stop, repeated sessions and preserving startup errors. They do not prove Npcap injection or Splatoon interoperability. `.github/workflows/build.yml` includes a Windows native build and these tests; that workflow must run on GitHub to provide Windows execution results.

The underlying CMake targets remain usable directly. For a CLI-only cross build (no Qt):

```sh
cmake -S . -B build-windows-cli -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-mingw.cmake \
  -DNPCAP_SDK_DIR="$PWD/deps/windows/npcap-sdk" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-windows-cli --parallel 4
```

## Implementation notes

The Windows relay polls nonblocking Npcap captures in bounded batches on the libuv loop, avoiding the old capture-thread shutdown deadlock. Windows adapter MACs come from `GetAdaptersAddresses`. Packet capture files use Unicode paths and OS handles rather than exchanging `FILE*` values across C runtimes. Stop signals a per-session Windows event; the relay also watches the desktop PID and exits if its parent disappears. A timeout can terminate only the child process started by this controller. The original macOS privileged supervisor remains in use on macOS.

The Windows look is Qt Widgets' `windows11` style, with system theme/palette support. It is not a WinUI application. See Qt's [styling documentation](https://doc.qt.io/qt-6/qwidget-styling.html) and Npcap's [developer guide](https://npcap.com/guide/npcap-devguide.html).

## 0.6.1 startup fix

The first 0.6.0 Windows preview had an entry-point loop: Qt renamed the application `main` to `qMain`, while the custom WinMain called MinGW's fallback `main`, which called WinMain again. The app could consume a CPU core without creating any window. Version 0.6.1 uses an explicitly named application entry point, and a linked-binary regression check rejects the original broken executable.

End any stuck older Grid0 Relay processes in Task Manager, extract the replacement ZIP into a fresh folder, and run **Grid0Relay.exe**. The dashed `grid0-relay.exe` is the command-line relay and does not have a GUI.

Windows startup stages and Qt messages are appended to `%TEMP%\Grid0-Relay-startup.log`. If no window appears, include that file in a report. Each line is tagged with its process ID; no packet capture is enabled by this log. A failure before the application entry point may not create a log.

The CI GUI smoke test uses the same entry point and window sources without an administrator manifest, renders a preview through the offscreen Qt plugin, and requires exit within 20 seconds. UAC and real desktop appearance still require a Windows PC. This test is configured for Windows CI, not claimed as executed on the Mac.

## 0.6.2 capture and layout fixes

Windows Npcap interface matching is now case-insensitive, so the GUI's uppercase GUID matches a lowercase ID returned by Npcap. The relay opens the exact name returned by Npcap and never substitutes another adapter. Capture failures now preserve the local/ZeroTier side and the failing operation or missing adapter name. Positive `pcap_activate` results remain warnings, as required by Npcap's API, instead of aborting an already activated capture.

The Switch settings use a grid with minimum row sizes. The Play page scrolls when long errors or font scaling need more room, preserving visible IP/mask/gateway values. If capture still fails, export the report from Advanced; the screenshot of `uv_pcap_init` alone cannot distinguish adapter lookup, permissions, driver activation, MAC lookup, filter, and event-loop errors.

## 0.6.3 Windows interface-name resolution

Qt can identify adapters using Windows LUID names such as `wireless_32768` and `ethernet_32769`. These are not Npcap paths. The app now uses `ConvertInterfaceNameToLuidW` and `ConvertInterfaceLuidToGuid` to obtain the selected adapter's GUID, then builds its Npcap path. This applies to both the local and ZeroTier interfaces. Unresolvable names block Start with an actionable message; the app does not guess from an IP or select another adapter. Session logs include both the Qt names and resulting capture paths. Existing saved adapter selections remain usable.
