"""Download TTPlayerRes and verify every legacy resource before staging it."""
import argparse
import hashlib
import io
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

SDK = Path(__file__).resolve().parents[1] / 'third_party/ttpres-sdk'
sys.path.insert(0, str(SDK / 'tools'))
from audit import audit
from resource_model import load_json, save_json
from download_lrcsh_component import HttpsRedirect

REPOSITORY = 'https://github.com/TTPlayerRebuild/TTPlayerRes'
API_BASE = 'https://api.github.com/repos/TTPlayerRebuild/TTPlayerRes/releases/'
LIMIT = 16 * 1024 * 1024


def download(url, limit, api=False):
    headers = {'User-Agent': 'TTPlayerRebuild-build', 'Accept-Encoding': 'identity'}
    if api:
        if not url.startswith(API_BASE):
            raise ValueError('Unexpected resource API URL')
        headers['Accept'] = 'application/vnd.github+json'
        if os.environ.get('GH_TOKEN'):
            headers['Authorization'] = 'Bearer ' + os.environ['GH_TOKEN']
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, headers=headers)
            with urllib.request.build_opener(HttpsRedirect()).open(request, timeout=45) as response:
                data = response.read(limit + 1)
                if response.status != 200 or len(data) > limit:
                    raise ValueError('Invalid resource download status or size')
                length = response.headers.get('Content-Length')
                if length is not None and int(length) != len(data):
                    raise ValueError('Truncated resource download')
                return data
        except urllib.error.HTTPError as error:
            if error.code not in (429,500,502,503,504) or attempt == 2:
                raise
        except (urllib.error.URLError, TimeoutError):
            if attempt == 2:
                raise
        time.sleep(2 ** attempt)


def unpack(release, archive=None):
    tag = release.get('tag_name', '')
    if release.get('draft') or release.get('prerelease') or not re.fullmatch(
            r'\d{4}\.\d{2}\.\d{2}(?:p[1-9]\d*)?', tag):
        raise ValueError('Expected a stable date-versioned resource release')
    name = f'ttpres-{tag}.zip'
    assets = [a for a in release['assets'] if a.get('name') == name]
    if len(assets) != 1:
        raise ValueError('Expected exactly one resource ZIP')
    asset = assets[0]
    url = f'{REPOSITORY}/releases/download/{tag}/{name}'
    digest = asset.get('digest') or ''
    if asset.get('browser_download_url') != url or not 0 < asset['size'] <= LIMIT or not re.fullmatch(r'sha256:[0-9a-f]{64}', digest):
        raise ValueError('Invalid resource release asset metadata')
    data = archive if archive is not None else download(url, LIMIT)
    if len(data) != asset['size'] or hashlib.sha256(data).hexdigest() != digest[7:]:
        raise ValueError('Resource ZIP hash or size mismatch')
    with zipfile.ZipFile(io.BytesIO(data)) as package:
        if len(package.namelist()) != 2 or set(package.namelist()) != {'ttpres.dll','SHA256SUMS.txt'}:
            raise ValueError('Resource ZIP must contain only DLL and checksum')
        for item in package.infolist():
            if item.file_size > (65536 if item.filename == 'SHA256SUMS.txt' else LIMIT) or item.flag_bits & 1:
                raise ValueError('Unsupported resource ZIP entry')
        dll = package.read('ttpres.dll')
        sums = package.read('SHA256SUMS.txt').decode('utf-8-sig').strip()
        if sums != f'{hashlib.sha256(dll).hexdigest()}  ttpres.dll':
            raise ValueError('Resource DLL checksum mismatch')
    return dll, dict(asset=name, url=url, archive_sha256=digest[7:], version=tag)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', default='latest')
    args = parser.parse_args()
    if args.version != 'latest' and not re.fullmatch(r'\d{4}\.\d{2}\.\d{2}(?:p[1-9]\d*)?', args.version):
        raise ValueError('Invalid resource version')
    api = API_BASE + ('latest' if args.version == 'latest' else 'tags/' + args.version)
    try:
        release = json.loads(download(api, 2 * 1024 * 1024, api=True))
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
        raise SystemExit('TTPlayerRes: no releases found.') from None
    dll, metadata = unpack(release)
    if args.version != 'latest' and metadata['version'] != args.version:
        raise ValueError('Resource release tag does not match requested version')
    args.output.mkdir(parents=True, exist_ok=True)
    temporary = args.output / 'ttpres.dll.download'
    try:
        temporary.write_bytes(dll)
        report = audit(temporary, load_json(SDK / 'baseline.json'))
        if report['version'] != metadata['version']:
            raise ValueError('Embedded resource build version does not match release tag')
        temporary.replace(args.output / 'ttpres.dll')
    finally:
        temporary.unlink(missing_ok=True)
    report.update(metadata)
    save_json(args.output / 'ttpres-component.json', report)
    print(f"Staged TTPlayerRes {report['version']}: 192 original resources verified; ABI 1; no runtime imports")


if __name__ == '__main__':
    main()
