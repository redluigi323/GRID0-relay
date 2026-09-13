#!/usr/bin/env python3
"""Package a MinGW Windows x64 build on macOS, Linux, or Windows."""
import argparse
import hashlib
import json
import shutil
import subprocess
import tarfile
import zipfile
import sys
from pathlib import Path
from windows_pe import PE, audit, is_system


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build-windows'))
    parser.add_argument('--qt', type=Path, required=True, help='Windows Qt prefix, containing bin and plugins')
    parser.add_argument('--qt-source', type=Path, required=True, help='Matching Qt source .tar.xz to include')
    parser.add_argument('--compiler', default='x86_64-w64-mingw32-g++')
    parser.add_argument('--output', type=Path, default=Path('dist/windows-x64'))
    parser.add_argument('--include-tests', action='store_true', help='Include launcher mocks/tests for CI')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    subprocess.run([sys.executable, str(root / 'tests/check_windows_entrypoint.py'),
                    str(args.build / 'desktop/Grid0Relay.exe'),
                    '--objdump', args.compiler.replace('g++', 'objdump')], check=True)
    if args.output.exists(): parser.error('Use a fresh output directory; existing packages are never overwritten.')
    if not args.qt_source.is_file(): parser.error('The matching Qt source archive is required.')
    app = args.output / 'GRID0-Relay'
    app.mkdir(parents=True)
    for name in ['Grid0Relay.exe', 'grid0-relay.exe'] + (['zll-desktop-tests.exe', 'zll-test-relay.exe', 'zll-startup-test.exe', 'zll-layout-tests.exe'] if args.include_tests else []):
        packaged = 'GRID0Relay.exe' if name == 'Grid0Relay.exe' else name
        shutil.copy2(args.build / 'desktop' / name, app / packaged)
    for name in ['platforms/qwindows.dll', 'styles/qmodernwindowsstyle.dll'] + (['platforms/qoffscreen.dll'] if args.include_tests else []):
        dest = app / 'plugins' / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.qt / 'plugins' / name, dest)
    # Prefer the compiler actually used, not a potentially older runtime in Qt.
    runtime = Path(subprocess.check_output([args.compiler, '-print-file-name=libstdc++-6.dll'], text=True).strip()).resolve()
    if not runtime.is_file(): parser.error('Cannot locate the selected MinGW compiler runtime.')
    compiler_path = Path(shutil.which(args.compiler)).resolve()
    search = [runtime.parent, runtime.parent.parent / 'bin', compiler_path.parent, args.qt / 'bin']
    candidates = {}
    for directory in search:
        for path in directory.glob('*.dll'): candidates.setdefault(path.name.lower(), path)
    queue = list(app.rglob('*.exe')) + list(app.rglob('*.dll'))
    copied = set()
    while queue:
        for name in PE(queue.pop()).imports():
            if is_system(name) or name in copied: continue
            if name not in candidates: raise RuntimeError(f'Missing runtime {name}; checked {search}')
            dest = app / candidates[name].name
            shutil.copy2(candidates[name], dest); copied.add(name); queue.append(dest)
    (app / 'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=plugins\n', encoding='utf-8')
    shutil.copy2(root / 'LICENSE.txt', app / 'LICENSE.txt')
    shutil.copy2(root / 'FORK_NOTICE.md', app / 'FORK_NOTICE.md')
    shutil.copy2(root / 'docs/windows.md', app / 'README-Windows.md')
    shutil.copytree(root / 'desktop/licenses', app / 'licenses')
    shutil.copy2(root / 'desktop/THIRD-PARTY.md', app / 'THIRD-PARTY.md')
    source = app / 'source'; source.mkdir()
    shutil.copy2(args.qt_source, source / args.qt_source.name)
    # Explicit source allowlist excludes session logs, PCAPs, build caches, and
    # developer credentials. It includes the pinned libuv/uvw implementation.
    allowed = ['CMakeLists.txt', 'README.md', 'LICENSE.txt', 'FORK_NOTICE.md', '.gitmodules',
               'src', 'base', 'cmake', 'desktop', 'external', 'lwip', 'uv_lwip', 'tests', 'scripts', 'docs', '.github']
    with tarfile.open(source / 'GRID0-Relay-source.tar.gz', 'w:gz') as tar:
        for name in allowed:
            item = root / name
            files = sorted(item.rglob('*')) if item.is_dir() else [item]
            for file in files:
                if not file.is_file() or file.is_symlink(): continue
                if any(part in ('.git', '__pycache__', '.DS_Store') for part in file.parts): continue
                if file.suffix in ('.pyc', '.pcap', '.log'): continue
                tar.add(file, arcname='GRID0-Relay/' + str(file.relative_to(root)), recursive=False)
    # Strip only copies; retain the build's debugging information locally.
    strip = shutil.which(args.compiler.replace('g++', 'strip'))
    if strip:
        for file in list(app.rglob('*.exe')) + list(app.rglob('*.dll')):
            subprocess.run([strip, '--strip-debug', str(file)], check=True)
    dependencies = audit(app)
    (app / 'build-info.json').write_text(json.dumps({
        'target': 'Windows x64', 'compiler': subprocess.check_output([args.compiler, '--version'], text=True).splitlines()[0],
        'qt': '6.11.2', 'npcap': 'Installed separately; dynamically loaded',
        'dependencies': dependencies,
    }, indent=2) + '\n', encoding='utf-8')
    checksums = {str(p.relative_to(app)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(app.rglob('*')) if p.is_file()}
    (app / 'SHA256SUMS.txt').write_text(''.join(f'{digest}  {name}\n' for name, digest in checksums.items()), encoding='utf-8')
    archive = args.output / 'GRID0-Relay-Windows-x64.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zip:
        for path in sorted(app.rglob('*')):
            if path.is_file(): zip.write(path, path.relative_to(args.output))
    print(f'Packaged and audited {len(dependencies)} Windows executables/libraries: {archive.resolve()}')


if __name__ == '__main__': main()
