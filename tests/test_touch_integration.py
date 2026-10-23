"""Touch controller publication, native HID matching and offline handoff ABI."""
import copy
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
spec = importlib.util.spec_from_file_location('touch_bridge', ROOT / 'scripts/build-bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


def apple_nodes(blob):
    nodes = {}

    def walk(pos, path):
        count, children = struct.unpack_from('<II', blob, pos)
        pos += 8
        props = {}
        for _ in range(count):
            key = blob[pos:pos+32].split(b'\0')[0].decode()
            length, = struct.unpack_from('<I', blob, pos+32)
            props[key] = blob[pos+36:pos+36+length]
            pos += 36 + ((length+3) & ~3)
        path += '/' + props['name'].rstrip(b'\0').decode()
        nodes[path] = props
        for _ in range(children):
            pos = walk(pos, path)
        return pos
    assert walk(0, '') == len(blob)
    return nodes


class TouchIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        output = cls.directory / 'board.dtb'
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-i', str(ROOT/'device_trees'),
                        '-o', str(output), str(ROOT/'device_trees/xiaomi-sagit.dts')], check=True,
                       capture_output=True)
        cls.tree = bridge.parse_fdt(output.read_bytes())

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @staticmethod
    def profile():
        # Synthetic test contract, not evidence of firmware/hardware preparation.
        return dict(version=1, board='xiaomi,sagit', core_clock_hz=19200000, bus_clock_hz=400000,
                    firmware_quiesced=True, clocks_retained=True, pins_retained=True,
                    supplies_retained=True, reset_deasserted=True,
                    swap_axes=False, invert_x=False, invert_y=False, rotation=0)

    def enable(self, tree, profile):
        path = self.directory / 'synthetic-profile.json'
        path.write_text(json.dumps(profile))
        return bridge.configure_touch_handoff(tree, path)

    def test_default_tree_never_claims_touch_resources(self):
        nodes = apple_nodes(bridge.apple_dt(self.tree))
        bus = nodes['/device-tree/arm-io/i2c@c179000']
        touch = nodes['/device-tree/arm-io/i2c@c179000/touchscreen@20']
        self.assertEqual(bus['status'], b'disabled\0')
        self.assertEqual(touch['status'], b'disabled\0')
        self.assertNotIn('qcom,i2c-handoff', bus)
        self.assertNotIn('synaptics,touch-handoff', touch)
        self.assertEqual(bus['reg'], struct.pack('<QQ', 0xc179000, 0x600))
        self.assertEqual(touch['reg'], struct.pack('<Q', 0x20))  # address, not MMIO

    def test_handoff_is_little_endian_and_only_grants_irq_pin(self):
        tree = copy.deepcopy(self.tree)
        profile = self.profile()
        profile.update(invert_x=True, swap_axes=True, rotation=90)
        report = self.enable(tree, profile)
        self.assertFalse(report['hardware_verified'])
        nodes = apple_nodes(bridge.apple_dt(tree))
        base = '/device-tree/arm-io/'
        self.assertEqual(nodes[base+'i2c@c179000']['qcom,i2c-handoff'], struct.pack('<4I', 1, 7, 19200000, 400000))
        touch = nodes[base+'i2c@c179000/touchscreen@20']
        self.assertEqual(touch['synaptics,touch-handoff'], struct.pack('<3I', 1, 7, 3))
        self.assertEqual(touch['touchscreen-rotation'], struct.pack('<I', 90))
        self.assertEqual(touch['interrupts'], struct.pack('<2I', 125, 8))
        self.assertEqual(nodes[base+'gpio-controller@3400000']['qcom,owned-gpios'], struct.pack('<I', 125))
        self.assertEqual(touch['status'], b'okay\0')
        self.assertEqual(nodes[base+'clock-controller@100000']['qcom,owned-clocks'],
                         struct.pack('<4I', 37, 0, 46, 0))
        again = copy.deepcopy(tree)
        self.enable(tree, profile)
        self.assertEqual(tree, again)

    def test_runtime_prepare_defers_authorization_until_real_sensor_query(self):
        tree=copy.deepcopy(self.tree)
        path=ROOT/'docs/touch/handoff-template.json'
        report=bridge.configure_touch_handoff(tree,path,runtime_prepare=True)
        self.assertEqual(report['version_at_build'],0)
        self.assertFalse(report['hardware_verified'])
        nodes=apple_nodes(bridge.apple_dt(tree))
        bus=nodes['/device-tree/arm-io/i2c@c179000']
        touch=nodes['/device-tree/arm-io/i2c@c179000/touchscreen@20']
        self.assertEqual(bus['qcom,i2c-handoff'],struct.pack('<4I',0,7,19200000,400000))
        self.assertEqual(touch['synaptics,touch-handoff'],struct.pack('<3I',0,7,0))
        # The same unverified file remains invalid without runtime preparation.
        with self.assertRaises(ValueError):
            bridge.configure_touch_handoff(copy.deepcopy(self.tree),path)

    def test_profile_failures_are_atomic(self):
        failures = [('version', True), ('board', 'other'), ('core_clock_hz', 20000000),
                    ('bus_clock_hz', 0), ('rotation', 45), ('swap_axes', 1),
                    ('firmware_quiesced', False), ('clocks_retained', False),
                    ('pins_retained', False), ('supplies_retained', False), ('reset_deasserted', False)]
        for key, value in failures:
            with self.subTest(key=key):
                tree = copy.deepcopy(self.tree)
                profile = self.profile()
                profile[key] = value
                with self.assertRaises(ValueError):
                    self.enable(tree, profile)
                self.assertEqual(tree, self.tree)
        tree = copy.deepcopy(self.tree)
        soc = next(n for n in tree['children'] if n['name'].startswith('soc'))
        bus = next(n for n in soc['children'] if n['name'] == 'i2c@c179000')
        bus['children'][0]['props']['interrupts'] = struct.pack('>2I', 124, 8)
        original = copy.deepcopy(tree)
        with self.assertRaises(ValueError):
            self.enable(tree, self.profile())
        self.assertEqual(tree, original)

    def test_gpio_handoff_preserves_fingerprint_ownership(self):
        tree = copy.deepcopy(self.tree)
        soc = next(n for n in tree['children'] if n['name'].startswith('soc'))
        gpio = next(n for n in soc['children'] if n['name'] == 'gpio-controller@3400000')
        gpio['props']['qcom,owned-gpios'] = struct.pack('>3I', 35, 37, 121)
        self.enable(tree, self.profile())
        props = apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/gpio-controller@3400000']
        self.assertEqual(struct.unpack('<4I', props['qcom,owned-gpios']), (35, 37, 121, 125))

    def test_native_hid_personality_and_dependency_closure(self):
        root = ROOT / 'drivers'
        info = plistlib.loads((root/'input/SynapticsTouch/Info.plist').read_bytes())
        event = info['IOKitPersonalities']['Synaptics Touch HID Events']
        self.assertEqual(event['IOClass'], 'IOHIDEventDriver')
        self.assertEqual(event['CFBundleIdentifier'], 'com.apple.iokit.IOHIDFamily')
        self.assertEqual(event['IOProviderClass'], 'IOHIDInterface')
        self.assertEqual(event['DeviceUsagePairs'], [{'DeviceUsagePage': 13, 'DeviceUsage': 4}])
        self.assertTrue(event['DigitizerCollectionDispatch'])
        self.assertTrue(event['DisplayIntegrated'])
        self.assertEqual(info['IOKitPersonalities']['SynapticsTouch']['IOProviderClass'], 'GenericI2CDevice')
        self.assertIn('org.darwin.driver.GenericI2C', info['OSBundleLibraries'])
        build = json.loads((root/'Build.json').read_text())
        self.assertIn('org.darwin.driver.SynapticsTouch', build['collections']['input']['roots'])
        self.assertEqual(build['vtable_contracts']['SynapticsTouch']['slots'], 207)
        self.assertEqual(build['vtable_contracts']['SynapticsTouch']['bindings']['181'], 'IOHIDDevice::handleReport')
        self.assertIn('i2c', build['collections']['input']['tests'])
        self.assertIn('touch', build['collections']['input']['tests'])
        qup = plistlib.loads((root/'bus/QcomI2C/Info.plist').read_bytes())
        self.assertIn('org.darwin.driver.QcomGCC', qup['OSBundleLibraries'])

    def test_clock_grants_merge_without_retuning_other_devices(self):
        tree = copy.deepcopy(self.tree)
        soc = next(n for n in tree['children'] if n['name'].startswith('soc'))
        gcc = next(n for n in soc['children'] if n['name'] == 'clock-controller@100000')['props']
        gcc['qcom,owned-clocks'] = struct.pack('>4I', 37, 0, 53, 0)
        gcc['qcom,owned-roots'] = struct.pack('>2I', 14, 48000000)
        self.enable(tree, self.profile())
        gcc = apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/clock-controller@100000']
        self.assertEqual(gcc['qcom,owned-clocks'], struct.pack('<6I', 37, 0, 46, 0, 53, 0))
        self.assertEqual(gcc['qcom,owned-roots'], struct.pack('<2I', 14, 48000000))

    def test_conflicting_clock_handoff_does_not_enable_touch(self):
        for clocks in (struct.pack('>2I', 46, 19200000), struct.pack('>4I', 37, 0, 37, 0), b'\1'):
            tree = copy.deepcopy(self.tree)
            soc = next(n for n in tree['children'] if n['name'].startswith('soc'))
            gcc = next(n for n in soc['children'] if n['name'] == 'clock-controller@100000')['props']
            gcc['qcom,owned-clocks'] = clocks
            before = copy.deepcopy(tree)
            with self.assertRaises(ValueError):
                self.enable(tree, self.profile())
            self.assertEqual(tree, before)

    def test_pinned_primary_sources(self):
        directory = ROOT/'docs/touch/reference'
        for name, record in json.loads((directory/'sources.json').read_text()).items():
            with self.subTest(name=name):
                self.assertEqual(hashlib.sha256((directory/name).read_bytes()).hexdigest(), record['sha256'])


if __name__ == '__main__':
    unittest.main()
