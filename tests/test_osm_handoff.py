"""Offline producer contract: no JSON/DT can claim completed CPU setup."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_bridge', ROOT/'scripts/build-bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class OSMHandoffTests(unittest.TestCase):
    def setUp(self):
        self.osm = {'name': 'performance-controller@179c0000', 'children': [],
                    'props': {'compatible': b'qcom,cpu-clock-osm-msm8998-v2\0',
                              'reg': struct.pack('>4I', 0, 0x179c0000, 0, 0x4000),
                              'status': b'okay\0'}}
        self.tree = {'name': '', 'props': {'compatible': b'xiaomi,sagit\0qcom,msm8998\0',
                                          '#address-cells': struct.pack('>I', 2),
                                          '#size-cells': struct.pack('>I', 2)},
                     'children': [self.osm]}

    def test_only_runtime_producer_can_commit(self):
        report = bridge.configure_osm_handoff(self.tree)
        value = self.tree['children'][0]['props']['qcom,cpu-osm-handoff']
        self.assertEqual(struct.unpack('>20I', value), (0, 80) + (0,) * 18)
        self.assertEqual(report['version_at_build'], 0)
        self.assertEqual(report['runtime_contract_version'], 3)
        self.assertEqual(report['boot_rates_hz'], [1555200000, 1728000000])
        self.assertFalse(report['protected_post_init_completion_observed'])
        self.assertFalse(report['hardware_tested'])
        self.assertTrue(report['efi_only'])
        self.assertIn('qcom,cpu-osm-handoff', bridge.NUMERIC)
        before = copy.deepcopy(self.tree)
        with self.assertRaises(ValueError):
            bridge.configure_osm_handoff(self.tree)
        self.assertEqual(self.tree, before)

    def test_apple_dt_converts_record_cells(self):
        bridge.configure_osm_handoff(self.tree)
        blob = bridge.apple_dt(self.tree)
        values = []

        def walk(pos):
            count, children = struct.unpack_from('<II', blob, pos)
            pos += 8
            for _ in range(count):
                name = blob[pos:pos + 32].split(b'\0')[0]
                length, = struct.unpack_from('<I', blob, pos + 32)
                data = blob[pos + 36:pos + 36 + length]
                if name == b'qcom,cpu-osm-handoff':
                    values.append(struct.unpack('<20I', data))
                pos += 36 + ((length + 3) & ~3)
            for _ in range(children):
                pos = walk(pos)
            return pos

        self.assertEqual(walk(0), len(blob))
        self.assertEqual(values, [(0, 80) + (0,) * 18])

    def test_driver_verifier_pins_all_cpu_references(self):
        expected = {}
        for filename in ('osm-reference-sources.json', 'osm-firmware-sources.json'):
            manifest = json.loads((ROOT/'docs/power'/filename).read_text())
            for entry in manifest['sources']:
                if 'path' not in entry:
                    continue
                path = ROOT/entry['path']
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), entry['sha256'])
                expected[path.name] = {'url': entry['url'], 'sha256': entry['sha256']}
        index = ROOT/'docs/power/reference/cpu-osm-sources.json'
        self.assertEqual(json.loads(index.read_text()), expected)
        config = json.loads((ROOT/'drivers/Build.json').read_text())
        self.assertIn('../docs/power/reference/cpu-osm-sources.json', config['source_manifests'])

    def test_incompatible_or_ambiguous_tree_is_atomic(self):
        for change in (
            lambda: self.tree['props'].update(compatible=b'qcom,msm8998\0'),
            lambda: self.osm['props'].update(compatible=b'qcom,cpu-clock-osm-msm8998-v1\0'),
            lambda: self.osm['props'].update(status=b'disabled\0'),
            lambda: self.osm['props'].update(reg=struct.pack('>4I', 0, 0x179c4000, 0, 0x4000)),
            lambda: self.tree['children'].append(copy.deepcopy(self.osm)),
            lambda: self.osm['props'].update({'qcom,cpu-osm-handoff': bytes(80)}),
        ):
            self.setUp()
            change()
            before = copy.deepcopy(self.tree)
            with self.assertRaises(ValueError):
                bridge.configure_osm_handoff(self.tree)
            self.assertEqual(self.tree, before)


if __name__ == '__main__':
    unittest.main()
