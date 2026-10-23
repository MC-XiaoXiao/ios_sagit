"""Check the WLAN framework against the actual Apple and Qualcomm evidence."""
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ROOT / 'drivers'
sys.path.insert(0, str(ROOT / 'scripts'))
import toolchain


class WLANIntegrationTests(unittest.TestCase):
    def test_reference_files_are_hash_pinned(self):
        reference = ROOT / 'docs/wifi/reference'
        for name, record in json.loads((reference / 'sources.json').read_text()).items():
            with self.subTest(name=name):
                self.assertEqual(hashlib.sha256((reference / name).read_bytes()).hexdigest(), record['sha256'])

    def test_kernel_model_matches_the_current_provider(self):
        model = json.loads((ROOT / 'docs/wifi/ios18-model.json').read_text())
        image = ROOT / 'build/ios18-kext-analysis' / model['kernel']['file']
        self.assertEqual(hashlib.sha256(image.read_bytes()).hexdigest(), model['kernel']['sha256'])
        inventory = json.loads((ROOT / 'build/ios18-kext-analysis/prelink-info.json').read_text())
        by_id = {k['CFBundleIdentifier']: k for k in inventory['_PrelinkInfoDictionary']}
        for observed in model['kernel_kexts']:
            actual = by_id[observed['CFBundleIdentifier']]
            for key, value in observed.items():
                self.assertEqual(actual[key], value)
        self.assertNotIn('com.apple.iokit.IO80211Family', by_id)
        self.assertEqual(by_id['com.apple.driver.AppleOLYHAL']['IOKitPersonalities']['AppleARMIODevice']['IOProviderClass'], 'AppleARMIODevice')

    def test_driverkit_recipe_uses_the_observed_native_interface(self):
        model = json.loads((ROOT / 'docs/wifi/ios18-model.json').read_text())
        observed = model['user_driver']['personalities']['DriverKit_IO80211Infra']
        proposed = plistlib.loads((ROOT / 'drivers/wifi/QcomWLANDriver/NativeInterface.plist').read_bytes())
        for key in ('IOClass', 'CFBundleIdentifierKernel', 'IOProviderClass', 'IO80211InterfaceRole'):
            self.assertEqual(proposed[key], observed[key])
        dext = plistlib.loads((ROOT / 'drivers/wifi/QcomWLANDriver/Info.plist').read_bytes())
        self.assertEqual(dext['CFBundlePackageType'], 'DEXT')
        for personality in dext['IOKitPersonalities'].values():
            self.assertEqual(personality['IOClass'], 'IOUserService')
            self.assertEqual(personality['CFBundleIdentifierKernel'], 'com.apple.kpi.iokit')
            self.assertEqual(personality['IOProviderClass'], 'QcomWLAN')
            self.assertIs(personality['IOPropertyMatch']['WLANTransportReady'], True)
            # The unimplemented native-interface recipe is not installed.
            self.assertNotEqual(personality['IOClass'], proposed['IOClass'])

    def test_transport_uses_apple_matching_without_claiming_broadcom(self):
        info = plistlib.loads((DRIVERS / 'wifi/QcomWLAN/Info.plist').read_bytes())
        self.assertEqual(info['CFBundleIdentifier'], 'org.darwin.driver.QcomWLAN')
        self.assertEqual(info['CFBundlePackageType'], 'KEXT')
        for personality in info['IOKitPersonalities'].values():
            self.assertEqual(personality['IOProviderClass'], 'AppleARMIODevice')
            self.assertEqual(personality['IONameMatch'], 'qcom,wcn3990-wifi')
        self.assertFalse(any('BCM' in name or '80211' in name for name in info['OSBundleLibraries']))
        build = json.loads((DRIVERS / 'Build.json').read_text())
        collection = build['collections']['wifi']
        self.assertIn(info['CFBundleIdentifier'], collection['roots'])
        self.assertEqual(collection['tests'], ['wlan', 'wlan-firmware', 'wlan-runtime'])
        for family in ('IONetworkingFamily','IOSkywalkFamily','IONetworkFamily'):
            self.assertIn('com.apple.iokit.' + family, collection['roots'])

    def test_extracted_driverkit_evidence_if_available(self):
        model = json.loads((ROOT / 'docs/wifi/ios18-model.json').read_text())['user_driver']
        bundle = ROOT / 'build/wifi/reference/com.apple.DriverKit-AppleBCMWLAN.dext'
        if not bundle.exists():
            self.skipTest('optional extracted Apple DEXT is not present')
        data = (bundle / 'Info.plist').read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), model['info_plist_sha256'])
        info = plistlib.loads(data)
        self.assertEqual(info['DTSDKName'], model['sdk'])
        self.assertEqual(hashlib.sha256((bundle / info['CFBundleExecutable']).read_bytes()).hexdigest(), model['executable_sha256'])

    def test_compiled_driverkit_is_a_real_driverkit_executable(self):
        executable = ROOT / 'build/output/QcomWLANDriver.dext/QcomWLANDriver'
        if not executable.exists():
            self.skipTest('build with make wifi-driverkit before checking the DEXT')
        metadata = toolchain.driverkit_macho(executable)
        self.assertEqual(metadata['platform'], 'DriverKit')
        self.assertEqual(metadata['entry'], '__osclassinfo')
        self.assertEqual(metadata['superclass'], 'IOService')
        self.assertIn('/System/DriverKit/System/Library/Frameworks/NetworkingDriverKit.framework/NetworkingDriverKit', metadata['dependencies'])
        self.assertTrue(all(path.startswith('/System/DriverKit/') for path in metadata['dependencies']))
        with tempfile.TemporaryDirectory() as directory:
            invalid = Path(directory) / 'invalid'
            data = bytearray(executable.read_bytes()); struct.pack_into('<I', data, 12, 6)
            invalid.write_bytes(data)
            with self.assertRaises(ValueError):
                toolchain.driverkit_macho(invalid)
            data = bytearray(executable.read_bytes()); data[0x4000] ^= 1
            invalid.write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'signed code page'):
                toolchain.driverkit_macho(invalid)

    def test_native_registration_uses_apple_child_properties(self):
        info = plistlib.loads((ROOT / 'drivers/wifi/QcomWLANDriver/Info.plist').read_bytes())
        child = info['IOKitPersonalities']['WLAN']['WLANInterfaceProperties']
        self.assertEqual(child['IOClass'], 'IOUserNetworkWLAN')
        self.assertEqual(child['IOUserClass'], 'IOUserNetworkWLAN')
        self.assertEqual(child['CFBundleIdentifierKernel'], 'com.apple.iokit.IOSkywalkFamily')
        self.assertEqual(child['IO80211InterfaceRole'], 'Infrastructure')

    def test_firmware_import_is_pinned_and_does_not_invent_remote_firmware(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory); archive = directory / 'input.zip'; profile_path = directory / 'policy.json'
            data = bytes(range(256))
            with zipfile.ZipFile(archive, 'w') as source:
                source.writestr('firmware/board.bin', data)
            profile = {'schema': 1, 'controller': 'WCN3990', 'archive_sha256': toolchain.digest(archive),
                       'chip_id': 0x3990, 'board_id': 0x123,
                       'board': {'format': 'raw', 'member': 'firmware/board.bin', 'sha256': hashlib.sha256(data).hexdigest()}}
            profile_path.write_text(json.dumps(profile)); output = directory / 'prepared'
            manifest = toolchain.prepare_wifi_firmware(archive, profile_path, output)
            self.assertFalse(manifest['remote_firmware_included'])
            self.assertFalse(manifest['msa_reserved'])
            self.assertFalse(manifest['hardware_tested'])
            self.assertEqual((output / 'board.bin').read_bytes(), data)
            self.assertEqual(struct.unpack('<6I', (output / 'configuration.bin').read_bytes()), (1, 24, 0x3990, 0x123, 256, 0))
            profile['board']['sha256'] = '0' * 64; profile_path.write_text(json.dumps(profile))
            with self.assertRaises(ValueError):
                toolchain.prepare_wifi_firmware(archive, profile_path, directory / 'rejected')
            self.assertFalse((directory / 'rejected').exists())

    def test_board_container_has_exact_selection_and_rejects_ambiguous_tail(self):
        identity = b'bus=snoc,qmi-board-id=123,qmi-chip-id=3990'
        def record(kind, value):
            return struct.pack('<II', kind, len(value)) + value + b'\0' * (-len(value) % 4)
        entry = record(0, identity) + record(1, b'calibration')
        data = b'QCA-ATH10K-BOARD\0' + b'\0' * 3 + record(0, entry)
        self.assertEqual(toolchain.select_wifi_board(data, identity.decode()), b'calibration')
        for malformed in (data[:-1], data + record(0, entry), data + b'\0'):
            with self.assertRaises(ValueError):
                toolchain.select_wifi_board(malformed, identity.decode())
        with self.assertRaises(ValueError):
            toolchain.select_wifi_board(data, 'bus=snoc,qmi-board-id=ffff')


if __name__ == '__main__':
    unittest.main()
