#!/usr/bin/env python3
"""Restore the pinned Mi 6 EDK2 sources and local source changes."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'tools/edk2-sagit.json'


def git(repo: Path, *args: str, env=None, check=True):
    return subprocess.run(['git', '-C', str(repo), *args], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=check, timeout=600)


def ensure_clone(path: Path, url: str, commit: str, check_only: bool):
    if not (path / '.git').exists():
        if check_only:
            raise RuntimeError(f'Missing source checkout: {path}')
        path.mkdir(parents=True, exist_ok=True)
        if any(path.iterdir()):
            raise RuntimeError(f'Refusing to replace nonempty directory: {path}')
        print(f'Fetching {path.relative_to(ROOT)} at {commit}', flush=True)
        git(path, 'init', '--quiet')
        git(path, 'remote', 'add', 'origin', url)
    current = git(path, 'rev-parse', '--verify', 'HEAD', check=False)
    if current.returncode:
        if check_only or any(p.name != '.git' for p in path.iterdir()):
            raise RuntimeError(f'Unfinished source checkout: {path}')
        git(path, 'fetch', '--depth=1', 'origin', commit)
        git(path, '-c', 'advice.detachedHead=false', 'checkout', '--detach', commit)
        current = git(path, 'rev-parse', 'HEAD')
    actual = current.stdout.decode().strip()
    if actual != commit:
        raise RuntimeError(f'{path}: expected {commit}, found {actual}; checkout preserved')


def apply_snapshot(repo: Path, spec: dict, check_only: bool):
    patch = ROOT / spec['file']
    if hashlib.sha256(patch.read_bytes()).hexdigest() != spec['sha256']:
        raise RuntimeError(f'Source patch checksum mismatch: {patch}')
    update = None
    # A private index recognizes the complete source snapshot, including new
    # files, without changing the developer's staging area or build outputs.
    with tempfile.TemporaryDirectory(prefix='mi6-edk2-index-') as directory:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(directory) / 'index'))
        git(repo, 'read-tree', 'HEAD', env=env)
        git(repo, 'apply', '--cached', str(patch), env=env)
        paths = spec['source_files']
        result = git(repo, 'diff', '--quiet', '--', *paths, env=env, check=False)
        if result.returncode == 0:
            return
        if check_only:
            raise RuntimeError(f'Source snapshot differs: {repo}; checkout preserved')
        if 'upgrade' in spec:
            upgrade = ROOT / spec['upgrade']['file']
            if hashlib.sha256(upgrade.read_bytes()).hexdigest() != spec['upgrade']['sha256']:
                raise RuntimeError(f'Source patch checksum mismatch: {upgrade}')
            # Derive the previous complete snapshot from the new one. Only an
            # exact match may be upgraded; developer edits must be preserved.
            git(repo, 'apply', '--cached', '--reverse', str(upgrade), env=env)
            previous = git(repo, 'diff', '--quiet', '--', *paths, env=env, check=False)
            if previous.returncode == 0:
                update = upgrade
    # git apply checks every hunk before writing. Never reset or clean a tree
    # to make a source patch apply over conflicting local edits.
    git(repo, 'apply', '--check', str(update or patch))
    git(repo, 'apply', str(update or patch))
    apply_snapshot(repo, spec, True)


def prepare(check_only: bool = False):
    manifest = json.loads(MANIFEST.read_text())
    if manifest['schema'] != 1:
        raise RuntimeError('Unsupported EDK2 source manifest')
    for spec in manifest['repositories']:
        repo = ROOT / spec['path']
        ensure_clone(repo, spec['url'], spec['commit'], check_only)
        if 'patch' in spec:
            apply_snapshot(repo, spec['patch'], check_only)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true',
                        help='verify existing sources without downloading or applying patches')
    args = parser.parse_args()
    output = ROOT / 'build/edk2-sagit'
    output.mkdir(parents=True, exist_ok=True)
    try:
        with (output / 'prepare.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            prepare(args.check)
    except subprocess.CalledProcessError as error:
        parser.exit(1, error.stderr.decode(errors='replace') + '\n')
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'{error}\n')
    print('Mi 6 EDK2 source snapshots verified')


if __name__ == '__main__':
    main()
