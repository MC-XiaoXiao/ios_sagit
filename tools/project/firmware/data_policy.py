"""Prepare iOS Data policy files; the APFS writer only imports their manifest."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import stat
import subprocess

from project import ROOT

PREFERENCES = {
    'com.apple.purplebuddy.plist': {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
        'daemonizeNeedsToRun': False,
    },
    'com.apple.purplebuddy.notbackedup.plist': {
        'ShouldRunDiagnosticsMiniBuddy': False, 'ShouldRunHSA2MiniBuddy': False,
        'ShouldRunKeychainSyncMiniBuddy': False, 'ShouldRunMesaMiniBuddy': False,
        'ShouldRunPaymentMiniBuddy': False, 'ShouldRunStandardMiniBuddy': False,
        'SilentICDPUpgrade': False,
    },
}
PERSONA = {'UsePersonaManifestVersion': 1, 'UsePersonaGenerationID': 1,
           'UserPersonaDictionary': {}}


def preference_values(*, setup_complete: bool, linear_surfaces: bool) -> dict:
    values = dict(PREFERENCES) if setup_complete else {}
    if linear_surfaces:
        values['.GlobalPreferences.plist'] = {'CAEnableIOSurfaceCompression': False}
    return values


def stage(extracted: Path, metadata: dict, prefix: str, output: Path,
          *, persona: bool, setup_complete: bool, linear_surfaces: bool = False) -> dict:
    """Merge policy in private staging; preserve existing persona and plist keys."""
    entries = []
    def source_path(relative):
        return '/'.join(part for part in (prefix, relative) if part)
    def original(relative):
        return metadata.get(source_path(relative), {'exists': False})
    def directory(relative, uid, gid, mode):
        previous = original(relative)
        if previous['exists'] and not stat.S_ISDIR(previous['mode']):
            raise ValueError('Data policy parent is not a directory: ' + relative)
        entries.append(dict(path=relative, type='directory', uid=uid, gid=gid,
                            mode=mode, existing='keep'))
    def file(relative, payload, uid, gid, mode, existing):
        path = output / 'files' / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        path.chmod(mode)
        entries.append(dict(path=relative, type='file', uid=uid, gid=gid, mode=mode,
                            existing=existing, source='files/' + relative,
                            sha256=hashlib.sha256(payload).hexdigest()))
    # Validate every requested source before publishing any prepared policy.
    if persona:
        previous = original('keybags')
        if previous['exists'] and (not stat.S_ISDIR(previous['mode']) or
                                   previous['uid'] != 0 or previous['mode'] & 0o022):
            raise ValueError('Data/keybags must be a root-owned directory without group/other writes')
        current = original('keybags/persona.kb')
        if current['exists'] and not stat.S_ISREG(current['mode']):
            raise ValueError('Data persona metadata is not a regular file')
    merged = {}
    preferences = preference_values(setup_complete=setup_complete, linear_surfaces=linear_surfaces)
    if preferences:
        for relative in ('mobile', 'mobile/Library', 'mobile/Library/Preferences'):
            previous = original(relative)
            if previous['exists'] and not stat.S_ISDIR(previous['mode']):
                raise ValueError('Data policy parent is not a directory: ' + relative)
        for name, values in preferences.items():
            relative = 'mobile/Library/Preferences/' + name
            previous = original(relative)
            current = {}
            if previous['exists']:
                if not stat.S_ISREG(previous['mode']):
                    raise ValueError('Data preference is not a regular file: ' + relative)
                path = extracted / source_path(relative)
                if path.stat().st_size > 1 << 20:
                    raise ValueError('Data preference exceeds 1 MiB')
                raw = path.read_bytes()
                if raw:
                    current = plistlib.loads(raw)
                    if not isinstance(current, dict):
                        raise ValueError('Data preference must contain a dictionary')
            merged[relative] = plistlib.dumps({**current, **values}, fmt=plistlib.FMT_BINARY)
    output.mkdir(parents=True, exist_ok=False)
    if persona:
        directory('keybags', 0, 0, 0o700)
        if not original('keybags/persona.kb')['exists']:
            file('keybags/persona.kb', plistlib.dumps(PERSONA, fmt=plistlib.FMT_XML),
                 0, 0, 0o600, 'keep')
    if preferences:
        for relative, mode in [('mobile', 0o755), ('mobile/Library', 0o711),
                               ('mobile/Library/Preferences', 0o755)]:
            directory(relative, 501, 501, mode)
        for relative, payload in merged.items():
            file(relative, payload, 501, 501, 0o600, 'replace')
    manifest = {'schema': 1, 'entries': entries}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def prepare(source: Path, output: Path, *, persona: bool, setup_complete: bool,
            linear_surfaces: bool = False, volume='system', prefix='private/var', writer=None) -> dict:
    if not persona and not setup_complete and not linear_surfaces:
        raise ValueError('select at least one Data policy')
    if prefix and (PurePosixPath(prefix).is_absolute() or
                   any(p in ('', '.', '..') for p in prefix.split('/'))):
        raise ValueError('source prefix must be a canonical volume-relative directory')
    writer = writer or ROOT / 'build/cache/apfs/apfs-image'
    output.parent.mkdir(parents=True, exist_ok=True)
    requests = []
    preferences = preference_values(setup_complete=setup_complete, linear_surfaces=linear_surfaces)
    directories = (['keybags'] if persona else []) + (
        ['mobile', 'mobile/Library', 'mobile/Library/Preferences'] if preferences else [])
    for relative in directories:
        requests.append(dict(path='/'.join(p for p in (prefix, relative) if p),
                             metadata_only=True, optional=True))
    if persona:
        requests.append(dict(path='/'.join(p for p in (prefix, 'keybags/persona.kb') if p),
                             metadata_only=True, optional=True))
    for name in preferences:
        requests.append(dict(path='/'.join(p for p in (prefix, 'mobile/Library/Preferences/' + name) if p),
                             optional=True))
    request_path = output.with_suffix('.requests.json')
    request_path.write_text(json.dumps(requests, indent=2) + '\n')
    extracted = output.with_name(output.name + '-original')
    subprocess.run([str(writer), 'extract-files', '--source', str(source), '--volume', volume,
                    '--manifest', str(request_path), '--output', str(extracted)], check=True, cwd=ROOT)
    metadata = json.loads(Path(str(extracted) + '.metadata.json').read_text())
    return stage(extracted, metadata, prefix, output, persona=persona, setup_complete=setup_complete,
                 linear_surfaces=linear_surfaces)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--volume', default='data')
    parser.add_argument('--source-prefix', default='')
    parser.add_argument('--persona', action='store_true')
    parser.add_argument('--setup-complete', action='store_true')
    parser.add_argument('--linear-surfaces', action='store_true',
                        help='disable Apple IOSurface compression for a backend requiring linear surfaces')
    args = parser.parse_args(argv)
    prepare(args.source.resolve(), args.output.resolve(), persona=args.persona,
            setup_complete=args.setup_complete, linear_surfaces=args.linear_surfaces,
            volume=args.volume, prefix=args.source_prefix)


if __name__ == '__main__':
    main()
