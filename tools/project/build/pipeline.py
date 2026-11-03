"""Compose existing component builds using the selected board and firmware."""
import argparse
import json
from pathlib import Path
from zipfile import ZipFile
import shutil

from project import ROOT, config
from project.execution import external, invoke, stage
from project.build import context


def firmware_command(selected: dict, action: str, options: list[str]) -> None:
    directory = Path(selected['workspace'])
    profile = directory / 'obj/filesystem-profile.json'
    profile.parent.mkdir(parents=True, exist_ok=True)
    profile.write_text(json.dumps(config.filesystem_profile(selected), indent=2) + '\n')
    args = [action, '--workspace', str(directory / 'obj/filesystem')]
    if action in ('prepare', 'build', 'release'):
        args += ['--profile', str(profile)]
    invoke('project.firmware.image', args + options)
    if action in ('package', 'release'):
        publish_release(selected)


def complete(selected: dict, jobs: int, options: list[str]) -> None:
    parser = argparse.ArgumentParser(description=__doc__, epilog='Input: original IPSW and board description. Output: paired APFS and boot images in the selected output directory. This source pipeline still needs hardware validation.')
    parser.add_argument('--ipsw', type=Path, required=True)
    parser.add_argument('--edk2', type=Path, help='reuse an explicitly supplied board EDK2 boot image')
    parser.add_argument('--bridge-options', nargs=argparse.REMAINDER, default=[])
    args = parser.parse_args(options)
    if selected['profile'] != 'desktop':
        raise ValueError('complete system images require a board desktop profile')
    if not selected['board_config'].get('edk2_manifest') and args.edk2 is None:
        raise ValueError('this board has no EDK2 source recipe; supply --edk2')
    from project.firmware import apfs
    if apfs.sha(args.ipsw) != selected['firmware_config']['identity']['ipsw_sha256']:
        raise ValueError('IPSW differs from the selected firmware identity')
    from project.build import native, kernel, drivers, gpu
    directory = Path(selected['workspace'])
    with stage(selected, 'toolchain'):
        from project.build.references import prepare_platform
        prepare_platform(Path(selected['platform_dir']))
        native.bootstrap(jobs)
        apfs.bootstrap()
    with stage(selected, 'firmware-inputs'):
        from project.build.board_inputs import prepare as prepare_board_inputs
        prepare_board_inputs(selected)
        provider = context.PROVIDER_IMAGE
        provider.parent.mkdir(parents=True, exist_ok=True)
        name = selected['firmware_config']['kernelcache']
        raw = provider.parent / name
        with ZipFile(args.ipsw) as archive, archive.open(name) as source, raw.open('wb') as target:
            shutil.copyfileobj(source, target)
        decoded = provider.parent / 'decoded'
        external([apfs.IPSW, 'kernel', 'dec', raw, '-o', decoded])
        result = decoded / (name + '.decompressed')
        if provider.is_dir():
            provider.rename(provider.with_suffix(provider.suffix + '.previous-directory'))
        shutil.copy2(result, provider)
        context.NATIVE_KERNELCACHE = provider
        apfs.prepare(args.ipsw, selected['firmware_config']['identity']['board'], directory / 'obj/filesystem/inputs')
    with stage(selected, 'kernel'):
        kernel.build_kernel(jobs)
    with stage(selected, 'drivers'):
        drivers.pack_drivers(jobs, selected['profile_config']['collection'])
    if 'gpu' in selected['profile_config']['features']:
        with stage(selected, 'gpu'):
            gpu.build_gpu_shaders()
            gpu.build_gpu_metal()
            gpu.build_gpu_client()
    if 'battery' in selected['profile_config']['features']:
        with stage(selected, 'battery-client'):
            from project.build.clients import build_battery_client
            build_battery_client()
    with stage(selected, 'image'):
        firmware_command(selected, 'build', ['--ipsw', str(args.ipsw.resolve())])
    with stage(selected, 'bridge'):
        collection = drivers.driver_configuration()['collections'][context.COLLECTION]
        firmware_command(selected, 'bridge', ['--kernel', collection['output'], '--',
            '--dts', selected['device_tree'], '--cpus', str(selected['profile_config']['cpus']), *args.bridge_options])
    edk2 = args.edk2
    if edk2 is None:
        with stage(selected, 'edk2'):
            external(['bash', Path(selected['board_dir']) / selected['board_config']['edk2_builder']], cwd=ROOT)
        edk2 = directory / 'obj/edk2' / selected['board_config']['edk2_boot_image']
    with stage(selected, 'package'):
        firmware_command(selected, 'package', ['--runtime', str(directory / 'obj/filesystem/bridge'), '--edk2', str(edk2)])


def publish_release(selected: dict) -> None:
    from project.firmware import apfs
    directory = Path(selected['workspace'])
    output = directory / 'output'
    filesystem = directory / 'obj/filesystem'
    receipt = json.loads((filesystem / 'release.json').read_text())
    files = {}
    for relative, record in receipt['files'].items():
        source = filesystem / relative
        target = output / source.name
        external(['cp', '--reflink=auto', '--', source, target])
        if apfs.sha(target) != record['sha256'] or target.stat().st_size != record['bytes']:
            raise ValueError('published release differs from verified input: ' + relative)
        files[target.name] = record
    receipt['files'] = files
    (output / 'release.json').write_text(json.dumps(receipt, indent=2) + '\n')
