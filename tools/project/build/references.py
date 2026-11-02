"""Restore hash-pinned platform reference inputs into the disposable build cache."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import tempfile
import time
import urllib.request

from project import ROOT


def specification(manifest: Path) -> dict:
    spec = json.loads(manifest.read_text())
    names = set()
    for entry in spec['sources']:
        name = entry['path']
        if Path(name).name != name or name in ('.', '..') or name in names:
            raise ValueError('reference inputs require unique plain file names')
        if not entry['url'].startswith('https://') or len(entry['sha256']) != 64 or not 0 < entry['bytes'] <= 4 * 1024 * 1024:
            raise ValueError('invalid pinned reference input: ' + name)
        names.add(name)
    return spec


def cache_directory(manifest: Path) -> Path:
    relative = manifest.resolve().relative_to(ROOT / 'platforms')
    return ROOT / 'build/cache/references' / relative.parent / manifest.stem


def valid(path: Path, entry: dict) -> bool:
    return (path.is_file() and path.stat().st_size == entry['bytes'] and
            hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256'])


def prepare(manifest: Path, *, download: bool = True, cache: Path | None = None) -> Path:
    spec = specification(manifest)
    destination = cache if cache is not None else cache_directory(manifest)
    destination.mkdir(parents=True, exist_ok=True)
    with (destination / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for entry in spec['sources']:
            path = destination / entry['path']
            if valid(path, entry):
                continue
            if not download:
                raise ValueError('missing or changed reference input: ' + str(path) + '; run ./project.sh bootstrap')
            for attempt in range(3):
                try:
                    with urllib.request.urlopen(entry['url'], timeout=30) as response:
                        data = response.read(entry['bytes'] + 1)
                    if len(data) != entry['bytes'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
                        raise ValueError('reference download hash/length mismatch: ' + entry['path'])
                    break
                except OSError:
                    if attempt == 2:
                        raise
                    time.sleep(2 ** attempt)
            descriptor, pending = tempfile.mkstemp(prefix='.' + path.name, dir=destination)
            try:
                with os.fdopen(descriptor, 'wb') as stream:
                    stream.write(data)
                Path(pending).replace(path)
            finally:
                Path(pending).unlink(missing_ok=True)
    return destination


def prepare_platform(platform: Path, *, download: bool = True) -> None:
    import tomllib
    settings = tomllib.loads((platform / 'platform.toml').read_text())
    for name in settings.get('reference_manifests', []):
        prepare(platform / name, download=download)
