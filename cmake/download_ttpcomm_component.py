"""Stage the latest stable TTPlayerComm release without executing its DLL."""
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
from ttpcomm_abi import inspect_exports

REPOSITORY = 'https://github.com/TTPlayerRebuild/TTPlayerComm'
API = 'https://api.github.com/repos/TTPlayerRebuild/TTPlayerComm/releases/latest'
LIMIT = 16 * 1024 * 1024


class CommRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        target = urllib.parse.urlsplit(newurl)
        if target.scheme != 'https' or target.username or target.password:
            raise ValueError('TTPCOMM component download redirected to an insecure URL')
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
            raise ValueError('Unexpected TTPCOMM component API URL')
        if os.environ.get('GH_TOKEN'):
            headers['Authorization'] = 'Bearer ' + os.environ['GH_TOKEN']
    request = urllib.request.Request(url, headers=headers)
    for attempt in range(3):
        try:
            with urllib.request.build_opener(CommRedirect()).open(request, timeout=60) as response:
                if response.status != 200:
                    raise ValueError('Unexpected HTTP status')
                data = response.read(limit + 1)
                if len(data) > limit:
                    raise ValueError('TTPCOMM release response exceeds size limit')
                length = response.headers.get('Content-Length')
                if length is not None and int(length) != len(data):
                    raise ValueError('Truncated TTPCOMM release response')
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
        raise ValueError('Expected a stable date-versioned TTPCOMM release')
    asset_name = f'ttpcomm-{tag}.zip'
    assets = [a for a in release['assets'] if a.get('name') == asset_name]
    if len(assets) != 1:
        raise ValueError('Expected exactly one TTPCOMM binary ZIP asset')
    asset = assets[0]
    url = f'{REPOSITORY}/releases/download/{tag}/{asset_name}'
    if asset.get('browser_download_url') != url or not 0 < asset['size'] <= LIMIT:
        raise ValueError('Unexpected TTPCOMM asset URL or size')
    digest = asset.get('digest') or ''
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', digest):
        raise ValueError('GitHub did not supply an TTPCOMM ZIP SHA-256 digest')
    data = archive if archive is not None else download(url, LIMIT)
    if len(data) != asset['size'] or hashlib.sha256(data).hexdigest() != digest[7:]:
        raise ValueError('TTPCOMM ZIP size or SHA-256 mismatch')
    with zipfile.ZipFile(io.BytesIO(data)) as package:
        names = package.namelist()
        if len(names) != 2 or set(names) != {'ttpcomm.dll', 'SHA256SUMS.txt'}:
            raise ValueError('TTPCOMM ZIP must contain only the DLL and its checksum')
        for entry in package.infolist():
            maximum = 65536 if entry.filename == 'SHA256SUMS.txt' else LIMIT
            if entry.file_size > maximum or entry.flag_bits & 1:
                raise ValueError('Unsupported TTPCOMM ZIP entry')
        dll = package.read('ttpcomm.dll')  # Also checks the ZIP CRC.
        dll_hash = hashlib.sha256(dll).hexdigest()
        sums = package.read('SHA256SUMS.txt').decode('utf-8-sig').strip()
        if sums != f'{dll_hash}  ttpcomm.dll':
            raise ValueError('TTPCOMM DLL SHA-256 mismatch')
    if len(dll) < 64 or dll[:2] != b'MZ':
        raise ValueError('Missing TTPCOMM DLL PE header')
    pe = struct.unpack_from('<I', dll, 0x3c)[0]
    if pe + 24 > len(dll) or not struct.unpack_from('<H', dll, pe + 22)[0] & 0x2000:
        raise ValueError('TTPCOMM component is not a DLL')
    return dll, dict(repository=REPOSITORY, version=tag, asset=asset_name,
                     url=url, archive_sha256=digest[7:], sha256=dll_hash)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', default='latest')
    parser.add_argument('--release-json', type=Path)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--exports', type=Path, action='append', required=True)
    args = parser.parse_args()
    if {p.name for p in args.exports} != {'5.1.2600.txt', '6.1.7600.txt'}:
        raise ValueError('Both XP and Win7 export inventories are required')
    global API
    if args.version != 'latest':
        if not re.fullmatch(r'[0-9]{4}\.[0-9]{2}\.[0-9]{2}(?:p[1-9][0-9]*)?', args.version):
            raise ValueError('Invalid TTPCOMM version')
        API = API.rsplit('/',1)[0] + '/tags/' + args.version
    if bool(args.release_json) != bool(args.archive):
        raise ValueError('Offline verification requires both release JSON and ZIP')
    release = json.loads(args.release_json.read_text(encoding='utf-8') if args.release_json else download(API, 2 * 1024 * 1024, api=True))
    if args.version != 'latest' and release.get('tag_name') != args.version:
        raise ValueError('Release version mismatch')
    dll, metadata = unpack(release, args.archive.read_bytes() if args.archive else None)
    folder = args.output
    folder.mkdir(parents=True, exist_ok=True)
    temporary = folder / 'ttpcomm.dll.download'
    try:
        temporary.write_bytes(dll)
        exported = inspect_exports(temporary)
        expected = set(range(1,6)) | set(range(10,15)) | set(range(50,84)) | set(range(90,94)) | set(range(100,107)) | set(range(200,207)) | set(range(300,303)) | {400,401}
        named = {1:'ttpcomm_getversion',2:'_resetstkoflw',3:'srand48',4:'lrand48',5:'_set_security_error_handler'}
        extensions = {500: 'ttpcomm_query_extension', 501: 'ttpcomm_query_runtime', 502: 'ttpcomm_query_archive'}
        required = expected | extensions.keys()
        if (not required.issubset(exported) or
            any(e['name'] != named.get(n, '') for n,e in exported.items() if n in expected) or
            any(exported[n]['name'] != name for n,name in extensions.items()) or
            any(n < 500 or not e['name'].startswith('ttpcomm_') for n,e in exported.items() if n not in required) or
            exported[12]['rva'] != exported[78]['rva']):
            raise ValueError('TTPCOMM export ABI mismatch')
        imports = inspect(temporary)
        for inventory in args.exports:
            available = exports(inventory)
            for library, names in imports.items():
                for name in names:
                    if name not in available.get(library, set()):
                        raise ValueError(f'Unsupported TTPCOMM import: {inventory.name}: {library}!{name}')
        temporary.replace(folder / 'ttpcomm.dll')
    finally:
        temporary.unlink(missing_ok=True)
    metadata['inventories'] = [p.name for p in args.exports]
    metadata['runtime_api_export'] = 'ttpcomm_query_runtime'
    metadata['archive_api_export'] = 'ttpcomm_query_archive'
    (args.output / 'ttpcomm-component.json').write_text(
        json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    print(f"Staged TTPlayerComm {metadata['version']}: {len(dll)} bytes; SHA-256 and XP/Win7 imports verified")


if __name__ == '__main__':
    main()
