"""Offline board, dependency and build contracts; never contacts a device."""
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


class HapticsIntegrationTests(unittest.TestCase):
    def test_pinned_hardware_references(self):
        folder = ROOT / 'docs/haptics/reference'
        manifest = json.loads((folder / 'sources.json').read_text())
        self.assertIn('sagit-msm8998-mtp.dtsi', manifest)
        self.assertIn('qpnp-haptic.c', manifest)
        for name, record in manifest.items():
            self.assertEqual(hashlib.sha256((folder/name).read_bytes()).hexdigest(), record['sha256'])
            self.assertRegex(record['url'], r'/[0-9a-f]{40}/')

    def test_real_board_conversion_and_disabled_ownership(self):
        bridge = load('haptics_bridge', 'scripts/build-bridge.py')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'board.dtb'
            subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-i', str(ROOT/'device_trees'),
                            '-o', str(path), str(ROOT/'device_trees/xiaomi-sagit.dts')], check=True)
            blob = bridge.apple_dt(bridge.parse_fdt(path.read_bytes()), bytes(range(64)))
        seen = []

        def walk(pos):
            np, nc = struct.unpack_from('<II', blob, pos)
            pos += 8
            for _ in range(np):
                key = blob[pos:pos+32].split(b'\0')[0]
                size, = struct.unpack_from('<I', blob, pos+32)
                if key == b'qcom,pmi8998-haptics':
                    seen.append(struct.unpack('<8I', blob[pos+36:pos+36+size]))
                pos += 36 + ((size+3) & ~3)
            for _ in range(nc):
                pos = walk(pos)
            return pos

        self.assertEqual(walk(0), len(blob))
        self.assertEqual(seen, [(1, 0, 3, 0xc000, 5000, 3248, 800, 15000)])

    def test_collection_and_matching_do_not_claim_apple_hardware(self):
        toolchain = load('haptics_toolchain', 'scripts/toolchain.py')
        config = toolchain.driver_configuration()
        metadata = {}
        for path in (ROOT/'drivers').glob('*/*/Info.plist'):
            info = plistlib.loads(path.read_bytes())
            metadata[info['CFBundleIdentifier']] = info
        info = metadata['org.darwin.driver.PMI8998Haptics']
        p = info['IOKitPersonalities']['PMI8998Haptics']
        self.assertEqual(p['IOProviderClass'], 'QcomSPMI')
        self.assertNotEqual(p['IOMatchCategory'], 'DisplayBacklight')
        self.assertFalse(any('AppleHaptics' in dep or 'AppleAOP' in dep for dep in info['OSBundleLibraries']))
        for item in list(metadata.values()):
            for dep in item.get('OSBundleLibraries', {}):
                metadata.setdefault(dep, {})
        order = toolchain.collection_bundle_order(metadata, ['org.darwin.driver.PMI8998Haptics'])
        self.assertLess(order.index('org.darwin.driver.QcomSPMI'), order.index('org.darwin.driver.PMI8998Haptics'))
        for name in ('haptics', 'haptics-runtime'):
            self.assertIn(info['CFBundleIdentifier'], config['collections'][name]['roots'])
        self.assertIn('org.darwin.driver.QcomDisplay', config['collections']['haptics-runtime']['roots'])
        self.assertNotIn(info['CFBundleIdentifier'], config['collections']['platform']['roots'])

    def test_client_cross_links_original_iokit(self):
        path = ROOT/'build/output/hapticsctl'
        self.assertEqual(struct.unpack_from('<4I', path.read_bytes()), (0xfeedfacf, 0x100000c, 0, 2))
        report = json.loads(Path(str(path)+'.json').read_text())
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), report['sha256'])
        self.assertFalse(report['hardware_tested'])
        for name in ('IOServiceOpen', 'IOServiceClose', 'IOConnectCallScalarMethod'):
            self.assertIn(name, report['iokit_exports_verified'])
        for name, digest in report['inputs'].items():
            self.assertEqual(hashlib.sha256((ROOT/name).read_bytes()).hexdigest(), digest)


if __name__ == '__main__':
    unittest.main()
