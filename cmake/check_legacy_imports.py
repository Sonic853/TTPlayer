"""Reject PE imports missing from YY-Thunks' XP and Win7 export inventories.

No third-party Python modules or DLLs from an old Windows installation needed.
This is a loader check; it does not replace playback/UI tests on those systems.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def inspect(path):
    data = path.read_bytes()

    def unpack(fmt, offset):
        return struct.unpack_from('<' + fmt, data, offset)

    if data[:2] != b'MZ':
        raise ValueError('Missing DOS header')
    pe, = unpack('I', 0x3C)
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Missing PE header')
    machine, count = unpack('HH', pe + 4)
    size, = unpack('H', pe + 20)
    optional = pe + 24
    if machine != 0x14C or unpack('H', optional)[0] != 0x10B:
        raise ValueError('The legacy player must be x86 PE32')
    os_version = unpack('HH', optional + 40)
    subsystem_version = unpack('HH', optional + 48)
    if os_version > (5, 1) or subsystem_version > (5, 1):
        raise ValueError(f'Not an XP PE header: OS {os_version}, subsystem {subsystem_version}')
    # A delay import can conceal the next missing entry until a feature opens.
    if unpack('II', optional + 96 + 13 * 8) != (0, 0):
        raise ValueError('Unexpected delay imports; use explicit optional API loading')
    sections = [unpack('IIII', optional + size + i * 40 + 8) for i in range(count)]

    def offset(rva):
        for virtual_size, virtual, raw_size, raw in sections:
            if virtual <= rva < virtual + max(virtual_size, raw_size):
                return raw + rva - virtual
        raise ValueError(f'Unmapped RVA {rva:#x}')

    def string(rva):
        start = offset(rva)
        return data[start:data.index(0, start)].decode('ascii')

    imports = {}
    table, = unpack('I', optional + 104)
    descriptor = offset(table)
    while True:
        original, timestamp, chain, name, first = unpack('IIIII', descriptor)
        if not any((original, timestamp, chain, name, first)):
            break
        dll = string(name).lower()
        entries = imports.setdefault(dll, [])
        thunk = offset(original or first)
        while True:
            entry, = unpack('I', thunk)
            if not entry:
                break
            entries.append('#' + str(entry & 0xFFFF) if entry & 0x80000000 else string(entry + 2))
            thunk += 4
        descriptor += 20
    return imports


def exports(path):
    modules = {}
    current = None
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        line = line.strip()
        if line.startswith('[') and line.endswith(']'):
            current = modules.setdefault(line[1:-1].lower(), set())
        elif current is not None and '=' in line:
            ordinal, name = line.split('=', 1)
            current.update(('#' + ordinal, name))
    if not modules:
        raise ValueError(f'Empty export inventory: {path}')
    return modules


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--exports', type=Path, action='append', required=True)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--require-ttpcomm-tls', action='store_true',
                        help='Require the original ttpcomm ordinal 3 as an XP startup dependency')
    args = parser.parse_args()
    imports = inspect(args.executable)
    failures = []
    if args.require_ttpcomm_tls and imports.get('ttpcomm.dll') != ['#3']:
        failures.append('The XP player must import ttpcomm.dll ordinal 3 at startup for static TLS')
    for inventory in args.exports:
        available = exports(inventory)
        for dll, names in imports.items():
            for name in names:
                # This is the sole application DLL exemption, not an OS API.
                # Other ttpcomm imports and all other DLLs still need auditing.
                if args.require_ttpcomm_tls and dll == 'ttpcomm.dll' and name == '#3':
                    continue
                if name not in available.get(dll, set()):
                    failures.append(f'{inventory.stem}: {dll}!{name}')
    if failures:
        raise ValueError('Unsupported static imports:\n' + '\n'.join(failures))
    report = dict(executable=args.executable.name, architecture='x86',
                  sha256=hashlib.sha256(args.executable.read_bytes()).hexdigest(),
                  minimum_subsystem='5.01', inventories=[p.name for p in args.exports],
                  imports=imports)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'XP/Win7 import check passed: {len(imports)} DLLs, '
          f'{sum(map(len, imports.values()))} imports')


if __name__ == '__main__':
    main()
