import importlib.util
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from bridge_filesystems import parse_apple_dt
spec = importlib.util.spec_from_file_location('build_bridge', ROOT/'scripts/build-bridge.py')
bridge = importlib.util.module_from_spec(spec); spec.loader.exec_module(bridge)


class DevelopmentHactivationTests(unittest.TestCase):
    def tree(self):
        return {'name':'', 'props':{
            'compatible':b'xiaomi,sagit\0qcom,msm8998\0',
            'model':b'iPhoneMi6,1\0',
        }, 'children':[{'name':'chosen', 'props':{}, 'children':[]}]}

    def test_native_hactivation_status_tuple_round_trips_as_apple_dt_cells(self):
        tree = self.tree()
        report = bridge.configure_development_hactivation(tree)
        self.assertEqual(report['path'], '/chosen')
        self.assertFalse(report['hardware_boot_verified'])
        parsed = parse_apple_dt(bridge.apple_dt(tree))
        chosen = next(node for node in parsed['children'] if node['name'] == 'chosen')
        self.assertEqual({key: struct.unpack('<I', chosen['props'][key])[0]
                          for key in report['properties']}, report['properties'])

    def test_hactivation_is_limited_to_the_sagit_development_identity(self):
        for changes in (
            {'model':b'iPhone10,3\0'},
            {'compatible':b'qcom,msm8998\0'},
        ):
            tree = self.tree()
            tree['props'].update(changes)
            with self.assertRaisesRegex(ValueError, 'restricted'):
                bridge.configure_development_hactivation(tree)

    def test_conflicting_preexisting_status_is_rejected(self):
        tree = self.tree()
        tree['children'][0]['props']['certificate-security-mode'] = struct.pack('>I', 0)
        with self.assertRaisesRegex(ValueError, 'conflicting'):
            bridge.configure_development_hactivation(tree)


if __name__ == '__main__':
    unittest.main()
