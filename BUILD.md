# Building GRID0 Relay

This is mostly here for people who want to build it themselves instead of
waiting for a release. You will need a compiler, CMake, and the right Qt
install for your platform.

## macOS (ARM)

This is the easiest one to build right now.

1. Install Xcode command line tools, Homebrew, CMake, and Qt:

   ```bash
   xcode-select --install
   brew install cmake qtbase
   ```

2. Clone the repo with submodules, then enter it:

   ```bash
   git clone --recurse-submodules https://github.com/redluigi323/GRID0-relay.git
   cd GRID0-relay
   ```

3. Configure and build the Qt GUI and relay:

   ```bash
   cmake -S . -B build -DZLL_BUILD_GUI=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
   cmake --build build --parallel 4
   ```

4. Your app will be at:

   ```text
   build/desktop/Grid0Relay.app
   ```

5. To make a shareable app bundle and ZIP, run:

   ```bash
   python3 desktop/package-macos.py --build build --output dist/macos --macdeployqt "$(brew --prefix qtbase)/bin/macdeployqt"
   ```

The packaged app bundles Qt. Users still need the ZeroTier app installed on
their Mac. macOS already includes libpcap, so Npcap is not needed here.

## Windows

You can build this natively on Windows, or cross compile it from macOS.
The Windows build needs a MinGW-w64 compiler, CMake, Ninja, Python 3, and Qt.

### Building directly on Windows

The easiest way is using **MSYS2 UCRT64**. Do not use Visual Studio for this
one: the checked-in Windows Qt SDK is made for MinGW, so you need a MinGW-w64
compiler too.

1. Install [MSYS2](https://www.msys2.org/), run its update command, then close
   and reopen the **MSYS2 UCRT64** terminal when it asks you to.

   ```bash
   pacman -Syu
   ```

2. In the UCRT64 terminal, install the build tools:

   ```bash
   pacman -S --needed git python mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja p7zip
   ```

3. Clone GRID0 Relay with its submodules:

   ```bash
   git clone --recurse-submodules https://github.com/redluigi323/GRID0-relay.git
   cd GRID0-relay
   ```

   The submodules are not optional. GitHub's "Download ZIP" button leaves
   `external/libuv` and `external/uvw` empty, and a clone without
   `--recurse-submodules` does the same. The build checks for them and stops
   with a message instead of failing halfway through.

4. Download the build-only Qt and Npcap SDK files, then build and package:

   ```bash
   python scripts/fetch-windows-deps.py --with-qt
   python scripts/build-windows.py --compiler g++
   ```

5. The release ZIP will be under `dist/windows-x64/`. Extract the whole ZIP
   before running `GRID0Relay.exe`.

   Packaging never writes over a folder that already exists. Building a second
   time means deleting `dist/windows-x64/` first, or passing a new path with
   `--output`.

The downloaded Npcap SDK is only used to compile. It is not the capture driver
players need. The finished app can offer to install Npcap and ZeroTier when it
starts, or the player can install them beforehand.

If CMake says it cannot find `g++`, you opened the regular MSYS terminal by
mistake. Close it and open **MSYS2 UCRT64** instead.

If it stops with `libuv is missing from external/libuv`, run
`git submodule update --init --recursive` from the top of the repository. If it
instead says a folder under `external/` does not contain what belongs there,
something else got checked out over it: delete that folder and run the same
command again. Configuring a copy of this project as its own dependency is what
produces a wall of "another target with the same name already exists" errors.

### Cross compiling from macOS

On a Mac, the quickest way is:

```bash
brew install mingw-w64 cmake ninja qtbase
python3 scripts/build-windows.py --host-qt "$(brew --prefix qtbase)"
```

The script downloads the matching Windows Qt and Npcap SDK needed to compile.
It does not put Npcap or ZeroTier into the release. The app handles helping the
user install those later.

Your packaged Windows release will be in `dist/`. It includes `GRID0Relay.exe`,
the CLI relay, Qt runtime files, licenses, and the buildable source.

## CLI only

If you only want the relay and do not want the GUI, Qt is not required:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build --parallel 4
```

The native relay is built under `build/src/`.

## Before making a release

Run the tests after building:

```bash
ctest --test-dir build/desktop --output-on-failure
python3 tests/run_native_tests.py
```

GitHub Actions runs the same native tests on Linux and macOS for every push,
and builds and tests the packaged Windows launcher on a Windows runner.

Do not commit `build/`, `dist/`, downloaded SDKs, packet captures, or logs.
They are ignored already. Put the finished macOS and Windows ZIPs on a GitHub
Release instead.
