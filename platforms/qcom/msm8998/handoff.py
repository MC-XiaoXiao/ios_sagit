"""Explicit retained-resource handoff for the sagit QUP5/Synaptics pair."""
import copy
import hashlib
import json
import struct


def configure_touch_handoff(tree, path, runtime_prepare=False):
    raw = path.read_bytes()
    profile = json.loads(raw)
    fields = {'version', 'board', 'core_clock_hz', 'bus_clock_hz', 'firmware_quiesced',
              'clocks_retained', 'pins_retained', 'supplies_retained', 'reset_deasserted',
              'swap_axes', 'invert_x', 'invert_y', 'rotation'}
    if not isinstance(profile, dict) or set(profile) != fields:
        raise ValueError('touch profile has missing or unknown fields')
    if type(profile['version']) is not int or profile['version'] != 1 or profile['board'] != 'xiaomi,sagit':
        raise ValueError('unsupported touch handoff version/board')
    for key in ('firmware_quiesced', 'clocks_retained', 'pins_retained', 'supplies_retained', 'reset_deasserted'):
        if type(profile[key]) is not bool or (not runtime_prepare and profile[key] is not True):
            raise ValueError('touch handoff requires ' + key)
    for key in ('swap_axes', 'invert_x', 'invert_y'):
        if type(profile[key]) is not bool:
            raise ValueError('touch transform must be boolean')
    if type(profile['core_clock_hz']) is not int or profile['core_clock_hz'] != 19200000:
        raise ValueError('sagit QUP5 requires a retained 19.2 MHz source')
    if type(profile['bus_clock_hz']) is not int or profile['bus_clock_hz'] not in (100000, 400000):
        raise ValueError('unsupported touch bus frequency')
    if type(profile['rotation']) is not int or profile['rotation'] not in (0, 90, 180, 270):
        raise ValueError('invalid touch rotation')
    candidate = copy.deepcopy(tree)
    if b'xiaomi,sagit' not in candidate['props'].get('compatible', b'').split(b'\0'):
        raise ValueError('touch profile requires the sagit device tree')

    def one(compatible):
        found = []

        def visit(node):
            if compatible in node['props'].get('compatible', b'').split(b'\0'):
                found.append(node)
            for child in node['children']:
                visit(child)
        visit(candidate)
        if len(found) != 1:
            raise ValueError('touch handoff requires exactly one ' + compatible.decode())
        return found[0]

    bus = one(b'qcom,i2c-qup-v2.2.1')
    touch = one(b'synaptics,dsx-i2c-force')
    gpio = one(b'qcom,msm8998-pinctrl')
    gcc = one(b'qcom,gcc-msm8998')['props']
    if (bus['props'].get('reg') != struct.pack('>4I', 0, 0xc179000, 0, 0x600)
            or bus['props'].get('#address-cells') != struct.pack('>I', 1)
            or bus['props'].get('#size-cells') != struct.pack('>I', 0)
            or touch not in bus['children'] or touch['props'].get('reg') != struct.pack('>I', 0x20)
            or touch['props'].get('interrupts') != struct.pack('>2I', 125, 8)
            or len(gpio['props'].get('phandle', b'')) != 4
            or touch['props'].get('interrupt-parent') != gpio['props']['phandle']):
        raise ValueError('touch controller/address/IRQ routing mismatch')
    owned = gpio['props'].get('qcom,owned-gpios', b'')
    if len(owned) % 4:
        raise ValueError('malformed GPIO ownership')
    pins = list(struct.unpack('>' + 'I' * (len(owned) // 4), owned))
    if len(set(pins)) != len(pins) or any(pin >= 150 for pin in pins):
        raise ValueError('invalid GPIO ownership')
    if 125 not in pins:
        pins.append(125)
    gpio['props']['qcom,owned-gpios'] = struct.pack('>' + 'I' * len(pins), *pins)
    version = struct.pack('>I', 1)
    if gcc.get('qcom,gcc-handoff-version', version) != version:
        raise ValueError('incompatible GCC handoff version')
    owned_clocks = gcc.get('qcom,owned-clocks', b'')
    if len(owned_clocks) % 8:
        raise ValueError('malformed GCC clock ownership')
    clocks = dict(struct.iter_unpack('>II', owned_clocks))
    if len(clocks) != len(owned_clocks) // 8:
        raise ValueError('duplicate GCC clock ownership')
    for clock in (37, 46):
        if clocks.get(clock, 0) != 0:
            raise ValueError('conflicting I2C clock policy')
        clocks[clock] = 0
    gcc['qcom,gcc-handoff-version'] = version
    gcc['qcom,owned-clocks'] = b''.join(struct.pack('>II', *item) for item in sorted(clocks.items()))
    transform = profile['swap_axes'] | (profile['invert_x'] << 1) | (profile['invert_y'] << 2)
    # Runtime preparation publishes version 1 only after EFI exit, dedicated
    # clock/pin setup and a successful real sensor query. A JSON is no proof.
    published_version = 0 if runtime_prepare else 1
    bus['props']['qcom,i2c-handoff'] = struct.pack('>4I', published_version, 7, profile['core_clock_hz'], profile['bus_clock_hz'])
    touch['props']['synaptics,touch-handoff'] = struct.pack('>3I', published_version, 7, transform)
    touch['props']['touchscreen-rotation'] = struct.pack('>I', profile['rotation'])
    bus['props']['status'] = touch['props']['status'] = b'okay\0'
    # Atomic validation: failures must not partially grant GPIO/MMIO ownership.
    tree.clear()
    tree.update(candidate)
    return {'profile_sha256': hashlib.sha256(raw).hexdigest(),
            'mode': 'post-EFI sagit initialization' if runtime_prepare else 'retained resources',
            'version_at_build': published_version,
            'efi_only': runtime_prepare,
            'hardware_verified': False, 'firmware_updated': False, 'profile': profile}


"""Sagit MSM8998 v2 OSM: reserve a runtime-only initialization record."""
import copy
import struct


def configure_osm_handoff(tree):
    candidate = copy.deepcopy(tree)
    if b'xiaomi,sagit' not in candidate['props'].get('compatible', b'').split(b'\0'):
        raise ValueError('CPU OSM initialization requires the sagit device tree')
    found = []

    def visit(node):
        compatible = node['props'].get('compatible', b'').split(b'\0')
        if any(c.startswith(b'qcom,cpu-clock-osm-') for c in compatible):
            found.append(node)
        for child in node['children']:
            visit(child)

    visit(candidate)
    if len(found) != 1:
        raise ValueError('CPU OSM handoff requires exactly one controller')
    props = found[0]['props']
    if (props.get('compatible') != b'qcom,cpu-clock-osm-msm8998-v2\0' or
            props.get('reg') != struct.pack('>4I', 0, 0x179c0000, 0, 0x4000) or
            props.get('status') != b'okay\0'):
        raise ValueError('CPU OSM initialization requires the enabled v2 controller')
    if 'qcom,cpu-osm-handoff' in props:
        raise ValueError('a supplied CPU OSM handoff cannot assert runtime completion')
    # No build-time completion flags, voltage values or hardware identity.
    props['qcom,cpu-osm-handoff'] = struct.pack('>20I', 0, 80, *([0] * 18))
    tree.clear()
    tree.update(candidate)
    return {'version_at_build': 0, 'runtime_contract_version': 3,
            'firmware_profile': 'TZ.BF.4.0.6-00178 MSM8998 v2 boot parameters',
            'protected_post_init_completion_observed': False,
            'firmware_prerequisite': 'successful original TZ post-table initialization and XBL CPU PLL/mux setup',
            'boot_rates_hz': [1555200000, 1728000000],
            'boot_clock_requirement': 'stable local APCS PLL path at stock boot rates before publication',
            'record_bytes': 80, 'efi_only': True,
            'board': 'xiaomi,sagit', 'silicon': ['MSM8998 v2.0', 'MSM8998 v2.1'],
            'calibration_source': 'runtime QFPROM and pinned sagit CPRh policy',
            'hardware_tested': False}


"""Reserve a final-map-verified EFI SMEM handoff; never assume modem ownership."""
import struct


def configure_smem_handoff(tree):
    def walk(node):
        yield node
        for child in node['children']:
            yield from walk(child)

    if b'qcom,msm8998' not in tree['props'].get('compatible', b'').split(b'\0'):
        raise ValueError('SMEM handoff requires an MSM8998 tree')
    nodes = [n for n in walk(tree) if b'qcom,msm8998-smem' in
             n['props'].get('compatible', b'').split(b'\0')]
    expected = struct.pack('>8I', 0, 0x86000000, 0, 0x200000, 0, 0x1f40000, 0, 0x20000)
    if len(nodes) != 1 or nodes[0]['name'] != 'shared-memory@86000000' or nodes[0]['props'].get('reg') != expected:
        raise ValueError('SMEM requires the exact sagit RAM and TCSR mutex windows')
    chosen = [n for n in tree['children'] if n['name'] == 'chosen']
    maps = [n for n in chosen[0]['children'] if n['name'] == 'memory-map'] if len(chosen) == 1 else []
    if len(maps) != 1 or sum(n['name'] == 'memory-map' for n in walk(tree)) != 1:
        raise ValueError('SMEM requires one chosen/memory-map handoff node')
    # A zero placeholder grants nothing. The raw entry never fills it. The EFI
    # entry aborts unless its final memory map proves the complete reservation.
    maps[0]['props']['QcomSMEM'] = bytes(16)
    nodes[0]['props']['status'] = b'okay\0'
    return {'requested': True, 'efi_only': True, 'base': 0x86000000, 'bytes': 0x200000,
            'final_memory_map_required_type': 'EfiReservedMemoryType',
            'bootstrap_arena_overlap_permitted': False, 'raw_entry_publishes_reservation': False,
            'modem_enabled': False, 'remote_ownership_granted': False, 'hardware_tested': False}
