"""Send conditional DLL release requests and report the destination build jobs."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import time
import urllib.parse

from check_component_release import (ApiError, GitHubApi, boolean, commit_sha,
    markdown_cell, release_decision, repository_info, repository_name)

OBSERVE_SECONDS = 120
POLL_SECONDS = 10
SNAPSHOT_SECONDS = 60
CHECK_JOB_NAMES = ('Check for unpublished commits', 'release_check')


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
        build_job = item.get('build_job', 'build')
        if not isinstance(build_job, str) or not build_job.strip() or build_job in CHECK_JOB_NAMES:
            raise ValueError('Invalid component build job name.')
        folders.add(folder)
        repositories.add(repo.lower())
    return data['components']


def preview_component(api, repo, info):
    branch = info['default_branch']
    head = commit_sha(api.get(f'/repos/{repo}/branches/{urllib.parse.quote(branch, safe="")}')['commit']['sha'])
    decision = release_decision(api, repo, head, 'refs/heads/' + branch, info)
    return dict(status='would-build' if decision['should_build'] else 'would-skip',
                reason=decision['reason'], commit=head, release_tag=decision['release_tag'])


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
        return dict(result, **preview_component(api, repo, info))
    inputs = dict(item.get('inputs', {}), release_a_version=True, only_if_unreleased=True)
    code, response = api.request(endpoint + '/dispatches', dict(ref=branch, inputs=inputs))
    # Never retry a successful dispatch if a proxy/older API omits the run ID.
    run_id = response.get('workflow_run_id') if isinstance(response, dict) else None
    result.update(status='requested', http_status=code,
                  run_url=f'https://github.com/{repo}/actions/workflows/{workflow}',
                  build_job=item.get('build_job', 'build'), build_status='unavailable',
                  reason='GitHub accepted the request but did not return a run ID.')
    if isinstance(run_id, int) and not isinstance(run_id, bool) and run_id > 0:
        result.update(run_id=run_id, run_url=f'https://github.com/{repo}/actions/runs/{run_id}',
                      build_status='pending', reason='Waiting for the destination release check.')
    return result


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


def destination_build(api, item):
    # Follow the precise dispatched run and its first attempt, not the latest
    # run on a branch (which could be a manual build or a different dispatch).
    repo, run_id = item['repository'], item['run_id']
    jobs = api.get(f'/repos/{repo}/actions/runs/{run_id}/attempts/1/jobs?per_page=100')['jobs']
    if not isinstance(jobs, list):
        raise ValueError('GitHub did not return a job list.')
    check = next((job for job in jobs if job['name'] in CHECK_JOB_NAMES), None)
    if not check or check['status'] != 'completed' or not check.get('conclusion'):
        return None
    if check['conclusion'] != 'success':
        return dict(build_status='check-not-successful',
                    reason=f"Destination release check ended with {check['conclusion']}; not a normal skip.")
    build = next((job for job in jobs if job['name'] == item['build_job']), None)
    if not build:
        # A successful check alone does not tell us its should_build output.
        return None
    status = build['status']
    if status in ('requested', 'queued', 'pending', 'waiting'):
        return dict(build_status='build-queued',
                    reason='The destination build job is queued or waiting to start.')
    if status == 'in_progress':
        return dict(build_status='building', reason='The destination build job has started.')
    if status == 'completed':
        conclusion = build.get('conclusion')
        if conclusion == 'skipped':
            return dict(build_status='skipped', reason='The destination build job was skipped.')
        if conclusion == 'success':
            return dict(build_status='built', reason='The destination build job succeeded.')
        if conclusion:
            return dict(build_status='build-not-successful',
                        reason=f'The destination build job ended with {conclusion}.')
    return None


def observe_builds(api, results):
    pending = [item for item in results if item.get('run_id')]
    if pending:
        print(f'Checking destination build jobs for up to {OBSERVE_SECONDS} seconds.')
    deadline = time.monotonic() + OBSERVE_SECONDS
    while pending and time.monotonic() < deadline:
        for item in pending[:]:
            if time.monotonic() >= deadline:
                break
            try:
                update = destination_build(api, item)
            except (ApiError, ValueError, KeyError, TypeError) as error:
                # The exact run may not be available on the API immediately.
                if isinstance(error, ApiError) and error.status == 404:
                    continue
                update = dict(build_status='unavailable', reason=f'Could not read destination jobs: {error}')
            if update:
                item.update(update)
                pending.remove(item)
        if pending and (remaining := deadline - time.monotonic()) > 0:
            time.sleep(min(POLL_SECONDS, remaining))
    for item in pending:
        item['reason'] = 'Destination build decision was not visible before the observation window ended.'

    # Keep accepted requests distinct from failures to observe them. A current
    # default-branch snapshot is useful for pending/untracked runs, but it is
    # never reported as the outcome of the SHA GitHub actually dispatched.
    deadline = time.monotonic() + SNAPSHOT_SECONDS
    for item in results:
        if time.monotonic() >= deadline:
            break
        if item.get('build_status') not in ('pending', 'unavailable'):
            continue
        try:
            repo = item['repository']
            item['snapshot'] = preview_component(api, repo, repository_info(api, repo))
        except (ApiError, ValueError, KeyError, TypeError) as error:
            item['snapshot_error'] = str(error)


def write_report(results, path, summary_path=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(results, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    if summary_path:
        lines = ['## DLL release requests', '',
            'Requested means dispatched, not published. Build shows the destination job observed during this run. '
            '`would-build` / `would-skip` are read-only snapshots, not confirmed build outcomes. '
            'Open the linked run for its latest status.', '',
            '| Folder | Repository | Request | Build | Details |', '|---|---|---|---|---|']
        for item in results:
            repo = item['repository']
            request = item['status']
            build = item.get('build_status', '—')
            detail = markdown_cell(item['reason'])
            if request in ('would-build', 'would-skip'):
                request, build = 'preview', request
            snapshot = item.get('snapshot')
            if snapshot:
                build += f" ({snapshot['status']} now)"
                detail += ' Snapshot: ' + markdown_cell(snapshot['reason'])
            if snapshot or item.get('commit'):
                evidence = snapshot or item
                detail += f" Commit `{evidence['commit'][:12]}`; latest Release: {markdown_cell(evidence['release_tag'] or '(none)')}."
            if item.get('snapshot_error'):
                detail += ' Snapshot unavailable: ' + markdown_cell(item['snapshot_error'])
            if item.get('run_url'):
                label = 'Run' if item.get('run_id') else 'Workflow'
                detail += f" [{label}]({item['run_url']})"
            lines.append(f"| {item['folder']} | [{repo}](https://github.com/{repo}) | {request} | {build} | {detail} |")
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
    if not dry_run:
        observe_builds(api, results)
    write_report(results, args.report, os.environ.get('GITHUB_STEP_SUMMARY'))
    return 1 if any(item['status'] == 'failed' for item in results) else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ApiError, ValueError, KeyError, TypeError) as error:
        print(f'DLL dispatch failed: {error}', file=sys.stderr)
        sys.exit(1)
