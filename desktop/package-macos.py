"""Bundle Qt and the relay into an ad-hoc signed development app and ZIP."""
import argparse
import os
import pathlib
import shutil
import subprocess
import tempfile
import plistlib
import struct
import re

p = argparse.ArgumentParser()
p.add_argument('--build', type=pathlib.Path, default=pathlib.Path('build'))
p.add_argument('--output', type=pathlib.Path, default=pathlib.Path('dist'))
p.add_argument('--macdeployqt', default='macdeployqt')
args = p.parse_args()
source = args.build.resolve() / 'desktop/Grid0Relay.app'
destination = args.output.resolve() / 'GRID0 Relay.app'
args.output.mkdir(parents=True, exist_ok=True)
if destination.exists():
    raise SystemExit('Output app already exists; use a fresh output folder.')
shutil.copytree(source, destination, symlinks=True)
with tempfile.TemporaryDirectory(prefix='zll-icon-') as temporary:
    work = pathlib.Path(temporary)
    original = work / 'icon.png'
    icon_environment = os.environ | {'QT_QPA_PLATFORM': 'offscreen'}
    subprocess.run([str(source / 'Contents/MacOS/Grid0Relay'), '--write-icon', str(original)], check=True, env=icon_environment)
    # ICNS can contain standard PNG payloads. Writing the six current icon
    # sizes directly avoids iconutil rejecting valid iconsets on some macOS 26
    # builds, while preserving a full-resolution Finder and Dock icon.
    payloads = [(16, 'icp4'), (32, 'icp5'), (128, 'ic07'), (256, 'ic08'),
                (512, 'ic09'), (1024, 'ic10')]
    chunks = []
    for size, kind in payloads:
        png = work / f'icon-{size}.png'
        subprocess.run(['sips', '-z', str(size), str(size), str(original), '--out', str(png)], check=True, stdout=subprocess.DEVNULL)
        data = png.read_bytes()
        chunks.append(kind.encode('ascii') + struct.pack('>I', len(data) + 8) + data)
    icon = destination / 'Contents/Resources/AppIcon.icns'
    data = b''.join(chunks)
    icon.write_bytes(b'icns' + struct.pack('>I', len(data) + 8) + data)
info = destination / 'Contents/Info.plist'
with info.open('rb') as file:
    values = plistlib.load(file)
values['CFBundleDisplayName'] = 'GRID0 Relay'
values['CFBundleIconFile'] = 'AppIcon.icns'
with info.open('wb') as file:
    plistlib.dump(values, file)
subprocess.run([args.macdeployqt, str(destination), '-always-overwrite'], check=True)
executable = destination / 'Contents/MacOS/Grid0Relay'
# macdeployqt normally rewrites these paths. Keep a final explicit pass because
# a Homebrew Qt executable can retain absolute framework paths, which loads a
# second Qt beside the bundle's Cocoa plugin and aborts during QApplication.
for line in subprocess.check_output(['otool', '-L', str(executable)], text=True).splitlines()[1:]:
    loaded = line.strip().split(' ', 1)[0]
    match = re.search(r'/((Qt[^/]+)\.framework/Versions/A/\2)$', loaded)
    if not match or loaded.startswith('@executable_path/../Frameworks/'):
        continue
    framework = match.group(2)
    bundled = f'@executable_path/../Frameworks/{framework}.framework/Versions/A/{framework}'
    subprocess.run(['install_name_tool', '-change', loaded, bundled, str(executable)], check=True)
# macdeployqt handles the Qt executable; our bundled helper and relay use system libraries.
subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(destination)], check=True)
subprocess.run(['codesign', '--verify', '--deep', '--strict', str(destination)], check=True)
archive = args.output.resolve() / 'GRID0-Relay-macOS.zip'
subprocess.run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', str(destination), str(archive)], check=True)
print(destination)
print(archive)
