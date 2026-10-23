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


class FilesystemDTTests(unittest.TestCase):
    def test_native_numeric_properties_survive_conversion(self):
        root = {'name':'', 'props':{}, 'children':[
            {'name':'filesystems', 'props':{'e-apfs':struct.pack('<I',1)}, 'apple_native':True, 'children':[
                {'name':'fstab', 'props':{'os_env_type':struct.pack('<I',1),'max_fs_entries':struct.pack('<I',7)}, 'apple_native':True, 'children':[
                    {'name':'data-vol', 'props':{'vol.fs_role':struct.pack('<I',64),'vol.fs_file':b'/private/var\0'}, 'apple_native':True, 'children':[]}
                ]}
            ]}
        ]}
        encoded = bridge.apple_dt(root)
        parsed = parse_apple_dt(encoded)
        data = parsed['children'][0]['children'][0]['children'][0]
        self.assertEqual(data['props']['vol.fs_role'], b'@\0\0\0')
        self.assertEqual(bridge.apple_dt(parsed), encoded)
        for bad in (encoded[:7], encoded[:-1], encoded+b'\0'):
            with self.assertRaises(ValueError): parse_apple_dt(bad)


if __name__ == '__main__': unittest.main()
