"""Bind a checked runtime payload to a freshly built filesystem.

Kernel, Drivers and GPU firmware come from an explicitly supplied bridge build.
Only filesystem selection and trust caches change. No historical build path is
used to locate an input, and no commands from an input report are executed.
"""
from __future__ import annotations
from project import ROOT

import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from project.firmware.filesystems import parse_apple_dt
from project.firmware.trustcache import handoff, module_info
from project.build.nvram import replace_boot_args
from project.build.boot_format import check_efi

PAYLOADS = {'kernel', 'dt', 'trust_cache', 'gpu_zap', 'gpu_firmware', 'driver_image',
            'usb_npa_pin', 'usb_config_pin', 'usb_glink_pin'}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def split_trust_cache(data: bytes) -> list[bytes]:
    if len(data) < 8:
        raise ValueError('truncated trust-cache handoff')
    count = struct.unpack_from('<I', data)[0]
    if not 1 <= count <= 64 or 4 + count * 4 > len(data):
        raise ValueError('invalid trust-cache handoff count')
    offsets = list(struct.unpack_from('<' + 'I' * count, data, 4)) + [len(data)]
    if offsets[0] != 4 + count * 4 or any(a >= b for a, b in zip(offsets, offsets[1:])):
        raise ValueError('invalid trust-cache handoff offsets')
    modules = [data[a:b] for a, b in zip(offsets, offsets[1:])]
    for module in modules:
        module_info(module)
    return modules


def encode_dt(node: dict) -> bytes:
    result = bytearray(struct.pack('<II', len(node['props']), len(node['children'])))
    for name, value in node['props'].items():
        encoded = name.encode('ascii')
        if len(encoded) > 31:
            raise ValueError('oversized Apple DT property name')
        result += encoded.ljust(32, b'\0') + struct.pack('<I', len(value)) + value
        result += bytes(-len(value) % 4)
    for child in node['children']:
        result += encode_dt(child)
    return bytes(result)


def bind_dt(data: bytes, image: dict, boot_args: list[str]) -> tuple[bytes, dict]:
    from project.build import bridge
    tree = parse_apple_dt(data)
    if encode_dt(tree) != data:
        raise ValueError('template DT is not losslessly round-trippable')
    policy = bridge.configure_apfs_root(tree, image)
    chosen = next(node for node in tree['children'] if node['name'] == 'chosen')
    replacements = {value.split('=', 1)[0] for value in boot_args}
    retained = [value for value in chosen['props']['bootargs'].rstrip(b'\0').decode('ascii').split()
                if value.split('=', 1)[0] not in replacements]
    chosen['props']['bootargs'] = ' '.join(retained + boot_args).encode('ascii') + b'\0'
    if 'nvram-proxy-data' in chosen['props']:
        chosen['props']['nvram-proxy-data'] = replace_boot_args(
            chosen['props']['nvram-proxy-data'], chosen['props']['bootargs'].rstrip(b'\0'))
    return encode_dt(tree), policy


def load_runtime(directory: Path) -> tuple[dict, list[tuple[str, str, bytes]]]:
    report = json.loads((directory / 'build.json').read_text())
    efi = (directory / 'XnuBridge.efi').read_bytes()
    check_efi(efi)
    if digest(efi) != report['artifacts']['XnuBridge.efi']['sha256']:
        raise ValueError('runtime EFI differs from its build report')
    assembly = (directory / 'payload.S').read_text()
    matches = re.findall(r'^([a-z_]+)_blob:\n\.incbin "([^"\n]+)"$', assembly, re.M)
    names = [name for name, _ in matches]
    if len(names) != len(set(names)) or not {'kernel', 'dt', 'trust_cache'} <= set(names) or set(names) - PAYLOADS:
        raise ValueError('unsupported, duplicate or incomplete runtime payloads')
    if ('driver_image' in names) != bool(report.get('driver_image')):
        raise ValueError('runtime driver volume differs from its build report')
    if report.get('baseband_handoff'):
        raise ValueError('runtime with a separate baseband payload requires the source bridge builder')
    payloads = []
    for name, source in matches:
        # Reports from another checkout often have obsolete absolute paths.
        # Only local sibling artifacts, with checked contents, are inputs.
        filename = Path(source).name
        data = (directory / filename).read_bytes()
        record = report['artifacts'].get(filename)
        expected = report['kernel_sha256'] if name == 'kernel' else record['sha256']
        if digest(data) != expected or efi.count(data) != 1:
            raise ValueError('runtime payload identity mismatch: ' + name)
        if record and len(data) != record['bytes']:
            raise ValueError('runtime payload length mismatch: ' + name)
        payloads.append((name, filename, data))
    return report, payloads


def build_runtime(template: Path, workspace: Path, output: Path) -> dict:
    from project.firmware.image import load_bundle, run, sha, write_json
    bundle = load_bundle(workspace)
    image = json.loads((workspace / 'image/ios.apfs.json').read_text())
    report, payloads = load_runtime(template)
    if not all(report.get(key) for key in ('development_unsealed_root', 'development_no_sep', 'development_legacy_data', 'development_hactivation')):
        raise ValueError('runtime must provide the tested no-SEP/legacy Data/Setup development policies')
    output.mkdir(parents=True, exist_ok=False)
    modules = split_trust_cache(next(data for name, _, data in payloads if name == 'trust_cache'))
    modules += [path.read_bytes() for path in sorted((workspace / 'payload/trust-cache').glob('*.bin'))]
    unique = {digest(module): module for module in modules}
    trust = handoff(list(unique.values()))
    report['trust_cache'] = {'modules': [module_info(module) for module in unique.values()]}
    assembly = '.section .rdata,"dr"\n.p2align 4\n'
    artifacts, retained = {}, {}
    for name, filename, data in payloads:
        if name == 'dt':
            data, report['apfs_root'] = bind_dt(data, image, bundle['extra_boot_args'])
            tree = parse_apple_dt(data)
            chosen = next(node for node in tree['children'] if node['name'] == 'chosen')
            report['effective_boot_args'] = chosen['props']['bootargs'].rstrip(b'\0').decode('ascii')
            if 'nvram-proxy-data' in chosen['props']:
                report['nvram'] = {'bytes': len(chosen['props']['nvram-proxy-data']),
                    'proxy_sha256': digest(chosen['props']['nvram-proxy-data']),
                    'boot_args_match_chosen': True, 'persistent_storage_written': False}
        elif name == 'trust_cache':
            data = trust
        else:
            retained[name] = digest(data)
        if name == 'kernel':
            report['kernel'] = filename
        elif name == 'driver_image':
            report['driver_image'] = dict(report['driver_image'], source=filename)
        target = output / filename
        if any(character in str(target) for character in '\n"\\'):
            raise ValueError('unsupported assembly output path')
        target.write_bytes(data)
        artifacts[filename] = {'sha256': digest(data), 'bytes': len(data)}
        assembly += f'.globl {name}_blob\n{name}_blob:\n.incbin "{target}"\n.globl {name}_end\n{name}_end:\n.p2align 4\n'
    (output / 'payload.S').write_text(assembly)
    include = ROOT / 'boot/edk2-msm8998/Common/edk2/MdePkg/Include'
    if not include.exists():
        raise ValueError('EDK2 headers are missing; run make edk2-sagit')
    flags = ['--target=aarch64-unknown-windows', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-fno-asynchronous-unwind-tables', '-mstrict-align',
             '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror',
             '-I' + str(ROOT / 'boot/bridge'), '-I' + str(include), '-I' + str(include / 'AArch64'),
             '-I' + str(ROOT / 'drivers/include'),
             '-DBRIDGE_WATCHDOG_SECONDS=' + str(report['watchdog_seconds']),
             '-DBRIDGE_MIN_PHYS=' + str(report.get('arena_min_requested', 0xa0000000)) + 'ULL',
             '-DBRIDGE_ARENA_SIZE=' + str(report['arena_bytes_requested']) + 'ULL']
    sources = ['core/core.c', 'platform/platform.c', 'entry/uefi.c', 'entry/enter.S']
    if report.get('boot_menu', {}).get('enabled'):
        flags += ['-DBRIDGE_BOOT_MENU=1', '-DBRIDGE_BOOT_MENU_DELAY=' + str(report['boot_menu']['delay_seconds'])]
        sources += ['menu/boot_menu.c', 'menu/menu_ui.c']
    if report.get('fastboot_service', {}).get('enabled'):
        flags.append('-DBRIDGE_FASTBOOT_SERVICE=1')
        sources.append('services/fastboot.c')
    features = [
        ('FULL_MEMORY', report['full_memory_requested'], None),
        ('DRIVER_IMAGE', report['driver_image'], None),
        ('UFS_HANDOFF', report['ufs_handoff'], 'handoff/ufs_handoff.c'),
        ('SMEM_HANDOFF', report.get('smem_handoff'), 'handoff/smem_handoff.c'),
        ('USB_TOPOLOGY', report.get('usb_topology'), 'handoff/usb_topology.cpp'),
        ('DISPLAY_PROBE', report.get('display_probe'), None),
        ('GPU_PROBE', report.get('gpu_probe'), None),
        ('DISPLAY_HANDOFF', report.get('display_handoff'), None),
        ('TOUCH_PREPARE', report.get('touch_handoff'), 'handoff/touch_handoff.cpp'),
        ('CPU_OSM_HANDOFF', report.get('cpu_osm_handoff'), 'handoff/osm_handoff.cpp'),
        ('DISPLAY_SMMU_HANDOFF', (report.get('display_handoff') or {}).get('display_smmu', {}).get('requested'), 'handoff/display_smmu.cpp'),
        ('GPU_HANDOFF', report.get('gpu_handoff'), 'handoff/gpu_handoff.cpp'),
        ('GPU_ZAP', 'gpu_zap' in retained, None),
        ('GPU_FIRMWARE', 'gpu_firmware' in retained, None),
        ('GPU_SMMU_INIT', report.get('gpu_smmu_init'), None),
        ('GPU_COLD_START', report.get('gpu_cold_start'), None),
        ('WATCHDOG_TEST', report.get('watchdog_test'), None),
        ('USB_RPM_SNAPSHOT', 'usb_npa_pin' in retained, 'handoff/usb_rpm.cpp'),
        ('USB_FIRMWARE_STOP', 'usb_config_pin' in retained, 'handoff/usb_firmware_stop.cpp'),
        ('RPM_COLD_HANDOFF', 'usb_glink_pin' in retained, 'handoff/usb_rpm.cpp'),
        ('USB_HARDWARE_STOP', (report.get('usb_topology') or {}).get('hardware_stop', {}).get('enabled'), 'handoff/usb_hardware_stop.cpp'),
    ]
    if report.get('cpu_osm_handoff'):
        flags += ['-I' + str(ROOT / 'drivers/include')]
    if report.get('display_probe') or report.get('display_handoff'):
        sources.append('handoff/display_handoff.c')
    for name, enabled, source in features:
        if enabled:
            flags.append('-DBRIDGE_' + name + '=1')
            if source and source not in sources:
                sources.append(source)
    objects, commands, source_hashes = [], [], {}
    for source in [*[ROOT / 'boot/bridge' / name for name in sources], output / 'payload.S']:
        extra = ['-std=c++17', '-fno-rtti', '-fno-exceptions', '-fno-threadsafe-statics', '-fno-use-cxa-atexit'] if source.suffix == '.cpp' else []
        obj = output / (source.stem + '.obj')
        command = ['clang', *flags, *extra, '-c', str(source), '-o', str(obj)]
        commands.append(command)
        source_hashes[str(source.relative_to(ROOT)) if source.is_relative_to(ROOT / 'boot') else source.name] = sha(source)
        run(*command)
        objects.append(obj)
    efi = output / 'XnuBridge.efi'
    command = ['lld-link', '/machine:arm64', '/subsystem:efi_application', '/entry:efi_main',
               '/nodefaultlib', '/base:0', '/filealign:512', '/out:' + str(efi), *map(str, objects)]
    commands.append(command)
    run(*command)
    result = efi.read_bytes()
    check_efi(result)
    for name, filename, _ in payloads:
        data = (output / filename).read_bytes()
        if result.count(data) != 1:
            raise ValueError('rebuilt EFI payload readback failed: ' + name)
    artifacts[efi.name] = {'sha256': digest(result), 'bytes': len(result)}
    report.pop('hardware_acceptance', None)
    report.pop('frozen_baseline', None)
    report.update(artifacts=artifacts, hardware_tested=False,
        filesystem_recipe_sha256=bundle['recipe_sha256'],
        runtime_template_sha256=sha(template / 'XnuBridge.efi'),
        retained_runtime_payloads=retained,
        loader_build={'sources': source_hashes, 'commands': commands})
    write_json(output / 'build.json', report)
    return report
