# Bundled components

GRID0 Relay is GPLv3; see LICENSE.txt and FORK_NOTICE.md. Its source archive includes the native relay, Qt launcher, build scripts, and the upstream-pinned libuv/uvw/lwIP sources with their notices. Session logs and packet captures are excluded.

On macOS, the desktop app statically includes the vendored [qt-liquid-glass](https://github.com/fsalinas26/qt-liquid-glass) project by Fernando Salinas. It provides the native Liquid Glass effects used by the relay-summary and Switch-settings surfaces. The upstream repository identifies the project as MIT licensed; its source and upstream README are retained under `external/qt-liquid-glass`.

The Windows package dynamically links Qt 6.11.2 (Qt Core, Gui, Network, Widgets and platform/style plugins). Qt is available under LGPLv3/GPLv3 and commercial terms; this package uses the open-source distribution. Qt license texts and third-party notices are in `licenses/qt`. The matching, unmodified Qt source archive is included under `source`, including Qt's build instructions and third-party sources. The DLLs can be replaced with compatible builds. Source: https://download.qt.io/official_releases/qt/6.11/6.11.2/submodules/qtbase-everywhere-src-6.11.2.tar.xz

MinGW GCC runtime DLLs (libgcc and libstdc++) use GPLv3 with the GCC Runtime Library Exception. See `licenses/GCC-GPL3.txt` and `licenses/GCC-RUNTIME.txt`. Compiler version is recorded in build-info.json. GCC sources: https://gcc.gnu.org/releases.html. MinGW-w64 and winpthreads notices are in `licenses/mingw-w64.txt` and `licenses/winpthreads.txt`; sources: https://www.mingw-w64.org/.

Npcap SDK declarations are used to build the runtime loader, but the Npcap driver, wpcap.dll and Packet.dll are not redistributed. Install Npcap separately from https://npcap.com. ZeroTier One is also a separate install from https://www.zerotier.com/download/.
