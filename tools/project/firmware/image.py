#!/usr/bin/env python3
"""Build a complete patched iOS filesystem from pinned IPSW inputs, without device writes."""
from __future__ import annotations
from project import ROOT

import argparse
import contextlib
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

from project.firmware import load_firmware_report
from project.build import services
from project.firmware import apfs, preboot, serial_shell, launchd, wallpaper
from project.firmware.patches import watchdog, system_ui

WRITER = ROOT / 'build/cache/apfs/apfs-image'
CHECKER = ROOT / 'tools/src/apfsprogs/apfsck/apfsck'
BLOCK = 4096
PATCHES = {'serial-console', 'watchdog-no-pmu', 'dyld-paths', 'keybag-no-sep',
           'poster-store', 'onboarding-template', 'wallpaper', 'event-timing',
           'initial-persona', 'setup-complete', 'migration-services', 'fairplay-readiness',
           'linear-surfaces'}


def sha(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    temporary.replace(path)




def run(*args, **kwargs) -> None:
    subprocess.run([str(arg) for arg in args], check=True, cwd=ROOT, **kwargs)


def read_profile(path: Path) -> dict:
    profile = json.loads(path.read_text())
    if profile.get('schema') != 1 or not profile.get('patches'):
        raise ValueError('unsupported or empty firmware profile')
    patches = profile['patches']
    if len(patches) != len(set(patches)) or set(patches) - PATCHES:
        raise ValueError('duplicate or unknown firmware patch')
    # These policies are a unit: partial no-SEP setups have not been validated.
    if not {'keybag-no-sep', 'poster-store', 'dyld-paths', 'initial-persona'} <= set(patches):
        raise ValueError('profile is missing required no-SEP filesystem policies')
    if not isinstance(profile['data_free_mib'], int) or profile['data_free_mib'] < 2048:
        raise ValueError('profile requires at least 2048 MiB of shared free space')
    if manifest := profile.get('file_overlay_manifest'):
        board = (ROOT / manifest).parent
        workspace = ROOT / profile['artifact_workspace']
        def portable(path):
            return str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)
        profile['file_overlay_manifest'] = portable(ROOT / manifest)
        profile['artifact_workspace'] = portable(workspace)
        profile['bridge_paths'] = {'board': portable(board), 'workspace': portable(workspace)}
        profile['bridge_flags'] = [value.replace(str(board), '{board}').replace(str(workspace), '{workspace}')
                                   for value in profile['bridge_flags']]
    return profile


def implementation() -> dict:
    paths = [ROOT / name for name in ['tools/project/firmware/image.py', 'tools/project/firmware/__init__.py', 'tools/project/firmware/apfs.py', 'tools/project/firmware/preboot.py', 'tools/project/firmware/serial_shell.py', 'tools/project/firmware/patches/watchdog.py', 'tools/project/firmware/launchd.py', 'tools/project/firmware/wallpaper.py', 'tools/project/firmware/patches/system_ui.py', 'tools/project/firmware/patches/keybag.py', 'tools/project/firmware/patches/poster.py', 'tools/project/firmware/sealed.py', 'tools/project/firmware/trustcache.py', 'tools/project/firmware/codesign.py', 'tools/project/firmware/macho.py', 'tools/project/build/services.py']]
    paths += [path for path in (ROOT / 'tools/apfs-image').glob('*.go') if not path.name.endswith('_test.go')]
    paths += [ROOT / 'tools/apfs-tools.json', ROOT / 'tools/serial-bash-packages.json']
    paths += list((ROOT / 'tools/patches').glob('go-apfs-*.patch'))
    paths += list((ROOT / 'tools/displayctl').glob('*.S'))
    paths += list((ROOT / 'tools/overlays/event-timing').rglob('*.plist'))
    paths += [ROOT / 'tools/project/firmware/file_overlay.py', ROOT / 'tools/project/firmware/data_policy.py']
    return {str(path.relative_to(ROOT)): sha(path) for path in sorted(paths)}


@contextlib.contextmanager
def workspace_lock(workspace: Path):
    workspace.mkdir(parents=True, exist_ok=True)
    with (workspace / '.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError('another firmware command is using this workspace') from error
        yield


def inventory(root: Path) -> dict:
    records = {}
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            records[relative] = {'symlink': os.readlink(path)}
        elif path.is_file():
            records[relative] = {'sha256': sha(path), 'bytes': path.stat().st_size,
                                 'mode': path.stat().st_mode & 0o7777}
    return records


def verify_inventory(root: Path, records: dict) -> None:
    actual = inventory(root)
    if actual != records:
        changed = sorted(name for name in actual.keys() | records.keys() if actual.get(name) != records.get(name))
        raise ValueError('prepared artifacts changed: ' + ', '.join(changed[:8]))


def merge_overlay(source: Path, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.rglob('*')):
        relative = path.relative_to(source)
        destination = target / relative
        # An earlier overlay must not redirect later writes outside the staging tree.
        if any(parent.is_symlink() for parent in [destination, *destination.parents] if parent != target.parent):
            raise ValueError('overlay collision with symlink: ' + str(relative))
        if path.is_dir() and not path.is_symlink():
            destination.mkdir(exist_ok=True)
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            if path.is_file() and destination.is_file() and path.read_bytes() == destination.read_bytes():
                continue
            raise ValueError('conflicting firmware overlays: ' + str(relative))
        if path.is_symlink():
            destination.symlink_to(os.readlink(path))
        else:
            shutil.copy2(path, destination)


def delta_records(path: Path, size: int):
    with path.open('rb') as stream:
        header = stream.read(24)
        if len(header) != 24:
            raise ValueError('truncated sealed block delta')
        magic, length, count = struct.unpack('<8sQQ', header)
        if magic != b'APFSPAT1' or length != size or size % BLOCK or path.stat().st_size != 24 + count * 8200:
            raise ValueError('sealed block delta geometry mismatch')
        previous = -1
        for _ in range(count):
            record = stream.read(8200)
            block = struct.unpack_from('<Q', record)[0]
            if not previous < block < size // BLOCK:
                raise ValueError('unordered or out-of-range sealed block delta')
            previous = block
            yield block, record[8:4104], record[4104:]


def apply_sealed_delta(image: Path, delta: Path, manifest: dict) -> set[int]:
    if sha(image) != manifest['source_sha256'] or sha(delta) != manifest['delta_sha256']:
        raise ValueError('sealed patch source or delta changed')
    # All old bytes are checked before modifying the private, rebuildable image.
    records = list(delta_records(delta, image.stat().st_size))
    with image.open('r+b') as stream:
        for block, before, _ in records:
            stream.seek(block * BLOCK)
            if stream.read(BLOCK) != before:
                raise ValueError('sealed patch preimage mismatch')
        for block, _, after in records:
            stream.seek(block * BLOCK)
            stream.write(after)
        stream.flush()
        os.fsync(stream.fileno())
    if sha(image) != manifest['derived_sha256']:
        raise ValueError('derived Cryptex hash mismatch')
    return {block for block, _, _ in records}


def firmware_inputs(args, profile: dict) -> dict:
    if args.firmware_report:
        report = load_firmware_report(args.firmware_report)
    else:
        if not args.ipsw:
            raise ValueError('provide --ipsw or --firmware-report')
        report = apfs.prepare(args.ipsw, profile['firmware']['board'], args.workspace / 'inputs')
    expected = profile['firmware']
    if (report['board'], report['product_version'], report['build']) != (expected['board'], expected['version'], expected['build']):
        raise ValueError('firmware identity differs from selected profile')
    print('Verifying original firmware inputs', flush=True)
    if report['ipsw_sha256'] != expected['ipsw_sha256'] or sha(Path(report['ipsw'])) != expected['ipsw_sha256']:
        raise ValueError('IPSW differs from pinned firmware profile')
    for key, expected_hash in expected['components'].items():
        component = report['components'][key]
        if component['sha256'] != expected_hash or sha(Path(component['image'])) != expected_hash:
            raise ValueError('prepared firmware component changed: ' + key)
    return report


def portable_report(report: dict, directory: Path) -> dict:
    result = json.loads(json.dumps(report))
    result['path_base'] = 'report'
    result['ipsw'] = os.path.relpath(report['ipsw'], directory)
    for component in result['components'].values():
        component['image'] = os.path.relpath(component['image'], directory)
    return result


def prepare(args) -> dict:
    profile = read_profile(args.profile)
    if manifest := profile.get('file_overlay_manifest'):
        from project.firmware.file_overlay import describe
        profile['file_overlay'] = describe(ROOT / manifest, ROOT / profile['artifact_workspace'])
    workspace = args.workspace
    original = firmware_inputs(args, profile)
    recipe = {'schema': 1, 'profile': profile, 'implementation': implementation(),
              'ipsw_sha256': original['ipsw_sha256']}
    recipe_hash = hashlib.sha256(json.dumps(recipe, sort_keys=True).encode()).hexdigest()
    bundle_path = workspace / 'bundle.json'
    if bundle_path.exists():
        bundle = json.loads(bundle_path.read_text())
        if bundle['recipe_sha256'] != recipe_hash:
            raise ValueError('recipe changed; select a new workspace to preserve the existing build')
        verify_inventory(workspace / 'payload', bundle['artifacts'])
        print('Prepared bundle verified; no patches reapplied', flush=True)
        return bundle
    if (workspace / 'payload').exists():
        raise ValueError('unrecorded payload directory; choose a new workspace')
    write_json(workspace / 'recipe.json', recipe)
    write_json(workspace / 'prepared-firmware.json', portable_report(original, workspace))
    with tempfile.TemporaryDirectory(prefix='.prepare-', dir=workspace) as temporary:
        stage = Path(temporary)
        overlay = stage / 'system-overlay'
        overlay.mkdir()
        paths = [
            {'path': 'System/Library/xpc/launchd.plist'},
            {'path': 'usr/libexec/watchdogd'},
            {'path': 'System/Library/DataClassMigrators/SystemAppMigrator.migrator/SystemAppMigrator'},
            {'path': 'System/Library/DataClassMigrators/SpringBoard.migrator/SpringBoard'},
            {'path': 'System/Library/LaunchDaemons/com.apple.fairplayd.A2.plist'},
            {'path': 'Library/Wallpaper', 'recursive': True, 'suffix': '.plist'},
            *[{'path': 'System/Library/PrivateFrameworks/ProgressUI.framework/' + name + '@2x~ipad.png'}
              for name in ('apple-logo', 'apple-logo-black')],
        ]
        write_json(stage / 'extract.json', paths)
        extracted = stage / 'original-files'
        run(WRITER, 'extract-files', '--source', original['components']['OS']['image'],
            '--manifest', stage / 'extract.json', '--output', extracted)
        if {'initial-persona', 'setup-complete', 'linear-surfaces'} & set(profile['patches']):
            from project.firmware.data_policy import prepare as prepare_data_policy
            prepare_data_policy(Path(original['components']['OS']['image']), stage / 'data-policy',
                                persona='initial-persona' in profile['patches'],
                                setup_complete='setup-complete' in profile['patches'],
                                linear_surfaces='linear-surfaces' in profile['patches'], writer=WRITER)
        trust = stage / 'trust-cache'
        trust.mkdir()
        preboot.prepare(workspace / 'prepared-firmware.json', stage / 'preboot-overlay')
        from project.firmware.trustcache import raw_module, development_module
        for kind in ('os', 'app'):
            module = raw_module((stage / 'preboot-overlay/cryptex1/current' / (kind + '.dmg.trustcache')).read_bytes())
            (trust / (kind + '.bin')).write_bytes(module)
        if 'serial-console' in profile['patches']:
            serial_shell.prepare(stage / 'bash')
            merge_overlay(stage / 'bash/overlay', overlay)
        if {'migration-services', 'fairplay-readiness'} & set(profile['patches']):
            policies = services
            if 'migration-services' in profile['patches']:
                for name, relative, generate in [
                    ('system-app-migrator', 'SystemAppMigrator.migrator/SystemAppMigrator', policies.prepare_system_app_migrator),
                    ('springboard-migrator', 'SpringBoard.migrator/SpringBoard', policies.prepare_springboard_migrator),
                ]:
                    output = stage / 'patches' / name
                    generate(extracted / 'System/Library/DataClassMigrators' / relative, output)
                    merge_overlay(output / 'overlay', overlay)
            if 'fairplay-readiness' in profile['patches']:
                output = stage / 'patches/fairplay-readiness'
                policies.prepare_no_sep_service_readiness(
                    extracted / 'System/Library/LaunchDaemons/com.apple.fairplayd.A2.plist', output)
                merge_overlay(output / 'overlay', overlay)
        launchd.merge_cache(extracted / 'System/Library/xpc/launchd.plist', overlay)
        if 'watchdog-no-pmu' in profile['patches']:
            watchdog.prepare(extracted / 'usr/libexec/watchdogd', stage / 'watchdog/watchdogd')
            destination = overlay / 'usr/libexec/watchdogd'
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(stage / 'watchdog/watchdogd', destination)
            destination.chmod(0o755)
        if 'wallpaper' in profile['patches']:
            report = wallpaper.prepare(extracted, stage / 'wallpaper',
                profile['product'], profile['logical_screen_class'])
            write_json(stage / 'wallpaper.json', report)
            merge_overlay(stage / 'wallpaper', overlay)
        if 'event-timing' in profile['patches']:
            merge_overlay(ROOT / 'tools/overlays/event-timing', overlay)
        if 'dyld-paths' in profile['patches']:
            for name, target in {
                'System/Cryptexes/OS': '/private/preboot/Cryptexes',
                'System/Library/Caches/com.apple.dyld': '/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld',
            }.items():
                link = overlay / name
                link.parent.mkdir(parents=True, exist_ok=True)
                link.symlink_to(target)
        image = stage / 'SystemOS.dmg'
        source = Path(original['components']['Cryptex1,SystemOS']['image'])
        # Btrfs/XFS share unchanged extents; other hosts copy this one input.
        run('cp', '--reflink=auto', '--', source, image)
        changed = set()
        for policy in ('keybag-no-sep', 'poster-store', 'onboarding-template'):
            if policy not in profile['patches']:
                continue
            print('Applying ' + policy, flush=True)
            output = stage / 'patches' / policy
            if policy == 'keybag-no-sep':
                from project.firmware.patches.keybag import prepare as generate
                manifest = generate(image, output)
            elif policy == 'poster-store':
                from project.firmware.patches.poster import prepare as generate
                manifest = generate(image, output, development_unprotected_data=True)
            else:
                manifest = system_ui.prepare(image, output,
                    extracted / 'System/Library/PrivateFrameworks/ProgressUI.framework')
                merge_overlay(output / 'overlay', overlay)
            changed |= apply_sealed_delta(image, output / 'sealed-blocks.bin', manifest)
            shutil.copy2(output / 'trust-cache.bin', trust / (policy + '.bin'))
        from project.firmware.sealed import SealedImage
        sealed = SealedImage(image)
        try:
            sealed.verify_seal()
            cryptex_uuid = str(uuid.UUID(hex=sealed.volume_uuid)).upper()
        finally:
            sealed.close()
        # One cumulative patch is useful for existing installations as well.
        with source.open('rb') as before, image.open('rb') as after:
            records = []
            for block in sorted(changed):
                before.seek(block * BLOCK); after.seek(block * BLOCK)
                old, new = before.read(BLOCK), after.read(BLOCK)
                if old != new:
                    records.append(struct.pack('<Q', block) + old + new)
        delta = stage / 'SystemOS.blocks'
        with delta.open('xb') as stream:
            stream.write(struct.pack('<8sQQ', b'APFSPAT1', image.stat().st_size, len(records)))
            stream.writelines(records)
        write_json(stage / 'SystemOS.patch.json', {
            'path': 'usr/standalone/firmware/Cryptex1/SystemOS.dmg', 'delta': delta.name,
            'source_sha256': sha(source), 'derived_sha256': sha(image), 'delta_sha256': sha(delta)})
        if profile.get('file_overlay'):
            from project.firmware.file_overlay import stage as stage_files
            stage_files(profile['file_overlay'], overlay)
            write_json(stage / 'file-replacements.json', profile['file_overlay'])
        module, files = development_module([overlay])
        (trust / 'system-overlay.bin').write_bytes(module)
        write_json(stage / 'system-signatures.json', files)
        additions = [dict(volume, uuid=str(uuid.uuid4()).upper(),
                          overlay='preboot-overlay' if volume['role'] == 'preboot' else '')
                     for volume in profile['volumes']]
        write_json(stage / 'volumes.json', additions)
        # Internal report paths refer to the final directory, never the scratch name.
        for path in stage.rglob('*.json'):
            path.write_text(path.read_text().replace(str(stage), str(workspace / 'payload')))
        stage.rename(workspace / 'payload')
    boot_args = [
        'imageboot-attach=file:///usr/standalone/firmware/Cryptex1/SystemOS.dmg',
        'imageboot-mountpoint=/private/preboot/Cryptexes', 'imageboot-fstype=apfs',
        'imageboot-volume-uuid=' + cryptex_uuid, 'imageboot-parent-role=preboot',
        'imageboot-parent-mountpoint=/private/preboot', 'vm-pagesize=16384', '-wdt_no_pmu', 'wdt=-1',
        *profile.get('boot_args', []),
    ]
    bundle = {'schema': 1, 'recipe_sha256': recipe_hash, 'profile': profile,
              'cryptex_volume_uuid': cryptex_uuid, 'extra_boot_args': boot_args,
              'artifacts': inventory(workspace / 'payload'),
              'device_written': False, 'hardware_tested': False}
    write_json(bundle_path, bundle)
    print('Prepared complete filesystem inputs: ' + str(workspace), flush=True)
    return bundle


def load_bundle(workspace: Path) -> dict:
    bundle = json.loads((workspace / 'bundle.json').read_text())
    verify_inventory(workspace / 'payload', bundle['artifacts'])
    return bundle


def build_image(workspace: Path) -> None:
    bundle = load_bundle(workspace)
    profile = bundle['profile']
    firmware = load_firmware_report(workspace / 'prepared-firmware.json')
    payload = workspace / 'payload'
    destination = workspace / 'image'
    if destination.exists():
        verify_image(workspace)
        return
    with tempfile.TemporaryDirectory(prefix='.image-', dir=workspace) as temporary:
        stage = Path(temporary)
        additions = json.loads((payload / 'volumes.json').read_text())
        for volume in additions:
            if volume['overlay']:
                volume['overlay'] = str(payload / volume['overlay'])
        write_json(stage / 'volumes.json', additions)
        args = [WRITER, '--source', firmware['components']['OS']['image'],
                '--cryptex-os', payload / 'SystemOS.dmg',
                '--cryptex-app', firmware['components']['Cryptex1,AppOS']['image'],
                '--system-min-mib', firmware['system_min_mib'], '--data-free-mib', profile['data_free_mib'],
                '--system-overlay', payload / 'system-overlay', '--add-volumes', stage / 'volumes.json',
                '--output', stage / 'ios.apfs']
        if (payload / 'data-policy/manifest.json').exists():
            args += ['--data-overlay', payload / 'data-policy/manifest.json']
        run(*args)
        with (stage / 'apfsck.log').open('w') as log:
            run(CHECKER, '-c', stage / 'ios.apfs', stdout=log, stderr=subprocess.STDOUT)
        report = json.loads((stage / 'ios.apfs.json').read_text())
        report['recipe_sha256'] = bundle['recipe_sha256']
        write_json(stage / 'ios.apfs.json', report)
        write_json(stage / 'validation.json', {'schema': 1, 'image_sha256': report['image_sha256'],
            'apfsck_sha256': sha(CHECKER), 'apfsck_exit_code': 0, 'hardware_tested': False})
        stage.rename(destination)
    verify_image(workspace)


def verify_image(workspace: Path) -> None:
    bundle = load_bundle(workspace)
    image = workspace / 'image/ios.apfs'
    report_path = image.with_suffix('.apfs.json')
    report = json.loads(report_path.read_text())
    validation = json.loads((image.parent / 'validation.json').read_text())
    digest = sha(image)
    if digest != report['image_sha256'] or digest != validation['image_sha256'] or report['recipe_sha256'] != bundle['recipe_sha256']:
        raise ValueError('image, validation and patch recipe do not match')
    run(WRITER, 'verify-device', '--device', image, '--offset', 0,
        '--report', report_path, '--output', image.parent / 'filesystem-verified.json')
    expected = dict(report, new_free_bytes=report['shared_free_bytes'], overlay_files={
        name.removeprefix('system-overlay/'): {key: record[key] for key in ('sha256', 'symlink') if key in record}
        for name, record in bundle['artifacts'].items() if name.startswith('system-overlay/')})
    write_json(image.parent / 'expected-patches.json', expected)
    run(WRITER, 'verify-update', '--device', image, '--report', image.parent / 'expected-patches.json',
        '--output', image.parent / 'patched-files-verified.json')
    with tempfile.TemporaryDirectory(prefix='.verify-', dir=workspace) as temporary:
        stage = Path(temporary)
        paths = [{'path': 'keybags/persona.kb'}]
        if 'setup-complete' in bundle['profile']['patches']:
            paths += [{'path': 'mobile/Library/Preferences/' + name + '.plist'}
                      for name in ('com.apple.purplebuddy', 'com.apple.purplebuddy.notbackedup')]
        if 'linear-surfaces' in bundle['profile']['patches']:
            paths.append({'path': 'mobile/Library/Preferences/.GlobalPreferences.plist'})
        write_json(stage / 'data.json', paths)
        run(WRITER, 'extract-files', '--source', image, '--volume', 'data',
            '--manifest', stage / 'data.json', '--output', stage / 'data')
        persona = plistlib.loads((stage / 'data/keybags/persona.kb').read_bytes())
        if persona.get('UsePersonaManifestVersion') != 1 or persona.get('UserPersonaDictionary') != {}:
            raise ValueError('initial persona metadata differs from the fresh-install policy')
        if 'setup-complete' in bundle['profile']['patches']:
            prefs = plistlib.loads((stage / 'data/mobile/Library/Preferences/com.apple.purplebuddy.plist').read_bytes())
            if prefs.get('SetupDone') is not True or prefs.get('SetupFinishedAllSteps') is not True:
                raise ValueError('Setup completion missing from the installed image')
        if 'linear-surfaces' in bundle['profile']['patches']:
            prefs = plistlib.loads((stage / 'data/mobile/Library/Preferences/.GlobalPreferences.plist').read_bytes())
            if prefs.get('CAEnableIOSurfaceCompression') is not False:
                raise ValueError('linear surface preference missing from the installed image')
    print('Complete patched image verified: ' + str(image), flush=True)


def bridge(args) -> None:
    verify_image(args.workspace)
    bundle = load_bundle(args.workspace)
    options = list(args.bridge_options)
    if options[:1] == ['--']:
        options.pop(0)
    reserved = ('--apfs-report', '--firmware-report', '--kernel', '--output', '--extra-boot-args')
    if any(option.split('=', 1)[0] in reserved for option in options):
        raise ValueError('filesystem identity and boot arguments are managed by the bundle')
    paths = {name: str(ROOT / value) for name, value in bundle['profile'].get('bridge_paths', {}).items()}
    flags = [value.format(**paths) for value in bundle['profile']['bridge_flags']]
    command = [sys.executable, ROOT / 'tools/project/build/bridge.py', '--kernel', args.kernel.resolve(),
        '--apfs-report', args.workspace / 'image/ios.apfs.json',
        '--firmware-report', args.workspace / 'prepared-firmware.json',
        *flags, '--extra-boot-args', ' '.join(bundle['extra_boot_args']),
        '--output', args.output or args.workspace / 'bridge']
    for path in sorted((args.workspace / 'payload/trust-cache').glob('*.bin')):
        command += ['--trust-cache', path]
    command += options
    write_json(args.workspace / 'bridge-command.json', [str(value) for value in command])
    from project.execution import invoke
    invoke('project.build.bridge', command[2:])


def check_release_inputs(args) -> None:
    from project.build.runtime import load_runtime
    from project.build.boot_format import parse_image, FIRMWARE_NAME, ZERO_DELAY_FIRMWARE_NAME
    firmware = parse_image(args.edk2.read_bytes()).header[48:64].rstrip(b'\0')
    if firmware not in (FIRMWARE_NAME, ZERO_DELAY_FIRMWARE_NAME):
        raise ValueError('EDK2 does not support automatic zero-delay boot; run project.sh build edk2 first')
    load_runtime(args.runtime.resolve())


def package_release(args) -> None:
    """Bind an already verified filesystem to an explicitly selected runtime."""
    check_release_inputs(args)
    verify_image(args.workspace)
    from project.build.runtime import build_runtime
    builder_paths = [ROOT / name for name in ['tools/project/firmware/image.py', 'tools/project/build/runtime.py', 'tools/project/build/nvram.py', 'tools/project/firmware/filesystems.py', 'tools/project/firmware/trustcache.py', 'tools/project/build/bridge.py', 'tools/project/build/boot_image.py', 'tools/project/build/boot_format.py']]
    builder_paths += [ROOT / name for name in ['tools/project/analysis/cprh.py',
        'tools/project/build/references.py', 'platforms/qcom/msm8998/osm-sources.json']]
    builder_paths += [path for path in (ROOT / 'boot/bridge').rglob('*') if path.is_file()]
    builder_sources = {str(path.relative_to(ROOT)): sha(path) for path in sorted(builder_paths)}
    output = args.workspace / 'boot'
    if output.exists():
        receipt = json.loads((args.workspace / 'release.json').read_text())
        if receipt['runtime_sha256'] != sha(args.runtime / 'XnuBridge.efi') or receipt['edk2_sha256'] != sha(args.edk2):
            raise ValueError('release inputs changed; select a new workspace')
        if receipt.get('runtime_builder_sources') != builder_sources:
            raise ValueError('runtime builder changed; preserve the old boot output before packaging again')
        for name, record in receipt['files'].items():
            if sha(args.workspace / name) != record['sha256']:
                raise ValueError('release artifact changed: ' + name)
        print('Existing matched release verified', flush=True)
        return
    with tempfile.TemporaryDirectory(prefix='.boot-', dir=args.workspace) as temporary:
        stage = Path(temporary) / 'boot'
        build_runtime(args.runtime.resolve(), args.workspace, stage)
        from project.execution import invoke
        invoke('project.build.boot_image', [ '--edk2', args.edk2.resolve(),
            '--bridge', stage / 'XnuBridge.efi', '--output', stage / 'boot.img'])
        for path in stage.glob('*.json'):
            path.write_text(path.read_text().replace(str(stage), str(output)))
        stage.rename(output)
    bundle = load_bundle(args.workspace)
    write_json(args.workspace / 'release.json', {
        'schema': 1, 'recipe_sha256': bundle['recipe_sha256'],
        'runtime_sha256': sha(args.runtime / 'XnuBridge.efi'), 'edk2_sha256': sha(args.edk2),
        'runtime_builder_sources': builder_sources,
        'files': {str(path.relative_to(args.workspace)): {'sha256': sha(path), 'bytes': path.stat().st_size}
                  for path in [args.workspace / 'image/ios.apfs', args.workspace / 'image/ios.apfs.json',
                               output / 'XnuBridge.efi', output / 'boot.img']},
        'post_flash_filesystem_patches_required': False,
        'device_written': False, 'hardware_tested': False})
    print('Matched APFS and automatic boot images: ' + str(args.workspace / 'release.json'), flush=True)


def release(args) -> None:
    """Produce the full APFS image and its automatic boot image as a matched pair."""
    check_release_inputs(args)
    prepare(args)
    build_image(args.workspace)
    package_release(args)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    plan = sub.add_parser('plan', help='show the pinned patch recipe without creating images')
    plan.add_argument('--profile', type=Path, required=True)
    for name in ('prepare', 'build', 'release'):
        command = sub.add_parser(name, help='prepare inputs' if name == 'prepare' else 'prepare and build the complete flashable APFS image')
        inputs = command.add_mutually_exclusive_group(required=True)
        inputs.add_argument('--ipsw', type=Path)
        inputs.add_argument('--firmware-report', type=Path, help='reuse checked, original decrypted inputs')
        command.add_argument('--profile', type=Path, required=True)
        command.add_argument('--workspace', type=Path, required=True)
        if name == 'release':
            command.add_argument('--runtime', type=Path, required=True, help='explicit checked bridge build directory (build.json, payload.S and payload artifacts)')
            command.add_argument('--edk2', type=Path, required=True, help='plain sagit EDK2 boot image')
    for name in ('image', 'verify'):
        command = sub.add_parser(name)
        command.add_argument('--workspace', type=Path, required=True)
    command = sub.add_parser('package', help='package an existing checked filesystem with a matching runtime')
    command.add_argument('--workspace', type=Path, required=True)
    command.add_argument('--runtime', type=Path, required=True)
    command.add_argument('--edk2', type=Path, required=True)
    command = sub.add_parser('bridge', help='build a bridge bound to this exact filesystem and all its signatures')
    command.add_argument('--workspace', type=Path, required=True)
    command.add_argument('--kernel', type=Path, required=True)
    command.add_argument('--output', type=Path)
    command.add_argument('bridge_options', nargs=argparse.REMAINDER, help='additional graphics/hardware options after --')
    args = parser.parse_args()
    if args.command == 'plan':
        print(json.dumps(read_profile(args.profile), ensure_ascii=False, indent=2))
        return
    args.workspace = args.workspace.resolve()
    with workspace_lock(args.workspace):
        if args.command == 'release':
            release(args)
        elif args.command == 'package':
            package_release(args)
        elif args.command in ('prepare', 'build'):
            prepare(args)
            if args.command == 'build':
                build_image(args.workspace)
        elif args.command == 'image':
            build_image(args.workspace)
        elif args.command == 'verify':
            verify_image(args.workspace)
        else:
            bridge(args)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print('firmware: ' + str(error), file=sys.stderr)
        sys.exit(1)
