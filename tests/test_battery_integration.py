"""Battery build/matching/DT contracts. No device access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BatteryIntegrationTests(unittest.TestCase):
    def test_reference_hashes(self):
        folder = ROOT / 'docs/battery/reference'
        manifest = json.loads((folder / 'sources.json').read_text())
        self.assertIn('qpnp-fg-gen3.c', manifest)
        for name, record in manifest.items():
            self.assertEqual(hashlib.sha256((folder / name).read_bytes()).hexdigest(), record['sha256'])
            self.assertRegex(record['url'], r'/(?:\+/)?[0-9a-f]{40}/')

    def test_real_board_fdt_converts_battery_cells(self):
        bridge = load('battery_bridge', 'scripts/build-bridge.py')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'board.dtb'
            subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-i', str(ROOT / 'device_trees'),
                            '-o', str(path), str(ROOT / 'device_trees/xiaomi-sagit.dts')], check=True)
            blob = bridge.apple_dt(bridge.parse_fdt(path.read_bytes()), bytes(range(64)))
        seen = []
        def walk(pos):
            np, nc = struct.unpack_from('<II', blob, pos)
            pos += 8
            for _ in range(np):
                key = blob[pos:pos+32].split(b'\0')[0]
                size, = struct.unpack_from('<I', blob, pos+32)
                if key == b'qcom,battery-observer':
                    seen.append(struct.unpack('<7I', blob[pos+36:pos+36+size]))
                pos += 36 + ((size+3) & ~3)
            for _ in range(nc):
                pos = walk(pos)
            return pos
        self.assertEqual(walk(0), len(blob))
        self.assertEqual(seen, [(1, 2, 0x4000, 0x4100, 0x1000, 0x1600, 5000)])

    def test_collection_resolves_local_dependency_closure(self):
        toolchain = load('battery_toolchain', 'scripts/toolchain.py')
        config = toolchain.driver_configuration()
        metadata = {}
        for path in (ROOT / 'drivers').glob('*/*/Info.plist'):
            info = plistlib.loads(path.read_bytes())
            metadata[info['CFBundleIdentifier']] = info
        info = metadata['org.darwin.driver.QcomBattery']
        p = info['IOKitPersonalities']['QcomBattery']
        self.assertEqual(p['IOProviderClass'], 'QcomSPMI')
        self.assertNotEqual(p['IOMatchCategory'], 'DisplayBacklight')
        for item in list(metadata.values()):
            for dep in item.get('OSBundleLibraries', {}):
                metadata.setdefault(dep, {})
        order = toolchain.collection_bundle_order(metadata, ['org.darwin.driver.QcomBattery'])
        self.assertLess(order.index('org.darwin.driver.QcomSPMI'), order.index('org.darwin.driver.QcomBattery'))
        for name in ('battery', 'battery-runtime'):
            self.assertIn('org.darwin.driver.QcomBattery', config['collections'][name]['roots'])
        self.assertIn('org.darwin.driver.QcomDisplay', config['collections']['battery-runtime']['roots'])

    def test_client_links_system_power_api(self):
        path = ROOT / 'build/output/batteryctl'
        self.assertTrue(path.exists(), 'build with make battery-client')
        report = json.loads(Path(str(path)+'.json').read_text())
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), report['sha256'])
        self.assertFalse(report['hardware_tested'])
        for name in ('IORegistryEntryCreateCFProperties', 'IOPSCopyPowerSourcesInfo',
                     'IOPSCopyPowerSourcesList', 'IOPSGetPowerSourceDescription'):
            self.assertIn(name, report['iokit_exports_verified'])
        for name, digest in report['inputs'].items():
            self.assertEqual(hashlib.sha256((ROOT/name).read_bytes()).hexdigest(), digest)


if __name__ == '__main__':
    unittest.main()
