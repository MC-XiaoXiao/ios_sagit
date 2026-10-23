#!/usr/bin/env python3
"""Prepare a pinned V40 checkout, reusing local Git data in independent clones."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT / 'boot/edk2-sdm845'
SEED = ROOT / 'boot/edk2-msm8998'
REVISION = 'e1952621f419f8db60ed28271264e1b5184c571d'


def git(repo, *args, capture=False):
    return subprocess.run(['git', '-C', str(repo), *args], check=True,
                          text=True, stdout=subprocess.PIPE if capture else None).stdout


def ensure_clone(path, url, revision, seed=None):
    if not (path / '.git').exists():
        path.mkdir(parents=True, exist_ok=True)
        if any(path.iterdir()):
            raise RuntimeError(f'Refusing to initialize nonempty directory: {path}')
        git(path, 'init', '--quiet')
        git(path, 'remote', 'add', 'origin', url)
        fetch_from = url
        if seed and (seed / '.git').exists():
            present = subprocess.run(['git', '-C', str(seed), 'cat-file', '-e',
                                      revision + '^{commit}'], capture_output=True)
            if present.returncode == 0:
                fetch_from = str(seed)
                print(f'Reusing independent copy of {seed.relative_to(ROOT)}', flush=True)
        # Fetch creates a separate object store; no hard links or alternates.
        git(path, 'fetch', '--depth=1', fetch_from, revision)
        git(path, '-c', 'advice.detachedHead=false', 'checkout', '--detach', revision)
    actual = git(path, 'rev-parse', 'HEAD', capture=True).strip()
    if actual != revision:
        raise RuntimeError(f'{path}: expected {revision}, found {actual}')
    git(path, 'remote', 'set-url', 'origin', url)
    if (Path(git(path, 'rev-parse', '--absolute-git-dir', capture=True).strip()) /
            'objects/info/alternates').exists():
        raise RuntimeError(f'{path}: shared object alternates are not allowed')


def child(parent, relative, seed=None):
    revision = git(parent, 'rev-parse', 'HEAD:' + relative, capture=True).strip()
    modules = git(parent, 'config', '--file', '.gitmodules', '--get-regexp',
                  r'^submodule\..*\.path$', capture=True)
    key = next(line.split()[0][:-5] for line in modules.splitlines()
               if line.split(None, 1)[1] == relative)
    url = git(parent, 'config', '--file', '.gitmodules', '--get', key + '.url',
              capture=True).strip()
    ensure_clone(parent / relative, url, revision, seed)


def apply(repo, patch):
    reverse = subprocess.run(['git', '-C', str(repo), 'apply', '--reverse',
                              '--check', str(patch)], capture_output=True)
    if reverse.returncode == 0:
        return
    git(repo, 'apply', '--check', str(patch))
    git(repo, 'apply', str(patch))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/edk2-judypn')
    args = parser.parse_args()
    ensure_clone(REPO, 'https://github.com/edk2-porting/edk2-msm.git', REVISION)
    child(REPO, 'Common/edk2', SEED / 'Common/edk2')
    child(REPO, 'Common/edk2-platforms', SEED / 'Common/edk2-platforms')
    child(REPO, 'GPLDrivers/Library/SimpleInit', SEED / 'GPLDrivers/Library/simple-init')
    child(REPO, 'Platform/EFI_Binaries')
    edk = REPO / 'Common/edk2'
    for rel in ['CryptoPkg/Library/OpensslLib/openssl',
                'ArmPkg/Library/ArmSoftFloatLib/berkeley-softfloat-3',
                'BaseTools/Source/C/BrotliCompress/brotli',
                'MdeModulePkg/Library/BrotliCustomDecompressLib/brotli']:
        child(edk, rel, SEED / 'Common/edk2' / rel)
    child(REPO / 'GPLDrivers/Library/SimpleInit', 'libs/freetype',
          SEED / 'GPLDrivers/Library/simple-init/libs/freetype')
    # The V40 follow-up changes context in the original application's patch.
    extra = ROOT / 'tools/patches/edk2-judypn/edk2-fastboot-session.patch'
    followup_applied = extra.exists() and subprocess.run(
        ['git', '-C', str(edk), 'apply', '--reverse', '--check', str(extra)],
        capture_output=True).returncode == 0
    if not followup_applied:
        apply(edk, ROOT / 'tools/patches/edk2-sagit/edk2-fastboot-app.patch')
    patches = sorted((ROOT / 'tools/patches/edk2-judypn').glob('000*.patch'))
    for patch in patches:
        apply(REPO, patch)
    # Additional application fixes apply only in this independent core checkout.
    if extra.exists():
        apply(edk, extra)
    paths = [REPO, edk, REPO / 'Common/edk2-platforms',
             REPO / 'GPLDrivers/Library/SimpleInit', REPO / 'Platform/EFI_Binaries']
    manifest = {str(p.relative_to(ROOT)): git(p, 'rev-parse', 'HEAD', capture=True).strip()
                for p in paths}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'source-versions.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
