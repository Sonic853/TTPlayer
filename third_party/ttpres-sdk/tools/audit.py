"""Build-time resource contract audit, without loading the DLL or running tests."""
import argparse
import json
import struct
from pathlib import Path
from resource_model import decode_dialog, decode_strings, legacy_checksum, load_json, pe, resources, save_json, sha256, version_company_name


def audit(path, manifest):
    data = Path(path).read_bytes()
    checksum = legacy_checksum(data)
    if struct.unpack_from('<I', data, 0x20)[0] != checksum:
        raise ValueError('Original player resource checksum at DOS +0x20 is invalid')
    info, _ = pe(data)
    if (info['machine'] != 0x14c or not info['dll'] or info['entry'] or info['code_size'] or
            info['executable_sections'] or info['subsystem'] != [5, 1]):
        raise ValueError('Expected an x86 XP-compatible resource-only DLL')
    for index in (0, 1, 9, 13):
        if any(info['directories'][index]):
            raise ValueError('Resources must not export/import/execute code or use TLS')
    leaves = list(resources(data))
    actual = {key: raw for key, raw, _ in leaves}
    if len(actual) != len(leaves):
        raise ValueError('Duplicate resource leaves')
    company_name_overrides = 0
    for entry in manifest['resources']:
        key = (entry['type'], entry['name'], entry['language'])
        raw = actual.get(key, b'')
        if key == (16, 1, 2052) and sha256(raw) != entry['original_sha256']:
            # Accept released baseline DLLs and the exact Sonic853 override.
            # Reversing it must reproduce the entire original VERSION resource.
            raw = version_company_name(raw, original=True)
            company_name_overrides += 1
        if len(raw) != entry['original_size'] or sha256(raw) != entry['original_sha256']:
            raise ValueError(f'Original resource differs: {key}')
    if actual.get((10, 'TTPLAYER_RES_ABI', 2052)) != b'TTPRES1\0\1\0\0\0\1\0\0\0':
        raise ValueError('Missing extension ABI 1 descriptor')
    metadata = json.loads(actual[(10, 'TTPLAYER_RES_BUILD', 2052)].decode('utf-8'))
    if metadata['baseline_sha256'] != manifest['baseline_sha256'] or metadata['original_resources'] != 192:
        raise ValueError('Wrong baseline metadata')
    # Extension dialog/string IDs are separate from every original key.
    expected = {(5, n, 2052) for n in (4090,4200,4210,4220,4230,5800,5810,5820,5901)}
    expected |= {(6, n, 2052) for n in range(256,262)}
    expected |= {(10, n, 2052) for n in ('TTPLAYER_RES_ABI','TTPLAYER_RES_BUILD')}
    baseline = {(e['type'], e['name'], e['language']) for e in manifest['resources']}
    if not (baseline | expected) <= set(actual) or baseline & expected:
        raise ValueError('Missing or overlapping ABI 1 extension resources')
    for kind, name, language in expected:
        if kind == 5:
            decode_dialog(actual[(kind, name, language)])
    required_strings = {4091} | set(range(4093, 4107)) | set(range(4110, 4177))
    for block in range(256, 262):
        for index, text in enumerate(decode_strings(actual[(6, block, 2052)])['strings']):
            if (block - 1) * 16 + index in required_strings and not text:
                raise ValueError('Empty required extension string')
    # Future ABI 1 releases may add high-numbered resources. They cannot add
    # language variants of an original identity (Win32 might select those).
    original_ids = {(kind, name) for kind, name, _ in baseline}
    for kind, name, language in set(actual) - baseline - expected:
        if (kind, name) in original_ids:
            raise ValueError('Extension shadows an original resource identity')
        if not (language == 2052 and isinstance(name, int) and (
                (kind == 6 and 256 <= name <= 4096) or
                (kind in (2,3,4,5,12,14,241) and 60000 <= name <= 65535))):
            raise ValueError('Unexpected future extension resource namespace')
    return dict(**metadata, sha256=sha256(data), bytes=len(data), resources=len(actual),
                original_verified=192 - company_name_overrides,
                company_name_overrides=company_name_overrides,
                architecture='x86', minimum_subsystem='5.01',
                imports={}, exports=[], entry_point=0, legacy_checksum=f'{checksum:08X}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--exe', type=Path, help='Reject extension backups in the rebuilt player')
    args = parser.parse_args()
    report = audit(args.dll, load_json(args.manifest))
    if args.exe:
        executable = args.exe.read_bytes()
        for (kind, name, _), _, _ in resources(executable):
            if (kind == 5 and name in (4090,4200,4210,4220,4230,5800,5810,5820,5901)) or (
                    kind == 6 and isinstance(name, int) and 256 <= name <= 261):
                raise ValueError('Player EXE must not embed TTPlayerRes extension backups')
        report.update(host_sha256=sha256(executable), no_executable_backups=True)
    save_json(args.output, report)
    print(f"Resource audit: {report['original_verified']}/192 identical; "
          f"{report['company_name_overrides']} CompanyName overrides; "
          f"{report['resources'] - 192} extensions; no code/CRT; XP subsystem")


if __name__ == '__main__':
    main()
