"""Prefer public Gitee access; embed the approved read-only token only if needed.

Never print URLs containing credentials, responses, or the credential itself.
The generated header is a build product, not a repository source/artifact.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import urllib.parse
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('--output', required=True)
args = parser.parse_args()
base = 'https://gitee.com/api/v5/repos/Sonic853/TTPlayer/releases?per_page=5&direction=desc'

def probe(token=''):
    def read(url, limit):
        if not url.startswith('https://gitee.com/'):
            raise ValueError('Unexpected release origin')
        if token:
            url += ('&' if '?' in url else '?') + urllib.parse.urlencode({'access_token': token})
        request = urllib.request.Request(url, headers={'User-Agent': 'TTPlayerRebuild/Updater'})
        with urllib.request.urlopen(request, timeout=30) as response:
            data = response.read(limit + 1)
            if response.status != 200 or len(data) > limit:
                raise ValueError('Invalid release response')
            return data
    releases = json.loads(read(base, 2 * 1024 * 1024))
    if not isinstance(releases, list):
        raise ValueError('Invalid release list')
    for release in releases:
        name = 'TTPlayerRebuild-' + release.get('tag_name', '') + '.zip'
        assets = {a.get('name'): a.get('browser_download_url') for a in release.get('assets', [])}
        if name not in assets or 'SHA256SUMS.txt' not in assets:
            continue
        sums = read(assets['SHA256SUMS.txt'], 65536).decode('utf-8-sig')
        expected = next(line.split()[0].lower() for line in sums.splitlines() if line.split()[-1] == name)
        data = read(assets[name], 64 * 1024 * 1024)
        if not data.startswith(b'PK') or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('Anonymous download did not match release checksum')
        return
    if releases:
        raise ValueError('No complete release available to probe')

credential = ''
try:
    probe()
    print('Gitee anonymous release API and ZIP download verified; updater has no token.')
except Exception:
    candidate = os.environ.get('GITEE_TOKEN_UPDATER', '')
    if candidate:
        try:
            probe(candidate)
            credential = candidate
            print('Gitee authenticated read fallback verified; using dedicated public read-only updater token.')
        except Exception:
            raise SystemExit('Gitee anonymous and read-only fallback probes failed; no credential output was generated.')
    else:
        raise SystemExit('Gitee anonymous probe failed and GITEE_TOKEN_UPDATER is unavailable.')
path = Path(args.output)
path.parent.mkdir(parents=True, exist_ok=True)
# Byte escapes avoid C++ quoting/injection and non-ASCII source encodings.
encoded = ''.join('\\x%02x' % c for c in credential.encode('utf-8'))
path.write_text('#pragma once\nnamespace ttplayer::update { inline constexpr char kUpdaterGiteeToken[] = "' + encoded + '"; }\n', encoding='utf-8')
