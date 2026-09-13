#!/usr/bin/env python3
"""Download pinned build SDKs. This never installs the Npcap runtime/driver."""
import argparse
import hashlib
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile
from pathlib import Path

QT_VERSION = '6.11.2'
QT_ARCHIVE = '6.11.2-0-202608131017qtbase-Windows-Windows_11_24H2-Mingw-Windows-Windows_11_24H2-X86_64.7z'
QT_URL = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6112/qt6_6112_mingw/qt.qt6.6112.win64_mingw/' + QT_ARCHIVE
QT_SOURCE = 'qtbase-everywhere-src-6.11.2.tar.xz'


def download(url, target, expected):
    if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == expected: return
    temp = target.with_suffix(target.suffix + '.part')
    print('Downloading', url, flush=True)
    with urllib.request.urlopen(url, timeout=90) as source, temp.open('wb') as output:
        shutil.copyfileobj(source, output)
    if hashlib.sha256(temp.read_bytes()).hexdigest() != expected:
        temp.unlink(); raise RuntimeError('SHA-256 mismatch: ' + url)
    temp.replace(target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('deps/windows'))
    parser.add_argument('--with-qt', action='store_true', help='Also fetch Windows Qt and its source for packaging')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cache = args.output / 'downloads'; cache.mkdir(exist_ok=True)
    sdk = cache / 'npcap-sdk-1.16.zip'
    download('https://npcap.com/dist/npcap-sdk-1.16.zip', sdk, 'f0a8be7778ee3ae1b99bbbecb27a3ff0f6c111a4093f1c78c5c5a099607184db')
    destination = args.output / 'npcap-sdk'
    if not destination.exists():
        with zipfile.ZipFile(sdk) as zip:
            for info in zip.infolist():
                if not (destination / info.filename).resolve().is_relative_to(destination.resolve()):
                    raise RuntimeError('Unsafe SDK archive path')
            zip.extractall(destination)
    if args.with_qt:
        archive = cache / QT_ARCHIVE
        download(QT_URL, archive, '8bbd42aa6b7dbb3ac9c88762bbf6f974e29348054186ef5cb920b105c786d81d')
        qt = args.output / 'qt'
        if not qt.exists():
            # macOS and Windows bsdtar can unpack 7z. On Linux use 7zz/7z.
            with tempfile.TemporaryDirectory(dir=args.output) as stage:
                seven = shutil.which('7zz') or shutil.which('7z')
                command = [seven, 'x', '-y', '-o' + stage, str(archive)] if seven else ['tar', 'xf', str(archive), '-C', stage]
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
                shutil.move(stage, qt)
        source = cache / QT_SOURCE
        download('https://download.qt.io/official_releases/qt/6.11/6.11.2/submodules/' + QT_SOURCE, source,
                 '5b2e00eccaf5a4d8c14134ffa0ea8dfd0a35ae1ffc7f8d87fa4305a1ed23cf22')
        print('Windows Qt:', qt.resolve(), '\nQt source:', source.resolve())
    print('Npcap SDK:', destination.resolve())


if __name__ == '__main__': main()
