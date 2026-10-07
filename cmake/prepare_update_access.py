"""Probe Gitee updater access without exposing credentials in diagnostics.

Only a successfully verified authenticated fallback may embed the dedicated,
publicly distributable read-only token. An optional CI availability probe can
fall back to an empty token; invalid content and integrity errors remain fatal.
"""
import argparse
import datetime
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import time
import urllib.error
import urllib.parse
import urllib.request

API = 'https://gitee.com/api/v5/repos/Sonic853/TTPlayer/releases?per_page=20&direction=desc'
DOWNLOAD_PREFIX = 'https://gitee.com/Sonic853/TTPlayer/releases/download/'
ACCESS_STATUSES = {401, 403, 404}
REDIRECT_HOSTS = {'gitee.com', 'foruda.gitee.com'}
HEADER_PREFIX = '#pragma once\nnamespace ttplayer::update { inline constexpr char kUpdaterGiteeToken[] = "'


class ProbeFailure(Exception):
    # All fields come from local constants / numeric HTTP status, never a
    # response body, URL, redirect query, token, or exception's free-form text.
    def __init__(self, stage, code, *, status=None, unavailable=False, retryable=False):
        super().__init__(code)
        self.stage, self.code, self.status = stage, code, status
        self.unavailable, self.retryable = unavailable, retryable
        self.attempts = 1

    def summary(self):
        status = f'; HTTP {self.status}' if self.status is not None else ''
        return f'{self.stage}: {self.code}{status}; attempts={self.attempts}'


def valid_url(url, stage, *, redirect=False):
    if not isinstance(url, str) or any(c in url for c in '\r\n\\'):
        raise ProbeFailure(stage, 'invalid-url')
    try:
        parsed = urllib.parse.urlsplit(url)
        valid = (parsed.scheme == 'https' and parsed.hostname in REDIRECT_HOSTS and
                 parsed.port in (None, 443) and not parsed.username and not parsed.password)
    except ValueError:
        valid = False
    if not valid or (not redirect and parsed.hostname != 'gitee.com'):
        raise ProbeFailure(stage, 'unexpected-origin')


class ProbeRedirect(urllib.request.HTTPRedirectHandler):
    def __init__(self, stage):
        self.stage = stage

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        valid_url(newurl, self.stage, redirect=True)
        previous = urllib.parse.urlsplit(req.full_url)
        target = urllib.parse.urlsplit(newurl)
        if target.netloc != previous.netloc:
            # Preserve the spelling/order of signed CDN query parameters.
            query = '&'.join(part for part in target.query.split('&')
                if urllib.parse.unquote_plus(part.split('=', 1)[0]).lower() != 'access_token')
            newurl = urllib.parse.urlunsplit(target._replace(query=query))
        result = super().redirect_request(req, fp, code, msg, headers, newurl)
        if result and target.netloc != previous.netloc:
            result.remove_header('Authorization')
        return result


class ProbeClient:
    def __init__(self, token='', *, attempts=3, timeout=30):
        self.token, self.attempts, self.timeout = token, attempts, timeout
        self.used_token = False

    def _once(self, url, limit, stage, authenticated):
        valid_url(url, stage)
        if authenticated:
            url += ('&' if '?' in url else '?') + urllib.parse.urlencode({'access_token': self.token})
        request = urllib.request.Request(url, headers={
            'User-Agent': 'TTPlayerRebuild/Updater', 'Accept-Encoding': 'identity',
            'Accept': 'application/json' if stage == 'release-api' else '*/*'})
        try:
            with urllib.request.build_opener(ProbeRedirect(stage)).open(request, timeout=self.timeout) as response:
                data = response.read(limit + 1)
                if response.status != 200:
                    raise ProbeFailure(stage, 'unexpected-http-status', status=response.status, unavailable=True)
                if len(data) > limit:
                    raise ProbeFailure(stage, 'response-exceeds-limit')
                length = response.headers.get('Content-Length')
                if length is not None:
                    if not length.isdecimal():
                        raise ProbeFailure(stage, 'invalid-content-length')
                    if int(length) != len(data):
                        raise ProbeFailure(stage, 'truncated-response', unavailable=True, retryable=True)
                return data
        except urllib.error.HTTPError as error:
            # Close the error response without logging its body or URL.
            status = error.code
            error.close()
            raise ProbeFailure(stage, 'http-error', status=status, unavailable=True,
                retryable=status in {408, 425, 429} or 500 <= status <= 599) from None
        except (urllib.error.URLError, TimeoutError, ConnectionError, http.client.HTTPException) as error:
            reason = error.reason if isinstance(error, urllib.error.URLError) else error
            code = 'timeout' if isinstance(reason, TimeoutError) else 'network-error'
            raise ProbeFailure(stage, code, unavailable=True, retryable=True) from None

    def _read(self, url, limit, stage, authenticated=False):
        for attempt in range(1, self.attempts + 1):
            try:
                return self._once(url, limit, stage, authenticated)
            except ProbeFailure as error:
                error.attempts = attempt
                if not error.retryable or attempt == self.attempts:
                    raise
                time.sleep(min(2 ** (attempt - 1), 4))

    def read(self, url, limit, stage):
        # Match the running updater: each request starts anonymously, and only
        # access-related HTTP errors can justify trying the Gitee-only token.
        try:
            return self._read(url, limit, stage)
        except ProbeFailure as error:
            if not self.token or error.status not in ACCESS_STATUSES:
                raise
        result = self._read(url, limit, stage, authenticated=True)
        self.used_token = True
        return result


def version_key(tag):
    if not isinstance(tag, str):
        return None
    match = re.fullmatch(r'([0-9]{4}\.[0-9]{2}\.[0-9]{2})(?:p([1-9][0-9]{0,4}))?', tag)
    if not match:
        return None
    try:
        date = datetime.datetime.strptime(match[1], '%Y.%m.%d').date()
        patch = int(match[2] or '0')
        return (date, patch) if patch <= 65535 else None
    except ValueError:
        return None


def expected_hash(raw, name):
    try:
        text = raw.decode('utf-8-sig')
    except UnicodeError:
        raise ProbeFailure('checksum', 'invalid-utf8') from None
    found = []
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r'([0-9a-fA-F]{64})[ \t]+\*?(.+)', line)
        if match and match[2] == name:
            found.append(match[1].lower())
        elif not match:
            fields = line.split(maxsplit=1)
            if len(fields) == 2 and fields[1].lstrip('*') == name:
                raise ProbeFailure('checksum', 'invalid-package-hash')
    if len(found) != 1:
        raise ProbeFailure('checksum', 'missing-or-duplicate-package-hash')
    return found[0]


def probe(client):
    try:
        releases = json.loads(client.read(API, 2 * 1024 * 1024, 'release-api'))
    except (ValueError, UnicodeError):
        raise ProbeFailure('release-api', 'invalid-json') from None
    if not isinstance(releases, list) or any(not isinstance(row, dict) for row in releases):
        raise ProbeFailure('release-api', 'invalid-release-list')
    candidates = []
    for release in releases:
        tag = release.get('tag_name')
        version = version_key(tag)
        if not version or release.get('draft') or release.get('prerelease'):
            continue
        assets = release.get('assets', [])
        if not isinstance(assets, list) or any(not isinstance(asset, dict) for asset in assets):
            raise ProbeFailure('release-api', 'invalid-asset-list')
        name = f'TTPlayerRebuild-{tag}.zip'
        selected = {}
        for asset in assets:
            key = asset.get('name')
            if key not in (name, 'SHA256SUMS.txt') or asset.get('state') not in (None, 'uploaded'):
                continue
            if key in selected:
                raise ProbeFailure('release-api', 'duplicate-release-asset')
            url = asset.get('browser_download_url')
            if url != f'{DOWNLOAD_PREFIX}{tag}/{key}':
                raise ProbeFailure('release-api', 'unexpected-asset-url')
            selected[key] = url
        if len(selected) == 2:
            candidates.append((version, name, selected))
    if not candidates:
        raise ProbeFailure('release-api', 'no-complete-stable-release', unavailable=True)
    _, name, assets = max(candidates, key=lambda item: item[0])
    expected = expected_hash(client.read(assets['SHA256SUMS.txt'], 65536, 'checksum'), name)
    data = client.read(assets[name], 64 * 1024 * 1024, 'package')
    if not data.startswith(b'PK'):
        raise ProbeFailure('package', 'not-a-zip')
    if hashlib.sha256(data).hexdigest() != expected:
        raise ProbeFailure('package', 'sha256-mismatch')


def write_header(path, credential):
    path.parent.mkdir(parents=True, exist_ok=True)
    # Byte escapes avoid C++ quoting/injection and non-ASCII source encodings.
    encoded = ''.join('\\x%02x' % c for c in credential.encode('utf-8'))
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(HEADER_PREFIX + encoded + '"; }\n', encoding='utf-8')
    temporary.replace(path)


def prepare(path, *, allow_unverified_public=False):
    # Failed probes must not leave a previous build's embedded credential.
    path.unlink(missing_ok=True)
    credential = ''
    try:
        probe(ProbeClient())
        print('Gitee anonymous release API, checksum and ZIP verified; updater has no token.')
    except ProbeFailure as anonymous:
        print('Gitee anonymous probe failed: ' + anonymous.summary())
        candidate = os.environ.get('GITEE_TOKEN_UPDATER', '')
        failure = anonymous
        if candidate and anonymous.status in ACCESS_STATUSES:
            client = ProbeClient(candidate)
            try:
                probe(client)
                credential = candidate if client.used_token else ''
                print('Gitee read fallback verified; using dedicated public read-only updater token.'
                      if credential else 'Gitee anonymous retry verified; updater has no token.')
                failure = None
            except ProbeFailure as authenticated:
                print('Gitee read-only fallback failed: ' + authenticated.summary())
                failure = authenticated
        else:
            print('Gitee read-only fallback not attempted: ' +
                  ('no token configured.' if not candidate else 'failure is not an access-related HTTP error.'))
        if failure:
            if not allow_unverified_public or not failure.unavailable:
                raise SystemExit('Gitee updater access was not verified; no credential output was generated.')
            print('::warning::Gitee updater availability could not be verified. Continuing with an empty '
                  'updater token; Gitee access is unverified for this build. No credential was embedded.')
    write_header(path, credential)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--allow-unverified-public', action='store_true',
        help='On service/access unavailability, warn and build with an empty token; integrity errors remain fatal.')
    args = parser.parse_args()
    prepare(args.output, allow_unverified_public=args.allow_unverified_public)


if __name__ == '__main__':
    main()
