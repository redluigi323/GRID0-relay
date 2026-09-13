"""Small PE reader for auditing Windows x64 packages without running them."""
import struct
from pathlib import Path


class PE:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        offset = self.u32(0x3c)
        if self.data[:2] != b'MZ' or self.data[offset:offset + 4] != b'PE\0\0':
            raise ValueError(f'Not a PE executable: {path}')
        if self.u16(offset + 4) != 0x8664 or self.u16(offset + 24) != 0x20b:
            raise ValueError(f'Expected Windows x64 PE32+: {path}')
        self.directory = offset + 24 + 112
        section = offset + 24 + self.u16(offset + 20)
        self.sections = []
        for i in range(self.u16(offset + 6)):
            vsize, rva, size, raw = struct.unpack_from('<IIII', self.data, section + 40 * i + 8)
            self.sections.append((rva, max(vsize, size), raw))

    def u16(self, pos): return struct.unpack_from('<H', self.data, pos)[0]
    def u32(self, pos): return struct.unpack_from('<I', self.data, pos)[0]
    def u64(self, pos): return struct.unpack_from('<Q', self.data, pos)[0]
    def offset(self, rva):
        for start, size, raw in self.sections:
            if start <= rva < start + size: return raw + rva - start
        raise ValueError(f'Invalid RVA {rva:x} in {self.path}')
    def string(self, rva):
        pos = self.offset(rva)
        return self.data[pos:self.data.index(b'\0', pos)].decode('ascii')

    def imports(self):
        result = {}
        rva = self.u32(self.directory + 8)
        if not rva: return result
        pos = self.offset(rva)
        while any(self.data[pos:pos + 20]):
            original, _, _, name, first = struct.unpack_from('<IIIII', self.data, pos)
            symbols = set()
            table = self.offset(original or first)
            while self.u64(table):
                value = self.u64(table)
                symbols.add(value & 0xffff if value >> 63 else self.string(value + 2))
                table += 8
            result[self.string(name).lower()] = symbols
            pos += 20
        # These application/Qt builds have no delay imports. Fail explicitly if
        # a future SDK adds them instead of silently claiming a complete audit.
        if self.u32(self.directory + 13 * 8):
            raise ValueError(f'Delay-import audit is not implemented: {self.path}')
        return result

    def exports(self):
        rva = self.u32(self.directory)
        if not rva: return set()
        pos = self.offset(rva)
        base, count, names, funcs, names_rva, ordinals = struct.unpack_from('<IIIIII', self.data, pos + 16)
        result = {base + i for i in range(count) if self.u32(self.offset(funcs) + i * 4)}
        result.update(self.string(self.u32(self.offset(names_rva) + i * 4)) for i in range(names))
        return result


SYSTEM_DLLS = set('''advapi32 authz bcrypt bcryptprimitives cfgmgr32 comctl32 comdlg32
crypt32 cryptbase d3d9 d3d11 d3d12 d3dcompiler_47 dbghelp dnsapi dwmapi dwrite dxgi
 gdi32 imm32 iphlpapi kernel32 kernelbase mpr msimg32 msvcrt netapi32 ntdll ole32
 oleaut32 opengl32 powrprof profapi propsys psapi rpcrt4 secur32 setupapi shcore
 shell32 shlwapi user32 userenv usp10 uxtheme version winhttp winmm winspool
 wintrust ws2_32 wtsapi32 ucrtbase ncrypt normaliz'''.split())


def is_system(name):
    return name.startswith(('api-ms-win-', 'ext-ms-win-')) or name.removesuffix('.dll') in SYSTEM_DLLS


def audit(directory):
    root = Path(directory)
    files = list(root.rglob('*.exe')) + list(root.rglob('*.dll'))
    libraries = {p.name.lower(): p for p in root.glob('*.dll')}
    exports = {name: PE(path).exports() for name, path in libraries.items()}
    dependencies = {}
    for path in files:
        imported = PE(path).imports()
        dependencies[str(path.relative_to(root))] = sorted(imported)
        for name, symbols in imported.items():
            if name in libraries:
                missing = symbols - exports[name]
                if missing:
                    raise ValueError(f'{path.name}: {name} lacks imports {sorted(map(str, missing))[:8]}')
            elif not is_system(name):
                raise ValueError(f'{path.name}: missing DLL {name}')
    return dependencies
