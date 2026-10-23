import importlib.util
from pathlib import Path
import unittest
import json
import hashlib
import tempfile

spec = importlib.util.spec_from_file_location(
    "kernelcache_analysis", Path(__file__).parents[1] / "scripts/analyze-kernelcache.py")
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class AnalysisTests(unittest.TestCase):
    def test_apple_idrefs_preserve_versions_and_shared_dependencies(self):
        raw = b'''<dict><key>version</key><string ID="1">24.6.0</string>
        <key>again</key><string IDREF="1"/><key>deps</key><dict ID="2">
        <key>com.apple.kpi.iokit</key><string IDREF="1"/></dict>
        <key>shared</key><dict IDREF="2"/><key>address</key>
        <integer>0xfffffff007004000</integer></dict>'''
        parsed = analysis.decode_plist(raw)
        self.assertEqual(parsed["again"], "24.6.0")
        self.assertEqual(parsed["shared"], {"com.apple.kpi.iokit": "24.6.0"})
        self.assertEqual(parsed["address"], 0xfffffff007004000)

    def test_missing_dependencies_are_not_silently_dropped(self):
        bundles = {"driver": {"OSBundleLibraries": {"family": "1", "missing": "1"}},
                   "family": {"OSBundleLibraries": {"kpi": "1"}}, "kpi": {}}
        closure = analysis.closure(bundles, ["driver"])
        self.assertEqual(closure["dependency_order"], ["kpi", "family", "driver"])
        self.assertEqual(closure["missing"], ["missing"])
        self.assertEqual(closure["status"], "blocked-missing-dependencies")

    def test_invalid_refs_and_cycles_fail(self):
        with self.assertRaises(ValueError):
            analysis.decode_plist(b'<string IDREF="unknown"/>')
        with self.assertRaises(ValueError):
            analysis.closure({"a": {"OSBundleLibraries": {"b": ""}},
                              "b": {"OSBundleLibraries": {"a": ""}}}, ["a"])

    def test_architecture_dictionary_replaces_fallback_and_preserves_original(self):
        kext = {"OSBundleLibraries": {"unavailable-generic": "1"},
                "OSBundleLibraries_arm64": {"arm-provider": "2"},
                "OSBundleLibraries_x86_64": {"x86-provider": "3"}}
        snapshot = json.loads(json.dumps(kext))
        self.assertEqual(analysis.architecture_libraries(kext), {"arm-provider": "2"})
        self.assertEqual(analysis.architecture_libraries(kext, 'x86_64'), {"x86-provider": "3"})
        self.assertEqual(analysis.architecture_libraries(kext, 'armv7'), {"unavailable-generic": "1"})
        graph = {"client": kext, "arm-provider": {}, "x86-provider": {}}
        self.assertEqual(analysis.closure(graph, ['client'])['dependency_order'], ['arm-provider', 'client'])
        self.assertFalse(analysis.closure(graph, ['client'])['missing'])
        self.assertEqual(kext, snapshot)
        kext['OSBundleLibraries_arm64'] = {}
        self.assertEqual(analysis.architecture_libraries(kext), {})
        kext['OSBundleLibraries_arm64'] = 'invalid override'
        with self.assertRaisesRegex(ValueError, 'architecture-specific'):
            analysis.architecture_libraries(kext)

    def test_real_arm64_smc_does_not_require_x86_fallback_libraries(self):
        root = Path(__file__).parents[1]
        path = root / 'build/ios18-kext-analysis/kernelcache.release.ipad7c.macho'
        if not path.exists():
            self.skipTest('reference image is not installed')
        metadata, kexts = analysis.inspect(path)
        self.assertEqual(analysis.image_architecture(metadata), 'arm64')
        smc = next(k for k in kexts if k['CFBundleIdentifier'] == 'com.apple.driver.AppleSMC')
        self.assertIn('com.apple.iokit.IOACPIFamily', smc['OSBundleLibraries'])
        selected = analysis.architecture_libraries(smc)
        self.assertNotIn('com.apple.iokit.IOACPIFamily', selected)
        self.assertNotIn('com.apple.driver.watchdog', selected)
        self.assertIn('com.apple.driver.AppleARMPlatform', selected)
        self.assertEqual(analysis.architecture_property(smc, 'OSBundleCompatibleVersion'), '1.0')
        self.assertEqual(smc['OSBundleCompatibleVersion'], '1.0.1d1')

    def test_architecture_version_uses_os_override_but_never_cf_override(self):
        metadata = {'OSBundleCompatibleVersion': '3', 'OSBundleCompatibleVersion_arm64': '1',
                    'CFBundleVersion': '4', 'CFBundleVersion_arm64': '99'}
        self.assertEqual(analysis.architecture_property(metadata, 'OSBundleCompatibleVersion'), '1')
        self.assertEqual(analysis.architecture_property(metadata, 'OSBundleCompatibleVersion', 'x86_64'), '3')
        self.assertEqual(analysis.architecture_property(metadata, 'CFBundleVersion'), '4')

    def test_dependency_audit_rejects_missing_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'image'
            path.write_bytes(b'')
            with self.assertRaisesRegex(ValueError, 'provider absent'):
                analysis.dependency_audit(path, {'segments': []}, [], 'provider')

    def test_resolved_native_pmgr_references(self):
        root = Path(__file__).parents[1]
        path = root / 'build/ios18-kext-analysis/kernelcache.release.ipad7c.macho'
        if not path.exists():
            self.skipTest('pinned reference kernelcache is not installed')
        if hashlib.sha256(path.read_bytes()).hexdigest() != 'aa65c36c486a8b5cb1d7a2d565d1d6b3cc228b3c172f8c69c5da664ee19832cf':
            self.skipTest('different provider image requires a new audit')
        metadata, kexts = analysis.inspect(path)
        audit = analysis.dependency_audit(path, metadata, kexts, 'com.apple.driver.ApplePMGR')
        clients = {c['bundle']: c['references'] for c in audit['clients']}
        self.assertEqual(len(clients), 4)
        refs = clients['com.apple.driver.AppleMobileFileIntegrity']
        self.assertEqual(sum(r['kind'] == 'pointer' for r in refs), 3)
        calls = {r['address']: r['target'] for r in refs if r['kind'] == 'import-call'}
        self.assertEqual(calls, {'0xfffffff005a74bc8': '0xfffffff0059dfaa4',
                                 '0xfffffff005a74bd8': '0xfffffff0059ddfe8'})
        bad = json.loads(json.dumps(kexts))
        next(k for k in bad if k['CFBundleIdentifier'] == 'com.apple.driver.ApplePMGR')['_PrelinkExecutableLoadAddr'] = 1
        with self.assertRaisesRegex(ValueError, 'outside file-backed'):
            analysis.dependency_audit(path, metadata, bad, 'com.apple.driver.ApplePMGR')


if __name__ == "__main__":
    unittest.main()
