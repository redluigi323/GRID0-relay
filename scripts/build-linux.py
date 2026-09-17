#!/usr/bin/env python3
"""Build and package the Linux x86_64 AppImage, on Linux or in a container from a Mac."""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=root / 'build-linux')
parser.add_argument('--output', type=Path, default=root / 'dist/linux-x64')
parser.add_argument('--native', action='store_true', help='Build here instead of in a container')
parser.add_argument('--image', default='grid0-relay-linux-builder')
parser.add_argument('--container-tool', help='docker or podman; found on PATH when omitted')
parser.add_argument('--no-package', action='store_true', help='Build only, skip the AppImage')
args = parser.parse_args()

native = args.native or (platform.system() == 'Linux' and not args.container_tool)
if native:
    if platform.system() != 'Linux':
        parser.error('A native build needs a Linux host; drop --native to build in a container.')
    for tool in ['cmake', 'readelf'] + ([] if args.no_package else ['desktop-file-validate']):
        if not shutil.which(tool):
            parser.error(f'{tool} is required. On Debian and Ubuntu: apt install cmake binutils desktop-file-utils')
    subprocess.run(['cmake', '-S', str(root), '-B', str(args.build), '-DCMAKE_BUILD_TYPE=Release',
                    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DZLL_BUILD_GUI=ON', '-DZLL_STATIC_RUNTIME=ON'], check=True)
    subprocess.run(['cmake', '--build', str(args.build), '--parallel', '4'], check=True)
    if not args.no_package:
        subprocess.run([sys.executable, str(root / 'desktop/package-linux.py'),
                        '--build', str(args.build), '--output', str(args.output)], check=True)
    raise SystemExit(0)

tool = args.container_tool or shutil.which('docker') or shutil.which('podman')
if not tool:
    parser.error('Docker or Podman is required to build Linux from this host. Install Docker Desktop, '
                 'or run this script on a Linux machine with --native.')
subprocess.run([tool, 'build', '--platform', 'linux/amd64', '-t', args.image,
                '-f', str(root / 'docker/Dockerfile.linux'), str(root / 'docker')], check=True)
# The container writes into the repository as the invoking user, so the build
# and dist directories do not come back owned by root.
command = [tool, 'run', '--rm', '--platform', 'linux/amd64',
           '-v', f'{root}:/src', '-w', '/src', '-e', 'HOME=/tmp']
if os.name == 'posix' and platform.system() != 'Darwin':
    command += ['--user', f'{os.getuid()}:{os.getgid()}']
command += [args.image, 'python3', 'scripts/build-linux.py', '--native',
            '--build', '/src/' + str(args.build.relative_to(root)),
            '--output', '/src/' + str(args.output.relative_to(root))]
if args.no_package:
    command.append('--no-package')
subprocess.run(command, check=True)
