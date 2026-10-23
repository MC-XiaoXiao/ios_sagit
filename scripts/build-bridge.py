#!/usr/bin/env python3
"""Build standard FDT, explicit XNU DT conversion and a self-contained ARM64 EFI."""
import argparse
import copy
import importlib.util
import hashlib
import gzip
import json
import re
from pathlib import Path
import secrets
import struct
import subprocess
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bridge_nvram import chosen_properties, with_boot_args
from bridge_trustcache import firmware_static, raw_module, module_info, handoff, development_module
from bridge_filesystems import configure_firmware_filesystems
from bridge_amfi import prepare_no_apple_pmgr
from bridge_watchdog_policy import prepare_watchdog_service_policy
from bridge_io_policy import prepare_unthrottled_io
from input_firmware import configure_input_firmware
from touch_handoff import configure_touch_handoff
from osm_handoff import configure_osm_handoff
from smem_handoff import configure_smem_handoff
from baseband_handoff import configure_baseband_handoff, configure_rmtfs_storage_policy
from toolchain import configure_bluetooth_firmware

ROOT = Path(__file__).resolve().parents[1]


def configure_usb_topology(tree, component, rpm_snapshot=False, firmware_stop=False, hardware_stop=False):
    """Reserve an EFI-populated IORT record without enabling DMA or USB."""
    if not isinstance(component, str) or not re.fullmatch(r'\\[A-Z0-9_.]{1,63}', component):
        raise ValueError('USB topology requires an absolute ACPI component name')
    if b'qcom,msm8998' not in tree['props'].get('compatible', b'').split(b'\0'):
        raise ValueError('USB topology capture currently requires an MSM8998 tree')
    chosen = next(n for n in tree['children'] if n['name'] == 'chosen')['props']
    chosen['usb-iort-component'] = component.encode('ascii') + b'\0'
    chosen['qcom,usb-firmware-iort'] = bytes(65536)
    chosen['qcom,usb-firmware-controller'] = bytes(40)
    firmware_stop = bool(firmware_stop or hardware_stop)
    hardware_stop = bool(hardware_stop or firmware_stop)
    rpm_snapshot = bool(rpm_snapshot or firmware_stop)
    if rpm_snapshot:
        chosen['qcom,usb-firmware-rpm'] = bytes(20544)
    if firmware_stop:
        chosen['qcom,usb-firmware-stop'] = bytes(64)
    if hardware_stop:
        chosen['qcom,usb-hardware-stop'] = bytes(64)
    return {'component': component, 'record_bytes': 65536, 'efi_only': True,
            'source': 'checksummed installed ACPI IORT; rechecked across ExitBootServices',
            'controller_binding': 'installed FADT/DSDT and SDT namespace, matched to EFI USB Config primary-core base; 40-byte resource record, no DMA grant',
            'firmware_usb_binding': {'revision': '0x10003', 'core_type': 5,
                                     'maximum_handles': 64, 'queried_after_efi_exit': False,
                                     'pinned_owner_before_any_config_query': bool(firmware_stop)},
            'dma_ownership_granted': False, 'hardware_verified': False,
            'hardware_stop': {'enabled': bool(hardware_stop), 'bytes': 64 if hardware_stop else 0,
                              'sequence': 'post-EFI actual inherited Host/Device halt, then dedicated USB BCR; recheck before XNU entry',
                              'resource_or_DMA_mapping_grants': False, 'build_executes_hardware': False},
            'firmware_stop': {'enabled': bool(firmware_stop), 'bytes': 64 if firmware_stop else 0,
                              'events': ['poll', 'configuration', 'mode toggle', 'EFI exit'],
                              'post_exit_state_rechecked': True, 'hardware_halt_proven': False,
                              'resource_ownership_granted': False},
            'rpm_snapshot': {'enabled': bool(rpm_snapshot), 'bytes': 20544 if rpm_snapshot else 0,
                             'resource_ownership_granted': False, 'ACK_success_inferred': False}}


def pack_firmware_pin(source, output, expected, image_size, code_rva, code_bytes, protocol_rva, protocol_bytes):
    """Pin complete code and immutable protocol bytes before private RVA use."""
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError('unsupported firmware image for EFI USB handoff')
    pe, = struct.unpack_from('<I', data, 0x3c)
    machine, sections = struct.unpack_from('<HH', data, pe+4)
    optional_size, = struct.unpack_from('<H', data, pe+20)
    optional = pe+24
    magic, = struct.unpack_from('<H', data, optional)
    base, = struct.unpack_from('<Q', data, optional+24)
    image_bytes, = struct.unpack_from('<I', data, optional+56)
    if data[pe:pe+4] != b'PE\0\0' or machine != 0xaa64 or magic != 0x20b or base or image_bytes != image_size:
        raise ValueError('unsupported pinned firmware PE layout')
    ranges = []
    for i in range(sections):
        name, virtual_size, rva, size, raw = struct.unpack_from('<8sIIII', data, optional+optional_size+40*i)
        if not virtual_size or rva+virtual_size > image_bytes or raw+size > len(data):
            raise ValueError('invalid pinned firmware section')
        ranges.append((name.rstrip(b'\0'), rva, virtual_size, raw, size))
    def read(rva, size):
        for _, start, virtual_size, raw, raw_size in ranges:
            if start <= rva and rva+size <= start+min(virtual_size, raw_size):
                return data[raw+rva-start:raw+rva-start+size]
        raise ValueError('unbacked pinned firmware range')
    code = [r for r in ranges if r[0] == b'.text']
    if len(code) != 1 or code[0][1:3] != (code_rva, code_bytes):
        raise ValueError('unsupported pinned firmware code section')
    reloc_rva, reloc_bytes = struct.unpack_from('<II', data, optional+112+5*8)
    reloc_data = read(reloc_rva, reloc_bytes)
    relocations = []; cursor = 0
    while cursor < len(reloc_data):
        if not any(reloc_data[cursor:]): break
        page, size = struct.unpack_from('<II', reloc_data, cursor)
        if size < 8 or size % 2 or cursor+size > len(reloc_data):
            raise ValueError('invalid pinned firmware relocation block')
        for pos in range(cursor+8, cursor+size, 2):
            entry, = struct.unpack_from('<H', reloc_data, pos)
            if not entry >> 12: continue
            if entry >> 12 != 10:
                raise ValueError('unsupported pinned firmware relocation kind')
            location = page+(entry & 4095)
            if any(start <= location and location+8 <= start+length for start, length in
                   ((code_rva, code_bytes), (protocol_rva, protocol_bytes))):
                value, = struct.unpack('<Q', read(location, 8))
                if location % 8 or value > image_bytes:
                    raise ValueError('invalid pinned firmware code relocation')
                relocations.append(location)
        cursor += size
    relocations.sort()
    if any(b < a+8 for a, b in zip(relocations, relocations[1:])):
        raise ValueError('overlapping pinned firmware relocations')
    pin = struct.pack('<10I', 0x31495046, 1, image_bytes, code_rva, code_bytes,
                      protocol_rva, protocol_bytes, len(relocations), 0, 0)
    pin += b''.join(struct.pack('<I', r) for r in relocations)
    pin += read(code_rva, code_bytes)+read(protocol_rva, protocol_bytes)
    output.write_bytes(pin)
    return {'source': str(source), 'source_sha256': expected,
            'pin_sha256': hashlib.sha256(pin).hexdigest(), 'pin_bytes': len(pin),
            'relocation_count': len(relocations)}


def pack_usb_npa_pin(source, output):
    report = pack_firmware_pin(source, output,
        'c5b1549fc0430671bece73c5d4f0f2e061aa605f7c8b6b51dbdfe47e94c73ea5',
        0x1b000, 0x1000, 0x17000, 0x180f0, 0x1b0)
    report.update(private_layout_revision='0x10002', firmware_callbacks_invoked=False, resource_ownership_granted=False)
    return report


def pack_usb_config_pin(source, output):
    report = pack_firmware_pin(source, output,
        'c81450d139adbb7bc1744a2dc21388e35dac7876913224d04b1c298c4f386d6f',
        0x13000, 0x1000, 0x10000, 0x11140, 160)
    report.update(private_layout_revision='0x10003', build_executes_firmware=False, resource_ownership_granted=False)
    return report


def pack_usb_glink_pin(source, output):
    report = pack_firmware_pin(source, output,
        '7937a8bba87276ff7a14ef2faa4f0ac9486552c708ac4639ff54e1cffe8c24ba',
        0xc000, 0x1000, 0x8000, 0x9090, 0x78)
    report.update(private_layout_revision='0x10001', build_executes_firmware=False)
    return report


def configure_usb_rpm_cold(tree, bridge_consumer=False):
    soc = next(n for n in tree['children'] if n['name']=='soc' or n['name'].startswith('soc@'))
    rpm = next(n for n in soc['children'] if n['name']=='power-controller@778000')['props']
    if bridge_consumer:
        if rpm.get('qcom,rpm-handoff-version') != struct.pack('>I',2) or len(rpm.get('qcom,rpm-transport-handoff',b''))!=48:
            raise ValueError('RPM cold fence for bridge requires its existing live-channel handoff')
    else:
        if 'qcom,rpm-transport-handoff' in rpm:
            raise ValueError('RPM cold and live channel ownership must have one consumer')
        rpm['qcom,rpm-handoff-version'] = struct.pack('>I',3)
    rpm['qcom,rpm-transport-cold-handoff'] = bytes(40)
    return {'record_bytes':40,'consumer':'bridge GPU startup, then Native live-channel adoption' if bridge_consumer else 'Native QcomRPM cold initialization',
            'firmware_GLINK_revision':'0x10001','reset_fence':'all four actual RPM FIFO cursors zero after EFI exit',
            'resource_or_DMA_grants':False,'hardware_verified':False}


def patch_no_sep_keystore_gate(code):
    """Check native aks-endpoint=0 before a potentially blocking SEP lookup."""
    if len(code) != 0x11c or code[:24] != bytes.fromhex(
            'ff4302d1f44f07a9fd7b08a9fd030291f40300aabfc31eb8'):
        raise ValueError('unsupported AppleKeyStore SEP gate prologue')
    call, zero, load, branch = struct.unpack_from('<4I', code, 0x18)
    if (call & 0xfc000000 != 0x94000000 or zero != 0x52800013 or
            load != 0xb941da88 or branch & 0xff00001f != 0x35000008):
        raise ValueError('unsupported AppleKeyStore SEP gate instructions')
    delta = (branch >> 5) & 0x7ffff
    if delta & 0x40000: delta -= 0x80000
    if 0x24 + 4 * delta != 0x88:
        raise ValueError('unsupported AppleKeyStore disabled-endpoint continuation')
    # Preserve W19=0 and both original branch destinations. Only the order
    # changes: MOV; LDR; CBNZ; BL. Ordinary SEP-enabled behavior is retained.
    result = bytearray(code)
    moved_branch = (branch & ~0xffffe0) | (((delta + 1) & 0x7ffff) << 5)
    moved_call = 0x94000000 | (((call & 0x3ffffff) - 3) & 0x3ffffff)
    struct.pack_into('<4I', result, 0x18, zero, load, moved_branch, moved_call)
    return bytes(result)


def prepare_no_sep_keystore(source, destination):
    spec = importlib.util.spec_from_file_location('keystore_verifier',
        Path(__file__).with_name('verify-kernelcache.py'))
    verifier = importlib.util.module_from_spec(spec); spec.loader.exec_module(verifier)
    data = source.read_bytes(); outer = verifier.Image(data)
    _, kexts = verifier.inventory.inspect(source)
    selected = [k for k in kexts if k['CFBundleIdentifier'] == 'com.apple.driver.AppleSEPKeyStore']
    if not selected:
        return None
    if len(selected) != 1:
        raise ValueError('expected one native AppleSEPKeyStore module')
    image = verifier.Image(data, outer.offset(selected[0]['_PrelinkExecutableLoadAddr'], 32), outer=outer)
    texts = [s for s in image.sections if s['name'] == '__text']
    if len(texts) != 1 or texts[0]['size'] != 219076:
        raise ValueError('unsupported AppleSEPKeyStore executable layout')
    address = texts[0]['vm'] + 0x7c98
    offset = image.offset(address, 0x11c)
    code = data[offset:offset+0x11c]
    call, = struct.unpack_from('<I', code, 0x18)
    delta = call & 0x3ffffff
    if delta & 0x2000000: delta -= 0x4000000
    if address + 0x18 + delta * 4 != texts[0]['vm'] + 0x2b240:
        raise ValueError('AppleKeyStore SEP manager lookup target mismatch')
    replacement = patch_no_sep_keystore_gate(code)
    result = data[:offset] + replacement + data[offset+len(code):]
    destination.write_bytes(result)
    return {'policy':'development-no-sep-endpoint-before-manager-lookup',
            'source':str(source), 'source_sha256':hashlib.sha256(data).hexdigest(),
            'output_sha256':hashlib.sha256(result).hexdigest(),
            'instruction_address':hex(address+0x18), 'file_offset':offset+0x18,
            'before':code[0x18:0x28].hex(), 'after':replacement[0x18:0x28].hex(),
            'native_failure_results_preserved':True, 'sandbox_exec_bypassed':False}


def configure_no_sep(tree):
    """Use native APFS/AppleKeyStore no-SEP policy without changing Sandbox."""
    filesystems = next(n for n in tree['children'] if n['name'] == 'filesystems')
    chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
    args = chosen['props'].get('bootargs', b'').rstrip(b'\0').decode('ascii').split()
    # The audited iOS 18 AppleSEPKeyStore uses this argument to avoid SEP
    # endpoint initialization. It still publishes its original user client;
    # SEP-dependent operations retain their native failure results.
    if any(arg.startswith('aks-endpoint=') and arg != 'aks-endpoint=0' for arg in args):
        raise ValueError('no-SEP policy conflicts with aks-endpoint boot argument')
    args = [arg for arg in args if not arg.startswith('aks-endpoint=')]
    args.append('aks-endpoint=0')
    command_line = ' '.join(args).encode('ascii')
    if len(command_line) >= 1024:
        raise ValueError('no-SEP boot arguments exceed boot_args command line')
    # APFS checks the presence of this Apple DT property, not its value.
    filesystems['props']['DoNotUseSEP'] = b''
    chosen['props']['bootargs'] = command_line + b'\0'


def configure_serial_no_iolog(tree):
    """Keep UART input/output and native os_log, omit the IOLog UART copy."""
    chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
    args = chosen['props']['bootargs'].rstrip(b'\0').decode('ascii').split()
    modes = [int(arg.split('=', 1)[1], 0) for arg in args if arg.startswith('serial=')]
    if len(modes) != 1:
        raise ValueError('expected one native serial mode')
    mode = modes[0] | 0x10  # SERIALMODE_NO_IOLOG in console/serial_protos.h.
    args = [arg for arg in args if not arg.startswith('serial=')]
    args.append('serial=' + str(mode))
    chosen['props']['bootargs'] = ' '.join(args).encode('ascii') + b'\0'
    return mode


def configure_usb_cdc_console(tree, vid, pid):
    """Opt in to CDC and console buffering; this does not grant USB resources."""
    if (not isinstance(vid, int) or isinstance(vid, bool) or not 1 <= vid < 0xffff or
            not isinstance(pid, int) or isinstance(pid, bool) or not 1 <= pid <= 0xffff):
        raise ValueError('USB CDC requires explicit nonzero 16-bit VID and PID')
    chosen = next(n for n in tree['children'] if n['name'] == 'chosen')['props']
    args = chosen.get('bootargs', b'').rstrip(b'\0').decode('ascii').split()
    args = [a for a in args if not a.startswith(('qcom.usb-cdc=', 'qcom.usb-cdc-vid=', 'qcom.usb-cdc-pid='))]
    args += ['qcom.usb-cdc=1', f'qcom.usb-cdc-vid={vid}', f'qcom.usb-cdc-pid={pid}']
    # The native console requires serial output/input enabled even without UART.
    modes = [int(a.split('=', 1)[1], 0) for a in args if a.startswith('serial=')]
    if len(modes) > 1:
        raise ValueError('expected at most one native serial mode')
    mode = (modes[0] if modes else 0) | 3
    args = [a for a in args if not a.startswith('serial=')]
    args.append(f'serial={mode}')
    chosen['bootargs'] = (' '.join(args)).encode('ascii') + b'\0'
    chosen['xnu,allow-no-uart'] = struct.pack('>I', 1)
    return {'enabled': True, 'vid': vid, 'pid': pid, 'hardware_verified': False,
            'requires_usb_platform_resources': True, 'early_output': 'bounded RAM buffer'}


def disable_console_mirror(tree):
    """Keep serial output while omitting per-character framebuffer drawing."""
    chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
    args = chosen['props']['bootargs'].rstrip(b'\0').decode('ascii').split()
    args = [arg for arg in args if not arg.startswith('mirror-console=')]
    args.append('mirror-console=0')
    chosen['props']['bootargs'] = ' '.join(args).encode('ascii') + b'\0'


def configure_vfs_limits(tree, max_vnodes, disable_system_sleep=False):
    """Supply the native platform default missing from generic ARM trees."""
    if not 1024 <= max_vnodes <= 1048576:
        raise ValueError('max-vnodes must be between 1024 and 1048576')
    defaults = next((n for n in tree['children'] if n['name'] == 'defaults'), None)
    if defaults is None:
        defaults = {'name': 'defaults', 'props': {}, 'children': []}
        tree['children'].append(defaults)
    # PE_get_default copies host-endian bytes from the Apple device tree.
    # Without this property the embedded xsmall configuration caps at 1024.
    defaults['props']['kern.maxvnodes'] = struct.pack('<I', max_vnodes)
    if disable_system_sleep:
        # GenericARMCPU has no suspend backend. Let IOPMrootDomain reject
        # system sleep before it attempts to suspend the CPUs.
        defaults['props']['sleep-disabled'] = struct.pack('<I', 1)
    return {'property': '/defaults/kern.maxvnodes', 'value': max_vnodes,
            'system_sleep_disabled': disable_system_sleep}


def configure_cpu_count(tree, count):
    """Use XNU's native cpus= filter, with one unambiguous boot argument."""
    if not isinstance(count, int) or isinstance(count, bool) or not 1 <= count <= 8:
        raise ValueError('CPU count must be between 1 and 8')
    chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
    args = chosen['props'].get('bootargs', b'').rstrip(b'\0').decode('ascii').split()
    if any(arg.startswith('cpumask=') for arg in args):
        raise ValueError('CPU count cannot be combined with cpumask=')
    args = [arg for arg in args if not arg.startswith('cpus=')]
    args.append(f'cpus={count}')
    chosen['props']['bootargs'] = ' '.join(args).encode('ascii') + b'\0'


def configure_apfs_root(tree, report):
    """Select the System *volume* UUID, never the GPT/container UUID or BSD index."""
    system = str(uuid.UUID(report['system_uuid'])).upper()
    data = str(uuid.UUID(report['data_uuid'])).upper()
    if system == data:
        raise ValueError('System and Data UUIDs must differ')
    chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
    props = chosen['props']
    args = props.get('bootargs', b'').rstrip(b'\0').decode('ascii').split()
    args = [arg for arg in args if not arg.startswith(('rd=', 'rootdev=', 'boot-uuid='))]
    args += ['rd=uuid', 'boot-uuid='+system]
    if len(' '.join(args).encode()) >= 1024:
        raise ValueError('APFS boot arguments exceed boot_args command line')
    props['bootargs'] = ' '.join(args).encode()+b'\0'
    # iOS APFS publishes IOMedia volumes directly. The macOS
    # AppleFileSystemDriver which resolves boot-uuid-media is absent from
    # this provider, so match the actual System volume by its UUID.
    props['root-matching'] = (
        '<dict><key>IOProviderClass</key><string>IOMedia</string>'
        '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' + system +
        '</string></dict></dict>').encode('ascii') + b'\0'
    props['boot-uuid'] = system.encode()+b'\0'
    return {'system_uuid': system, 'data_uuid': data, 'hardware_boot_verified': False}


def configure_development_hactivation(tree):
    """Select the reference port's native Hactivation branch for Sagit only."""
    compatible = tree.get('props', {}).get('compatible', b'').split(b'\0')
    if (b'xiaomi,sagit' not in compatible or b'qcom,msm8998' not in compatible or
            tree.get('props', {}).get('model') != b'iPhoneMi6,1\0'):
        raise ValueError('development Hactivation is restricted to the iPhoneMi6,1 Sagit port')
    chosen = next((node for node in tree['children'] if node['name'] == 'chosen'), None)
    if chosen is None:
        raise ValueError('development Hactivation requires /chosen')
    expected = {
        'certificate-production-status': 0,
        'effective-production-status-ap': 0,
        'certificate-security-mode': 1,
        'effective-security-mode-sep': 1,
    }
    for name, value in expected.items():
        existing = chosen['props'].get(name)
        if existing is not None and (len(existing) != 4 or struct.unpack('>I', existing)[0] != value):
            raise ValueError(f'conflicting /chosen/{name} value')
        chosen['props'][name] = struct.pack('>I', value)
    return {'requested': True, 'path': '/chosen', 'properties': expected,
            'source': 'ios-sweet development-fused status tuple', 'hardware_boot_verified': False}


def configure_display_handoff(tree, path, smmu_handoff=False, disable_backlight=False):
    profile = json.loads(path.read_text())
    if profile.get('version') != 1 or profile.get('compatible') != 'qcom,msm8998':
        raise ValueError('unsupported display handoff profile')
    width, height, rate = (profile[k] for k in ('physical_width_mm', 'physical_height_mm', 'refresh_hz'))
    if not all(type(v) is int for v in (width, height, rate)) or not (20 <= width <= 1000 and 20 <= height <= 1000 and 24 <= rate <= 120):
        raise ValueError('invalid panel dimensions or refresh rate')
    identifier = str(uuid.UUID(profile['display_uuid'])).upper()
    if b'qcom,msm8998' not in tree['props'].get('compatible', b'').split(b'\0'):
        raise ValueError('display handoff requires an MSM8998 device tree')
    nodes = []
    def visit(node):
        if b'qcom,msm8998-dpu' in node['props'].get('compatible', b'').split(b'\0'): nodes.append(node)
        for child in node['children']: visit(child)
    visit(tree)
    if len(nodes) != 1: raise ValueError('display handoff requires exactly one DPU node')
    scale = profile.get('display_scale', 1)
    rotation = profile.get('display_rotation', 0)
    if type(scale) is not int or not 1 <= scale <= 3:
        raise ValueError('display_scale must be an integer from 1 to 3')
    if type(rotation) is not int or rotation not in (0, 90, 180, 270):
        raise ValueError('display_rotation must be a clockwise quarter-turn in degrees')
    chosen = next(n for n in tree['children'] if n['name'] == 'chosen')['props']
    # iOS 18 MGIOMFBSupport reads these Apple boot properties. Missing scale
    # defaults to zero, which cannot describe a valid FBSDisplayMode UI scale.
    chosen['display-scale'] = struct.pack('>I', scale)
    chosen['display-rotation'] = struct.pack('>I', rotation)
    props = nodes[0]['props']
    if smmu_handoff:
        if props.get('reg') != struct.pack('>4I', 0, 0x0c901000, 0, 0x8f000) or props.get('interrupts') != struct.pack('>3I', 0, 83, 4):
            raise ValueError('display SMMU handoff requires the native DPU resource/interrupt layout')
        if 'iommu-parent' in props or 'iommus' in props:
            raise ValueError('display SMMU handoff cannot replace an existing mapper binding')
        irqs = (263,266,267,268,244,245,247,248,249,250,251,252,253,254,255,256,260,261,262,272)
        props['reg'] += struct.pack('>8I', 0, 0x0cd00000, 0, 0x40000, 0, 0x0c8c0000, 0, 0x40000)
        props['interrupts'] += b''.join(struct.pack('>3I', 0, irq, 4) for irq in irqs)
        # Opaque, initially uncommitted native records. Only the runtime EFI
        # producer fills them, after capture/reservation and EFI retirement.
        props['qcom,display-smmu-handoff'] = bytes(4288)
        props['qcom,display-smmu-observed'] = bytes(128)
        props['qcom,display-smmu-status'] = bytes(4)
    props['status'] = b'okay\0'
    for name in ('qcom,display-handoff-version', 'qcom,display-handoff-flags', 'qcom,display-interface',
                 'qcom,display-control', 'qcom,display-source', 'qcom,display-vsync-bit', 'qcom,display-pingpong',
                 'display-width', 'display-height', 'display-row-bytes', 'dot-pitch',
                 'canvas-width', 'canvas-height'):
        props[name] = bytes(4)
    for name in ('framebuffer-address', 'framebuffer-size', 'qcom,scanout-device-address'):
        props[name] = bytes(8)
    props['display-refresh-hz'] = struct.pack('>I', rate)
    props['display-physical-width-mm'] = struct.pack('>I', width)
    props['display-physical-height-mm'] = struct.pack('>I', height)
    props['display-uuid'] = identifier.encode() + b'\0'
    backlight = None if disable_backlight else profile.get('backlight')
    if disable_backlight:
        props.pop('display-backlight', None)
        def remove_backlight_handoff(node):
            node['props'].pop('qcom,pmi8998-wled-handoff', None)
            for child in node['children']: remove_backlight_handoff(child)
        remove_backlight_handoff(tree)
    if backlight is not None:
        if not isinstance(backlight, dict) or backlight.get('compatible') != 'qcom,pmi8998-wled':
            raise ValueError('unsupported display backlight controller')
        fields = ('spmi_sid', 'control_base', 'sink_base', 'strings_mask', 'sync_delay_us')
        values = [backlight.get(k) for k in fields]
        if not all(type(v) is int for v in values):
            raise ValueError('invalid PMI8998 WLED resources')
        sid, control, sink, strings, delay = values
        if not (1 <= sid <= 15 and sid & 1 and control == 0xd800 and sink == 0xd900 and
                1 <= strings <= 15 and 1000 <= delay <= 10000):
            raise ValueError('invalid PMI8998 WLED resources')
        buses = []
        def find_spmi(node):
            if b'qcom,spmi-pmic-arb' in node['props'].get('compatible', b'').split(b'\0'): buses.append(node)
            for child in node['children']: find_spmi(child)
        find_spmi(tree)
        if len(buses) != 1: raise ValueError('backlight requires exactly one SPMI controller')
        bus = buses[0]['props']
        handle = bus.get('phandle', bus.get('AAPL,phandle', b''))
        if len(handle) != 4 or not int.from_bytes(handle, 'big'):
            raise ValueError('backlight requires a nonzero SPMI phandle')
        handle = int.from_bytes(handle, 'big')
        props['display-backlight'] = struct.pack('>I', handle)
        # Version/flags remain uncommitted until post-ExitBootServices raster
        # verification. The Kext then verifies PMIC identity, AP ownership,
        # actual enabled strings and firmware brightness without writing.
        bus['qcom,pmi8998-wled-handoff'] = struct.pack('>8I', 0, 0, handle, *values)
    return {'profile':str(path.resolve()), 'profile_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'display_scale':scale, 'display_rotation':rotation,
            'canvas':'native canvas-width/canvas-height CFData populated from the verified runtime raster',
            'efi_only':True, 'hardware_boot_verified':False, 'version_at_build':0,
            'display_smmu': {'requested':smmu_handoff, 'runtime_capture_required':True,
                'runtime_root_initialization_supported':True, 'bypass_enabled':False, 'direct_scanout_enabled_at_build':False,
                'supported_profile':'exclusive inherited AArch64/4K stage-1 or disabled stage-1 bank initialized after post-EFI VBIF DMA halt; EFI WB tables, vacant 32-bit IOVA aperture',
                'initialization_requirements':['existing exact SID/bank/VMID and no other active contexts',
                    'single command-mode DSI CTL0/LM0/VIG0 or DMA0, other fetch paths inactive',
                    'hardware VBIF halt ACK, real context TLBI/TLBSYNC and register readback',
                    'GOP-only read-only mapping retained; faults polled until native kernel IRQ setup']},
            'backlight_controller':backlight.get('compatible') if backlight else None,
            'backlight_handoff_disabled':disable_backlight,
            'runtime_checks':['GOP carveout', 'single source/CTL routing', 'DSI mode', 'live TE or auto refresh',
                              'retained EFI clocks', 'post-ExitBootServices register state']}


def parse_fdt(blob):
    if len(blob) < 40:
        raise ValueError('short FDT')
    magic, total, off, strings, _, version, _, _, slen, tlen = struct.unpack_from('>10I', blob)
    if magic != 0xd00dfeed or version < 17 or total > len(blob) or off+tlen > total or strings+slen > total:
        raise ValueError('invalid FDT header')
    stop, stringstop = off+tlen, strings+slen
    def word():
        nonlocal off
        if off+4 > stop: raise ValueError('truncated FDT')
        v, = struct.unpack_from('>I', blob, off); off += 4
        return v
    def node(depth=0):
        nonlocal off
        if depth > 64: raise ValueError('tree too deep')
        end = blob.index(0, off, stop)
        name = blob[off:end].decode('ascii'); off = (end+4)&~3
        n = {'name': name, 'props': {}, 'children': []}
        while True:
            token = word()
            if token == 1: n['children'].append(node(depth+1))
            elif token == 2: return n
            elif token == 3:
                size, nameoff = word(), word()
                if nameoff >= slen or off+size > stop: raise ValueError('bad FDT property')
                pstart = strings+nameoff
                pend = blob.index(0, pstart, stringstop)
                key = blob[pstart:pend].decode('ascii')
                if key in n['props']: raise ValueError('duplicate property')
                n['props'][key] = blob[off:off+size]; off = (off+size+3)&~3
            elif token != 4: raise ValueError(f'bad token {token}')
    while (token := word()) == 4: pass
    if token != 1: raise ValueError('missing root')
    root = node()
    while (token := word()) == 4: pass
    if token != 9: raise ValueError('missing end')
    return root


NUMERIC = {'#address-cells', '#size-cells', '#interrupt-cells', 'phandle', 'linux,phandle',
           'serial-device', 'interrupt-parent', 'interrupts', 'current-speed', 'max-speed', 'debug-enabled',
           'embedded-panic-log-size', 'nvram-bank-size', 'nvram-bank-count', 'nvram-current-bank',
           'amfi-only-platform-code',
           'certificate-production-status', 'effective-production-status-ap',
           'certificate-security-mode', 'effective-security-mode-sep',
           'qcom,pmic-rtc', 'qcom,battery-observer', 'qcom,pmi8998-haptics',
           'reg-shift', 'reg-io-width', 'timebase-frequency', 'clock-frequency', 'bus-frequency', 'cpu-id', 'cluster-id',
           'l2-cache-id', 'l2-cache-size', 'l3-cache-id', 'l3-cache-size', 'cluster-core-id', 'die-cluster-id', 'cluster-type',
           '#clock-cells', '#reset-cells', '#power-domain-cells', 'qcom,gcc-handoff-version',
           'qcom,regulator-limits', 'qcom,pm8005-regulators', 'qcom,supply-points', 'qcom,owned-clocks', 'qcom,owned-roots', 'qcom,owned-domains', 'qcom,owned-resets'}
NUMERIC |= {'qcom,dsi-handoff-version', 'qcom,dsi-handoff-flags', 'dsi-lanes',
            'qcom,dsi-byte-clock-hz', 'qcom,dsi-escape-clock-hz',
            'qcom,display-handoff-version', 'qcom,display-handoff-flags',
            'qcom,display-interface', 'qcom,display-control', 'qcom,display-source',
            'qcom,display-vsync-bit', 'display-width', 'display-height',
            'display-row-bytes', 'display-refresh-hz', 'display-panel', 'dot-pitch', 'canvas-width', 'canvas-height'}
NUMERIC |= {'panel-handoff-version', 'panel-handoff-flags', 'panel-brightness-bits'}
NUMERIC |= {'qcom,display-pingpong', 'display-physical-width-mm', 'display-physical-height-mm'}
NUMERIC |= {'display-scale', 'display-rotation'}
NUMERIC |= {'display-backlight', 'qcom,pmi8998-wled-handoff'}
NUMERIC |= {'#gpio-cells', 'qcom,owned-gpios', 'qcom,owned-irqs', 'qcom,button-map', 'qcom,owned-pin-functions'}
NUMERIC |= {'power-gpios', 'reset-gpios', 'irq-gpios'}
NUMERIC |= {'qcom,i2c-handoff', 'synaptics,touch-handoff', 'touchscreen-rotation'}
NUMERIC.add('qcom,cpu-osm-handoff')
NUMERIC |= {'xnu,allow-no-uart', 'qcom,geni-console-handoff'}
NUMERIC |= {'qcom,spi-handoff', 'spi-max-frequency', 'spi-mode', 'gyro,mount'}
NUMERIC |= {'qcom,rpm-handoff-version', 'qcom,usb-smmu-capture', 'qcom,usb-dma-capture', 'qcom,usb-firmware-physical32'}
NUMERIC.add('qcom,display-smmu-status')
NUMERIC.add('qcom,rpm-usb-core-clock-hz')
NUMERIC |= {'qcom,usb-controller-resources', 'qcom,usb-phy-supplies',
            'qcom,usb-phy-differential-ref', 'qcom,usb-phy-device-init',
            'qcom,usb-phy-host-init', 'qcom,usb-connector', 'qcom,owned-qos-ports','qcom,usb-resource-handoff'}
NUMERIC |= {'transport-encoding'}
NUMERIC |= {'qcom,ufs-hs-power-mode','qcom,ufs-handoff-version', 'qcom,ufs-handoff-flags', 'qcom,ufs-core-clock-hz',
            'qcom,ufs-unipro-clock-hz', 'qcom,ufs-polled-completion', 'qcom,ufs-hs-gear', 'dma-address-bits', 'lanes-per-direction'}
U64 = {'redistributor-stride', 'dram-base', 'dram-size', 'framebuffer-address',
       'framebuffer-size', 'qcom,scanout-device-address'}
# Mixed-width hardware handoffs are records, not arrays of interchangeable
# 32-bit cells. Preserve the numerical u64 value across the FDT/Apple DT ABI.
RPM_HANDOFF_CLOCK_COUNT = 27
RPM_RESOURCE_POLICY_VERSION = 2
RPM_HANDOFF_CLOCK_LIMIT_IDS = (0,1,2,3,4,5,6,7,8,12,19,26)
HANDOFF_RECORDS = {'qcom,gpu-handoff':('IIIIQQQ',1,1),
                   'qcom,gpu-firmware-handoff':('II3Q8I',1,1),
                   'qcom,gpu-zap-memory':('QQ',1,1),
                   'qcom,gpu-firmware-power':('IIII',1,1),
                   'qcom,gpu-noc-handoff':('IIQQII',1,1),
                   'qcom,adreno-smmu-handoff':('8I3Q16I',1,1),
                   'qcom,rpm-clock-handoff':('IIII',RPM_HANDOFF_CLOCK_COUNT,RPM_HANDOFF_CLOCK_COUNT),
                   'qcom,rpm-clock-limit-handoff':('IIII',len(RPM_HANDOFF_CLOCK_LIMIT_IDS),len(RPM_HANDOFF_CLOCK_LIMIT_IDS)),
                   'qcom,rpm-domain-handoff':('IIII',4,4),
                   'qcom,rpm-transport-handoff':('12I',1,1),
                   'qcom,rpm-interconnect-handoff':('IIQQQ',8,8)}


def apple_dt(root, entropy=None):
    """Convert address tuples as integers, cells as LE32, and keep byte/string data."""
    gics = set()
    def collect_gics(node):
        if b'arm,gic-v3' in node['props'].get('compatible', b'').split(b'\0'):
            handle = node['props'].get('phandle')
            if not handle or len(handle) != 4:
                raise ValueError('GIC requires a phandle')
            gics.add(struct.unpack('>I', handle)[0])
        for child in node['children']:
            collect_gics(child)
    collect_gics(root)
    def cell(props, name, default):
        return struct.unpack('>I', props[name])[0] if name in props else default
    def tuples(data, counts):
        width = sum(counts)*4
        if not width or len(data) % width: raise ValueError('invalid address tuple')
        result = bytearray()
        for base in range(0, len(data), width):
            pos = base
            for count in counts:
                value = int.from_bytes(data[pos:pos+count*4], 'big'); pos += count*4
                if value >= 1 << 64: raise ValueError('address wider than 64 bits')
                if count: result += struct.pack('<Q', value)
        return bytes(result)
    def emit(n, parent_ac=2, parent_sc=1, path=''):
        p = dict(n['props']); name = n['name']
        if b'arm,gic-v3' in p.get('compatible', b'').split(b'\0'):
            p['#interrupt-cells'] = struct.pack('>I', 2)
            p['compatible'] = b'ARM,gicv3\0'+p['compatible']
        irqparent = p.get('interrupt-parent', root['props'].get('interrupt-parent'))
        if 'interrupts' in p and irqparent and len(irqparent) == 4 and struct.unpack('>I', irqparent)[0] in gics:
            raw = p['interrupts']
            if len(raw) % 12:
                raise ValueError('Linux GIC interrupts require three cells')
            converted = bytearray()
            for kind, number, flags in struct.iter_unpack('>III', raw):
                # Qualcomm timer PPIs use fixed active-low wiring in Linux
                # DTS. GIC/Apple specifiers encode edge versus level only.
                if kind not in (0, 1) or (flags not in (1, 4) and not (kind == 1 and flags == 8)) or number >= (988 if kind == 0 else 16):
                    raise ValueError('unsupported GIC interrupt specifier')
                converted += struct.pack('>II', number+(32 if kind == 0 else 16), 1 if flags in (4,8) else 0)
            p['interrupts'] = bytes(converted)
        if path == '/' and name == 'chosen': p = chosen_properties(p)
        ac, sc = cell(p, '#address-cells', 2), cell(p, '#size-cells', 1)
        full = path+'/'+name
        if name == 'soc' or name.startswith('soc@'): aname = 'arm-io'
        elif name.startswith('memory@'): aname = 'memory'
        elif name.startswith('pram@'): aname = 'pram'
        else: aname = name or 'device-tree'
        out = {'name': aname.encode()+b'\0'}
        if name == 'soc' or name.startswith('soc@'): out['device_type'] = b'soc\0'
        for key, data in p.items():
            if key == 'name': continue
            if n.get('apple_native'): pass
            elif key == 'random-seed' and entropy is not None: data = entropy
            elif key == 'reg':
                # CPU MPIDR is consumed as uint32_t, memory/MMIO as uint64_t pairs.
                if p.get('device_type') == b'cpu\0':
                    value = int.from_bytes(data, 'big')
                    if value > 0xffffffff: raise ValueError('MPIDR too wide')
                    data = struct.pack('<I', value)
                else: data = tuples(data, (parent_ac, parent_sc))
            elif key == 'ranges': data = tuples(data, (ac, parent_ac, sc))
            elif key in HANDOFF_RECORDS:
                fmt,minimum,maximum=HANDOFF_RECORDS[key];size=struct.calcsize('>'+fmt)
                if len(data)%size or not minimum<=len(data)//size<=maximum:raise ValueError(f'invalid handoff records: {key}')
                data=b''.join(struct.pack('<'+fmt,*values) for values in struct.iter_unpack('>'+fmt,data))
            elif key in NUMERIC:
                if len(data)%4: raise ValueError(f'non-cell {key}')
                data = b''.join(struct.pack('<I', x[0]) for x in struct.iter_unpack('>I', data))
            elif key in U64 or name == 'memory-map':
                if len(data)%8: raise ValueError(f'non-u64 {key}')
                data = b''.join(struct.pack('<Q', x[0]) for x in struct.iter_unpack('>Q', data))
            out[key] = data
            if key == "phandle": out["AAPL,phandle"] = data
        encoded = bytearray(struct.pack('<II', len(out), len(n['children'])))
        for key, data in out.items():
            keybytes = key.encode('ascii')
            if len(keybytes) >= 32: raise ValueError('Apple DT property name too long')
            encoded += keybytes.ljust(32, b'\0')+struct.pack('<I', len(data))+data
            encoded += bytes((-len(data))%4)
        for child in n['children']: encoded += emit(child, ac, sc, full)
        return encoded
    return bytes(emit(root))


def run(*args):
    subprocess.run([str(x) for x in args], check=True, cwd=ROOT)


def verify_raw_relocations(path, image_size):
    elf = path.read_bytes()
    if elf[:6] != b'\x7fELF\x02\x01': raise ValueError('expected ELF64 little endian')
    shoff, = struct.unpack_from('<Q', elf, 40)
    entsize, count = struct.unpack_from('<HH', elf, 58)
    if entsize != 64 or shoff+count*entsize > len(elf): raise ValueError('bad ELF section table')
    relocations = 0
    for i in range(count):
        section = struct.unpack_from('<IIQQQQIIQQ', elf, shoff+i*entsize)
        if section[1] != 4: continue
        offset, size = section[4], section[5]
        if size % 24 or offset+size > len(elf): raise ValueError('bad RELA table')
        for pos in range(offset, offset+size, 24):
            target, kind, addend = struct.unpack_from('<QQq', elf, pos)
            if kind != 1027 or target % 8 or target+8 > image_size or not 0 <= addend <= image_size:
                raise ValueError('raw entry only supports bounded R_AARCH64_RELATIVE relocations')
            relocations += 1
    return relocations


def rpm_policy_records(policy, owner):
    """One explicit AP baseline for all users of the common RPM channel."""
    def number(value, maximum):
        if type(value) is str:
            try: value = int(value, 0)
            except ValueError: raise ValueError(owner+' RPM policy integer is invalid') from None
        if type(value) is not int or not 0 <= value <= maximum:
            raise ValueError(owner+' RPM policy integer out of range')
        return value
    def rows(name, count, widths, expected):
        records = policy.get(name)
        if not isinstance(records,list) or len(records)!=count:
            raise ValueError('incomplete '+owner+' '+name)
        parsed=[]
        for row in records:
            if not isinstance(row,list) or len(row)!=len(widths):
                raise ValueError('invalid '+owner+' '+name+' record')
            parsed.append(tuple(number(value,(1<<width)-1) for value,width in zip(row,widths)))
        if {row[0] for row in parsed}!=set(expected):
            raise ValueError('duplicate/unknown '+owner+' '+name+' identity')
        return sorted(parsed)
    domains=rows('rpm_domains',4,[32,32,32],range(0x1000,0x1004))
    clocks=rows('rpm_clocks',RPM_HANDOFF_CLOCK_COUNT,[32,32,32],range(RPM_HANDOFF_CLOCK_COUNT))
    limits=rows('rpm_clock_limits',len(RPM_HANDOFF_CLOCK_LIMIT_IDS),[32,32,32],RPM_HANDOFF_CLOCK_LIMIT_IDS)
    ids=[0x2000,0x2001,0x2002,0x2100,0x2101,0x2102,0x2103,0x2104]
    bus=rows('rpm_interconnect',8,[32,64,64,64],ids)
    levels={0,16,32,48,64,128,192,256,320,384,416,448,512}
    if any(a not in levels or b not in levels for _,a,b in domains) or not domains[0][1] or not domains[1][1]:
        raise ValueError(owner+' CX/MX require nonzero valid RPM levels')
    branches={6,9,10,11,13,14,15,16,17,18,20,21,22,23,24,25}
    maxima={i:1 for i in branches}
    maxima[19]=4  # QDSS STATE enum; RPM/NPA clamps larger values before ACK.
    if any(a>maxima.get(i,0x7fffffff) or b>maxima.get(i,0x7fffffff) for i,a,b in clocks):
        raise ValueError('invalid '+owner+' RPM clock baseline')
    if clocks[6][1]!=1 or clocks[7][1]<19200:
        raise ValueError(owner+' RPM policy must preserve XO/PCNoC')
    for i,active,sleep in limits:
        if (i==6 and (active>1 or sleep>1)) or (i==19 and (active>4 or sleep>4)) or any(
                cap and request>cap for request,cap in zip(clocks[i][1:],(active,sleep))):
            raise ValueError(owner+' RPM clock request exceeds its declared limit')
    for i,maximum,active,sleep in bus:
        if not maximum or active>maximum or sleep>maximum or (i<0x2100 and
                (maximum>0xffffffff or maximum%1000 or active%1000 or sleep%1000)):
            raise ValueError('invalid '+owner+' RPM interconnect limits/floors')
    for i,clock_index in enumerate((0,3,2)):
        if bus[i][2:]!=tuple(value*1000 for value in clocks[clock_index][1:]):
            raise ValueError(owner+' RPM clock/interconnect baselines disagree')
        limit=next(r for r in limits if r[0]==clock_index)
        if any(cap and bus[i][1]>cap*1000 for cap in limit[1:]):
            raise ValueError(owner+' RPM NoC request ceiling exceeds its clock limit')
    return domains,clocks,bus,limits


def rpm_policy_properties(domains, clocks, bus, limits):
    return {'qcom,rpm-domain-handoff':b''.join(struct.pack('>4I',*r,0) for r in domains),
            'qcom,rpm-clock-handoff':b''.join(struct.pack('>4I',*r,0) for r in clocks),
            'qcom,rpm-clock-limit-handoff':b''.join(struct.pack('>4I',*r,0) for r in limits),
            'qcom,rpm-interconnect-handoff':b''.join(struct.pack('>IIQQQ',r[0],0,*r[1:]) for r in bus)}


def validate_existing_rpm_policy(props, values):
    present=set(props)&set(values)
    def normalized(key,data):
        fmt=HANDOFF_RECORDS[key][0];size=struct.calcsize('>'+fmt)
        if len(data)%size:raise ValueError('malformed existing RPM policy')
        records=sorted(struct.iter_unpack('>'+fmt,data))
        if len({r[0] for r in records})!=len(records):raise ValueError('duplicate existing RPM policy identity')
        return records
    if present and (present!=set(values) or any(normalized(k,props[k])!=normalized(k,v) for k,v in values.items())):
        raise ValueError('all RPM channel consumers must declare the same complete AP baseline policy')


def parse_rpm_policy(raw, owner):
    def unique(pairs):
        result={}
        for k,v in pairs:
            if k in result: raise ValueError('duplicate '+owner+' policy field: '+k)
            result[k]=v
        return result
    return json.loads(raw,object_pairs_hook=unique)


def load_usb_resource_profile(path):
    """Validate explicit board policy; no defaults stand in for old AP votes."""
    raw=Path(path).read_bytes()
    policy=parse_rpm_policy(raw,'USB')
    fields={'version','component','maximum_speed','core_clock_hz','dma','phy','connector',
            'regulator_limits','rpm_domains','rpm_clocks','rpm_clock_limits','rpm_interconnect'}
    if not isinstance(policy,dict) or set(policy)!=fields or type(policy['version']) is not int or policy['version']!=RPM_RESOURCE_POLICY_VERSION:
        raise ValueError('invalid USB resource policy version/fields')
    def number(v,maximum=0xffffffff):
        if type(v) is str:
            try:v=int(v,0)
            except ValueError:raise ValueError('invalid USB policy integer') from None
        if type(v) is not int or not 0<=v<=maximum:raise ValueError('USB policy integer out of range')
        return v
    def rows(values,width):
        if not isinstance(values,list):raise ValueError('USB policy requires a record array')
        result=[]
        for row in values:
            if not isinstance(row,list) or len(row)!=width:raise ValueError('invalid USB policy record')
            result.append(tuple(number(v) for v in row))
        return result
    def boolean(v):
        if type(v) is not bool:raise ValueError('USB policy requires a boolean')
        return int(v)
    component=policy['component']
    if not isinstance(component,str) or not re.fullmatch(r'\\[A-Z0-9_.]{1,63}',component):
        raise ValueError('USB resource policy requires an absolute ACPI component')
    speed=number(policy['maximum_speed']);hz=number(policy['core_clock_hz'])
    if speed not in (3,4) or hz not in (60000000,120000000,150000000) or (speed==4 and hz<120000000):
        raise ValueError('invalid USB speed/core clock policy')
    domains,clocks,bus,clock_limits=rpm_policy_records(policy,'USB')
    # MSM8998 GCC USB30 master FMAX and vdd-level-8998.h: LOWER=64,
    # LOW=128, NOMINAL=256. Nonzero CX alone does not support these rates.
    required_cx={60000000:64,120000000:128,150000000:256}[hz]
    if domains[0][1]<required_cx:
        raise ValueError('USB core clock requires a higher persistent active CX floor')
    # Match the production USB path's fixed240MB/s average,800MB/s peak and
    # BIMC153% utilization/two-channel aggregation, including declared floors.
    average,peak=240000000,800000000
    up=lambda v,d:(v+d-1)//d
    total=[r[2]+average for r in bus[3:]]
    if any(v>r[1] or v>(1<<64)-1 for v,r in zip(total,bus[3:])) or max(total[3:])>((1<<64)-1)//153:
        raise ValueError('USB NoC bandwidth exceeds declared RPM policy')
    bimc_average=up(up(max(total[3:])*153,100),2)
    rates=[max(up(peak,16),up(total[0],16)),
           max(up(peak,16),up(total[1],16),up(up(total[2],2),32)),
           up(max(peak,bimc_average),8)]
    if any(v>0xffffffff-999 for v in rates):raise ValueError('USB NoC rate rounding exceeds native arithmetic')
    rates=[up(max(19200000,v),1000)*1000 for v in rates]
    if any(v>r[1] or v>0xffffffff for v,r in zip(rates,bus[:3])):
        raise ValueError('USB NoC clock demand exceeds declared RPM policy')
    # Consume the same fixed hardware catalogue as the native RPM service.
    # Refuse an unrecognized catalogue format instead of duplicating ranges.
    catalogue_path=ROOT/'drivers/power/QcomRPM/RPMRails.h'
    catalogue={}
    for match in re.finditer(r'^\s*\{(\d+),(0x[0-9a-fA-F]+),(\d+),(\d+),(\d+),(\d+),"[^"\n]+"\},',catalogue_path.read_text(),re.M):
        identity,kind,resource,minimum,step,maximum=(int(v,0) for v in match.groups())
        if identity in catalogue:raise ValueError('duplicate native RPM rail identity')
        catalogue[identity]=(minimum,step,maximum)
    if len(catalogue)!=43:raise ValueError('unsupported native RPM rail catalogue')
    limits=rows(policy['regulator_limits'],4);assigned={}
    if not limits or len(limits)>43:raise ValueError('USB requires explicit regulator limits/floors')
    for identity,minimum,maximum,floor in limits:
        if identity not in catalogue or identity in assigned:raise ValueError('duplicate/unknown USB regulator')
        lo,step,hi=catalogue[identity]
        if not step or minimum<lo or minimum>maximum or maximum>hi or floor>maximum or (floor and (floor<minimum or (floor-lo)%step)):
            raise ValueError('unsafe USB regulator limit/floor')
        assigned[identity]=(minimum,maximum,floor)
    phy=policy['phy']
    if not isinstance(phy,dict) or set(phy)!=({'usb2','usb3'} if speed==4 else {'usb2'}):
        raise ValueError('USB speed requires exactly its declared PHYs')
    def supplies(values,count):
        values=rows(values,2)
        if len(values) not in count or len({r[0] for r in values})!=len(values):
            raise ValueError('invalid USB PHY supply list')
        for identity,uv in values:
            if identity not in assigned:raise ValueError('USB PHY supply lacks an explicit RPM limit/floor')
            minimum,maximum,_=assigned[identity];lo,step,_=catalogue[identity]
            if not uv or uv<minimum or uv>maximum or lo+up(uv-lo,step)*step>maximum:
                raise ValueError('USB PHY voltage exceeds its native RPM limits')
        return values
    def tuning(values,host):
        values=rows(values,2);allowed={0,4,0x2c,0xb4,0x184,0x18c,0x218,0x21c,0x23c,0x240}
        if len(values)>16:raise ValueError('USB PHY tuning table too large')
        for i,(offset,value) in enumerate(values):
            if value>255 or (offset not in allowed and (offset!=0x210 or not host or i not in (0,len(values)-1))):
                raise ValueError('USB PHY tuning addresses an unowned register')
        if host and values and (len(values)<3 or values[0][0]!=0x210 or not values[0][1]&1 or values[-1][0]!=0x210 or values[-1][1]&1):
            raise ValueError('USB host PHY tuning must bracket power down/up')
        return values
    usb2=phy['usb2']
    if not isinstance(usb2,dict) or set(usb2)!={'supplies','differential_ref','otp_trim','device_init','host_init'}:
        raise ValueError('incomplete USB2 PHY policy')
    usb2={'supplies':supplies(usb2['supplies'],(3,4)),'differential_ref':boolean(usb2['differential_ref']),
          'otp_trim':boolean(usb2['otp_trim']),'device_init':tuning(usb2['device_init'],False),
          'host_init':tuning(usb2['host_init'],True)}
    usb3=None
    if speed==4:
        if not isinstance(phy['usb3'],dict) or set(phy['usb3'])!={'supplies'}:raise ValueError('incomplete USB3 PHY policy')
        usb3={'supplies':supplies(phy['usb3']['supplies'],(2,))}
    connector=policy['connector']
    if not isinstance(connector,dict) or set(connector)!={'sid','adc_base','source_microamps','vconn'}:
        raise ValueError('incomplete USB connector policy')
    sid=number(connector['sid'],15);adc=number(connector['adc_base'],0xffff)
    budget=number(connector['source_microamps']);vconn=boolean(connector['vconn'])
    if sid!=2 or adc!=0x4500 or (budget and (budget<500000 or budget>1500000 or budget%250000)) or (speed==4 and budget and budget<900000):
        raise ValueError('unsupported PMI8998 routing or USB source budget')
    dma=policy['dma']
    if not isinstance(dma,dict) or set(dma)!={'mode','address_bits','smmu_base','smmu_bytes','context_irqs'}:
        raise ValueError('incomplete USB DMA capture policy')
    mode=dma['mode'];bits=number(dma['address_bits']);base=number(dma['smmu_base'],(1<<64)-1);span=number(dma['smmu_bytes'])
    irqs=dma['context_irqs']
    if mode not in ('active-context','fresh-context','architected-bypass','disabled-bypass','unmatched-bypass','firmware-physical32') or not 32<=bits<=40 or base!=0x1680000 or span!=0x10000 or (mode=='firmware-physical32' and bits!=32):
        raise ValueError('unsupported USB DMA topology/capture mode')
    if not isinstance(irqs,list):raise ValueError('USB context IRQs must be an explicit list')
    entries=[]
    for value in irqs:
        if isinstance(value,list):
            if len(value)!=2:raise ValueError('USB context IRQ rows require INTID and IORT flags')
            entries.append((number(value[0],1019),number(value[1],1)))
        else:entries.append((number(value,1019),0)) # An integer explicitly selects level, as in version1.
    irqs=tuple(entries)
    if (mode in ('active-context','fresh-context') and not 1<=len(irqs)<=128) or (mode not in ('active-context','fresh-context') and irqs) or len({v[0] for v in irqs})!=len(irqs) or any(v[0]<32 or v[0]==163 for v in irqs):
        raise ValueError('invalid USB context IRQ routing')
    return {'component':component,'maximum_speed':speed,'core_clock_hz':hz,'dma':(mode,bits,base,span,irqs),
            'usb2':usb2,'usb3':usb3,'connector':(sid,adc,budget,vconn),'regulator_limits':sorted(limits),
            'rpm':rpm_policy_properties(domains,clocks,bus,clock_limits),'noc_rates':rates,
            'profile_sha256':hashlib.sha256(raw).hexdigest(),'profile':str(Path(path).resolve()),
            'rail_catalogue_sha256':hashlib.sha256(catalogue_path.read_bytes()).hexdigest()}


def configure_usb_resources(tree, profile, bridge_consumer=False):
    """Produce Apple resource metadata atomically; runtime owners grant DMA."""
    candidate=copy.deepcopy(tree)
    if b'qcom,msm8998' not in candidate['props'].get('compatible',b'').split(b'\0'):
        raise ValueError('USB resource profile requires MSM8998')
    homes=[n for n in candidate['children'] if n['name']=='soc' or n['name'].startswith('soc@')]
    chosen=[n for n in candidate['children'] if n['name']=='chosen']
    if len(homes)!=1 or len(chosen)!=1:raise ValueError('ambiguous USB platform tree')
    soc=homes[0];chosen=chosen[0]['props']
    if any(n['props'].get(k)!=struct.pack('>I',2) for n in (candidate,soc) for k in ('#address-cells','#size-cells')):
        raise ValueError('USB resources require native64-bit platform address tuples')
    ranges=soc['props'].get('ranges')
    if ranges is None or len(ranges)%24:raise ValueError('missing/malformed USB platform ranges')
    translated=list(struct.iter_unpack('>QQQ',ranges))
    if any(a!=b or not length or length>(1<<64)-a for a,b,length in translated):
        raise ValueError('USB resources require identity physical platform ranges')
    if chosen.get('usb-iort-component')!=profile['component'].encode('ascii')+b'\0' or any(
            chosen.get(k)!=bytes(size) for k,size in [('qcom,usb-firmware-iort',65536),('qcom,usb-firmware-controller',40),
            ('qcom,usb-firmware-stop',64),('qcom,usb-hardware-stop',64),('qcom,usb-firmware-rpm',20544)]):
        raise ValueError('USB resources require matching empty live-capture and full stop slots')
    def provider(compatible,regions):
        nodes=[n for n in soc['children'] if compatible in n['props'].get('compatible',b'').split(b'\0')]
        if len(nodes)!=1 or nodes[0]['props'].get('reg',b'')!=b''.join(struct.pack('>QQ',a,b) for a,b in regions):
            raise ValueError('missing/ambiguous USB resource provider: '+compatible.decode())
        if nodes[0]['props'].get('status',b'okay\0') not in (b'okay\0',b'ok\0'):raise ValueError('disabled USB resource provider')
        return nodes[0]['props']
    gcc=provider(b'qcom,gcc-msm8998',[(0x100000,0x94000)])
    rpm=provider(b'qcom,glink-rpm',[(0x778000,0x7000),(0x17911000,0x1000)])
    spmi=provider(b'qcom,spmi-pmic-arb',[(0x800f000,0x1000),(0x8400000,0x1000000),
        (0x9400000,0x1000000),(0xa400000,0x220000),(0x800a000,0x3000)])
    if rpm.get('qcom,rpm-handoff-version',struct.pack('>I',3)) not in tuple(struct.pack('>I',v) for v in (1,2,3)):
        raise ValueError('unsupported existing RPM ownership version')
    gpus=[n['props'] for n in soc['children'] if 'qcom,gpu-handoff' in n['props']]
    if gpus and not bridge_consumer:
        raise ValueError('USB RPM cold handoff cannot preserve a warm GPU without bridge cold startup')
    if len(gpus)>1:raise ValueError('ambiguous GPU RPM consumer')
    if gpus:
        raw=gpus[0].get('qcom,gpu-noc-handoff',b'')
        if len(raw)!=32:raise ValueError('missing GPU NoC policy on the shared RPM channel')
        _,_,gpu_average,gpu_peak,flags,reserved=struct.unpack('>IIQQII',raw)
        bus=list(struct.iter_unpack('>IIQQQ',profile['rpm']['qcom,rpm-interconnect-handoff']))
        combined=bus[-1][3]+gpu_average+240000000
        up=lambda v,d:(v+d-1)//d
        if flags!=1 or reserved or not gpu_average or gpu_average>gpu_peak or combined>bus[-1][2] or combined>((1<<64)-1)//153:
            raise ValueError('combined GPU/USB bandwidth exceeds RPM policy')
        demand=max(combined,bus[-2][3]+240000000)
        rate=up(up(up(demand*153,100),2),8)
        rate=max(rate,up(max(gpu_peak,800000000),8),19200000)
        if rate>0xffffffff-999:raise ValueError('combined GPU/USB rate exceeds native arithmetic')
        rate=up(rate,1000)*1000
        if rate>bus[2][2]:raise ValueError('combined GPU/USB BIMC demand exceeds RPM policy')
    parent=candidate['props'].get('interrupt-parent')
    if not parent or len(parent)!=4 or not any(n['props'].get('phandle')==parent and b'arm,gic-v3' in n['props'].get('compatible',b'').split(b'\0') for n in soc['children']):
        raise ValueError('USB resources require the actual root GIC controller')
    if spmi.get('#interrupt-cells')!=struct.pack('>I',4) or len(spmi.get('phandle',b''))!=4:
        raise ValueError('USB connector requires the native SPMI interrupt provider')
    validate_existing_rpm_policy(rpm,profile['rpm']);rpm.update(profile['rpm'])
    usb_clock=struct.pack('>I',profile['core_clock_hz'])
    if rpm.get('qcom,rpm-usb-core-clock-hz',usb_clock)!=usb_clock:
        raise ValueError('conflicting RPM USB clock baseline policy')
    rpm['qcom,rpm-usb-core-clock-hz']=usb_clock
    def merge(props,key,records,width):
        old=props.get(key,b'');stride=4*width
        if len(old)%stride:raise ValueError('malformed existing resource ownership: '+key)
        existing={}
        for row in struct.iter_unpack('>'+str(width)+'I',old):
            if row[0] in existing:raise ValueError('duplicate existing resource ownership: '+key)
            existing[row[0]]=row
        for row in records:
            if row[0] in existing and existing[row[0]]!=tuple(row):raise ValueError('conflicting resource ownership: '+key)
            existing[row[0]]=tuple(row)
        props[key]=b''.join(struct.pack('>'+str(width)+'I',*existing[i]) for i in sorted(existing))
    merge(rpm,'qcom,regulator-limits',profile['regulator_limits'],4)
    if gcc.get('qcom,gcc-handoff-version',struct.pack('>I',1))!=struct.pack('>I',1):raise ValueError('unsupported GCC ownership version')
    gcc['qcom,gcc-handoff-version']=struct.pack('>I',1)
    speed=profile['maximum_speed'];clocks=[(i,0) for i in (32,71,116,117,118,121,170)]
    roots=[(163,profile['core_clock_hz']),(164,19200000)];resets=[(18,),(106,)]
    if speed==4:clocks.extend([(119,0),(120,125000000),(166,0)]);roots.append((165,1200000));resets.extend([(69,),(70,)])
    for key,records,width in [('qcom,owned-clocks',clocks,2),('qcom,owned-roots',roots,2),
                              ('qcom,owned-domains',[(2,)],1),('qcom,owned-resets',resets,1)]:merge(gcc,key,records,width)
    sid,adc,budget,vconn=profile['connector']
    irqs=[(sid,0x13,7,1),(sid,0x13,4,3),(sid,0x13,3,3),(sid,0x11,0,3),(sid,0x11,1,3),(sid,0x11,2,3)]
    irq_old=spmi.get('qcom,owned-irqs',b'')
    if len(irq_old)%16:raise ValueError('malformed SPMI IRQ ownership')
    owned={}
    for row in [*struct.iter_unpack('>4I',irq_old),*irqs]:
        identity=row[:3]
        if row[0]>15 or row[1]>255 or row[2]>7 or row[3] not in (1,2,3,4,8) or (identity in owned and owned[identity]!=row):
            raise ValueError('conflicting/invalid SPMI IRQ ownership')
        owned[identity]=row
    if len(owned)>64:raise ValueError('SPMI IRQ table exceeds native capacity')
    spmi['qcom,owned-irqs']=b''.join(struct.pack('>4I',*owned[k]) for k in sorted(owned))
    connector=struct.pack('>4I',sid,adc,budget,vconn)
    if 'qcom,usb-connector' in spmi and spmi['qcom,usb-connector']!=connector:raise ValueError('conflicting USB connector policy')
    spmi['qcom,usb-connector']=connector
    targets={}
    def node(name,compatible,regions,props):
        matches=[n for n in soc['children'] if n['name']==name or compatible in n['props'].get('compatible',b'').split(b'\0')]
        if len(matches)>1:raise ValueError('ambiguous USB resource node')
        values={'compatible':compatible+b'\0','reg':b''.join(struct.pack('>QQ',a,b) for a,b in regions),'status':b'okay\0',**props}
        if matches:
            n=matches[0]
            if n['name']!=name or n['children'] or any(k in n['props'] and n['props'][k]!=v for k,v in values.items() if k!='status'):
                raise ValueError('conflicting USB resource node: '+name)
        else:n={'name':name,'props':{},'children':[]};soc['children'].append(n)
        n['props'].update(values);targets[name]=regions;return n['props']
    mode,bits,base,span,context=profile['dma']
    usb=node('usb@a800000',b'qcom,msm8998-dwc3',[(0xa800000,0xcd00),(0xa8f8800,0x400),(base,span)],{
        'interrupt-parent':parent,'interrupts':b''.join(struct.pack('>3I',0,irq-32,1 if flags else 4) for irq,flags in ((163,0),*context)),
        'qcom,usb-controller-resources':struct.pack('>3I',profile['core_clock_hz'],bits,speed),
        'qcom,usb-resource-handoff':struct.pack('>I',1),
        'configuration-string':b'standardBringup\0',
        'usb-iort-component':profile['component'].encode('ascii')+b'\0'})
    if any(k in usb for k in ('iommu-parent','qcom,usb-smmu-handoff','qcom,usb-smmu-bypass','qcom,usb-dma-window','dma-coherent')):
        raise ValueError('USB profile cannot import another mapper or manufacture a DMA grant')
    capture='qcom,usb-firmware-physical32' if mode=='firmware-physical32' else 'qcom,usb-smmu-capture' if mode in ('active-context','fresh-context') else 'qcom,usb-dma-capture'
    value=struct.pack('>I',{'fresh-context':2,'disabled-bypass':2,'unmatched-bypass':3}.get(mode,1))
    if any(k in usb and (k!=capture or usb[k]!=value) for k in ('qcom,usb-firmware-physical32','qcom,usb-dma-capture','qcom,usb-smmu-capture')):raise ValueError('conflicting USB DMA capture request')
    usb[capture]=value
    node('interconnect@1660000',b'qcom,msm8998-a1noc',[(0x1660000,0x60000)],{'qcom,owned-qos-ports':struct.pack('>I',2)})
    phy=profile['usb2'];regions=[(0xc012000,0x2a8),(0x1fcb24c,4)]
    if phy['otp_trim']:regions.append((0x784238,4))
    node('phy@c012000',b'qcom,msm8998-qusb2-phy',regions,{
        'qcom,usb-phy-supplies':b''.join(struct.pack('>2I',*r) for r in phy['supplies']),
        'qcom,usb-phy-differential-ref':struct.pack('>I',phy['differential_ref']),
        'qcom,usb-phy-device-init':b''.join(struct.pack('>2I',*r) for r in phy['device_init']),
        'qcom,usb-phy-host-init':b''.join(struct.pack('>2I',*r) for r in phy['host_init'])})
    if speed==4:node('phy@c010000',b'qcom,msm8998-qmp-usb3-phy',[(0xc010000,0x1000),(0x1fcb244,4)],{
        'qcom,usb-phy-supplies':b''.join(struct.pack('>2I',*r) for r in profile['usb3']['supplies'])})
    if translated:
        required=[region for regions in targets.values() for region in regions]
        for p in (gcc,rpm,spmi):required.extend(struct.iter_unpack('>QQ',p['reg']))
        if any(not any(a<=start and length<=span-(start-a) for a,_,span in translated if a<=start<a+span) for start,length in required):
            raise ValueError('USB resource lies outside the identity platform ranges')
    # No enabled foreign nub may claim one of these resources. The broad NoC
    # aperture intentionally contains the SMMU; its driver writes only QoS2.
    for n in soc['children']:
        if n['name'] in targets or n['props'].get('status',b'okay\0')==b'disabled\0':continue
        reg=n['props'].get('reg',b'')
        if len(reg)%16:continue
        for start,length in struct.iter_unpack('>QQ',reg):
            for regions in targets.values():
                if any(length and start<a+b and a<start+length for a,b in regions):
                    raise ValueError('USB MMIO overlaps an enabled foreign provider: '+n['name'])
    configure_usb_rpm_cold(candidate,bridge_consumer)
    tree.clear();tree.update(candidate)
    return {'profile':profile['profile'],'profile_sha256':profile['profile_sha256'],
            'rail_catalogue_sha256':profile['rail_catalogue_sha256'],'nodes':sorted(targets),
            'maximum_speed':speed,'core_clock_hz':profile['core_clock_hz'],'dma_capture':mode,
            'NoC_minimum_client_Hz':profile['noc_rates'],'source_microamps':budget,
            'Apple_resource_metadata_produced':True,'DMA_or_resource_grants':False,
            'GCC_and_IRQ_control_policy_declared':True,'runtime_lease_produced_by_builder':False,
            'runtime_checks':'live installed IORT and pinned stop proofs; actual GCC/NoC/PHY/connector leases and fresh RPM ACKs',
            'hardware_verified':False,'build_executes_hardware':False}


def configure_gpu_handoff(tree, profile_path, cold_start=False):
    """Reserve runtime output slots; hardware identities/addresses come from EFI.

    RPM floors are explicit boot policy, not observations inferred from clock
    registers. The native RPM controller must ACK every supplied floor before
    the GPU can acquire its platform leases.
    """
    raw = profile_path.read_bytes()
    policy = parse_rpm_policy(raw,'GPU')
    fields = {'version','iova_first','iova_bytes','average_bandwidth','peak_bandwidth',
              'rpm_domains','rpm_clocks','rpm_clock_limits','rpm_interconnect'}
    if not isinstance(policy, dict) or not fields <= set(policy) or set(policy) - fields - {'zap_memory','firmware_power','gfx_regulator'} or type(policy['version']) is not int or policy['version'] != RPM_RESOURCE_POLICY_VERSION:
        raise ValueError('invalid GPU handoff policy version/fields')
    def number(value, maximum):
        if type(value) is str: value = int(value, 0)
        if type(value) is not int or not 0 <= value <= maximum:
            raise ValueError('GPU handoff policy integer out of range')
        return value
    domains,clocks,bus,clock_limits = rpm_policy_records(policy,'GPU')
    first = number(policy['iova_first'], (1 << 40)-1)
    size = number(policy['iova_bytes'], 0x20000000)
    average = number(policy['average_bandwidth'], (1 << 64)-1)
    peak = number(policy['peak_bandwidth'], (0xffffffff-999)*8)
    if first < 0x1000000 or (first | size) & 4095 or size < 0x800000 or size > (1 << 40)-first:
        raise ValueError('invalid GPU vacant IOVA allocation window')
    if not average or average > peak or average > bus[-1][1] or not bus[2][2] or not bus[-1][2] or \
            max(19200000, ((peak+7)//8+999)//1000*1000) > bus[2][1]:
        raise ValueError('GPU NoC budget exceeds RPM policy')
    firmware_power = None
    if 'firmware_power' in policy:
        point = policy['firmware_power']
        if not isinstance(point,dict) or set(point) != {'frequency_hz','voltage_uv'}:
            raise ValueError('invalid GPU firmware operating point')
        frequency = number(point['frequency_hz'],1000000000)
        voltage = number(point['voltage_uv'],2000000)
        if frequency < 19200000 or voltage < 400000 or voltage % 1000:
            raise ValueError('GPU firmware operating point outside GPMU representation')
        firmware_power = (1,frequency,voltage,0)
    if cold_start:
        # Fixed MSM8998 OPP/corner pairs from the pinned GPU DTS/rpmpd data.
        points = {257000000:48,342000000:64,414000000:128,515000097:192,
                  596000097:256,670000048:320,710000097:384}
        if not firmware_power or firmware_power[1] not in points or \
                min(domains[0][1],domains[1][1]) < points[firmware_power[1]]:
            raise ValueError('cold GPU startup requires a supported OPP and sufficient CX/MX policy corners')
    gfx_regulator = None
    if 'gfx_regulator' in policy:
        rail = policy['gfx_regulator']
        if not isinstance(rail,dict) or set(rail) != {'id','sid','minimum_uv','maximum_uv'} or not firmware_power:
            raise ValueError('GPU GFX supply requires PM8005 routing, limits and firmware operating voltage')
        rid,sid,minimum,maximum = (number(rail[k],0xffffffff) for k in ['id','sid','minimum_uv','maximum_uv'])
        if rid != 1 or sid > 15 or minimum < 524000 or maximum > 1100000 or \
                not minimum <= firmware_power[2] <= maximum or firmware_power[2] % 4000:
            raise ValueError('GPU GFX supply is outside the PM8005 S1 voltage policy')
        gfx_regulator = (rid,sid,minimum,maximum,firmware_power[2])
    if cold_start and not gfx_regulator:
        raise ValueError('cold GPU startup requires an explicit PM8005 GFX supply policy')
    zap_memory = None
    if 'zap_memory' in policy:
        region = policy['zap_memory']
        if not isinstance(region,list) or len(region)!=2: raise ValueError('invalid GPU ZAP reserved region')
        base,size = (number(value,0xffffffff) for value in region)
        if base < 0x80000000 or (base | size) & 4095 or not size or size > 0x1000000 or size > (1<<32)-base:
            raise ValueError('GPU ZAP reserved region is outside low DRAM or unaligned')
        zap_memory = (base,size)
    if b'qcom,msm8998\0' not in tree['props'].get('compatible',b''):
        raise ValueError('GPU retained handoff requires MSM8998')
    soc = next(n for n in tree['children'] if n['name'] == 'soc' or n['name'].startswith('soc@'))
    gpu = next(n for n in soc['children'] if n['name'] == 'gpu@5000000')
    rpm = next(n for n in soc['children'] if n['name'] == 'power-controller@778000')
    rpm_properties=rpm_policy_properties(domains,clocks,bus,clock_limits)
    validate_existing_rpm_policy(rpm['props'],rpm_properties)
    spmi = next((n for n in soc['children'] if n['name'] == 'power-controller@800f000'), None)
    config = struct.pack('>5I',*gfx_regulator) if gfx_regulator else None
    if gfx_regulator and (spmi is None or spmi['props'].get('qcom,pm8005-regulators',config) != config):
        raise ValueError('GPU GFX policy conflicts with existing PM8005 regulator routing')
    gpu['props'].pop('qcom,gpu-firmware-handoff',None)
    gpu['props'].pop('qcom,gpu-gfx-handoff',None)
    if spmi is not None: spmi['props'].pop('qcom,gpu-gfx-handoff',None)
    gpu['props'].update({
        'reg':struct.pack('>12I',0,0x05000000,0,0x40000,0,0x05040000,0,0x10000,0,0x01000000,0,0x80000),
        'interrupts':struct.pack('>6I',0,300,4,0,329,4),
        'status':b'okay\0',
        'qcom,gpu-handoff':bytes(40),
        'qcom,adreno-smmu-handoff':struct.pack('>8I3Q16I',*([0]*8),0,first,first+size,*([0]*16)),
        'qcom,gpu-noc-handoff':struct.pack('>IIQQII',0,0,average,peak,1,0),
    })
    rpm['props'].update({
        'qcom,rpm-handoff-version':struct.pack('>I',2 if cold_start else 1),
        **rpm_properties,
    })
    if cold_start: rpm['props']['qcom,rpm-transport-handoff'] = bytes(48)
    else: rpm['props'].pop('qcom,rpm-transport-handoff',None)
    if zap_memory: gpu['props']['qcom,gpu-zap-memory'] = struct.pack('>QQ',*zap_memory)
    else: gpu['props'].pop('qcom,gpu-zap-memory',None)
    if firmware_power: gpu['props']['qcom,gpu-firmware-power'] = struct.pack('>4I',*firmware_power)
    else: gpu['props'].pop('qcom,gpu-firmware-power',None)
    if gfx_regulator:
        spmi['props']['qcom,pm8005-regulators'] = config
        spmi['props']['qcom,gpu-gfx-handoff'] = bytes(16)
        gpu['props']['qcom,gpu-gfx-handoff'] = bytes(16)
    return {'profile_sha256':hashlib.sha256(raw).hexdigest(),'mode':'retained A540',
            'efi_only':True,'runtime_contract_versions_initially_zero':True,
            'rpm_baselines':'explicit policy; bridge and native controller ACK required' if cold_start else 'explicit policy; native controller ACK required',
            'cp_and_smmu_addresses':'captured from hardware at EFI runtime',
            'arena_excludes_all_retained_gpu_mappings':not cold_start,
            'initial_gx_off_required_before_arena_selection':cold_start,'zap_memory':zap_memory,
            'firmware_power':firmware_power,'gfx_regulator':gfx_regulator,'hardware_boot_verified':False}


def rpm_transport_version(kernel):
    """Inspect the actual collection, not a sidecar's claimed RPM version."""
    spec = importlib.util.spec_from_file_location('gpu_boot_verifier',ROOT/'scripts/verify-kernelcache.py')
    verifier = importlib.util.module_from_spec(spec);spec.loader.exec_module(verifier)
    _,kexts = verifier.inventory.inspect(kernel)
    matches = [k for k in kexts if k['CFBundleIdentifier']=='org.darwin.driver.QcomRPM']
    version = matches[0].get('CFBundleVersion','') if len(matches)==1 else ''
    if not isinstance(version,str) or not re.fullmatch(r'\d+(?:\.\d+){0,2}',version):
        raise ValueError('requires exactly one versioned QcomRPM in the actual kernel collection')
    return version


def require_gpu_rpm_transport(kernel):
    version = rpm_transport_version(kernel)
    if tuple((list(map(int,version.split('.')))+[0,0])[:3]) < (1,11,0):
        raise ValueError('--gpu-handoff-profile requires QcomRPM 1.11.0 or newer for ordered PMIC clock control, explicit clock limits and STATE bounds')
    return version


def require_gpu_gfx_runtime(kernel):
    spec = importlib.util.spec_from_file_location('gpu_gfx_verifier',ROOT/'scripts/verify-kernelcache.py')
    verifier = importlib.util.module_from_spec(spec);spec.loader.exec_module(verifier)
    _,kexts = verifier.inventory.inspect(kernel)
    minimum = {'org.darwin.driver.QcomSPMI':(1,3,0),'org.darwin.driver.QcomGPU':(1,62,0)}
    versions = {}
    for identity,required in minimum.items():
        matches = [k for k in kexts if k.get('CFBundleIdentifier') == identity]
        value = matches[0].get('CFBundleVersion','') if len(matches)==1 else ''
        if not isinstance(value,str) or not re.fullmatch(r'\d+(?:\.\d+){0,2}',value) or \
                tuple((list(map(int,value.split('.')))+[0,0])[:3]) < required:
            raise ValueError('cold GPU GFX handoff requires '+identity+' '+'.'.join(map(str,required)))
        versions[identity] = value
    return versions


def require_usb_rpm_transport(kernel):
    version = rpm_transport_version(kernel)
    if tuple((list(map(int,version.split('.')))+[0,0])[:3]) < (1,5,0):
        raise ValueError('--usb-rpm-cold-handoff requires QcomRPM 1.5.0 or newer in the actual collection')
    return version


def check_usb_resource_versions(kexts):
    minimum={'org.darwin.driver.QcomUSB':(1,13,0),'org.darwin.driver.QcomRPM':(1,13,0),
             'org.darwin.driver.QcomGCC':(1,2,0),'org.darwin.driver.GenericClockController':(1,3,0),
             'org.mi6.driver.GenericARMPlatform':(1,1,0),
             'org.darwin.driver.QcomPowerController':(1,2,0),'org.darwin.driver.QcomSPMI':(1,1,0),
             'org.darwin.driver.QcomPMIC':(1,1,0),'org.darwin.driver.QcomInterconnect':(1,0,0)}
    versions={}
    for identity,required in minimum.items():
        matches=[k for k in kexts if k.get('CFBundleIdentifier')==identity]
        value=matches[0].get('CFBundleVersion','') if len(matches)==1 else ''
        if not isinstance(value,str) or not re.fullmatch(r'\d+(?:\.\d+){0,2}',value) or tuple((list(map(int,value.split('.')))+[0,0])[:3])<required:
            raise ValueError('USB resource profile requires actual '+identity+' '+'.'.join(map(str,required))+' or newer')
        versions[identity]=value
    return versions


def require_usb_resource_runtime(kernel):
    spec=importlib.util.spec_from_file_location('usb_resource_verifier',ROOT/'scripts/verify-kernelcache.py')
    verifier=importlib.util.module_from_spec(spec);spec.loader.exec_module(verifier)
    _,kexts=verifier.inventory.inspect(kernel)
    return check_usb_resource_versions(kexts)


def pack_gpu_firmware(directory, output):
    """Package original CP/PFP/GPMU files; runtime C++ owns format validation."""
    data = bytearray(80); records = []
    for identity,name in enumerate(('a530_pm4.fw','a530_pfp.fw','a540_gpmu.fw2')):
        path = directory.resolve(strict=True)/name
        size = path.stat().st_size
        if not path.is_file() or not 8 <= size <= 1024*1024 or size % 4:
            raise ValueError('invalid GPU microcode input size: '+name)
        payload = path.read_bytes()
        if len(payload) != size: raise ValueError('GPU microcode changed during packaging')
        data += bytes((-len(data))%16)
        struct.pack_into('<4I',data,32+16*identity,identity,len(data),size,0)
        data += payload
        records.append({'name':name,'bytes':size,'sha256':hashlib.sha256(payload).hexdigest()})
    struct.pack_into('<8I',data,0,0x57464751,1,3,len(data),16,0,0,0)
    output.write_bytes(data)
    return {'archive_version':1,'archive_sha256':hashlib.sha256(data).hexdigest(),'inputs':records,
            'firmware_bytes_rewritten':False,'hardware_boot_verified':False,
            'authentication':'CP/PFP/GPMU are unsigned microcode; signed ZAP still requires real PAS-13',
            'requires_retained_clocks':True,'requires_retained_smmu':True,'resets_and_initializes_cp_gpmu':True}


def pack_gpu_zap(mdt_path, output):
    """Wrap original signed MDT/MBN and bXX files without rewriting firmware.

    The production C++ MDT parser owns layout, bounds and relocation decisions.
    Packing/hashing here supplies no authentication verdict.
    """
    mdt_path = mdt_path.resolve(strict=True)
    if mdt_path.suffix.lower() not in ('.mdt','.mbn'):
        raise ValueError('GPU ZAP input must be an MDT or monolithic MBN image')
    files = [(0xffffffff,mdt_path)]
    for path in sorted(mdt_path.parent.glob(mdt_path.stem+'.b[0-9][0-9]')):
        files.append((int(path.suffix[2:],10),path))
    if len(files)>101: raise ValueError('too many GPU ZAP segments')
    data = bytearray(32+16*len(files)); records = []
    for index,(identity,path) in enumerate(files):
        size = path.stat().st_size
        if not path.is_file() or not 0 < size <= 32*1024*1024: raise ValueError('invalid GPU ZAP input size')
        payload = path.read_bytes()
        if len(payload)!=size: raise ValueError('GPU ZAP input changed during packaging')
        data += bytes((-len(data))%16)
        offset = len(data)
        if offset+size>32*1024*1024: raise ValueError('GPU ZAP archive exceeds 32MiB')
        struct.pack_into('<4I',data,32+16*index,identity,offset,size,0)
        data += payload
        records.append({'name':path.name,'identity':identity,'bytes':size,'sha256':hashlib.sha256(payload).hexdigest()})
    struct.pack_into('<8I',data,0,0x50415a51,1,len(files),len(data),16,0,0,0)
    output.write_bytes(data)
    return {'archive_version':1,'archive_sha256':hashlib.sha256(data).hexdigest(),'inputs':records,
            'firmware_bytes_rewritten':False,'authentication':'real PAS-13 required at EFI runtime',
            'signature_verified':False,'hardware_boot_verified':False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dts', type=Path, default=ROOT/'device_trees/xiaomi-sagit.dts')
    parser.add_argument('--android-dtb', type=Path)
    parser.add_argument('--arena-min', type=lambda s: int(s, 0), default=0xa0000000)
    parser.add_argument('--arena-mib', type=int, default=256,
                        help='contiguous RAM reserved for XNU, 64–1024 MiB in 2 MiB increments')
    parser.add_argument('--full-memory', action='store_true',
                        help='UEFI: pass all available RAM ranges, retaining firmware/device reservations')
    parser.add_argument('--smem-handoff', action='store_true',
                        help='UEFI: require final-map reserved SMEM before enabling the shared AP allocator; does not enable modem/ADSP')
    parser.add_argument('--baseband-firmware', type=Path,
                        help='prepare-baseband directory: stage pinned MBA/modem outside DT and require final-map memory reservations; EFI only, leaves modem disabled')
    parser.add_argument('--baseband-nv-policy', type=Path,
                        help='explicit original NV partition UUID/LUN/range/access policy for RMTFS; requires --baseband-firmware, embeds no NV contents')
    parser.add_argument('--kernel', type=Path, default=ROOT/'build/output/mach.development.sdm835')
    parser.add_argument('--apfs-report', type=Path, help='apfs-image report; select its System volume as root')
    parser.add_argument('--firmware-report', type=Path, help='prepared-firmware report; pass its original StaticTrustCache')
    parser.add_argument('--input-firmware', type=Path, help='prepare-input output; enable physical fingerprint Home input')
    parser.add_argument('--touch-handoff-profile', type=Path, help='explicit sagit QUP5/touch retained clocks, pins, shared supplies and quiesced firmware contract')
    parser.add_argument('--cpu-osm-handoff', action='store_true', help='initialize sagit v2 OSM/CPRh after EFI exit using per-chip fuses; publish runtime-only DVFS handoff')
    parser.add_argument('--touch-prepare', action='store_true', help='EFI: prepare idle sagit QUP5, touch-only pins/reset after EFI exit; publish handoff only after real RMI query, retaining display rails')
    parser.add_argument('--bluetooth-firmware', type=Path, help='prepare-bluetooth output; verified ROM/board assets and dedicated UART resource policy')
    parser.add_argument('--trust-cache', type=Path, action='append', default=[], help='additional raw/IM4P trust-cache module (first module is static)')
    parser.add_argument('--trust-file', type=Path, action='append', default=[], help='signed Mach-O file or directory for a separate engineering trust cache')
    parser.add_argument('--ufs-handoff', action='store_true', help='verify warm MSM8998 UFS ownership and post-EFI physical DMA before enabling XNU storage')
    parser.add_argument('--usb-topology-component', help='capture installed IORT and controller resources using ACPI SDT and USB Config revision0x10003; does not grant DMA or enable USB')
    parser.add_argument('--usb-resource-profile', type=Path, help='version2 USB board/RPM policy with27 clock requests,12 explicit active/sleep MAX limits and a clock-qualified CX floor; produce Apple resource nodes, require shared AP baseline ACKs covering inherited and target USB clocks before GCC changes and select live DMA capture including explicit unmatched-bypass; requires QcomRPM1.13, QcomPMIC1.1 and QcomUSB1.13')
    parser.add_argument('--usb-rpm-snapshot', action='store_true', help='capture the pinned EFI NPA request cache across EFI exit; requires USB topology, grants no resources or DMA')
    parser.add_argument('--rpm-snapshot', action='store_true', help='capture the same pinned EFI NPA request cache for platform diagnostics, without requiring a USB topology or granting resources')
    parser.add_argument('--usb-firmware-stop', action='store_true', help='retire pinned USB firmware software, verify RPM cold transport and halt/reset hardware before XNU; requires USB topology, grants no DMA mappings')
    parser.add_argument('--usb-rpm-cold-handoff', action='store_true', help='verify the pinned GLINK APSS cleanup and cold FIFO fence; includes firmware stop, requires USB topology and QcomRPM 1.5; grants no resources or DMA')
    parser.add_argument('--usb-hardware-stop', action='store_true', help='after EFI exit, halt the inherited USB controller and assert its dedicated BCR before XNU; includes RPM cold handoff, does not enable USB or grant DMA mappings')
    parser.add_argument('--display-probe', action='store_true', help='read the MSM8998 firmware display topology before EFI exit; does not enable the display driver')
    parser.add_argument('--gpu-probe', action='store_true', help='log read-only GPUCC/GCC snapshots and, with --usb-rpm-snapshot, the bounded firmware RPM cache; does not enable GPU submission')
    parser.add_argument('--display-handoff-profile', type=Path, help='validate and retain the firmware MSM8998 display through EFI exit before enabling QcomDisplay')
    parser.add_argument('--disable-display-backlight', action='store_true', help='omit display-backlight and WLED runtime handoff so native backlight drivers do not match; retain firmware illumination for rendering diagnostics')
    parser.add_argument('--display-smmu-handoff', action='store_true', help='transfer an exclusive MMSS display context, optionally initializing a disabled stage-1 bank after a real post-EFI VBIF DMA halt; requires display handoff and display-direct-scanout=1, runtime validation remains mandatory')
    parser.add_argument('--gpu-handoff-profile', type=Path, help='A540 EFI handoff policy: complete RPM baseline records, NoC budgets and vacant kernel IOVA window')
    parser.add_argument('--gpu-zap-firmware', type=Path, help='signed ZAP MDT/MBN; pack companion bXX files and authenticate with PAS-13 after EFI exits')
    parser.add_argument('--gpu-firmware', type=Path, help='directory with a530_pm4.fw, a530_pfp.fw and a540_gpmu.fw2; reset/init CP and GPMU in EFI bridge (requires ZAP and policy operating point)')
    parser.add_argument('--gpu-smmu-init', action='store_true', help='initialize unused A540 SMMU bank 0 / SID 0 with a private EFI page table; requires --gpu-firmware and retained power/clocks')
    parser.add_argument('--gpu-cold-start', action='store_true', help='ACK RPM policy, power collapsed GX/CX, initialize GPU clocks and hand RPM channel to QcomRPM 1.4; requires --gpu-smmu-init')
    parser.add_argument('--gpu-driver-image', type=Path, help='standalone APFS/HFS driver volume and adjacent create-volume JSON report; EFI-reserved RAM, native DiskImages mount at /System/Library/Extensions')
    parser.add_argument('--development-unsealed-root', action='store_true', help='explicit DEVELOPMENT-kernel mode for this rebuilt unsealed System')
    parser.add_argument('--development-no-sep', action='store_true', help='use native APFS DoNotUseSEP and AppleKeyStore aks-endpoint=0 policy')
    parser.add_argument('--development-legacy-data', action='store_true', help='use native legacy shared Data initialization instead of firmware e-apfs split user volumes (requires --development-no-sep)')
    parser.add_argument('--development-hactivation', action='store_true', help='select the iOS native Hactivation branch using the development-fused /chosen tuple; restricted to this Sagit port')
    parser.add_argument('--development-no-apple-pmgr', action='store_true', help='explicitly omit the Apple-only AMFI PMGRAON hardware latch on the audited development port; requires no-SEP and unsealed-root modes')
    parser.add_argument('--development-watchdog-policy', action='store_true', help='adapt the audited watchdogd hardware service allowlist to QualcommWatchdogTimer; requires unsealed-root mode')
    parser.add_argument('--development-unthrottled-io', action='store_true', help='start with XNU low-priority I/O throttling disabled; requires unsealed-root mode')
    parser.add_argument('--extra-boot-args', default='', help='append diagnostic XNU boot arguments')
    parser.add_argument('--serial-no-iolog', action='store_true', help='omit IOLog UART copies using native SERIALMODE_NO_IOLOG; retain os_log and console input/output')
    parser.add_argument('--usb-cdc-console', action='store_true', help='enable optional CDC ACM console; requires working USB resource handoff and explicit VID/PID')
    parser.add_argument('--usb-cdc-vid', type=lambda s: int(s, 0), help='USB vendor ID authorized for this device')
    parser.add_argument('--usb-cdc-pid', type=lambda s: int(s, 0), help='USB product ID within the selected vendor ID')
    parser.add_argument('--disable-console-mirror', action='store_true', help='keep serial output while disabling native text-console framebuffer mirroring')
    parser.add_argument('--max-vnodes', type=int, default=16384, help='native kern.maxvnodes platform default for an APFS system root (default: 16384)')
    parser.add_argument('--disable-system-sleep', action='store_true', help='use native sleep-disabled platform policy when no system suspend backend is available')
    parser.add_argument('--cpus', type=int, choices=range(1, 9), metavar='1..8',
                        help='select CPU count through the native XNU cpus= argument')
    parser.add_argument('--os-environment', help='chosen/osenvironment supplied by the boot loader, e.g. darwinos or recoveryos')
    parser.add_argument('--volatile-nvram-boot-args', action='store_true', help='seed current kernel boot arguments in the empty RAM-only NVRAM bank')
    parser.add_argument('--output', type=Path, default=ROOT/'build/bridge')
    parser.add_argument('--watchdog-seconds', type=int, choices=range(0, 32), default=15, metavar='0..31')
    parser.add_argument('--watchdog-test', action='store_true')
    parser.add_argument('--panic-test', action='store_true')
    args = parser.parse_args()
    if args.development_hactivation and not (args.development_no_sep and args.development_legacy_data and args.development_unsealed_root and args.apfs_report):
        parser.error('--development-hactivation requires --development-no-sep, --development-legacy-data, --development-unsealed-root and --apfs-report')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    if args.usb_cdc_console:
        if args.usb_cdc_vid is None or args.usb_cdc_pid is None:
            parser.error('--usb-cdc-console requires --usb-cdc-vid and --usb-cdc-pid')
        if not 1 <= args.usb_cdc_vid < 0xffff or not 1 <= args.usb_cdc_pid <= 0xffff:
            parser.error('USB CDC VID and PID must be nonzero 16-bit values')
    elif args.usb_cdc_vid is not None or args.usb_cdc_pid is not None:
        parser.error('USB CDC IDs require --usb-cdc-console')
    if args.cpu_osm_handoff and not args.watchdog_seconds:
        parser.error('--cpu-osm-handoff requires the recovery watchdog')
    if args.baseband_nv_policy and not args.baseband_firmware:
        parser.error('--baseband-nv-policy requires --baseband-firmware')
    if args.baseband_firmware: args.smem_handoff = True
    usb_profile=load_usb_resource_profile(args.usb_resource_profile) if args.usb_resource_profile else None
    if usb_profile:
        if args.usb_topology_component and args.usb_topology_component!=usb_profile['component']:
            parser.error('--usb-resource-profile and --usb-topology-component must name the same installed component')
        args.usb_topology_component=usb_profile['component'];args.usb_hardware_stop=True
    if args.usb_rpm_snapshot and not args.usb_topology_component:
        parser.error('--usb-rpm-snapshot requires --usb-topology-component')
    args.usb_hardware_stop = args.usb_hardware_stop or args.usb_firmware_stop or args.usb_rpm_cold_handoff
    args.usb_rpm_cold_handoff = args.usb_rpm_cold_handoff or args.usb_hardware_stop
    args.usb_firmware_stop = args.usb_firmware_stop or args.usb_rpm_cold_handoff
    if args.usb_firmware_stop and not args.usb_topology_component:
        parser.error('--usb-firmware-stop requires --usb-topology-component')
    args.usb_rpm_snapshot = args.usb_rpm_snapshot or args.usb_firmware_stop or args.rpm_snapshot
    if args.gpu_zap_firmware and not args.gpu_handoff_profile:
        parser.error('--gpu-zap-firmware requires --gpu-handoff-profile')
    if args.gpu_firmware and not args.gpu_zap_firmware:
        parser.error('--gpu-firmware requires --gpu-zap-firmware and --gpu-handoff-profile')
    if args.gpu_smmu_init and not args.gpu_firmware:
        parser.error('--gpu-smmu-init requires --gpu-firmware, --gpu-zap-firmware and --gpu-handoff-profile')
    if args.gpu_cold_start and not args.gpu_smmu_init:
        parser.error('--gpu-cold-start requires --gpu-smmu-init and owned firmware')
    if args.usb_rpm_cold_handoff and args.gpu_handoff_profile and not args.gpu_cold_start:
        parser.error('USB RPM cold handoff with a GPU profile requires --gpu-cold-start; a warm GPU cannot survive an unproven AP resource cleanup')
    if not 64 <= args.arena_mib <= 1024 or args.arena_mib % 2:
        parser.error('--arena-mib must be 64–1024 MiB in 2 MiB increments')
    arena_bytes = args.arena_mib * 1024 * 1024
    if not 0 <= args.arena_min <= (1 << 48)-arena_bytes:
        parser.error('--arena-min must be a nonnegative physical address below 48 bits')
    dtname = args.dts.stem
    dtbpath, applepath = out/(dtname+'.dtb'), out/(dtname+'.apple-dt')
    kernel = args.kernel.resolve()
    driver_image = None
    if args.gpu_driver_image:
        image = args.gpu_driver_image.resolve()
        data = image.read_bytes()
        metadata = json.loads(Path(str(image)+'.json').read_bytes())
        symbols = subprocess.check_output(['llvm-nm', '--defined-only', str(kernel)], text=True)
        if not any(line.split()[-1:] == ['_imageboot_setup_driver_image'] for line in symbols.splitlines()):
            parser.error('--gpu-driver-image requires native imageboot driver-volume support')
        if not args.development_unsealed_root:
            parser.error('--gpu-driver-image requires the explicit development filesystem mode')
        filesystem = metadata.get('filesystem','apfs')
        signature = data[32:36] == b'NXSB' if filesystem == 'apfs' else data[1024:1026] in (b'H+',b'HX') if filesystem == 'hfs' else False
        if not 8*1024*1024 <= len(data) <= 64*1024*1024 or len(data) % 0x4000 or not signature:
            parser.error('driver image must be an 8–64 MiB APFS/HFS volume aligned to 16 KiB')
        if not any(line.split()[-1:] == ['_imageboot_driver_image_version'] for line in symbols.splitlines()):
            parser.error('--gpu-driver-image requires native imageboot driver-volume ABI 2')
        image_sha = hashlib.sha256(data).hexdigest()
        if metadata['container_bytes'] != len(data) or metadata['image_sha256'] != image_sha:
            parser.error('driver image does not match its creation report')
        volume_uuid = str(uuid.UUID(metadata['volume_uuid'])).upper() if filesystem == 'apfs' else None
        (out/'driver-image.img').write_bytes(data)
        driver_image = {'source':str(image), 'sha256':image_sha, 'bytes':len(data),
                        'volume_uuid':volume_uuid, 'mountpoint':'/System/Library/Extensions',
                        'filesystem':filesystem, 'read_only':True, 'efi_only':True,
                        'hardware_mount_verified':False}
    if args.full_memory:
        symbols = subprocess.check_output(['llvm-nm', '--defined-only', str(kernel)], text=True)
        if not any(line.split()[-1:] == ['_pmap_boot_memory_contains'] for line in symbols.splitlines()):
            parser.error('--full-memory requires kernel support for firmware RAM ranges')
        if args.gpu_handoff_profile and not args.gpu_cold_start:
            parser.error('--full-memory cannot yet reclaim RAM alongside a fixed GPU handoff arena')
    usb_resource_versions=require_usb_resource_runtime(kernel) if usb_profile else None
    usb_rpm_version = require_usb_rpm_transport(kernel) if args.usb_rpm_cold_handoff else None
    gpu_rpm_version = require_gpu_rpm_transport(kernel) if args.gpu_handoff_profile else None
    gpu_gfx_versions = require_gpu_gfx_runtime(kernel) if args.gpu_cold_start else None
    amfi_platform = None
    if args.development_no_apple_pmgr:
        if not args.development_no_sep or not args.development_unsealed_root:
            parser.error('--development-no-apple-pmgr requires --development-no-sep and --development-unsealed-root')
        prepared_kernel = out/'development.kernelcache'
        if kernel == prepared_kernel:
            parser.error('AMFI input must differ from generated development.kernelcache')
        amfi_platform = prepare_no_apple_pmgr(kernel, prepared_kernel)
        kernel = prepared_kernel
    watchdog_policy = None
    if args.development_watchdog_policy:
        if not args.development_unsealed_root:
            parser.error('--development-watchdog-policy requires --development-unsealed-root')
        prepared_kernel = out/'watchdog-policy.kernelcache'
        if kernel == prepared_kernel:
            parser.error('watchdog policy input must differ from generated output')
        watchdog_policy = prepare_watchdog_service_policy(kernel, prepared_kernel)
        kernel = prepared_kernel
    io_policy = None
    if args.development_unthrottled_io:
        if not args.development_unsealed_root:
            parser.error('--development-unthrottled-io requires --development-unsealed-root')
        prepared_kernel = out/'io-policy.kernelcache'
        if kernel == prepared_kernel:
            parser.error('I/O policy input must differ from generated output')
        io_policy = prepare_unthrottled_io(kernel, prepared_kernel)
        kernel = prepared_kernel
    keystore_policy = None
    if args.development_no_sep and args.development_unsealed_root:
        prepared_kernel = out/'keystore-policy.kernelcache'
        if kernel == prepared_kernel:
            parser.error('keystore policy input must differ from generated output')
        keystore_policy = prepare_no_sep_keystore(kernel, prepared_kernel)
        if keystore_policy:
            kernel = prepared_kernel
    run('dtc', '-I', 'dts', '-O', 'dtb', '-i', ROOT/'device_trees', '-o', dtbpath, args.dts.resolve())
    tree = parse_fdt((dtbpath).read_bytes())
    hactivation = configure_development_hactivation(tree) if args.development_hactivation else None
    if (args.gpu_probe or args.rpm_snapshot) and b'qcom,msm8998' not in tree['props'].get('compatible', b'').split(b'\0'):
        parser.error('GPU/RPM diagnostic snapshots require MSM8998')
    vfs_limits = configure_vfs_limits(tree, args.max_vnodes, args.disable_system_sleep) if args.apfs_report else None
    usb_topology = configure_usb_topology(tree, args.usb_topology_component, args.usb_rpm_snapshot, args.usb_firmware_stop, args.usb_hardware_stop) if args.usb_topology_component else None
    rpm_snapshot = None
    if args.usb_rpm_snapshot:
        if not usb_topology:
            next(n for n in tree['children'] if n['name'] == 'chosen')['props']['qcom,usb-firmware-rpm'] = bytes(20544)
        source = ROOT/'boot/edk2-msm8998/Platforms/Msm8998Binaries/common/Binaries/NpaDxe/NpaDxe.efi'
        rpm_snapshot = {'bytes':20544, 'observation_only':True,
                        'firmware_pin':pack_usb_npa_pin(source, out/'usb-npa-pin.bin')}
        if usb_topology:
            usb_topology['rpm_snapshot']['firmware_pin'] = rpm_snapshot['firmware_pin']
    if args.usb_firmware_stop:
        source = ROOT/'boot/edk2-msm8998/Platforms/Msm8998Binaries/common/Binaries/UsbConfigDxe/UsbConfigDxe.efi'
        usb_topology['firmware_stop']['firmware_pin'] = pack_usb_config_pin(source, out/'usb-config-pin.bin')
    if args.display_smmu_handoff and not args.display_handoff_profile:
        parser.error('--display-smmu-handoff requires --display-handoff-profile')
    if args.disable_display_backlight and not args.display_handoff_profile:
        parser.error('--disable-display-backlight requires --display-handoff-profile')
    display_handoff = configure_display_handoff(tree, args.display_handoff_profile, args.display_smmu_handoff,
        args.disable_display_backlight) if args.display_handoff_profile else None
    gpu_handoff = configure_gpu_handoff(tree, args.gpu_handoff_profile, args.gpu_cold_start) if args.gpu_handoff_profile else None
    usb_resources=configure_usb_resources(tree,usb_profile,args.gpu_cold_start) if usb_profile else None
    if usb_resources:usb_resources['actual_native_versions']=usb_resource_versions
    if args.usb_rpm_cold_handoff:
        report = configure_usb_rpm_cold(tree,args.gpu_cold_start)
        source = ROOT/'boot/edk2-msm8998/Platforms/Msm8998Binaries/common/Binaries/GlinkDxe/GlinkDxe.efi'
        report['firmware_pin'] = pack_usb_glink_pin(source,out/'usb-glink-pin.bin')
        report['native_RPM_version'] = usb_rpm_version
        usb_topology['rpm_cold_handoff'] = report
    if args.gpu_firmware and 'firmware_power' not in json.loads(args.gpu_handoff_profile.read_bytes()):
        parser.error('--gpu-firmware requires firmware_power in the GPU handoff policy')
    gpu_zap = pack_gpu_zap(args.gpu_zap_firmware,out/'gpu-zap.bin') if args.gpu_zap_firmware else None
    gpu_firmware = pack_gpu_firmware(args.gpu_firmware,out/'gpu-firmware.bin') if args.gpu_firmware else None
    if gpu_handoff:
        gpu_handoff['submission_probe'] = {
            'enabled':bool(gpu_zap),'commands':['Fill32','Barrier','Copy32','Barrier'],
            'workspace_bytes':16384,'ib_gpu_read_only':True,'guarded_source_and_destination':True,
            'completion':'hardware CACHE_FLUSH_TS followed by CP halt and AXI drain',
            'runtime_success_required_before_publication':bool(gpu_zap),'hardware_boot_verified':False,
        }
    if gpu_firmware:
        soc=next(n for n in tree['children'] if n['name']=='soc' or n['name'].startswith('soc@'))
        gpu=next(n for n in soc['children'] if n['name']=='gpu@5000000')
        gpu['props']['qcom,gpu-firmware-handoff']=bytes(64)
        gpu_firmware['native_recovery_handoff_bytes']=64
        gpu_handoff['mode'] = 'A540 CP/GPMU initialization with retained clocks and SMMU'
        gpu_handoff['cp_and_smmu_addresses'] = 'CP bases assigned from bridge boot mappings; SMMU captured from hardware'
        gpu_firmware['requires_retained_smmu'] = not args.gpu_smmu_init
    if args.gpu_smmu_init:
        gpu_handoff['mode'] = 'A540 CP/GPMU and SMMU bank-0 initialization with retained power/clocks'
        gpu_handoff['cp_and_smmu_addresses'] = 'CP GPU VAs from boot mappings; SMMU root in private EFI pages, bank 0 / SID 0 / ASID 0'
    if args.gpu_cold_start:
        gpu_handoff['mode'] = 'A540 cold GX/CX/clock/CP/GPMU/SMMU initialization and RPM channel transfer'
        gpu_firmware['requires_retained_clocks'] = False
    input_firmware = configure_input_firmware(tree, args.input_firmware) if args.input_firmware else None
    if args.touch_prepare and not args.touch_handoff_profile:
        raise ValueError('--touch-prepare requires --touch-handoff-profile')
    touch_handoff = configure_touch_handoff(tree, args.touch_handoff_profile, args.touch_prepare) if args.touch_handoff_profile else None
    cpu_osm_handoff = configure_osm_handoff(tree) if args.cpu_osm_handoff else None
    if cpu_osm_handoff:
        run(sys.executable, ROOT/'scripts/generate-cprh-tables.py', '--check')
        cpu_osm_handoff['source_manifest_sha256'] = hashlib.sha256(
            (ROOT/'docs/power/osm-reference-sources.json').read_bytes()).hexdigest()
        cpu_osm_handoff['generated_policy_sha256'] = hashlib.sha256(
            (ROOT/'drivers/clock/QcomOSM/CPRhSagitV2Tables.h').read_bytes()).hexdigest()
        cpu_osm_handoff['firmware_contract_sha256'] = hashlib.sha256(
            (ROOT/'drivers/clock/QcomOSM/OSMTZContract.h').read_bytes()).hexdigest()
    bluetooth_firmware = configure_bluetooth_firmware(tree,args.bluetooth_firmware) if args.bluetooth_firmware else None
    apfs_root = configure_apfs_root(tree, json.loads(args.apfs_report.read_text())) if args.apfs_report else None
    filesystem_policy = configure_firmware_filesystems(tree, args.firmware_report, out,
        ROOT/'tools/prefix/bin/ipsw') if args.firmware_report else None
    if args.development_no_sep:
        if not filesystem_policy:
            parser.error('--development-no-sep requires --firmware-report')
    if args.development_legacy_data:
        if not args.development_no_sep:
            parser.error('--development-legacy-data requires --development-no-sep')
        filesystems = next(n for n in tree['children'] if n['name'] == 'filesystems')
        # The reconstructed Data volume uses /private/var directly. Advertising
        # e-apfs would require pre-provisioned per-user encrypted volume sessions.
        filesystems['props'].pop('e-apfs', None)
        filesystem_policy['development_legacy_data'] = True
        # keybagd's native shared-Data path permits absent per-device keybags;
        # usermanagerd still uses DT e-apfs to select split user volumes.
        chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
        chosen['props']['bootargs'] = chosen['props']['bootargs'].rstrip(b'\0') + b' -apfs_shared_datavolume\0'
    if args.os_environment:
        environment = args.os_environment.encode('ascii')
        if len(environment) > 31 or b'\0' in environment:
            parser.error('invalid OS environment string')
        next(n for n in tree['children'] if n['name']=='chosen')['props']['osenvironment'] = environment+b'\0'
    modules = []; trust_report = {'modules': []}
    if args.firmware_report:
        module, provenance = firmware_static(args.firmware_report)
        modules.append(module); trust_report['firmware'] = provenance
    for path in args.trust_cache:
        modules.append(raw_module(path.read_bytes()))
    if args.trust_file:
        if not modules:
            parser.error('--trust-file requires a static --firmware-report or --trust-cache first')
        module, files = development_module(args.trust_file)
        modules.append(module); trust_report['development_files'] = files
    trustpath = out/'trust-cache.bin'
    trustpath.write_bytes(handoff(modules) if modules else b'')
    for module in modules:
        trust_report['modules'].append(module_info(module))
    if modules:
        chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
        memory_map = next(n for n in chosen['children'] if n['name'] == 'memory-map')
        memory_map['props']['TrustCache'] = bytes(16)
    if driver_image:
        chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
        memory_map = next(n for n in chosen['children'] if n['name'] == 'memory-map')
        memory_map['props']['DriverImage'] = bytes(16)
        chosen['props']['driver-image-fstype'] = driver_image['filesystem'].encode('ascii')+b'\0'
        if driver_image['volume_uuid']:
            chosen['props']['driver-image-volume-uuid'] = driver_image['volume_uuid'].encode('ascii')+b'\0'
    if args.extra_boot_args:
        args.extra_boot_args.encode('ascii')
        chosen=next(n for n in tree['children'] if n['name']=='chosen')
        chosen['props']['bootargs']=chosen['props']['bootargs'].rstrip(b'\0')+b' '+args.extra_boot_args.encode('ascii')+b'\0'
    usb_cdc_console = configure_usb_cdc_console(tree, args.usb_cdc_vid, args.usb_cdc_pid) if args.usb_cdc_console else None
    serial_mode = configure_serial_no_iolog(tree) if args.serial_no_iolog else None
    if args.disable_console_mirror:
        disable_console_mirror(tree)
    if args.development_no_sep:
        configure_no_sep(tree)
        filesystem_policy['development_no_sep'] = True
        filesystem_policy['native_aks_endpoint'] = False
    if args.development_unsealed_root:
        if not apfs_root:
            parser.error('--development-unsealed-root requires --apfs-report')
        chosen=next(n for n in tree['children'] if n['name']=='chosen')
        chosen['props']['bootargs']=chosen['props']['bootargs'].rstrip(b'\0')+b' development-unsealed-root=1\0'
    if args.cpus is not None:
        configure_cpu_count(tree, args.cpus)
    if args.ufs_handoff:
        if not args.apfs_report or args.dts.resolve() != ROOT/'device_trees/xiaomi-sagit.dts':
            parser.error('UFS handoff currently requires the verified sagit profile and --apfs-report')
        soc = next(n for n in tree['children'] if n['name'] == 'soc' or n['name'].startswith('soc@'))
        ufs = next(n for n in soc['children'] if n['name'] == 'storage@1da4000')
        ufs['props']['reg'] = struct.pack('>8I', 0,0x1da4000,0,0x2500,0,0x1da7000,0,0x1000)
        ufs['props']['status'] = b'okay\0'
        for key, val in {'qcom,ufs-handoff-version':0,'qcom,ufs-handoff-flags':0,
                         'qcom,ufs-polled-completion':0,
                         'qcom,ufs-hs-gear':0,
                         'qcom,ufs-hs-power-mode':0,
                         'qcom,ufs-core-clock-hz':200000000,'qcom,ufs-unipro-clock-hz':150000000,
                         'dma-address-bits':40,'lanes-per-direction':1}.items():
            ufs['props'][key] = struct.pack('>I',val)
    if args.panic_test:
        chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
        chosen['props']['bootargs'] = chosen['props']['bootargs'].rstrip(b'\0')+b' software-panic-test=1\0'
    if args.volatile_nvram_boot_args:
        chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
        props = chosen_properties(chosen['props'])
        chosen['props']['nvram-proxy-data'] = with_boot_args(props['nvram-proxy-data'],
            chosen['props']['bootargs'].rstrip(b'\0'))
    if args.full_memory:
        chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
        chosen['props']['boot-memory-ranges'] = bytes(256 * 16)
        chosen['props']['boot-memory-count'] = bytes(4)
        chosen['props']['boot-memory-bootstrap-end'] = bytes(8)
    smem_handoff = configure_smem_handoff(tree) if args.smem_handoff else None
    baseband_handoff = configure_baseband_handoff(tree, args.baseband_firmware, out, ROOT) if args.baseband_firmware else None
    rmtfs_storage = configure_rmtfs_storage_policy(tree, args.baseband_nv_policy) if args.baseband_nv_policy else None
    if baseband_handoff: baseband_handoff['rmtfs_storage_policy_provided'] = bool(rmtfs_storage)
    apple_blob = apple_dt(tree, secrets.token_bytes(64))
    if len(apple_blob)>0x400000:
        raise ValueError('Apple device tree exceeds the bridge 4 MiB bound')
    (applepath).write_bytes(apple_blob)
    asm = '.section .rdata,"dr"\n.p2align 4\n'
    payloads = [('kernel', kernel), ('dt', applepath), ('trust_cache', trustpath)]
    if args.usb_rpm_snapshot: payloads.append(('usb_npa_pin',out/'usb-npa-pin.bin'))
    if args.usb_firmware_stop: payloads.append(('usb_config_pin',out/'usb-config-pin.bin'))
    if args.usb_rpm_cold_handoff: payloads.append(('usb_glink_pin',out/'usb-glink-pin.bin'))
    if gpu_zap: payloads.append(('gpu_zap',out/'gpu-zap.bin'))
    if gpu_firmware: payloads.append(('gpu_firmware',out/'gpu-firmware.bin'))
    if driver_image: payloads.append(('driver_image',out/'driver-image.img'))
    for name, path in payloads:
        # Assembly paths are controlled local files, reject quoting/newline injection.
        if any(c in str(path) for c in '\n"\\'): raise ValueError('unsupported path')
        asm += f'.globl {name}_blob\n{name}_blob:\n.incbin "{path}"\n.globl {name}_end\n{name}_end:\n.p2align 4\n'
    (out/'payload.S').write_text(asm)
    if baseband_handoff:
        path = out/'baseband-firmware.bin'
        if any(c in str(path) for c in '\n"\\'): raise ValueError('unsupported baseband payload path')
        (out/'baseband-payload.S').write_text('.section .rdata,"dr"\n.p2align 4\n.globl baseband_firmware_blob\nbaseband_firmware_blob:\n.incbin "'+str(path)+'"\n.globl baseband_firmware_end\nbaseband_firmware_end:\n')
    inc = ROOT/'boot/edk2-msm8998/Common/edk2/MdePkg/Include'
    # Apple DT properties are only 4-byte aligned. EDK2 can enable SCTLR.A;
    # prevent Clang from folding e.g. the CPU state copy into an unaligned STR X.
    common = ['--target=aarch64-unknown-windows', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-asynchronous-unwind-tables', '-mstrict-align', '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'boot/bridge'), '-I'+str(inc), '-I'+str(inc/'AArch64')]
    common += ['-DBRIDGE_WATCHDOG_SECONDS='+str(args.watchdog_seconds), '-DBRIDGE_MIN_PHYS='+str(args.arena_min)+'ULL']
    common += ['-DBRIDGE_ARENA_SIZE='+str(arena_bytes)+'ULL']
    if args.full_memory:
        common += ['-DBRIDGE_FULL_MEMORY=1']
    if args.smem_handoff:
        common += ['-DBRIDGE_SMEM_HANDOFF=1']
    if baseband_handoff:
        common += ['-DBRIDGE_BASEBAND_HANDOFF=1', '-I'+str(ROOT/'drivers/include'), '-I'+str(ROOT/'drivers/baseband/QcomBaseband')]
    if driver_image:
        common += ['-DBRIDGE_DRIVER_IMAGE=1']
    if args.ufs_handoff:
        common += ['-DBRIDGE_UFS_HANDOFF=1']
    if usb_topology:
        common += ['-DBRIDGE_USB_TOPOLOGY=1']
    if args.usb_rpm_snapshot:
        common += ['-DBRIDGE_USB_RPM_SNAPSHOT=1', '-I'+str(ROOT/'drivers/include')]
    if args.usb_firmware_stop:
        common += ['-DBRIDGE_USB_FIRMWARE_STOP=1']
    if args.usb_hardware_stop:
        common += ['-DBRIDGE_USB_HARDWARE_STOP=1']
    if args.usb_rpm_cold_handoff:
        common += ['-DBRIDGE_RPM_COLD_HANDOFF=1']
    if args.display_probe:
        common += ['-DBRIDGE_DISPLAY_PROBE=1']
    if args.gpu_probe:
        common += ['-DBRIDGE_GPU_PROBE=1']
    if display_handoff:
        common += ['-DBRIDGE_DISPLAY_HANDOFF=1']
    if args.touch_prepare:
        common += ['-DBRIDGE_TOUCH_PREPARE=1', '-I'+str(ROOT/'drivers/include')]
    if args.cpu_osm_handoff:
        common += ['-DBRIDGE_CPU_OSM_HANDOFF=1', '-I'+str(ROOT/'drivers/include')]
    if args.display_smmu_handoff:
        common += ['-DBRIDGE_DISPLAY_SMMU_HANDOFF=1', '-I'+str(ROOT/'drivers/include')]
    if gpu_handoff:
        common += ['-DBRIDGE_GPU_HANDOFF=1', '-I'+str(ROOT/'drivers/include')]
    if gpu_zap:
        common += ['-DBRIDGE_GPU_ZAP=1']
    if gpu_firmware:
        common += ['-DBRIDGE_GPU_FIRMWARE=1']
    if args.gpu_smmu_init:
        common += ['-DBRIDGE_GPU_SMMU_INIT=1']
    if args.gpu_cold_start:
        common += ['-DBRIDGE_GPU_COLD_START=1']
    if args.watchdog_test:
        if not args.watchdog_seconds: raise ValueError('self-test requires enabled watchdog')
        common += ['-DBRIDGE_WATCHDOG_TEST=1']
    objs = []
    efi_sources = [ROOT/'boot/bridge/core.c', ROOT/'boot/bridge/platform.c', ROOT/'boot/bridge/uefi.c', ROOT/'boot/bridge/enter.S', out/'payload.S']
    if args.ufs_handoff:
        efi_sources.append(ROOT/'boot/bridge/ufs_handoff.c')
    if args.smem_handoff:
        efi_sources.append(ROOT/'boot/bridge/smem_handoff.c')
    if baseband_handoff:
        efi_sources += [ROOT/'boot/bridge/baseband_handoff.cpp', out/'baseband-payload.S']
    if usb_topology:
        efi_sources.append(ROOT/'boot/bridge/usb_topology.cpp')
    if args.usb_rpm_snapshot:
        efi_sources.append(ROOT/'boot/bridge/usb_rpm.cpp')
    if args.usb_firmware_stop:
        efi_sources.append(ROOT/'boot/bridge/usb_firmware_stop.cpp')
    if args.usb_hardware_stop:
        efi_sources.append(ROOT/'boot/bridge/usb_hardware_stop.cpp')
    if args.display_probe or display_handoff:
        efi_sources.append(ROOT/'boot/bridge/display_handoff.c')
    if args.touch_prepare:
        efi_sources.append(ROOT/'boot/bridge/touch_handoff.cpp')
    if args.cpu_osm_handoff:
        efi_sources.append(ROOT/'boot/bridge/osm_handoff.cpp')
    if args.display_smmu_handoff:
        efi_sources.append(ROOT/'boot/bridge/display_smmu.cpp')
    if gpu_handoff:
        efi_sources.append(ROOT/'boot/bridge/gpu_handoff.cpp')
    for src in efi_sources:
        cpp = ['-std=c++17','-fno-rtti','-fno-exceptions','-fno-threadsafe-statics','-fno-use-cxa-atexit'] if src.suffix == '.cpp' else []
        obj = out/(src.stem+'.obj'); run('clang', *common, *cpp, '-c', src, '-o', obj); objs.append(obj)
    run('lld-link', '/machine:arm64', '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib', '/base:0', '/filealign:512', '/out:'+str(out/'XnuBridge.efi'), *objs)
    # Same loader core, with a Linux arm64 Image entry instead of EFI services.
    (out/'raw-payload.S').write_text(asm.replace('.section .rdata,"dr"', '.section .rodata,"a"'))
    rawcommon = ['--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                 '-fpie', '-fvisibility=hidden', '-mstrict-align', '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-I'+str(ROOT/'boot/bridge'), '-DBRIDGE_WATCHDOG_SECONDS='+str(args.watchdog_seconds),
                 '-DBRIDGE_MIN_PHYS='+str(args.arena_min)+'ULL',
                 '-DBRIDGE_ARENA_SIZE='+str(arena_bytes)+'ULL']
    rawobjs = []
    if args.usb_firmware_stop:
        rawcommon += ['-DBRIDGE_USB_FIRMWARE_STOP=1']
    if gpu_handoff:
        rawcommon += ['-DBRIDGE_GPU_HANDOFF=1']
    if driver_image:
        rawcommon += ['-DBRIDGE_DRIVER_IMAGE=1']
    for src in [ROOT/'boot/bridge/core.c', ROOT/'boot/bridge/platform.c', ROOT/'boot/bridge/fdt.c', ROOT/'boot/bridge/raw.c', ROOT/'boot/bridge/enter.S', ROOT/'boot/bridge/raw-entry.S', out/'raw-payload.S']:
        obj = out/('raw-'+src.stem+'.o'); run('clang', *rawcommon, '-c', src, '-o', obj); rawobjs.append(obj)
    run('ld.lld', '-pie', '--no-dynamic-linker', '-Bsymbolic', '-z', 'max-page-size=4096', '-T', ROOT/'boot/bridge/raw.ld', '-o', out/'bridge.elf', *rawobjs)
    run('llvm-objcopy', '-O', 'binary', out/'bridge.elf', out/'Image')
    raw = (out/'Image').read_bytes()
    if raw[56:60] != b'ARM\x64': raise ValueError('invalid ARM64 Image magic')
    image_size, = struct.unpack_from('<Q', raw, 16)
    if not len(raw) <= image_size <= 0x4000000: raise ValueError('invalid ARM64 Image size')
    raw_relocations = verify_raw_relocations(out/'bridge.elf', image_size)
    raw = raw.ljust(image_size, b'\0'); (out/'Image').write_bytes(raw)
    android_dtb = args.android_dtb
    if android_dtb is None and args.dts.resolve() == ROOT/'device_trees/xiaomi-sagit.dts':
        android_dtb = ROOT/'boot/edk2-msm8998/Platforms/Msm8998Pkg/Device/xiaomi/sagit/DeviceTreeBlob/Android/Android-sagit.dtb'
    firmware_dtb = android_dtb.resolve().read_bytes() if android_dtb else b''
    artifacts = ['XnuBridge.efi', dtbpath.name, applepath.name, 'Image']
    if baseband_handoff: artifacts.append('baseband-firmware.bin')
    if args.usb_rpm_snapshot: artifacts.append('usb-npa-pin.bin')
    if args.usb_firmware_stop: artifacts.append('usb-config-pin.bin')
    if args.usb_rpm_cold_handoff: artifacts.append('usb-glink-pin.bin')
    if gpu_zap: artifacts.append('gpu-zap.bin')
    if gpu_firmware: artifacts.append('gpu-firmware.bin')
    if driver_image: artifacts.append('driver-image.img')
    if modules:
        artifacts.append('trust-cache.bin')
    chosen = next((n for n in tree['children'] if n['name'] == 'chosen'), None)
    nvram = chosen_properties(chosen['props']) if chosen else {}
    if 'nvram-proxy-data' in nvram:
        (out/'nvram-proxy.bin').write_bytes(nvram['nvram-proxy-data'])
        artifacts.append('nvram-proxy.bin')
    if firmware_dtb:
        # ABL selects its own board DTB and fills actual RAM/reservation data.
        (out/'Image.gz-dtb').write_bytes(gzip.compress(raw, mtime=0)+firmware_dtb)
        run('python3', ROOT/'boot/edk2-msm8998/mkbootimg.py', '--header_version', '0',
            '--kernel', out/'Image.gz-dtb', '--base', '0x80000000', '--kernel_offset', '0x80000',
            '--tags_offset', '0x1e00000', '--ramdisk_offset', '0x2000000', '--pagesize', '4096',
            '--cmdline', 'console=ttyMSM0,115200n8 androidboot.hardware=qcom', '-o', out/'boot-bridge.img')
        boot = (out/'boot-bridge.img').read_bytes()
        size, address = struct.unpack_from('<II', boot, 8)
        if boot[:8] != b'ANDROID!' or address != 0x80080000 or boot[4096:4096+size] != (out/'Image.gz-dtb').read_bytes():
            raise ValueError('invalid Android boot v0 wrapper')
        artifacts.append('boot-bridge.img')
    report = {'panic_test': args.panic_test, 'watchdog_seconds': args.watchdog_seconds, 'watchdog_test': args.watchdog_test, 'kernel': str(kernel), 'device_tree_source': str(args.dts.resolve()), 'android_dtb_sha256': hashlib.sha256(firmware_dtb).hexdigest() if firmware_dtb else None, 'android_boot_hardware_verified': False, 'input_firmware': input_firmware, 'touch_handoff': touch_handoff, 'bluetooth_firmware': bluetooth_firmware, 'artifacts': {}}
    for name in artifacts:
        b = (out/name).read_bytes(); report['artifacts'][name] = {'bytes': len(b), 'sha256': hashlib.sha256(b).hexdigest()}
    panic_stamp = ROOT/'build/.software-panic-sdm835'
    report['kernel_software_panic'] = int(panic_stamp.read_text()) if kernel == ROOT/'build/output/mach.development.sdm835' and panic_stamp.exists() else None
    report['raw_relative_relocations'] = raw_relocations
    report['apfs_root'] = apfs_root
    report['filesystem_policy'] = filesystem_policy
    report['os_environment'] = args.os_environment
    report['trust_cache'] = trust_report if modules else None
    report['ufs_handoff'] = {'requested':args.ufs_handoff,'efi_only':True,'hardware_boot_verified':False}
    report['smem_handoff'] = smem_handoff
    report['baseband_handoff'] = baseband_handoff
    report['cpu_osm_handoff'] = cpu_osm_handoff
    report['rmtfs_storage_policy'] = rmtfs_storage
    report['usb_topology'] = usb_topology
    report['usb_resources'] = usb_resources
    report['display_probe'] = args.display_probe
    report['gpu_probe'] = args.gpu_probe
    report['rpm_snapshot'] = rpm_snapshot
    report['display_handoff'] = display_handoff
    report['gpu_handoff'] = gpu_handoff
    report['driver_image'] = driver_image
    report['gpu_gfx_runtime_versions'] = gpu_gfx_versions
    report['gpu_zap_firmware'] = gpu_zap
    report['gpu_firmware'] = gpu_firmware
    report['gpu_smmu_init'] = {'requested':args.gpu_smmu_init,'bank':0,'sid':0,'asid':0,
        'root':'private EFI UC/CPU-XP page','requires_unused_context':not args.gpu_cold_start,
        'requires_retained_power_clocks':not args.gpu_cold_start,'hardware_boot_verified':False}
    report['gpu_cold_start'] = {'requested':args.gpu_cold_start,'rpm_kext_version':gpu_rpm_version,
        'rpm_policy_version':2 if args.gpu_cold_start else None,'rpm_channel_bytes':48,
        'requires_initially_collapsed_gx':True,'shared_control_bus_xo_gpll_and_analogue_config_from_firmware':True,
        'hardware_boot_verified':False}
    if gpu_handoff:
        notice = ROOT/'drivers/security/QcomSCM/NOTICE.txt'
        (out/'NOTICE.GPU.txt').write_text('The optional GPU bridge links GPL-2.0-only clock, SMMU and RPM helpers.\nboot/bridge/gpu_firmware.h, gpu_smmu.h and gpu_power.h adapt Linux A540, GPMU, SMMU, GPUCC/GDSC/Fabia and RPM protocols.\nCopyright (C) 2013 ARM Limited. Copyright (c) 2015-2019 The Linux Foundation. All rights reserved.\nCopyright (c) 2019 Jeffrey Hugo. Copyright (c) 2021, 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.\nComplete corresponding bridge/driver sources and pinned references are in this repository.\n\n'+notice.read_text())
        data = (out/'NOTICE.GPU.txt').read_bytes()
        report['artifacts']['NOTICE.GPU.txt'] = {'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
    if args.display_smmu_handoff:
        data=(ROOT/'drivers/display/QcomDisplay/NOTICE.SMMU.txt').read_bytes()
        (out/'NOTICE.DISPLAY.txt').write_bytes(data)
        report['artifacts']['NOTICE.DISPLAY.txt']={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
    report['development_unsealed_root'] = args.development_unsealed_root
    report['development_no_sep'] = args.development_no_sep
    report['development_hactivation'] = hactivation or {'requested': False}
    report['keystore_endpoint_policy'] = keystore_policy
    report['development_legacy_data'] = args.development_legacy_data
    report['amfi_platform'] = amfi_platform
    report['watchdog_service_policy'] = watchdog_policy
    report['io_throttle_policy'] = io_policy
    report['extra_boot_args'] = args.extra_boot_args
    report['serial_mode_override'] = serial_mode
    report['usb_cdc_console'] = usb_cdc_console
    report['console_mirror_disabled'] = args.disable_console_mirror
    report['vfs_limits'] = vfs_limits
    report['cpu_count_requested'] = args.cpus
    report['arena_bytes_requested'] = arena_bytes
    report['full_memory_requested'] = args.full_memory
    report['volatile_nvram_boot_args'] = args.volatile_nvram_boot_args
    report['strict_alignment'] = {'efi': True, 'raw': True}
    report['kernel_sha256'] = hashlib.sha256(kernel.read_bytes()).hexdigest()
    report['nvram'] = {'format': 'CHRP-v2', 'persistent': False,
                       'bank_size': len(nvram['nvram-proxy-data']), 'bank_count': 1,
                       'current_bank': 0, 'source': 'device-tree' if 'nvram-proxy-data' in chosen['props'] else 'empty-volatile'} if 'nvram-proxy-data' in nvram else None
    (out/'build.json').write_text(json.dumps(report, indent=2)+'\n')
    print(out/'XnuBridge.efi')

if __name__ == '__main__': main()
