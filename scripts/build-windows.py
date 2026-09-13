#!/usr/bin/env python3
"""Build/package Windows x64 on Windows (MinGW), macOS, or Linux."""
import argparse
import os
from pathlib import Path
import subprocess
import shutil
import sys

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--deps', type=Path, default=root / 'deps/windows')
parser.add_argument('--build', type=Path, default=root / 'build-windows')
parser.add_argument('--output', type=Path, default=root / 'dist/windows-x64')
parser.add_argument('--host-qt', type=Path, help='Host Qt 6.11.2 prefix, required for cross compilation')
parser.add_argument('--compiler', default='g++' if os.name == 'nt' else 'x86_64-w64-mingw32-g++')
parser.add_argument('--include-tests', action='store_true')
parser.add_argument('--no-package', action='store_true')
args = parser.parse_args()
compiler = shutil.which(args.compiler)
c_compiler = shutil.which(args.compiler.replace('g++', 'gcc'))
if not compiler or not c_compiler: parser.error('The selected MinGW C/C++ compilers must be on PATH.')
deps, build = args.deps.resolve(), args.build.resolve()
if not (deps / 'qt/lib/cmake/Qt6').is_dir() or not (deps / 'npcap-sdk/Include').is_dir():
    parser.error('Run scripts/fetch-windows-deps.py --with-qt first.')
command = ['cmake', '--fresh', '-S', str(root), '-B', str(build), '-G', 'Ninja',
           '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DZLL_BUILD_GUI=ON',
           '-DQt6_DIR=' + str(deps / 'qt/lib/cmake/Qt6'), '-DCMAKE_PREFIX_PATH=' + str(deps / 'qt'),
           '-DNPCAP_SDK_DIR=' + str(deps / 'npcap-sdk')]
if os.name != 'nt':
    if not args.host_qt: parser.error('--host-qt is required on macOS/Linux (same Qt version as the Windows SDK).')
    command += ['-DCMAKE_TOOLCHAIN_FILE=' + str(root / 'cmake/toolchains/windows-mingw.cmake'),
                '-DQT_HOST_PATH=' + str(args.host_qt.resolve())]
# These flags also override the toolchain default for a selected compiler.
command += ['-DCMAKE_CXX_COMPILER=' + compiler, '-DCMAKE_C_COMPILER=' + c_compiler]
subprocess.run(command, check=True)
subprocess.run(['cmake', '--build', str(build), '--parallel', '4'], check=True)
if not args.no_package:
    command = [sys.executable, str(root / 'desktop/package-windows.py'), '--build', str(build),
               '--qt', str(deps / 'qt'), '--qt-source', str(deps / 'downloads/qtbase-everywhere-src-6.11.2.tar.xz'),
               '--compiler', args.compiler, '--output', str(args.output.resolve())]
    if args.include_tests: command += ['--include-tests']
    subprocess.run(command, check=True)
