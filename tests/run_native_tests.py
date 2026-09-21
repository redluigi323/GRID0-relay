"""Native relay regressions and local UDP socket tests (build project first)."""
import pathlib
import struct
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
build = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'build'


def checksum(data):
    if len(data) & 1:
        data += b'\0'
    total = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def read_frame(path):
    data = path.read_bytes()
    endian = '<' if data[:4] == b'\xd4\xc3\xb2\xa1' else '>'
    assert struct.unpack(endian + 'I', data[20:24])[0] == 1  # Ethernet
    caplen, wirelen = struct.unpack(endian + 'II', data[32:40])
    assert caplen == wirelen == 662
    assert len(data) == 40 + caplen
    return data[40:]


with tempfile.TemporaryDirectory(prefix='zll-tests-') as tmp:
    binary = pathlib.Path(tmp) / 'native-test'
    prefix = str(pathlib.Path(tmp) / 'trace')
    cmd = ['cc', '-g', '-ffunction-sections', '-fdata-sections', '-UNDEBUG',
           '-DLANPLAY_LITTLE_ENDIAN']
    cmd += ['-DLANPLAY_DARWIN'] if sys.platform == 'darwin' else ['-DLANPLAY_LINUX']
    for include in ('base/include', 'external/libuv/include', 'uv_lwip',
                    'lwip/custom', 'lwip/src/include'):
        cmd += ['-I' + str(root / include)]
    cmd += [str(root / 'tests/native_relay_test.c'), str(root / 'src/arp.c'),
            str(root / 'src/dhcp-server.c'),
            str(build / 'base/libbase.a'), str(build / 'external/libuv/libuv_a.a'),
            '-lpcap', '-lpthread',
            '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
            '-o', str(binary)]
    subprocess.run(cmd, check=True)
    subprocess.run([str(binary), prefix], check=True)
    rx = read_frame(pathlib.Path(prefix + '-wifi-rx.pcap'))
    tx = read_frame(pathlib.Path(prefix + '-zerotier-tx.pcap'))
    assert rx[42:] == tx[42:]
    assert tx[:6] == bytes([2, 0x31, 0x32, 0x33, 0x34, 0x35])
    for frame in (rx, tx):
        ip = frame[14:]
        assert checksum(ip[:20]) == 0
        udp = ip[20:]
        pseudo = ip[12:20] + b'\0\x11' + struct.pack('!H', len(udp))
        assert checksum(pseudo + udp) == 0
        assert len(udp) - 8 == 620
    print('PASS: independent PCAP decode, 662-frame/620-payload equivalence, IP/UDP checksums')
    udp_binary = pathlib.Path(tmp) / 'native-udp-test'
    subprocess.run([
        'cc', '-g', '-UNDEBUG', '-I' + str(root / 'base/include'),
        '-I' + str(root / 'external/libuv/include'),
        str(root / 'tests/native_udp_test.c'), str(root / 'src/native-udp.c'),
        str(build / 'base/libbase.a'), str(build / 'external/libuv/libuv_a.a'),
        '-lpthread', '-o', str(udp_binary)
    ], check=True)
    subprocess.run([str(udp_binary)], check=True)
    io_binary = pathlib.Path(tmp) / 'pcap-io-test'
    io_cmd = ['c++', '-std=c++17', '-g', '-ffunction-sections', '-fdata-sections',
              '-UNDEBUG', '-DLANPLAY_LITTLE_ENDIAN',
              '-DLANPLAY_DARWIN' if sys.platform == 'darwin' else '-DLANPLAY_LINUX']
    for include in ('base/include', 'external/libuv/include'):
        io_cmd += ['-I' + str(root / include)]
    io_cmd += [str(root / 'tests/pcap_io_test.cpp'), str(build / 'base/libbase.a'),
               str(build / 'external/libuv/libuv_a.a'), '-lpcap', '-lpthread',
               '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
               '-o', str(io_binary)]
    subprocess.run(io_cmd, check=True)
    subprocess.run([str(io_binary)], check=True)
