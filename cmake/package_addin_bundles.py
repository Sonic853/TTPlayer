"""Package the latest stable codec and extra add-ins without executing their DLLs."""
import argparse
import datetime
import hashlib
import io
import json
import os
import re
import stat
import struct
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

from check_legacy_imports import exports, inspect


# These are independent public releases, not sibling source/build directories.
BUNDLES = {
    'CodecAddIn-Rebuild': (
        ('TTPlayerWebM', 'ttp_webm'), ('TTPlayerAPE', 'ttp_ape'),
        ('TTPlayerFLAC', 'ttp_flac'), ('TTPlayerEnc', 'ttp_enc'),
        ('TTPlayerOGG', 'ttp_ogg'), ('TTPlayerAAC', 'ttp_aac'),
    ),
    'ExtraAddIn': (
        ('TTPlayerWaskin', 'ttp_waskin'), ('TTPlayerMaki', 'ttp_maki'),
        ('TTPlayerI18n', 'ttp_i18n'),
    ),
}
ARCHIVE_LIMIT = 16 * 1024 * 1024
UNPACKED_LIMIT = 32 * 1024 * 1024
API_ROOT = 'https://api.github.com/repos/TTPlayerRebuild/'
WEB_ROOT = 'https://github.com/TTPlayerRebuild/'


def version_name(value):
    match = re.fullmatch(r'([0-9]{4}\.[0-9]{2}\.[0-9]{2})(?:p([1-9][0-9]{0,4}))?', value)
    if not match or int(match[2] or '0') > 65535:
        raise ValueError(f'Invalid date version: {value!r}')
    datetime.datetime.strptime(match[1], '%Y.%m.%d')
    return value


class ReleaseRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        target = urllib.parse.urlsplit(newurl)
        if target.scheme != 'https' or target.username or target.password:
            raise ValueError('Release download redirected to an invalid HTTPS URL')
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if target.netloc != urllib.parse.urlsplit(req.full_url).netloc:
            redirected.remove_header('Authorization')
        return redirected


def download(url, limit, *, api=False):
    headers = {'User-Agent': 'TTPlayerRebuild-addin-packager', 'Accept-Encoding': 'identity'}
    if api:
        if not url.startswith(API_ROOT):
            raise ValueError('Unexpected release API URL')
        headers['Accept'] = 'application/vnd.github+json'
        # The job token is used only for GitHub API rate limits, never for assets
        # or recorded in metadata. Public local downloads also work anonymously.
        if os.environ.get('GH_TOKEN'):
            headers['Authorization'] = 'Bearer ' + os.environ['GH_TOKEN']
    request = urllib.request.Request(url, headers=headers)
    for attempt in range(3):
        try:
            with urllib.request.build_opener(ReleaseRedirect()).open(request, timeout=45) as response:
                if response.status != 200:
                    raise ValueError('Unexpected release download status')
                data = response.read(limit + 1)
                if len(data) > limit:
                    raise ValueError('Release response exceeds size limit')
                length = response.headers.get('Content-Length')
                if length is not None and int(length) != len(data):
                    raise ValueError('Truncated release response')
                return data
        except urllib.error.HTTPError as error:
            if error.code not in (429, 500, 502, 503, 504) or attempt == 2:
                raise
        except (urllib.error.URLError, TimeoutError):
            if attempt == 2:
                raise
        time.sleep(2 ** attempt)


def select_asset(repository, target, release):
    tag = version_name(release.get('tag_name', ''))
    if release.get('draft') or release.get('prerelease'):
        raise ValueError(f'{repository}: expected a stable published release')
    prefix = target + ('-x86' if target == 'ttp_i18n' else '')
    name = f'{prefix}-{tag}.zip'
    candidates = [asset for asset in release['assets'] if asset.get('name') == name]
    if len(candidates) != 1:
        raise ValueError(f'{repository}: expected exactly one {name} asset')
    asset = candidates[0]
    url = f'{WEB_ROOT}{repository}/releases/download/{tag}/{name}'
    if asset.get('browser_download_url') != url or not 0 < asset['size'] <= ARCHIVE_LIMIT:
        raise ValueError(f'{repository}: unexpected asset URL or size')
    digest = asset.get('digest') or ''
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', digest):
        raise ValueError(f'{repository}: GitHub did not supply the ZIP SHA-256')
    return asset


def parse_checksums(text):
    checksums = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r'([0-9a-fA-F]{64}) [ *](.+)', line)
        if not match or match[2] in checksums:
            raise ValueError('Invalid or duplicate component checksum')
        checksums[match[2]] = match[1].lower()
    return checksums


def unpack_component(repository, target, release, archive):
    asset = select_asset(repository, target, release)
    if len(archive) != asset['size'] or hashlib.sha256(archive).hexdigest() != asset['digest'][7:]:
        raise ValueError(f'{repository}: ZIP size or SHA-256 mismatch')
    dll_path = f'AddIn/{target}.dll'
    files, directories, seen = {}, set(), set()
    with zipfile.ZipFile(io.BytesIO(archive)) as package:
        entries = package.infolist()
        if len(entries) > 256 or sum(entry.file_size for entry in entries) > UNPACKED_LIMIT:
            raise ValueError(f'{repository}: excessive ZIP contents')
        for entry in entries:
            name = entry.filename
            # Never extractall(): only allow explicit runtime paths. Reject
            # Windows aliases, traversal, links, duplicates and encrypted ZIPs.
            if (entry.orig_filename != name or '\\' in name or ':' in name or
                    name.startswith('/') or any(x in ('', '.', '..') for x in name.rstrip('/').split('/')) or
                    name.casefold() in seen or entry.flag_bits & 1 or
                    entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)):
                raise ValueError(f'{repository}: invalid ZIP path or entry: {name!r}')
            seen.add(name.casefold())
            mode = stat.S_IFMT(entry.external_attr >> 16)
            if mode not in (0, stat.S_IFREG, stat.S_IFDIR) or (mode == stat.S_IFDIR and not entry.is_dir()):
                raise ValueError(f'{repository}: unsupported ZIP file type')
            if entry.is_dir():
                if entry.file_size:
                    raise ValueError(f'{repository}: nonempty ZIP directory')
                directories.add(name)
                continue
            translation = target == 'ttp_i18n' and (
                name in ('i18n/README.md', 'i18n/ttplayer.pot') or
                re.fullmatch(r'i18n/[A-Za-z0-9_-]+/ttplayer\.(po|mo)', name))
            if name not in (dll_path, 'SHA256SUMS.txt') and not translation:
                raise ValueError(f'{repository}: unexpected runtime file: {name}')
            limit = 65536 if name == 'SHA256SUMS.txt' else ARCHIVE_LIMIT
            if entry.file_size > limit:
                raise ValueError(f'{repository}: oversized runtime file')
            files[name] = package.read(entry)  # Includes CRC verification.
    allowed_directories = {name[:index + 1] for name in files
                           for index, char in enumerate(name) if char == '/'}
    if directories - allowed_directories or dll_path not in files or 'SHA256SUMS.txt' not in files:
        raise ValueError(f'{repository}: incomplete or unexpected runtime layout')
    sums = parse_checksums(files.pop('SHA256SUMS.txt').decode('utf-8-sig'))
    actual = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    if sums != actual:
        raise ValueError(f'{repository}: runtime file SHA-256 mismatch or missing checksum')
    if target == 'ttp_i18n':
        for locale in ('chs', 'cht', 'en_US'):
            if not any(f'i18n/{locale}/ttplayer.{ext}' in files for ext in ('po', 'mo')):
                raise ValueError(f'{repository}: missing {locale} translation')
    dll = files[dll_path]
    if len(dll) < 64 or dll[:2] != b'MZ':
        raise ValueError(f'{repository}: missing DLL PE header')
    pe = struct.unpack_from('<I', dll, 0x3c)[0]
    if (pe + 24 > len(dll) or dll[pe:pe + 4] != b'PE\0\0' or
            not struct.unpack_from('<H', dll, pe + 22)[0] & 0x2000):
        raise ValueError(f'{repository}: component is not a PE DLL')
    metadata = dict(repository=WEB_ROOT + repository, version=release['tag_name'],
                    release_id=release['id'], asset_id=asset['id'], asset=asset['name'],
                    url=asset['browser_download_url'], archive_sha256=asset['digest'][7:],
                    dll=dll_path, sha256=actual[dll_path], files=actual)
    return files, metadata


def audit_component(files, metadata, inventories, directory):
    path = directory / Path(metadata['dll']).name
    path.write_bytes(files[metadata['dll']])
    imports = inspect(path)  # Enforces x86 PE32, XP subsystem and no delay imports.
    for inventory_name, available in inventories.items():
        for library, names in imports.items():
            for name in names:
                if name not in available.get(library, set()):
                    raise ValueError(f"{metadata['dll']}: unsupported {inventory_name} import {library}!{name}")
    metadata['inventories'] = list(inventories)


def write_bundle(path, files):
    sums = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name, data in sorted(files.items()))
    # Fixed timestamps/order allow repeatable ZIPs for a captured release set.
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted({**files, 'SHA256SUMS.txt': sums.encode('utf-8')}.items()):
            entry = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = (stat.S_IFREG | 0o644) << 16
            archive.writestr(entry, data, compresslevel=9)


def package_bundles(version, output, export_paths):
    version = version_name(version)
    if len(export_paths) != 2 or {p.name for p in export_paths} != {'5.1.2600.txt', '6.1.7600.txt'}:
        raise ValueError('Both XP and Win7 export inventories are required')
    inventories = {path.name: exports(path) for path in export_paths}
    # Resolve each latest release exactly once. All later reads use its captured
    # tag URL and asset digest, even if another release appears mid-job.
    resolved = {}
    for components in BUNDLES.values():
        for repository, target in components:
            release = json.loads(download(API_ROOT + repository + '/releases/latest', 2 * 1024 * 1024, api=True))
            asset = select_asset(repository, target, release)
            resolved[repository] = (release, asset)
    output.mkdir(parents=True, exist_ok=True)
    report = dict(schema=1, player_version=version, bundles=[])
    with tempfile.TemporaryDirectory(prefix='addin-bundles-', dir=output.parent) as temporary:
        stage = Path(temporary)
        for bundle, components in BUNDLES.items():
            combined, sources = {}, []
            for repository, target in components:
                release, asset = resolved[repository]
                archive = download(asset['browser_download_url'], ARCHIVE_LIMIT)
                files, metadata = unpack_component(repository, target, release, archive)
                audit_component(files, metadata, inventories, stage)
                if set(combined) & set(files):
                    raise ValueError(f'{bundle}: component file collision')
                combined.update(files)
                sources.append(metadata)
                print(f"Verified {repository} {metadata['version']}: ZIP/files SHA-256 and XP/Win7 imports", flush=True)
            filename = f'{bundle}-{version}.zip'
            write_bundle(stage / filename, combined)
            report['bundles'].append(dict(name=bundle, archive=filename,
                                          sha256=hashlib.sha256((stage / filename).read_bytes()).hexdigest(),
                                          components=sources))
        # Only publish the completed pair after every source has passed checks.
        (stage / 'addin-bundles.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        for name in [x['archive'] for x in report['bundles']] + ['addin-bundles.json']:
            (stage / name).replace(output / name)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True, help='Final player release version')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--exports', type=Path, action='append', required=True)
    args = parser.parse_args()
    report = package_bundles(args.version, args.output, args.exports)
    for bundle in report['bundles']:
        print(f"Packaged {bundle['archive']} ({len(bundle['components'])} DLLs)")


if __name__ == '__main__':
    main()
