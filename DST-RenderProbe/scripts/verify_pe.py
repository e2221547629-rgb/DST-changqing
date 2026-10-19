"""Check a built x64 Windows DLL without importing or executing it."""
import hashlib
import json
from pathlib import Path
import struct
import sys


def verify(path):
    data = path.read_bytes()
    def u16(offset):
        return struct.unpack_from('<H', data, offset)[0]
    def u32(offset):
        return struct.unpack_from('<I', data, offset)[0]
    def require(condition, message):
        if not condition:
            raise ValueError(message)
    def cstring(offset):
        return data[offset:data.index(b'\0', offset)].decode('ascii')

    require(data[:2] == b'MZ', 'Missing DOS header')
    pe = u32(0x3c)
    require(data[pe:pe + 4] == b'PE\0\0', 'Missing PE header')
    require(u16(pe + 4) == 0x8664, 'Not AMD64')
    require(u16(pe + 22) & 0x2000, 'Not a DLL')
    opt = pe + 24
    require(u16(opt) == 0x20b, 'Not PE32+')
    section_table = opt + u16(pe + 20)
    sections = []
    for i in range(u16(pe + 6)):
        pos = section_table + 40 * i
        sections.append((u32(pos + 12), u32(pos + 16), u32(pos + 20)))

    def file_offset(rva):
        for start, raw_size, raw_offset in sections:
            if start <= rva < start + raw_size:
                return raw_offset + rva - start
        raise ValueError(f'Unmapped RVA {rva:#x}')

    export_rva = u32(opt + 112)
    require(export_rva != 0, 'No exports')
    exports = file_offset(export_rva)
    names = file_offset(u32(exports + 32))
    exported_names = [cstring(file_offset(u32(names + 4 * i)))
                      for i in range(u32(exports + 24))]
    require({'NAME', 'DESCRIPTION'} <= set(exported_names), 'Missing add-on metadata exports')
    imports = []
    import_rva = u32(opt + 120)
    require(import_rva != 0, 'No imports')
    descriptor = file_offset(import_rva)
    while any(data[descriptor:descriptor + 20]):
        imports.append(cstring(file_offset(u32(descriptor + 12))))
        descriptor += 20
    require('user32.dll' in {x.lower() for x in imports}, 'Missing user32 dependency')
    require(not any(x.lower().startswith(('vcruntime', 'msvcp', 'ucrtbase', 'api-ms-win-crt'))
                    for x in imports), 'Unexpected dynamic MSVC runtime dependency')
    result = dict(file=path.name, machine='AMD64', format='PE32+ DLL', size=len(data),
                  exports=exported_names, imports=imports, sha256=hashlib.sha256(data).hexdigest())
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Usage: verify_pe.py FILE.addon64')
    verify(Path(sys.argv[1]))
