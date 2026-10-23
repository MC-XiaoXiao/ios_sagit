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
