"""Describe and stage board file replacements, independently of APFS storage."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def relative(value):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or any(part in ('', '.', '..') for part in value.split('/')):
        raise ValueError('file replacement requires a canonical relative path')
    return path


def describe(manifest, workspace):
    spec = json.loads(manifest.read_text())
    if spec.get('schema') != 1:
        raise ValueError('unsupported board file replacement manifest')
    workspace = workspace.resolve()
    records = []
    seen = set()
    for entry in spec['files']:
        source = workspace / str(relative(entry['source']))
        target = relative(entry['destination'])
        paths = sorted(source.rglob('*')) if source.is_dir() else [source]
        for path in paths:
            if path.is_symlink() or not path.resolve().is_relative_to(workspace):
                raise ValueError('board replacement source escapes the workspace')
            if path.is_dir():
                continue
            if not path.is_file():
                raise ValueError('missing board replacement source: ' + str(path))
            destination = str(target / path.relative_to(source)) if source.is_dir() else str(target)
            if destination in seen:
                raise ValueError('duplicate board replacement destination: ' + destination)
            seen.add(destination)
            records.append({'source': str(path), 'destination': destination,
                            'sha256': digest(path), 'mode': path.stat().st_mode & 0o777})
    return {'manifest_sha256': digest(manifest), 'files': records}


def stage(description, overlay):
    """Check all source hashes before copying into a private System overlay."""
    for record in description['files']:
        source = Path(record['source'])
        if source.is_symlink() or digest(source) != record['sha256']:
            raise ValueError('board replacement changed: ' + str(source))
        relative(record['destination'])
    for record in description['files']:
        destination = overlay / record['destination']
        if any(p.is_symlink() for p in [destination, *destination.parents]):
            raise ValueError('board replacement destination crosses a symlink')
        if destination.exists():
            raise ValueError('board replacement conflicts with another firmware policy: ' + record['destination'])
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(record['source'], destination)
        destination.chmod(record['mode'])
        if digest(destination) != record['sha256']:
            raise ValueError('board replacement copy verification failed')
