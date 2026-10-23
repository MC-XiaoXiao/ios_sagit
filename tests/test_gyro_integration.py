"""Offline native HID matching, independent SPI closure and Apple DT encoding."""
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
DRIVERS = ROOT / 'drivers'
spec = importlib.util.spec_from_file_location('gyro_bridge', ROOT / 'scripts/build-bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


def nodes(blob):
    result = {}

    def walk(offset, path):
        count, children = struct.unpack_from('<II', blob, offset)
        offset += 8
        props = {}
        for _ in range(count):
            key = blob[offset:offset+32].split(b'\0')[0].decode()
            size, = struct.unpack_from('<I', blob, offset+32)
            props[key] = blob[offset+36:offset+36+size]
            offset += 36 + ((size+3) & ~3)
        path += '/' + props['name'].rstrip(b'\0').decode()
        result[path] = props
        for _ in range(children):
            offset = walk(offset, path)
        return offset
    assert walk(0, '') == len(blob)
    return result


class GyroIntegrationTests(unittest.TestCase):
    def test_native_gyro_matching_and_feature_vtable(self):
        p = plistlib.loads((DRIVERS/'input/SPIGyro/Info.plist').read_bytes())
        self.assertIn('com.apple.iokit.IOHIDFamily', p['OSBundleLibraries'])
        self.assertIn('org.darwin.driver.GenericSPI', p['OSBundleLibraries'])
        for name, model in [('st,lsm6dsm', 1), ('invensense,icm20690', 2)]:
            match = p['IOKitPersonalities'][name]
            self.assertEqual(match['IOProviderClass'], 'GenericSPIDevice')
            self.assertEqual(match['IONameMatch'], name)
            self.assertEqual(match['GyroModel'], model)
        event = p['IOKitPersonalities']['SPI Gyro HID Events']
        self.assertEqual(event['CFBundleIdentifier'], 'com.apple.iokit.IOHIDFamily')
        self.assertEqual(event['IOClass'], 'IOHIDEventDriver')
        self.assertEqual(event['IOProviderClass'], 'IOHIDInterface')
        self.assertEqual(event['DeviceUsagePairs'], [{'DeviceUsagePage': 32, 'DeviceUsage': 118}])
        self.assertEqual(event['IOPropertyMatch'], {'Transport': 'SPI', 'Product': 'SPI Gyroscope'})
        build = json.loads((DRIVERS/'Build.json').read_text())
        layout = build['vtable_contracts']['SPIGyro']
        self.assertEqual(layout['slots'], 207)
        self.assertEqual(layout['bindings']['181'], 'IOHIDDevice::handleReport')
        self.assertEqual(layout['bindings']['182'], 'SPIGyro::getReport')
        self.assertEqual(layout['bindings']['183'], 'SPIGyro::setReport')

    def test_spi_is_a_separate_controller_and_dependency(self):
        p = plistlib.loads((DRIVERS/'bus/QcomSPI/Info.plist').read_bytes())
        self.assertIn('org.darwin.driver.GenericSPI', p['OSBundleLibraries'])
        self.assertEqual(p['IOKitPersonalities']['QcomSPI']['IOProviderClass'], 'AppleARMIODevice')
        generic = plistlib.loads((DRIVERS/'bus/GenericSPI/Info.plist').read_bytes())
        self.assertFalse(generic['IOKitPersonalities'])
        build = json.loads((DRIVERS/'Build.json').read_text())
        gyro = build['collections']['gyro']
        self.assertIn('org.darwin.driver.QcomSPI', gyro['roots'])
        self.assertIn('org.darwin.driver.SPIGyro', gyro['roots'])
        self.assertEqual(set(gyro['tests']), {'spi', 'gyro'})
        self.assertNotIn('org.darwin.driver.SPIGyro', build['collections']['touch']['roots'])

    def test_synthetic_spi_tree_endianness_and_chip_select(self):
        be = lambda *values: struct.pack('>'+'I'*len(values), *values)
        # Deliberately synthetic MMIO. This fixture is not an activation profile.
        handoff = (1, 31, 1000000, 0, 0xdead000, 0, 0x1000, 0)
        tree = {'name': '', 'props': {'#address-cells': be(2), '#size-cells': be(2)}, 'children': [
            {'name': 'soc', 'props': {'#address-cells': be(2), '#size-cells': be(2)}, 'children': [
                {'name': 'spi@test', 'props': {'#address-cells': be(1), '#size-cells': be(0),
                 'reg': be(0, 0xdead000, 0, 0x1000), 'qcom,spi-handoff': be(*handoff)}, 'children': [
                    {'name': 'gyro@0', 'props': {'reg': be(0), 'spi-max-frequency': be(1000000),
                     'spi-mode': be(3), 'gyro,mount': be(0xffffffff, 2, 0xfffffffd)}, 'children': []}]}]}]}
        converted = nodes(bridge.apple_dt(tree))
        parent = '/device-tree/arm-io/spi@test'
        self.assertEqual(converted[parent]['reg'], struct.pack('<QQ', 0xdead000, 0x1000))
        self.assertEqual(converted[parent]['qcom,spi-handoff'], struct.pack('<8I', *handoff))
        child = converted[parent+'/gyro@0']
        self.assertEqual(child['reg'], struct.pack('<Q', 0))
        self.assertEqual(child['spi-max-frequency'], struct.pack('<I', 1000000))
        self.assertEqual(child['spi-mode'], struct.pack('<I', 3))
        self.assertEqual(child['gyro,mount'], struct.pack('<iii', -1, 2, -3))

    def test_non_cell_property_rejected(self):
        for key in ('qcom,spi-handoff', 'spi-mode', 'spi-max-frequency', 'gyro,mount'):
            with self.subTest(key=key):
                tree = {'name': '', 'props': {key: b'\x01'}, 'children': []}
                with self.assertRaises(ValueError):
                    bridge.apple_dt(tree)

    def test_default_board_does_not_invent_ssc_ownership(self):
        with tempfile.TemporaryDirectory() as path:
            dtb = Path(path)/'board.dtb'
            subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-i', str(ROOT/'device_trees'),
                            '-o', str(dtb), str(ROOT/'device_trees/xiaomi-sagit.dts')],
                           check=True, capture_output=True)
            for props in nodes(bridge.apple_dt(bridge.parse_fdt(dtb.read_bytes()))).values():
                self.assertNotIn('qcom,spi-handoff', props)

    def test_reference_hashes_and_board_variants(self):
        directory = ROOT/'docs/gyro/reference'
        for name, record in json.loads((directory/'sources.json').read_text()).items():
            with self.subTest(name=name):
                self.assertEqual(hashlib.sha256((directory/name).read_bytes()).hexdigest(), record['sha256'])
        config = (directory/'sensor_def_qcomdev.conf').read_text()
        # The original file spells ICM20690 as ICM26090. Runtime WHO_AM_I
        # remains mandatory; a registry candidate is not proof of fitted silicon.
        self.assertIn('ICM26090 gyro', config)
        self.assertIn('LSM6DSM-Gyro', config)
        values = {}
        for line in config.splitlines():
            fields = line.split('#', 1)[0].split()
            if len(fields) == 3 and fields[0].isdigit():
                values[int(fields[0])] = int(fields[1], 0)
        self.assertEqual([values[i] for i in (800, 801, 802)], [-1, 2, -3])
        self.assertEqual([values[i] for i in (2109, 2110, 2120, 2121)], [0x1001, 0, 0x1001, 0])


if __name__ == '__main__':
    unittest.main()
