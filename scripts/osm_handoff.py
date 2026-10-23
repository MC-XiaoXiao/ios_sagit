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
