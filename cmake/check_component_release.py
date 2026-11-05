"""Gate a component release on unpublished commits (Python standard library only).

Canonical copy: Sonic853/TTPlayer, cmake/check_component_release.py.
Identical copies travel with each independent component repository so a queued
run never downloads mutable helper code from another repository.
"""
import json
import os
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


class ApiError(RuntimeError):
    def __init__(self, status, message):
        self.status = status
        super().__init__(message)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # Repository renames/configuration mistakes must not forward the token.
        return None


class GitHubApi:
    def __init__(self, token, opener=None, sleep=time.sleep):
        if not token or not token.strip():
            raise ValueError('Missing GH_TOKEN; authenticated GitHub API access is required.')
        self.token = token.strip()
        self.opener = opener or urllib.request.build_opener(NoRedirect())
        self.sleep = sleep

    def request(self, path, payload=None):
        if not path.startswith('/repos/') or any(c in path for c in '\r\n'):
            raise ValueError('Expected a repository-relative GitHub API path.')
        method = 'GET' if payload is None else 'POST'
        data = None if payload is None else json.dumps(payload).encode('utf-8')
        request = urllib.request.Request('https://api.github.com' + path, data=data,
            method=method, headers={
                'Authorization': 'Bearer ' + self.token,
                'Accept': 'application/vnd.github+json',
                'Content-Type': 'application/json',
                'X-GitHub-Api-Version': '2026-03-10',
                'User-Agent': 'TTPlayer-component-release',
            })
        for attempt in range(3):
            try:
                with self.opener.open(request, timeout=30) as response:
                    body = response.read(16 * 1024 * 1024 + 1)
                    if len(body) > 16 * 1024 * 1024:
                        raise ApiError(response.status, 'GitHub response exceeds size limit.')
                    if response.status not in (200, 204):
                        raise ApiError(response.status, f'Unexpected GitHub HTTP {response.status}.')
                    return response.status, json.loads(body) if body else None
            except urllib.error.HTTPError as error:
                if method == 'GET' and error.code in (429, 500, 502, 503, 504) and attempt < 2:
                    error.close()
                    self.sleep(2 ** attempt)
                    continue
                # Do not echo bodies/headers containing remote data or credentials.
                code = error.code
                error.close()
                raise ApiError(code, f'GitHub HTTP {code} for {method} {path}.') from None
            except (urllib.error.URLError, TimeoutError, OSError):
                # POST is deliberately never retried: a lost response can mean
                # that the remote workflow was already started.
                raise ApiError(0, f'GitHub {method} result unknown for {path}; inspect Actions before retrying.') from None
        raise AssertionError('unreachable')

    def get(self, path):
        return self.request(path)[1]


def repository_name(value):
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', value):
        raise ValueError('Invalid GitHub repository name.')
    return value


def commit_sha(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-fA-F]{40}', value):
        raise ValueError('Expected a complete Git commit SHA.')
    return value.lower()


def repository_info(api, repository):
    repository_name(repository)
    info = api.get(f'/repos/{repository}')
    if info.get('full_name', '').lower() != repository.lower() or info.get('archived') or info.get('disabled'):
        raise ValueError('Repository identity changed, or repository is archived/disabled.')
    branch = info.get('default_branch')
    if not isinstance(branch, str) or not branch or any(c in branch for c in '\r\n'):
        raise ValueError('Repository has no valid default branch.')
    return info


def release_commit(api, repository, tag):
    if not isinstance(tag, str) or not tag or any(c in tag for c in '\r\n'):
        raise ValueError('Release has no valid tag.')
    ref = api.get(f'/repos/{repository}/git/ref/tags/{urllib.parse.quote(tag, safe="")}')
    if ref.get('ref') != 'refs/tags/' + tag:
        raise ValueError('Release tag reference does not match the requested tag.')
    obj = ref['object']
    seen = set()
    for _ in range(16):
        sha = commit_sha(obj.get('sha'))
        if obj.get('type') == 'commit':
            return sha
        if obj.get('type') != 'tag' or sha in seen:
            raise ValueError('Release tag does not resolve to a commit.')
        seen.add(sha)
        obj = api.get(f'/repos/{repository}/git/tags/{sha}')['object']
    raise ValueError('Release tag nesting limit exceeded.')


def release_decision(api, repository, head, ref, info=None):
    repository_name(repository)
    head = commit_sha(head)
    info = info or repository_info(api, repository)
    if ref != 'refs/heads/' + info['default_branch']:
        raise ValueError('Conditional releases must run on the repository default branch.')
    result = dict(repository=repository, commit=head, release_tag='', release_commit='', should_build=False)
    try:
        release = api.get(f'/repos/{repository}/releases/latest')
    except ApiError as error:
        if error.status != 404:
            raise
        # Repository access was checked above; do not treat an inaccessible
        # repository, 403, or missing tag as an empty release history.
        return dict(result, should_build=True, reason='No published stable release; create the first release.')
    if release.get('draft') or release.get('prerelease'):
        raise ValueError('Latest-release endpoint did not return a stable published release.')
    tag = release.get('tag_name')
    base = release_commit(api, repository, tag)
    result.update(release_tag=tag, release_commit=base)
    if base == head:
        return dict(result, reason='This commit is already released.')
    comparison = api.get(f'/repos/{repository}/compare/{base}...{head}?per_page=1')
    status = comparison.get('status')
    ahead = comparison.get('ahead_by')
    behind = comparison.get('behind_by')
    if status == 'ahead' and isinstance(ahead, int) and ahead > 0 and behind == 0:
        return dict(result, should_build=True, reason=f'{ahead} unpublished commit(s) after the release.')
    if status == 'behind' and ahead == 0 and isinstance(behind, int) and behind > 0:
        return dict(result, reason='A newer descendant of this queued commit is already released.')
    # Diverged histories must not silently publish an older/different branch.
    raise ValueError(f'Release and requested commit have unexpected ancestry ({status}); review the history.')


def boolean(value, name):
    if value not in ('true', 'false'):
        raise ValueError(f'{name} must be true or false.')
    return value == 'true'


def markdown_cell(value):
    return str(value).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('|', '&#124;').replace('`', '&#96;').replace('\n', ' ').replace('\r', ' ')


def main():
    only_new = boolean(os.environ.get('ONLY_IF_UNRELEASED', 'false'), 'ONLY_IF_UNRELEASED')
    requested = boolean(os.environ.get('RELEASE_REQUESTED', 'false'), 'RELEASE_REQUESTED')
    if only_new and not requested:
        raise ValueError('Only-if-unreleased requires Release a Version to be enabled.')
    if only_new:
        result = release_decision(GitHubApi(os.environ.get('GH_TOKEN')),
            os.environ.get('GITHUB_REPOSITORY'), os.environ.get('GITHUB_SHA'), os.environ.get('GITHUB_REF'))
    else:
        result = dict(should_build=True, reason='Normal manual build/release; conditional gate was not requested.')
    output = 'should_build=' + str(result['should_build']).lower() + '\n'
    if os.environ.get('GITHUB_OUTPUT'):
        with Path(os.environ['GITHUB_OUTPUT']).open('a', encoding='utf-8') as file:
            file.write(output)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a', encoding='utf-8') as file:
            file.write('### Component release check\n\n' + markdown_cell(result['reason']) + '\n\n')
            if only_new:
                file.write(f"Commit: `{result['commit']}`. Release: {markdown_cell(result['release_tag'] or '(none)')}.\n")
    print(json.dumps(result, ensure_ascii=True))


if __name__ == '__main__':
    try:
        main()
    except (ApiError, ValueError, KeyError, TypeError) as error:
        print(f'Release check failed: {error}', file=sys.stderr)
        sys.exit(1)
