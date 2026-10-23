"""Input hardware catalog, native HID matching, and reproducible dependencies."""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ROOT / 'drivers'
REFERENCE = ROOT / 'docs/gpio/reference'


class InputIntegrationTests(unittest.TestCase):
    def test_all_tlmm_pin_offsets_follow_the_hardware_catalog(self):
        catalog = (REFERENCE / 'pinctrl-msm8998.c').read_text()
        tiles = {'NORTH': 0x500000, 'WEST': 0x100000, 'EAST': 0x900000}
        expected = {int(pin): tiles[tile] + 0x1000 * int(pin)
                    for pin, tile in re.findall(r'PINGROUP\((\d+), (NORTH|WEST|EAST),', catalog)}
        header = (DRIVERS / 'bus/QcomGPIO/TLMMPins.h').read_text()
        values = [int(value, 16) for value in re.findall(r'0x[0-9a-f]+', header.split('PinBase[PinCount] = {')[1])]
        self.assertEqual(set(expected), set(range(150)))
        self.assertEqual(values, [expected[pin] for pin in range(150)])

    def test_reference_files_are_hash_pinned(self):
        for name, source in json.loads((REFERENCE / 'sources.json').read_text()).items():
            with self.subTest(name=name):
                self.assertEqual(hashlib.sha256((REFERENCE / name).read_bytes()).hexdigest(), source['sha256'])

    def test_event_driver_matches_the_real_transport_and_usage(self):
        info = plistlib.loads((DRIVERS / 'input/QcomButtons/Info.plist').read_bytes())
        personality = info['IOKitPersonalities']['Platform Buttons HID Events']
        self.assertEqual(personality['IOClass'], 'IOHIDEventDriver')
        self.assertEqual(personality['CFBundleIdentifier'], 'com.apple.iokit.IOHIDFamily')
        self.assertIn(personality['CFBundleIdentifier'], info['OSBundleLibraries'])
        self.assertEqual(personality['IOProviderClass'], 'IOHIDInterface')
        self.assertEqual(personality['DeviceUsagePairs'], [{'DeviceUsagePage': 12, 'DeviceUsage': 1}])
        properties = personality['IOPropertyMatch']
        source = (DRIVERS / 'input/QcomButtons/QcomButtons.cpp').read_text()
        for key, method in [('Transport', 'newTransportString'), ('Product', 'newProductString')]:
            actual = re.search(method + r'\(\) const \{ return OSString::withCString\("([^"]+)"\)', source)
            self.assertIsNotNone(actual)
            self.assertEqual(actual[1], properties[key])

    def test_spmi_backlight_and_buttons_can_match_together(self):
        # IOService consumes one client per match category on a provider.
        # WLED must coexist with the PMIC interrupt controller for side keys.
        categories = []
        for name in ('QcomPMIC', 'PMI8998WLED'):
            info = plistlib.loads(next(DRIVERS.glob('*/'+name+'/Info.plist')).read_bytes())
            personality = info['IOKitPersonalities'][name]
            self.assertEqual(personality['IOProviderClass'], 'QcomSPMI')
            categories.append(personality.get('IOMatchCategory', 'IODefaultMatchCategory'))
        self.assertEqual(len(set(categories)), 2)

    def test_input_collection_and_native_class_contract(self):
        build = json.loads((DRIVERS / 'Build.json').read_text())
        roots = build['collections']['input']['roots']
        for name in ('QcomGPIO', 'QcomSPMI', 'QcomPMIC', 'QcomButtons', 'GoodixFingerprint', 'FPCFingerprint'):
            self.assertIn('org.darwin.driver.' + name, roots)
        self.assertIn('com.apple.iokit.IOHIDEventDriver', roots)
        abi = json.loads((DRIVERS / 'abi-22H374/apple-hid-abi.json').read_text())
        self.assertEqual(abi['classes']['IOHIDDevice']['instance_size'], 208)
        self.assertEqual(abi['classes']['IOHIDDevice']['vtable_slots'], 207)
        self.assertIn('IOHIDEventDriver', abi['classes'])
        self.assertIn('__ZN16IOHIDEventDriver10gMetaClassE', abi['required_symbols'])
        self.assertEqual(build['vtable_contracts']['QcomButtons']['slots'], 207)
        for name in ('FingerprintHID','GoodixFingerprint','FPCFingerprint'):
            self.assertEqual(build['vtable_contracts'][name]['slots'],207)
            self.assertEqual(build['vtable_contracts'][name]['bindings']['111'],'FingerprintHID::getWorkLoop')

    def test_chip_kexts_share_the_physical_provider_and_native_hid_base(self):
        scores=[]
        for name,compatible in [('GoodixFingerprint','goodix,fingerprint'),('FPCFingerprint','fpc,fpc1020')]:
            info=plistlib.loads((DRIVERS/'input'/name/'Info.plist').read_bytes())
            personality=info['IOKitPersonalities'][name]
            self.assertEqual(personality['IOProviderClass'],'AppleARMIODevice')
            self.assertEqual(personality['IONameMatch'],compatible)
            self.assertNotIn('IOMatchCategory',personality) # both use IOKit's single default category
            self.assertIn('org.darwin.driver.FingerprintHID',info['OSBundleLibraries'])
            scores.append(personality['IOProbeScore'])
        self.assertGreater(scores[0],scores[1])
        info=plistlib.loads((DRIVERS/'input/FingerprintHID/Info.plist').read_bytes())
        self.assertEqual(info['OSBundleLibraries']['org.darwin.driver.QcomSCM'],'1.0.0')
        personality=info['IOKitPersonalities']['Fingerprint Home HID Events']
        self.assertEqual(personality['CFBundleIdentifier'],'com.apple.iokit.IOHIDFamily')
        self.assertEqual(personality['IOPropertyMatch'],{'Transport':'SPI','Product':'Fingerprint Home'})


if __name__ == '__main__':
    unittest.main()
