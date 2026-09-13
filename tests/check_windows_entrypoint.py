"""Check the linked GUI startup route, including the historical looping build."""
import argparse
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary')
parser.add_argument('--objdump', default='x86_64-w64-mingw32-objdump')
args = parser.parse_args()
code = subprocess.check_output([args.objdump, '-d', '--disassemble=WinMain', args.binary], text=True)
assert '<WinMain>:' in code, 'Missing WinMain entry point'
assert re.search(r'\b(call|jmp)\b[^\n]*<[^>]*grid0RelayMain[^>]*>', code), 'WinMain does not reach the application entry point'
assert not re.search(r'\b(call|jmp)\b[^\n]*<main>', code), 'WinMain calls the MinGW fallback main: startup recursion'
print('PASS: linked WinMain reaches grid0RelayMain without recursing through main')
