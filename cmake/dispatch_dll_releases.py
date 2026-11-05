"""Send conditional release requests to the fixed component repository list."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import urllib.parse

from check_component_release import (ApiError, GitHubApi, boolean, commit_sha,
    markdown_cell, release_decision, repository_info, repository_name)


def load_components(path):
    data = json.loads(path.read_text(encoding='utf-8'))
    if data.get('schema_version') != 1 or not isinstance(data.get('components'), list) or not data['components']:
        raise ValueError('Invalid component manifest.')
    folders, repositories = set(), set()
    for item in data['components']:
        folder = item.get('folder')
        repo = repository_name(item.get('repository'))
        workflow = item.get('workflow')
        if not isinstance(folder, str) or not re.fullmatch(r'[a-z][a-z0-9_]*', folder):
            raise ValueError('Invalid component folder.')
        if not repo.startswith('TTPlayerRebuild/') or workflow not in ('build.yml', 'manual-build.yml'):
            raise ValueError('Unexpected component repository or workflow.')
        if folder in folders or repo.lower() in repositories:
            raise ValueError('Duplicate component in manifest.')
        if item.get('inputs', {}) not in ({}, {'configuration': 'Release'}):
            raise ValueError('Unexpected component inputs.')
        folders.add(folder)
        repositories.add(repo.lower())
    return data['components']


def request_component(api, item, dry_run):
    repo, workflow = item['repository'], item['workflow']
    info = repository_info(api, repo)
    branch = info['default_branch']
    endpoint = f'/repos/{repo}/actions/workflows/{workflow}'
    remote = api.get(endpoint)
    if remote.get('state') != 'active' or remote.get('path') != '.github/workflows/' + workflow:
        raise ValueError('The component build workflow is missing or disabled.')
    result = dict(folder=item['folder'], repository=repo, workflow=workflow, branch=branch)
    if dry_run:
        head = commit_sha(api.get(f'/repos/{repo}/branches/{urllib.parse.quote(branch, safe="")}')['commit']['sha'])
        decision = release_decision(api, repo, head, 'refs/heads/' + branch, info)
        return dict(result, status='would-build' if decision['should_build'] else 'would-skip',
                    reason=decision['reason'], commit=head, release_tag=decision['release_tag'])
    inputs = dict(item.get('inputs', {}), release_a_version=True, only_if_unreleased=True)
    code, response = api.request(endpoint + '/dispatches', dict(ref=branch, inputs=inputs))
    # Never retry a successful dispatch if a proxy/older API omits the run ID.
    run_id = response.get('workflow_run_id') if isinstance(response, dict) else None
    link = f'https://github.com/{repo}/actions/workflows/{workflow}'
    if isinstance(run_id, int) and not isinstance(run_id, bool) and run_id > 0:
        link = f'https://github.com/{repo}/actions/runs/{run_id}'
    return dict(result, status='requested', http_status=code, run_url=link,
                reason='Request accepted; the destination checks for unpublished commits before building.')


def run(api, components, dry_run):
    results = []
    for item in components:
        try:
            result = request_component(api, item, dry_run)
        except (ApiError, ValueError, KeyError, TypeError) as error:
            result = dict(folder=item['folder'], repository=item['repository'], status='failed', reason=str(error))
        results.append(result)
        message = result['reason'].replace('\r', ' ').replace('\n', ' ')
        print(f"{result['folder']}: {result['status']} — {message}")
    return results


def write_report(results, path, summary_path=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(results, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    if summary_path:
        lines = ['## DLL release requests', '',
            'Requested means dispatched, not published. Open each destination run to see its build/skip/release result.', '',
            '| Folder | Repository | Result | Details |', '|---|---|---|---|']
        for item in results:
            repo = item['repository']
            detail = markdown_cell(item['reason'])
            if item.get('run_url'):
                detail += f" [Run]({item['run_url']})"
            lines.append(f"| {item['folder']} | [{repo}](https://github.com/{repo}) | {item['status']} | {detail} |")
        with Path(summary_path).open('a', encoding='utf-8') as file:
            file.write('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, default=Path(__file__).with_name('dll_repositories.json'))
    parser.add_argument('--report', type=Path, default=Path('artifacts/dll-release-requests.json'))
    args = parser.parse_args()
    components = load_components(args.manifest)
    token = os.environ.get('GH_TOKEN')
    if not token or not token.strip():
        raise ValueError('Configure DLL_RELEASE_TOKEN (or GH_TOKEN) with access to all component repositories.')
    api = GitHubApi(token)
    dry_run = boolean(os.environ.get('DRY_RUN', 'false'), 'DRY_RUN')
    results = run(api, components, dry_run)
    write_report(results, args.report, os.environ.get('GITHUB_STEP_SUMMARY'))
    return 1 if any(item['status'] == 'failed' for item in results) else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ApiError, ValueError, KeyError, TypeError) as error:
        print(f'DLL dispatch failed: {error}', file=sys.stderr)
        sys.exit(1)
