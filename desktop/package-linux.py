#!/usr/bin/env python3
"""Package a Linux x86_64 build as an AppImage."""
import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import urllib.request
from pathlib import Path

APPIMAGETOOL_URL = 'https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage'
APPIMAGETOOL_SHA = '46fdd785094c7f6e545b61afcfb0f3d98d8eab243f644b4b17698c01d06083d1'

# Libraries that must come from the player's own system: the C runtime and
# loader, the graphics stack their drivers were built against, and the buses a
# desktop session owns. Everything else the build links against is bundled.
SYSTEM_LIBRARIES = {
    'linux-vdso.so.1', 'ld-linux-x86-64.so.2', 'libc.so.6', 'libm.so.6', 'libdl.so.2',
    'libpthread.so.0', 'librt.so.1', 'libresolv.so.2', 'libnsl.so.1', 'libutil.so.1',
    'libanl.so.1', 'libmvec.so.1', 'libgcc_s.so.1',
    'libGL.so.1', 'libEGL.so.1', 'libGLX.so.0', 'libGLdispatch.so.0', 'libOpenGL.so.0',
    'libglapi.so.0', 'libdrm.so.2', 'libgbm.so.1',
    'libdbus-1.so.3', 'libudev.so.1', 'libselinux.so.1', 'libsystemd.so.0',
    # Capture belongs to the host, like the ZeroTier daemon it talks to.
    'libpcap.so.0.8', 'libpcap.so.1',
}

# Qt plugins the app actually loads. The dependency walk below pulls in whatever
# each of these needs.
QT_PLUGINS = ['platforms/libqxcb.so', 'platforms/libqwayland-generic.so', 'platforms/libqoffscreen.so',
              'xcbglintegrations/libqxcb-glx-integration.so', 'imageformats/libqjpeg.so',
              'iconengines/libqsvgicon.so', 'platformthemes/libqxdgdesktopportal.so']

DESKTOP_ENTRY = """[Desktop Entry]
Type=Application
Name=GRID0 Relay
GenericName=Nintendo Switch LAN play relay
Comment=Nintendo Switch LAN play over ZeroTier
Exec=Grid0Relay
Icon=grid0-relay
Categories=Network;Game;
Terminal=false
StartupWMClass=Grid0Relay
"""

APPRUN = """#!/bin/sh
# The GUI runs as the player. It asks pkexec for the relay's capture privileges
# later, and pkexec deliberately discards the environment set here.
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/usr/plugins/platforms"
exec "$HERE/usr/bin/Grid0Relay" "$@"
"""


def download(url, target, expected):
    if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == expected:
        return target
    print('Downloading', url, flush=True)
    temp = target.with_suffix(target.suffix + '.part')
    with urllib.request.urlopen(url, timeout=180) as source, temp.open('wb') as output:
        shutil.copyfileobj(source, output)
    if hashlib.sha256(temp.read_bytes()).hexdigest() != expected:
        temp.unlink()
        raise RuntimeError('SHA-256 mismatch: ' + url)
    temp.replace(target)
    target.chmod(target.stat().st_mode | stat.S_IXUSR)
    return target


def dependencies(binary):
    """Shared libraries ldd resolves for one file, by name."""
    output = subprocess.run(['ldd', str(binary)], capture_output=True, text=True).stdout
    found = {}
    for line in output.splitlines():
        line = line.strip()
        if '=>' in line:
            name, _, rest = line.partition('=>')
            path = rest.strip().split(' (')[0].strip()
            name = name.strip()
            if path in ('not found', ''):
                if name not in SYSTEM_LIBRARIES:
                    raise RuntimeError(f'{binary}: {name} is not installed on this build machine')
                continue
            found[name] = Path(path)
        elif line.startswith('/') and '(' in line:
            path = Path(line.split(' (')[0])
            found[path.name] = path
    return found


def linked(binary):
    """Names in one file's own NEEDED entries, without their transitive closure."""
    output = subprocess.check_output(['readelf', '-d', str(binary)], text=True)
    return [line.split('[')[1].rstrip(']') for line in output.splitlines() if 'NEEDED' in line]


def bundle(targets, destination):
    """Copy every non-system library the targets need into destination."""
    destination.mkdir(parents=True, exist_ok=True)
    queue = list(targets)
    copied = {}
    while queue:
        for name, path in dependencies(queue.pop()).items():
            if name in SYSTEM_LIBRARIES or name in copied:
                continue
            local = destination / name
            shutil.copy2(path.resolve(), local)
            copied[name] = path
            queue.append(local)
    return copied


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path, default=Path('dist/linux-x64'))
    parser.add_argument('--appimagetool', type=Path, default=os.environ.get('APPIMAGETOOL'),
                        help='appimagetool binary; downloaded to the build directory when omitted')
    parser.add_argument('--qt-plugins', type=Path, help='Qt plugin directory; asked of qtpaths6 when omitted')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    binaries = {name: args.build / 'desktop' / name for name in ('Grid0Relay', 'grid0-relay', 'grid0-relay-supervisor')}
    for name, path in binaries.items():
        if not path.is_file():
            parser.error(f'{path} is missing. Build with -DZLL_BUILD_GUI=ON first.')
    if args.output.exists():
        parser.error('Use a fresh output directory; existing packages are never overwritten.')

    plugins = args.qt_plugins
    if not plugins:
        for query in (['qtpaths6', '--query', 'QT_INSTALL_PLUGINS'], ['qtpaths', '--query', 'QT_INSTALL_PLUGINS'],
                      ['qmake6', '-query', 'QT_INSTALL_PLUGINS']):
            try:
                plugins = Path(subprocess.check_output(query, text=True).strip())
                break
            except (OSError, subprocess.CalledProcessError):
                continue
    if not plugins or not plugins.is_dir():
        parser.error('Cannot locate the Qt plugin directory; pass --qt-plugins.')

    appdir = args.output / 'AppDir'
    (appdir / 'usr/bin').mkdir(parents=True)
    for name, path in binaries.items():
        shutil.copy2(path, appdir / 'usr/bin' / name)

    bundled_plugins = []
    for relative in QT_PLUGINS:
        source = plugins / relative
        if not source.is_file():
            continue
        target = appdir / 'usr/plugins' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        bundled_plugins.append(target)
    if not (appdir / 'usr/plugins/platforms/libqxcb.so').is_file():
        parser.error('Qt\'s xcb platform plugin is missing; install the Qt 6 GUI packages.')

    libraries = bundle([appdir / 'usr/bin/Grid0Relay'] + bundled_plugins, appdir / 'usr/lib')
    # The relay and its launcher are started by pkexec from outside this image,
    # so what they link against directly must all come from the host. libpcap's
    # own dependencies are the host libpcap's business, not ours.
    for name in ('grid0-relay', 'grid0-relay-supervisor'):
        for library in linked(appdir / 'usr/bin' / name):
            if library not in SYSTEM_LIBRARIES:
                raise RuntimeError(f'{name} links {library}, which pkexec would not find outside the '
                                   'AppImage. Configure with -DZLL_STATIC_RUNTIME=ON.')

    (appdir / 'AppRun').write_text(APPRUN, encoding='utf-8')
    (appdir / 'AppRun').chmod(0o755)
    (appdir / 'grid0-relay.desktop').write_text(DESKTOP_ENTRY, encoding='utf-8')
    applications = appdir / 'usr/share/applications'
    applications.mkdir(parents=True)
    shutil.copy2(appdir / 'grid0-relay.desktop', applications / 'grid0-relay.desktop')

    icon = appdir / 'grid0-relay.png'
    environment = os.environ | {'QT_QPA_PLATFORM': 'offscreen', 'QT_PLUGIN_PATH': str(plugins)}
    subprocess.run([str(appdir / 'usr/bin/Grid0Relay'), '--write-icon', str(icon), '256'],
                   check=True, env=environment)
    icons = appdir / 'usr/share/icons/hicolor/256x256/apps'
    icons.mkdir(parents=True)
    shutil.copy2(icon, icons / 'grid0-relay.png')
    shutil.copy2(icon, appdir / '.DirIcon')

    shutil.copy2(root / 'LICENSE.txt', appdir / 'usr/share/LICENSE.txt')
    shutil.copy2(root / 'FORK_NOTICE.md', appdir / 'usr/share/FORK_NOTICE.md')
    shutil.copy2(root / 'desktop/THIRD-PARTY.md', appdir / 'usr/share/THIRD-PARTY.md')

    tool = Path(args.appimagetool) if args.appimagetool else download(
        APPIMAGETOOL_URL, args.build / 'appimagetool-x86_64.AppImage', APPIMAGETOOL_SHA)
    archive = args.output / 'GRID0-Relay-x86_64.AppImage'
    # Extract-and-run keeps this working on build machines without FUSE.
    subprocess.run([str(tool), str(appdir), str(archive)], check=True,
                   env=os.environ | {'ARCH': 'x86_64', 'APPIMAGE_EXTRACT_AND_RUN': '1'})
    archive.chmod(0o755)

    (args.output / 'build-info.json').write_text(json.dumps({
        'target': 'Linux x86_64',
        'qtPlugins': [str(p.relative_to(appdir)) for p in bundled_plugins],
        'bundledLibraries': sorted(libraries),
        'systemRequirements': ['glibc of the build machine or newer', 'libpcap', 'ZeroTier One', 'pkexec for the relay prompt'],
    }, indent=2) + '\n', encoding='utf-8')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (args.output / 'SHA256SUMS.txt').write_text(f'{digest}  {archive.name}\n', encoding='utf-8')
    print(f'Packaged {archive.resolve()} with {len(libraries)} bundled libraries')


if __name__ == '__main__':
    main()
