"""Verify original inputs and measured evidence used by the modem components."""
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import baseband
from baseband_dyld import export_trie
from baseband_services import analyze as analyze_services
from apfs_sealed import SealedImage


class BasebandIntegration(unittest.TestCase):
    def test_remote_service_names_are_tied_to_msm8998_and_pinned_bytes(self):
        report = json.loads((ROOT / 'docs/baseband/remote-services.json').read_text())
        with tempfile.TemporaryDirectory() as directory:
            actual = analyze_services(ROOT, Path(directory) / 'services.json')
        self.assertEqual(actual, report)
        self.assertEqual(report['msm8998_peer_descriptors']['ADSP']['ssr_name'], 'lpass')
        self.assertEqual(report['msm8998_peer_descriptors']['ADSP']['sysmon_name'], 'adsp')
        self.assertEqual(report['msm8998_peer_descriptors']['SLPI']['ssr_name'], 'dsps')
        self.assertEqual(report['msm8998_peer_descriptors']['SLPI']['sysmon_name'], 'slpi')
        modem = ROOT / report['source']
        raw = modem.read_bytes()
        for row in report['literal_observations']:
            literal = row['literal'].encode() + b'\0'
            for position in row['file_offsets']:
                self.assertEqual(raw[position:position + len(literal)], literal)
        self.assertFalse(report['runtime_channel_names_proven'])
        self.assertFalse(report['runtime_qmi_version_proven'])
        self.assertFalse(report['foreign_stop_events_connected'])
        self.assertFalse(report['hardware_tested'])
        with self.assertRaisesRegex(ValueError, 'input sources'):
            analyze_services(ROOT, modem)
        self.assertEqual(hashlib.sha256(modem.read_bytes()).hexdigest(), report['sha256'])

    def test_reference_hashes_and_imported_firmware(self):
        directory = ROOT / 'docs/baseband/reference'
        for name, record in json.loads((directory / 'sources.json').read_text()).items():
            with self.subTest(name=name):
                self.assertEqual(baseband.sha(directory / name), record['sha256'])
        sources = json.loads((ROOT / 'docs/baseband/firmware-sources.json').read_text())
        self.assertIn('community extraction', sources['source_kind'])
        for name, record in sources['files'].items():
            path = ROOT / 'firmware/sagit-baseband' / name
            self.assertEqual(path.stat().st_size, record['bytes'])
            self.assertEqual(baseband.sha(path), record['sha256'])
        staged = json.loads((ROOT / 'build/baseband/firmware/manifest.json').read_text())
        for flag in ('original_xiaomi_package_verified', 'device_authentication_verified', 'memory_reserved', 'hardware_tested', 'installed'):
            self.assertIs(staged[flag], False)
        for name in ('mba.mbn', 'modem.mbn', 'modemr.jsn', 'modemuw.jsn'):
            self.assertEqual(baseband.sha(ROOT / 'build/baseband/firmware' / name), sources['files'][name]['sha256'])

    def test_staging_rejects_corruption_before_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'docs/baseband').mkdir(parents=True)
            (root / 'firmware/sagit-baseband').mkdir(parents=True)
            raw = bytearray(52); raw[:7] = b'\x7fELF\x01\x01\x01'; struct.pack_into('<H', raw, 18, 164)
            for name in ('mba.mbn', 'modem.mbn'):
                (root / 'firmware/sagit-baseband' / name).write_bytes(raw)
            manifest = {'source_kind': 'test-only-fixture', 'files': {n: {'bytes': 52, 'sha256': hashlib.sha256(raw).hexdigest()}
                        for n in ('mba.mbn', 'modem.mbn')}}
            for name in ('modemr.jsn', 'modemuw.jsn'):
                descriptor = b'{"test":true}'
                (root / 'firmware/sagit-baseband' / name).write_bytes(descriptor)
                manifest['files'][name] = {'bytes': len(descriptor), 'sha256': hashlib.sha256(descriptor).hexdigest()}
            (root / 'docs/baseband/firmware-sources.json').write_text(json.dumps(manifest))
            output = root / 'staging'
            baseband.prepare(root, output)
            good = (output / 'manifest.json').read_bytes()
            (root / 'firmware/sagit-baseband/modem.mbn').write_bytes(raw + b'bad')
            with self.assertRaises(ValueError):
                baseband.prepare(root, output)
            self.assertEqual((output / 'manifest.json').read_bytes(), good)

    def test_primary_is_the_actual_ipad_provider_and_manifest(self):
        model = json.loads((ROOT / 'docs/baseband/ios18-model.json').read_text())
        primary = model['firmware'][0]
        self.assertEqual((primary['role'], primary['ProductBuildVersion']), ('primary', '22H374'))
        with zipfile.ZipFile(ROOT / primary['path']) as source:
            raw = source.read('BuildManifest.plist')
        self.assertEqual(hashlib.sha256(raw).hexdigest(), primary['manifest_sha256'])
        self.assertEqual(plistlib.loads(raw)['ProductVersion'], primary['ProductVersion'])
        provider = ROOT / 'build/ios18-kext-analysis' / model['kernel']['file']
        self.assertEqual(baseband.sha(provider), model['kernel']['sha256'])
        spec = importlib.util.spec_from_file_location('baseband_native_analysis', ROOT / 'scripts/analyze-kernelcache.py')
        reader = importlib.util.module_from_spec(spec); spec.loader.exec_module(reader)
        _, kexts = reader.inspect(provider)
        by_id = {k['CFBundleIdentifier']: k for k in kexts}
        for expected in model['kernel_kexts']:
            for key, value in expected.items():
                self.assertEqual(by_id[expected['CFBundleIdentifier']][key], value)
        native = ROOT / 'build/baseband/reference/CommCenter'
        self.assertEqual(baseband.sha(native), model['user_space']['CommCenter']['sha256'])
        self.assertEqual(baseband.macho_evidence(native.read_bytes())['dependencies'],
                         model['user_space']['CommCenter']['dependencies'])

    def test_runtime_libraries_and_scalar_evidence_consistency(self):
        path = ROOT / 'docs/baseband/ipad-runtime-libraries.json'
        model = json.loads(path.read_text())
        self.assertEqual(model['build'], '22H374')
        self.assertTrue(model['read_only'])
        provenance = model['input_provenance']
        with zipfile.ZipFile(ROOT / provenance['ipsw']) as archive:
            root_blob = archive.read(provenance['root_hash_member'])
        self.assertEqual(hashlib.sha384(root_blob).hexdigest(), provenance['root_hash_im4p_sha384'])
        image = SealedImage(ROOT / model['source'])
        try:
            self.assertEqual(image.original_root_hash.hex(), provenance['apfs_seal_sha256'])
        finally:
            image.close()
        self.assertTrue(provenance['seal_matches_manifest_payload'])
        self.assertEqual(len({m['uuid'] for m in model['cache_members']}), 61)
        libraries = {l['path']: l for l in model['libraries']}
        self.assertIn('/usr/lib/libBasebandCommandDriversQMI.dylib', libraries)
        self.assertIn('/usr/lib/libBasebandManagerICE.dylib', libraries)
        calls = json.loads((ROOT / 'docs/baseband/ipad-apple-control.json').read_text())
        self.assertEqual(calls['runtime_evidence_sha256'], baseband.sha(path))
        self.assertEqual(calls['library_uuid'], libraries[calls['library']]['uuid'])
        self.assertEqual(calls['source_sha256'], model['source_sha256'])
        shapes = {row['name']: (row['selector'], row['scalar_inputs'], row['scalar_outputs']) for row in calls['observations']}
        self.assertEqual(shapes['_TelephonyBasebandPowerOnModem'], (1, 1, 0))
        self.assertEqual(shapes['_TelephonyBasebandGetBasebandState'], (26, 0, 1))
        self.assertNotIn('_TelephonyBasebandResetModem', shapes) # Multiple calls, not a single scalar API.
        for row in calls['observations']:
            for key in ('implementation', 'helper'):
                evidence = row[key]
                raw = b''.join(bytes.fromhex(i['bytes']) for i in evidence['instructions'])
                self.assertEqual(len(raw), evidence['bytes'])
                self.assertEqual(hashlib.sha256(raw).hexdigest(), evidence['sha256'])

    def test_export_trie_bounds_and_cycle_rejection(self):
        # Root -> "abc" -> terminal with regular export address base+4.
        valid = b'\0\x01abc\0\x07\x02\0\x04\0'
        self.assertEqual(export_trie(valid, 0x1000), [{'name': 'abc', 'flags': 0, 'address': '0x1004'}])
        for invalid in (valid[:-1], b'\0\x01x\0\0', b'\x80' * 12, b'\x7f\0'):
            with self.assertRaises(ValueError): export_trie(invalid, 0x1000)

    def test_device_node_remains_offline_and_collection_keeps_native_dependencies(self):
        info = plistlib.loads((ROOT / 'drivers/baseband/QcomBaseband/Info.plist').read_bytes())
        personality = info['IOKitPersonalities']['MSM8998 Baseband']
        self.assertEqual(personality['IOProviderClass'], 'AppleARMIODevice')
        self.assertEqual(personality['IONameMatch'], 'qcom,msm8998-mss-pil')
        tree = (ROOT / 'device_trees/msm8998.dtsi').read_text()
        modem = tree.split('modem: modem@4080000 {', 1)[1].split('};', 1)[0]
        self.assertIn('status = "disabled";', modem)
        self.assertNotIn('baseband-image', modem)
        shared = tree.split('smem: shared-memory@86000000 {', 1)[1].split('};', 1)[0]
        self.assertIn('status = "disabled";', shared)
        smem = plistlib.loads((ROOT / 'drivers/platform/QcomSMEM/Info.plist').read_bytes())
        personalities = smem['IOKitPersonalities'].values()
        self.assertTrue(any(p['IONameMatch'] == 'qcom,msm8998-smem' for p in personalities))
        self.assertTrue(all('IOUserClientClass' not in p for p in personalities))
        build = json.loads((ROOT / 'drivers/Build.json').read_text())
        roots = build['collections']['baseband']['roots']
        self.assertIn('org.darwin.driver.QcomBaseband', roots)
        self.assertIn('org.darwin.driver.QcomSCM', roots)
        self.assertIn('org.darwin.driver.QcomSMEM', roots)
        self.assertIn('org.darwin.driver.QcomUFS', roots)
        self.assertIn('org.darwin.driver.QcomSMEM', build['collections']['audio']['roots'])


if __name__ == '__main__':
    unittest.main()
