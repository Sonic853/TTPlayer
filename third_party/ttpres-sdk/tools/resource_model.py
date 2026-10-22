"""Lossless, dependency-free Win32 resource source/compiler and PE inspection.

Menus, DIALOGEX templates, string tables and toolbars use editable UTF-8 JSON.
Bitmap source files are ordinary BMPs. Unknown resource types remain raw data.
No DLL is loaded or executed by this module.
"""
import hashlib
import io
import json
import struct
import zipfile
from pathlib import Path


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def save_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def read(self, size):
        if size < 0 or self.at + size > len(self.data):
            raise ValueError('Truncated resource')
        result = self.data[self.at:self.at + size]
        self.at += size
        return result

    def unpack(self, fmt):
        return struct.unpack('<' + fmt, self.read(struct.calcsize('<' + fmt)))

    def word(self):
        return self.unpack('H')[0]

    def field(self):
        first = self.word()
        if first == 0xffff:
            return self.word()
        words = []
        while first:
            words.append(first)
            first = self.word()
        return struct.pack('<' + 'H' * len(words), *words).decode('utf-16-le')

    def align(self):
        return self.read(-self.at % 4).hex()

    def tail(self):
        return self.read(len(self.data) - self.at).hex()


def field(value):
    if isinstance(value, int):
        return struct.pack('<HH', 0xffff, value)
    return value.encode('utf-16-le') + b'\0\0'


def aligned(data, saved=''):
    pad = bytes.fromhex(saved)
    size = -len(data) % 4
    return data + (pad if len(pad) == size else bytes(size))


def version_company_name(data, original=False):
    """Apply/reverse the sole permitted change to the original VERSION resource.

    Both CompanyName records occupy 52 aligned bytes, so parent lengths and
    every other field stay byte-identical to the imported baseline.
    """
    def record(company):
        value = field(company)
        header = aligned(struct.pack('<HHH', 0, len(value) // 2, 1) + field('CompanyName'))
        payload = header + value
        return aligned(struct.pack('<H', len(payload)) + payload[2:])

    before, after = record('Alen Soft'), record('Sonic853')
    if original:
        before, after = after, before
    if len(before) != len(after) or data.count(before) != 1:
        raise ValueError('Unexpected VERSION CompanyName record')
    return data.replace(before, after, 1)


def decode_menu(data):
    r = Reader(data)
    version, offset = r.unpack('HH')
    if version != 0:
        raise ValueError('Only classic MENU templates are expected in 5.7.9')
    extra = r.read(offset).hex()

    def items():
        result = []
        while True:
            flags = r.word()
            item = {'flags': flags & ~0x90}
            if not flags & 0x10:
                item['id'] = r.word()
            item['text'] = r.field()
            if flags & 0x10:
                item['items'] = items()
            result.append(item)
            if flags & 0x80:
                return result
    return {'header': extra, 'items': items(), 'tail': r.tail()}


def encode_menu(value):
    extra = bytes.fromhex(value['header'])

    def items(source):
        result = b''
        if not source:
            raise ValueError('A MENU must contain an item (a separator is allowed)')
        for i, item in enumerate(source):
            flags = item['flags'] | (0x80 if i == len(source) - 1 else 0)
            popup = 'items' in item
            result += struct.pack('<H', flags | (0x10 if popup else 0))
            if not popup:
                result += struct.pack('<H', item['id'])
            result += field(item['text'])
            if popup:
                result += items(item['items'])
        return result
    return struct.pack('<HH', 0, len(extra)) + extra + items(value['items']) + bytes.fromhex(value['tail'])


def decode_dialog(data):
    r = Reader(data)
    if r.unpack('HH') != (1, 0xffff):
        raise ValueError('Expected DIALOGEX')
    help_id, exstyle, style, count, x, y, cx, cy = r.unpack('IIIHhhhh')
    value = dict(help_id=help_id, exstyle=exstyle, style=style, rect=[x,y,cx,cy],
                 menu=r.field(), window_class=r.field(), title=r.field())
    if style & 0x40:
        size, weight, italic, charset = r.unpack('HHBB')
        value['font'] = dict(points=size, weight=weight, italic=italic, charset=charset, face=r.field())
    value['controls'] = []
    for _ in range(count):
        padding = r.align()
        help_id, exstyle, style, x, y, cx, cy, control_id = r.unpack('IIIhhhhI')
        control = dict(padding=padding, help_id=help_id, exstyle=exstyle, style=style,
                       rect=[x,y,cx,cy], id=control_id, window_class=r.field(), title=r.field())
        control['creation_data'] = r.read(r.word()).hex()
        value['controls'].append(control)
    value['tail'] = r.tail()
    return value


def encode_dialog(value):
    result = struct.pack('<HHIIIHhhhh', 1, 0xffff, value['help_id'], value['exstyle'],
                         value['style'], len(value['controls']), *value['rect'])
    result += field(value['menu']) + field(value['window_class']) + field(value['title'])
    if value['style'] & 0x40:
        font = value['font']
        result += struct.pack('<HHBB', font['points'], font['weight'], font['italic'], font['charset']) + field(font['face'])
    for control in value['controls']:
        result = aligned(result, control.get('padding', ''))
        result += struct.pack('<IIIhhhhI', control['help_id'], control['exstyle'], control['style'],
                              *control['rect'], control['id'])
        result += field(control['window_class']) + field(control['title'])
        extra = bytes.fromhex(control['creation_data'])
        result += struct.pack('<H', len(extra)) + extra
    return result + bytes.fromhex(value['tail'])


def decode_strings(data):
    r = Reader(data)
    strings = [r.read(r.word() * 2).decode('utf-16-le') for _ in range(16)]
    return {'strings': strings, 'tail': r.tail()}


def encode_strings(value):
    if len(value['strings']) != 16:
        raise ValueError('RT_STRING requires exactly 16 entries per block')
    result = b''
    for text in value['strings']:
        raw = text.encode('utf-16-le')
        result += struct.pack('<H', len(raw) // 2) + raw
    return result + bytes.fromhex(value['tail'])


def decode_toolbar(data):
    r = Reader(data)
    version, width, height, count = r.unpack('HHHH')
    return dict(version=version, width=width, height=height, commands=list(r.unpack('H' * count)), tail=r.tail())


def encode_toolbar(value):
    commands = value['commands']
    return struct.pack('<HHHH' + 'H' * len(commands), value['version'], value['width'],
                       value['height'], len(commands), *commands) + bytes.fromhex(value['tail'])


CODECS = {4: (decode_menu, encode_menu), 5: (decode_dialog, encode_dialog),
          6: (decode_strings, encode_strings), 241: (decode_toolbar, encode_toolbar)}


def pe(data):
    if data[:2] != b'MZ' or len(data) < 64:
        raise ValueError('Not a PE image')
    pos = struct.unpack_from('<I', data, 60)[0]
    if data[pos:pos+4] != b'PE\0\0':
        raise ValueError('Invalid PE signature')
    machine, count = struct.unpack_from('<HH', data, pos + 4)
    optional_size, flags = struct.unpack_from('<HH', data, pos + 20)
    opt = pos + 24
    if struct.unpack_from('<H', data, opt)[0] != 0x10b:
        raise ValueError('Only PE32 is supported')
    sections = []
    for i in range(count):
        at = opt + optional_size + 40 * i
        vs, va, size, raw = struct.unpack_from('<IIII', data, at + 8)
        sections.append((va, vs, size, raw, struct.unpack_from('<I', data, at+36)[0]))

    def rva(address, size=1):
        for va, vs, raw_size, raw, _ in sections:
            if va <= address and address + size <= va + raw_size:
                offset = address - va + raw
                if offset + size <= len(data):
                    return offset
        raise ValueError('PE RVA outside file-backed sections')
    info = dict(machine=machine, dll=bool(flags & 0x2000),
                entry=struct.unpack_from('<I', data, opt+16)[0],
                code_size=struct.unpack_from('<I', data, opt+4)[0],
                subsystem=list(struct.unpack_from('<HH', data, opt+48)),
                executable_sections=sum(bool(s[4] & 0x20000000) for s in sections),
                directories=[struct.unpack_from('<II', data, opt+96+i*8) for i in range(16)])
    return info, rva


def legacy_checksum(data):
    """5.7.9 004566E2 -> 004BEBFF; DOS reserved DWORD at 0x20.

    This is the player's resource integrity format, not PE CheckSum or an
    Authenticode signature. The checksum field itself is outside the sum.
    """
    info, _ = pe(data)
    pos = struct.unpack_from('<I', data, 60)[0]
    sections = struct.unpack_from('<H', data, pos + 6)[0]
    if struct.unpack_from('<H', data, pos + 20)[0] != 224:
        raise ValueError('Original player requires the standard PE32 optional header')
    start = pos + 248 + sections * 40
    certificate_size = info['directories'][4][1]
    end = len(data) - certificate_size
    if start > end:
        raise ValueError('Invalid legacy checksum range')
    end -= (end - start) % 2
    total = sum(v[0] for v in struct.iter_unpack('<H', data[start:end])) & 0xffffffff
    # Original CONCAT31: [sum.byte2, sum.byte1, sum.byte0, sum.byte3].
    shuffled = (total & 0xff000000) | ((total & 0xff) << 16) | (total & 0xff00) | ((total >> 16) & 0xff)
    return (~shuffled) & 0xffffffff


def resources(data):
    info, rva = pe(data)
    address, size = info['directories'][2]
    base = rva(address, size)

    def bounds(offset, length):
        if offset < 0 or offset + length > size:
            raise ValueError('Resource directory outside bounds')
        return base + offset

    def identity(value):
        if not value & 0x80000000:
            return value
        at = value & 0x7fffffff
        length = struct.unpack_from('<H', data, bounds(at, 2))[0]
        start = bounds(at + 2, length * 2)
        return data[start:start + length*2].decode('utf-16-le')

    def walk(offset, path):
        at = bounds(offset, 16)
        named, ids = struct.unpack_from('<HH', data, at + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from('<II', data, bounds(offset+16+i*8, 8))
            key = (*path, identity(name))
            if target & 0x80000000:
                if len(key) >= 3:
                    raise ValueError('Resource tree has more than three levels')
                yield from walk(target & 0x7fffffff, key)
            else:
                if len(key) != 3:
                    raise ValueError('Incomplete resource key')
                addr, length, codepage, _ = struct.unpack_from('<IIII', data, bounds(target, 16))
                raw = rva(addr, length)
                yield key, data[raw:raw+length], codepage
    yield from walk(0, ())


def res_file(entries):
    def record(kind, name, language, payload):
        header = aligned(field(kind) + field(name))
        header += struct.pack('<IHHII', 0, 0x1030, language, 0, 0)
        return aligned(struct.pack('<II', len(payload), len(header)+8) + header + payload)
    return record(0, 0, 0, b'') + b''.join(record(*key, raw) for key, raw in entries)


def source_payload(root, entry):
    path = root / entry['source']
    form = entry['format']
    if form == 'json':
        return CODECS[entry['type']][1](load_json(path))
    if form == 'bmp':
        bmp = path.read_bytes()
        if bmp[:2] != b'BM' or struct.unpack_from('<I', bmp, 2)[0] != len(bmp):
            raise ValueError(f'Invalid BMP: {path}')
        return bmp[14:]
    if form == 'zip':
        original = path.read_bytes()
        directory = root / entry['directory']
        with zipfile.ZipFile(io.BytesIO(original)) as package:
            names = [p.filename for p in package.infolist() if not p.is_dir()]
            actual = sorted(p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file())
            if sorted(names) != actual:
                raise ValueError(f'ZIP member list changed; update the manifest deliberately: {path}')
            if all(package.read(name) == (directory / name).read_bytes() for name in names):
                return original  # Preserve the original compressor and ZIP metadata bit for bit.
            result = io.BytesIO()
            with zipfile.ZipFile(result, 'w') as output:
                for item in package.infolist():
                    output.writestr(item, b'' if item.is_dir() else (directory / item.filename).read_bytes())
                output.comment = package.comment
            return result.getvalue()
    return path.read_bytes()


def original_entries(source):
    manifest = load_json(source / 'manifest.json')
    entries = []
    seen = set()
    for item in manifest['resources']:
        key = (item['type'], item['name'], item['language'])
        if key in seen:
            raise ValueError(f'Duplicate resource: {key}')
        seen.add(key)
        entries.append((key, source_payload(source, item)))
    return manifest, entries
