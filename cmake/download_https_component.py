"""Stage the latest stable TTPlayerHttps release without executing its DLL."""
import argparse
import hashlib
import io
import json
import os
import re
import struct
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

from check_legacy_imports import exports, inspect

REPOSITORY = 'https://github.com/TTPlayerRebuild/TTPlayerHttps'
API = 'https://api.github.com/repos/TTPlayerRebuild/TTPlayerHttps/releases/latest'
LIMIT = 16 * 1024 * 1024
REQUIRED_ABI = 6


def capabilities(data):
    """Read the release-hash-verified PE resource, without loading native code."""
    def read(fmt, offset):
        count = struct.calcsize('<' + fmt)
        if offset < 0 or offset + count > len(data):
            raise ValueError('Truncated HTTPS capability PE data')
        return struct.unpack_from('<' + fmt, data, offset)

    if data[:2] != b'MZ':
        raise ValueError('Missing HTTPS DOS header')
    pe, = read('I', 0x3c)
    if data[pe:pe+4] != b'PE\0\0':
        raise ValueError('Missing HTTPS PE signature')
    machine, count = read('HH', pe+4)
    optional_size, = read('H', pe+20)
    optional = pe+24
    if machine != 0x14c or read('H', optional)[0] != 0x10b or optional_size < 120:
        raise ValueError('HTTPS component must be x86 PE32')
    sections = [read('IIII', optional+optional_size+i*40+8) for i in range(count)]

    def mapped(rva, size):
        for virtual_size, virtual, raw_size, raw in sections:
            if virtual <= rva and rva-virtual+size <= raw_size:
                offset = raw+rva-virtual
                if offset+size <= len(data):
                    return offset
        raise ValueError('Unmapped HTTPS capability resource')

    rva, size = read('II', optional+112)
    if not rva or size < 16:
        raise ValueError('HTTPS component has no capability resource; ABI 6 is required')

    def relative(at, length):
        if at < 0 or at+length > size:
            raise ValueError('Invalid HTTPS resource offset')
        return mapped(rva+at, length)

    def child(at, identifier, directory):
        node = relative(at, 16)
        named, numbered = read('HH', node+12)
        for i in range(named+numbered):
            name, target = read('II', relative(at+16+i*8, 8))
            if name == identifier:
                if bool(target & 0x80000000) != directory:
                    raise ValueError('Invalid HTTPS resource tree')
                return target & 0x7fffffff
        raise ValueError('HTTPS component lacks ABI 6 capability metadata')

    # RT_RCDATA / ID 100 / LANG_NEUTRAL, fixed 16-byte versioned record.
    entry = child(child(child(0, 10, True), 100, True), 0, False)
    payload, length = read('II', relative(entry, 16))
    if length != 16:
        raise ValueError('Unexpected HTTPS capability record size')
    magic, version, abi, features = read('IIII', mapped(payload, length))
    if magic != 0x48505454 or version != 1 or abi < REQUIRED_ABI or features & 0x1f != 0x1f:
        raise ValueError('HTTPS component does not provide required ABI 6 lyric capabilities')
    return {'max_abi': abi, 'features': features}


class HttpsRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        target = urllib.parse.urlsplit(newurl)
        if target.scheme != 'https' or target.username or target.password:
            raise ValueError('HTTPS component download redirected to an insecure URL')
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if target.netloc != urllib.parse.urlsplit(req.full_url).netloc:
            redirected.remove_header('Authorization')
        return redirected


def download(url, limit, *, api=False):
    headers = {
        'User-Agent': 'TTPlayerRebuild-build', 'Accept': 'application/vnd.github+json',
        'Accept-Encoding': 'identity',
    }
    if api:
        if url != API:
            raise ValueError('Unexpected HTTPS component API URL')
        if os.environ.get('GH_TOKEN'):
            headers['Authorization'] = 'Bearer ' + os.environ['GH_TOKEN']
    request = urllib.request.Request(url, headers=headers)
    for attempt in range(3):
        try:
            with urllib.request.build_opener(HttpsRedirect()).open(request, timeout=60) as response:
                if response.status != 200:
                    raise ValueError('Unexpected HTTP status')
                data = response.read(limit + 1)
                if len(data) > limit:
                    raise ValueError('HTTPS release response exceeds size limit')
                length = response.headers.get('Content-Length')
                if length is not None and int(length) != len(data):
                    raise ValueError('Truncated HTTPS release response')
                return data
        except urllib.error.HTTPError as error:
            if error.code not in (429, 500, 502, 503, 504) or attempt == 2:
                raise
        except (urllib.error.URLError, TimeoutError):
            if attempt == 2:
                raise
            time.sleep(2 ** attempt)


def unpack(release, archive):
    tag = release.get('tag_name', '')
    if release.get('draft') or release.get('prerelease') or not re.fullmatch(
            r'[0-9]{4}\.[0-9]{2}\.[0-9]{2}(?:p[1-9][0-9]*)?', tag):
        raise ValueError('Expected a stable date-versioned HTTPS release')
    asset_name = f'ttp_https-{tag}.zip'
    assets = [a for a in release['assets'] if a.get('name') == asset_name]
    if len(assets) != 1:
        raise ValueError('Expected exactly one HTTPS binary ZIP asset')
    asset = assets[0]
    url = f'{REPOSITORY}/releases/download/{tag}/{asset_name}'
    if asset.get('browser_download_url') != url or not 0 < asset['size'] <= LIMIT:
        raise ValueError('Unexpected HTTPS asset URL or size')
    digest = asset.get('digest') or ''
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', digest):
        raise ValueError('GitHub did not supply an HTTPS ZIP SHA-256 digest')
    data = archive if archive is not None else download(url, LIMIT)
    if len(data) != asset['size'] or hashlib.sha256(data).hexdigest() != digest[7:]:
        raise ValueError('HTTPS ZIP size or SHA-256 mismatch')
    with zipfile.ZipFile(io.BytesIO(data)) as package:
        names = package.namelist()
        if len(names) != 2 or set(names) != {'AddIn/ttp_https.dll', 'SHA256SUMS.txt'}:
            raise ValueError('HTTPS ZIP must contain only the DLL and its checksum')
        for entry in package.infolist():
            maximum = 65536 if entry.filename == 'SHA256SUMS.txt' else LIMIT
            if entry.file_size > maximum or entry.flag_bits & 1:
                raise ValueError('Unsupported HTTPS ZIP entry')
        dll = package.read('AddIn/ttp_https.dll')  # Also checks the ZIP CRC.
        dll_hash = hashlib.sha256(dll).hexdigest()
        sums = package.read('SHA256SUMS.txt').decode('utf-8-sig').strip()
        if sums != f'{dll_hash}  AddIn/ttp_https.dll':
            raise ValueError('HTTPS DLL SHA-256 mismatch')
    if len(dll) < 64 or dll[:2] != b'MZ':
        raise ValueError('Missing HTTPS DLL PE header')
    pe = struct.unpack_from('<I', dll, 0x3c)[0]
    if pe + 24 > len(dll) or not struct.unpack_from('<H', dll, pe + 22)[0] & 0x2000:
        raise ValueError('HTTPS component is not a DLL')
    contract = capabilities(dll)
    return dll, dict(repository=REPOSITORY, version=tag, asset=asset_name,
                     url=url, archive_sha256=digest[7:], sha256=dll_hash,
                     required_abi=REQUIRED_ABI, capabilities=contract)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--exports', type=Path, action='append', required=True)
    args = parser.parse_args()
    if {p.name for p in args.exports} != {'5.1.2600.txt', '6.1.7600.txt'}:
        raise ValueError('Both XP and Win7 export inventories are required')
    release = json.loads(download(API, 2 * 1024 * 1024, api=True))
    dll, metadata = unpack(release, None)
    folder = args.output / 'AddIn'
    folder.mkdir(parents=True, exist_ok=True)
    temporary = folder / 'ttp_https.dll.download'
    try:
        temporary.write_bytes(dll)
        imports = inspect(temporary)
        for inventory in args.exports:
            available = exports(inventory)
            for library, names in imports.items():
                for name in names:
                    if name not in available.get(library, set()):
                        raise ValueError(f'Unsupported HTTPS import: {inventory.name}: {library}!{name}')
        temporary.replace(folder / 'ttp_https.dll')
    finally:
        temporary.unlink(missing_ok=True)
    metadata['inventories'] = [p.name for p in args.exports]
    (args.output / 'https-component.json').write_text(
        json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    print(f"Staged TTPlayerHttps {metadata['version']}: {len(dll)} bytes; ABI {REQUIRED_ABI}, SHA-256 and XP/Win7 imports verified")


if __name__ == '__main__':
    main()
