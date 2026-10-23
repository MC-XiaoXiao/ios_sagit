"""Generic Kext discovery and a real C-only module build, with isolated outputs."""
import importlib.util
import json
import hashlib
from pathlib import Path
import plistlib
import subprocess
import struct
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('driver_toolchain', ROOT / 'scripts/toolchain.py')
toolchain = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolchain)


class DriverToolsTests(unittest.TestCase):
    def test_usb_firmware_elf_reads_only_file_backed_loads(self):
        for bits in (32, 64):
            with self.subTest(bits=bits):
                data = bytearray(0x300)
                data[:7] = b'\x7fELF' + bytes((1 if bits == 32 else 2, 1, 1))
                struct.pack_into('<H', data, 18, 40 if bits == 32 else 183)
                if bits == 32:
                    struct.pack_into('<I', data, 28, 52)
                    struct.pack_into('<HH', data, 42, 32, 2)
                    struct.pack_into('<8I', data, 52, 1, 0x200, 0x1000, 0x1000, 16, 32, 5, 4)
                    struct.pack_into('<8I', data, 84, 1, 0x240, 0x2000, 0x2000, 16, 16, 6, 4)
                else:
                    struct.pack_into('<Q', data, 32, 64)
                    struct.pack_into('<HH', data, 54, 56, 2)
                    struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0x200, 0x1000, 0x1000, 16, 32, 4)
                    struct.pack_into('<IIQQQQQQ', data, 120, 1, 6, 0x240, 0x2000, 0x2000, 16, 16, 4)
                data[0x200:0x210] = bytes(range(16))
                read = toolchain.usb_elf_reader(data, bits)
                self.assertEqual(read(0x1000, 16), bytes(range(16)))
                self.assertEqual(read(0x2000, 16), bytes(16))
                for address, length in ((0x1010, 1), (0xfff, 2), (0x1008, 16), (0x1800, 1), (-1, 1), (0x1000, 0)):
                    with self.assertRaises(ValueError):
                        read(address, length)
                for index, value in ((5, 2), (18, 0)):
                    bad = bytearray(data); bad[index] = value
                    with self.assertRaises(ValueError):
                        toolchain.usb_elf_reader(bad, bits)
                with self.assertRaises(ValueError):
                    toolchain.usb_elf_reader(data[:100], bits)
                bad = bytearray(data)
                struct.pack_into('<I' if bits == 32 else '<Q', bad, 84 + 8 if bits == 32 else 120 + 16, 0x1000)
                with self.assertRaisesRegex(ValueError, 'overlapping'):
                    toolchain.usb_elf_reader(bad, bits)
        with self.assertRaisesRegex(ValueError, 'class'):
            toolchain.usb_elf_reader(b'', 16)

    def test_usb_stock_resource_extraction_preserves_missing_runtime_data(self):
        directory = ROOT / 'build/usb/firmware/stock'
        if not (directory / 'hyp.mbn').exists():
            self.skipTest('stock firmware is an optional offline analysis input')
        output = self.root / 'evidence.json'
        report = toolchain.analyze_usb_resources(directory, output)
        self.assertEqual(json.loads(output.read_text()), report)
        namespace = report['hyp_static_namespace']
        self.assertEqual([d['name'] for d in namespace['devices']],
                         ['ANOC1', 'ANOC2', 'LPASS_CORE', 'LPASS_Q6', 'MMNOC', 'GPU'])
        usb = namespace['reference_usb_matches']
        self.assertEqual(len(usb), 1)
        self.assertEqual((usb[0]['assignment_index'], usb[0]['declared_vmid'], usb[0]['lookup_guard_word']), (6, 3, 0))
        self.assertFalse(usb[0]['lookup_eligible_in_initial_file'])
        self.assertFalse(namespace['assignment_index_is_a_usb_context_grant'])
        self.assertEqual(namespace['dynamic_sid_call']['accepted_device_name'], 'MMNOC')
        self.assertFalse(namespace['dynamic_sid_call']['usb_allocator_contract'])
        self.assertEqual({r['hz']: r['minimum_active_cx_level'] for r in report['usb_core_clock']['rates']},
                         {19200000: 64, 60000000: 64, 120000000: 128, 150000000: 256})
        proxy = report['rpm_initial_proxy_votes']
        apss = {(v['resource_type'], v['resource_id'], v['key'], v['set']): v['value']
                for v in proxy['votes'] if v['ee'] == 0}
        self.assertEqual(apss, {('0x6d7073', 0, '0x62616e45', 'active'): 1,
                                ('0x6d7073', 0, '0x62616e45', 'sleep'): 0,
                                ('0x306b6c63', 0, '0x62616e45', 'active'): 1,
                                ('0x78637772', 0, '0x6c766c76', 'active'): 256})
        self.assertEqual(len([v for v in proxy['votes'] if v['ee'] == 1]), 2)
        self.assertFalse(proxy['complete_AP_active_sleep_baseline'])
        self.assertFalse(proxy['current_device_values_proven'])
        self.assertTrue(all(v is None for v in report['production_inputs'].values()))
        for key in ('production_profile_created', 'production_USB_nodes_enabled', 'firmware_executed', 'hardware_tested'):
            self.assertFalse(report[key])
        self.assertEqual(report['runtime_resource_grants'], 0)
        self.assertFalse(list(self.root.glob('*.tmp')))

    def test_usb_resource_extraction_rejects_modified_inputs_without_committing(self):
        stock = ROOT / 'build/usb/firmware/stock'
        if not (stock / 'hyp.mbn').exists():
            self.skipTest('stock firmware is an optional offline analysis input')
        directory = self.root / 'firmware'; directory.mkdir()
        for name in ('hyp.mbn', 'rpm.mbn', 'tz.mbn', 'devcfg.mbn', 'xbl.elf'):
            (directory / name).symlink_to(stock / name)
        output = self.root / 'evidence.json'; output.write_bytes(b'previous evidence')
        before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.iterdir()}
        with self.assertRaisesRegex(ValueError, 'outside'):
            toolchain.analyze_usb_resources(directory, stock / 'hyp.mbn')
        with self.assertRaisesRegex(ValueError, 'outside'):
            toolchain.analyze_usb_resources(directory, directory / 'nested/output.json')
        (directory / 'hyp.mbn').unlink()
        changed = bytearray((stock / 'hyp.mbn').read_bytes()); changed[-1] ^= 1
        (directory / 'hyp.mbn').write_bytes(changed)
        with self.assertRaisesRegex(ValueError, 'firmware hash'):
            toolchain.analyze_usb_resources(directory, output)
        self.assertEqual(output.read_bytes(), b'previous evidence')
        self.assertFalse(list(self.root.glob('*.tmp')))
        self.assertEqual({name: hashlib.sha256((stock / name).read_bytes()).hexdigest() for name in before}, before)
        with self.assertRaisesRegex(ValueError, 'HYP firmware hash'):
            toolchain.usb_hyp_assignments(changed)

    def test_usb_resource_extraction_rejects_changed_kernel_reference(self):
        directory = self.root / 'docs/usb/reference/board'; directory.mkdir(parents=True)
        (directory / 'gcc.c').write_text('modified kernel reference')
        manifest = {'board/gcc.c': {'sha256': '0' * 64}}
        (directory.parent / 'sources.json').write_text(json.dumps(manifest))
        output = self.root / 'evidence.json'; output.write_bytes(b'previous evidence')
        with mock.patch.object(toolchain, 'ROOT', self.root):
            with self.assertRaisesRegex(ValueError, 'kernel reference hash'):
                toolchain.analyze_usb_resources(self.root / 'firmware', output)
        self.assertEqual(output.read_bytes(), b'previous evidence')

    def test_collection_inherits_dependencies_without_reusing_parent_output(self):
        path = self.root / 'Build.json'
        definitions = {'display': {'roots': ['surface', 'display'], 'output': 'display.kc', 'tests': ['display']},
                       'gpu': {'roots': ['surface', 'gpu'], 'output': 'gpu.kc', 'tests': ['gpu']},
                       'graphics': {'inherits': ['display', 'gpu'], 'output': 'graphics.kc', 'tests': ['gpu-submit']}}
        path.write_text(json.dumps({'collections': definitions}))
        result = toolchain.driver_configuration()['collections']['graphics']
        self.assertEqual(result['roots'], ['surface', 'display', 'gpu'])
        self.assertEqual(result['output'], 'graphics.kc')
        self.assertEqual(result['tests'], ['gpu-submit'])
        self.assertNotIn('roots', definitions['graphics'])
        definitions['display']['inherits'] = ['graphics']
        path.write_text(json.dumps({'collections': definitions}))
        with self.assertRaisesRegex(RuntimeError, 'inheritance cycle'):
            toolchain.driver_configuration()
        definitions['display']['inherits'] = ['missing']
        path.write_text(json.dumps({'collections': definitions}))
        with self.assertRaisesRegex(RuntimeError, 'unknown inherited collection'):
            toolchain.driver_configuration()

    def test_bsd_kpi_uses_actual_xnu_export_table(self):
        import fnmatch
        symbols=['_ctl_register','_ctl_deregister','_ctl_enqueuedata','_mbuf_copydata','_mbuf_freem','_mbuf_pkthdr_len']
        allowed=toolchain.driver_kpi_patterns({'com.apple.kpi.bsd':'8.0'})
        unrelated=toolchain.driver_kpi_patterns({'com.apple.kpi.iokit':'8.0'})
        for symbol in symbols:
            self.assertTrue(any(fnmatch.fnmatchcase(symbol,p) for p in allowed),symbol)
            self.assertFalse(any(fnmatch.fnmatchcase(symbol,p) for p in unrelated),symbol)
        self.assertFalse(any(fnmatch.fnmatchcase('_not_an_exported_kernel_symbol',p) for p in allowed))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='kext-tool-test-', dir=ROOT / 'build')
        self.root = Path(self.temp.name)
        self.previous = toolchain.DRIVERS_ROOT, toolchain.DRIVER_FILTERS
        toolchain.DRIVERS_ROOT, toolchain.DRIVER_FILTERS = self.root, []

    def tearDown(self):
        toolchain.DRIVERS_ROOT, toolchain.DRIVER_FILTERS = self.previous
        self.temp.cleanup()

    def bundle(self, directory='UnrelatedSource', identifier='org.example.fixture', executable='DifferentExecutable'):
        path = self.root / directory
        path.mkdir(parents=True, exist_ok=True)
        info = {'CFBundleIdentifier': identifier, 'CFBundleExecutable': executable,
                'CFBundleVersion': '1.2.3', 'CFBundlePackageType': 'KEXT'}
        (path / 'Info.plist').write_bytes(plistlib.dumps(info))
        return path

    def test_discovers_identity_from_plist(self):
        self.bundle()
        sources = toolchain.driver_sources()
        self.assertEqual(sources[0][1]['CFBundleExecutable'], 'DifferentExecutable')
        toolchain.DRIVER_FILTERS = ['org.example.fixture']
        self.assertEqual(len(toolchain.driver_sources()), 1)
        toolchain.DRIVER_FILTERS = ['nonexistent']
        with self.assertRaisesRegex(RuntimeError, 'unknown driver selection'):
            toolchain.driver_sources()

    def test_categorized_discovery_ignores_userspace_and_driverkit_bundles(self):
        kernel = self.bundle('usb/Controller')
        for category, package_type in [('wifi/DriverKit', 'DEXT'), ('gpu/Metal', 'BNDL')]:
            path = self.root / category
            path.mkdir(parents=True)
            (path / 'Info.plist').write_bytes(plistlib.dumps({'CFBundlePackageType': package_type}))
        self.assertEqual([source for source, _ in toolchain.driver_sources()], [kernel])
        toolchain.DRIVER_FILTERS = ['Controller']
        self.assertEqual([source for source, _ in toolchain.driver_sources()], [kernel])

    def test_duplicate_identity_across_categories_is_rejected(self):
        self.bundle('usb/Controller')
        self.bundle('storage/Controller', executable='OtherExecutable')
        with self.assertRaisesRegex(RuntimeError, 'duplicate Kext'):
            toolchain.driver_sources()

    def test_collection_preserves_provider_order_and_reports_all_missing_libraries(self):
        metadata = {
            'client': {'OSBundleLibraries': {'left': '1', 'right': '1'}},
            'left': {'OSBundleLibraries': {'shared': '1', 'missing-a': '1'}},
            'right': {'OSBundleLibraries': {'shared': '1', 'missing-b': '1'}},
            'shared': {},
        }
        with self.assertRaisesRegex(RuntimeError, 'missing collection dependencies') as error:
            toolchain.collection_bundle_order(metadata, ['client'])
        self.assertIn('missing-a (required by left)', str(error.exception))
        self.assertIn('missing-b (required by right)', str(error.exception))
        del metadata['left']['OSBundleLibraries']['missing-a']
        del metadata['right']['OSBundleLibraries']['missing-b']
        self.assertEqual(toolchain.collection_bundle_order(metadata, ['client', 'left']),
                         ['shared', 'left', 'right', 'client'])
        metadata['shared']['OSBundleLibraries'] = {'client': '1'}
        with self.assertRaisesRegex(RuntimeError, 'cyclic bundle dependency'):
            toolchain.collection_bundle_order(metadata, ['client'])
        with self.assertRaisesRegex(RuntimeError, 'empty collection'):
            toolchain.collection_bundle_order(metadata, [])

    def test_collection_uses_explicit_architecture_reader_without_mutating_plists(self):
        spec = importlib.util.spec_from_file_location('inventory', ROOT / 'scripts/analyze-kernelcache.py')
        inventory = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(inventory)
        metadata = {'client': {'OSBundleLibraries': {'unavailable': '1'},
                               'OSBundleLibraries_arm64': {'arm': '2'}}, 'arm': {}}
        snapshot = json.loads(json.dumps(metadata))
        self.assertEqual(toolchain.collection_bundle_order(metadata, ['client'], inventory.architecture_libraries),
                         ['arm', 'client'])
        self.assertEqual(metadata, snapshot)
        metadata['client']['OSBundleLibraries_arm64'] = {'actual-missing': '2'}
        with self.assertRaisesRegex(RuntimeError, 'actual-missing'):
            toolchain.collection_bundle_order(metadata, ['client'], inventory.architecture_libraries)

    def test_host_test_abi_flags_use_the_unified_compiler(self):
        (self.root / 'probe.cpp').write_text(
            'static_assert(sizeof(wchar_t)==2, "UEFI wide characters"); int main() { return 0; }\n')
        definition = {'tests': [{'name': 'abi-probe', 'sources': ['probe.cpp'],
                                  'host_flags': ['-fshort-wchar']}]}
        (self.root / 'Build.json').write_text(json.dumps(definition))
        previous = toolchain.BUILD
        toolchain.BUILD = self.root / 'output'
        try:
            toolchain.test_drivers(['abi-probe'])
            self.assertTrue((toolchain.BUILD / 'test-abi-probe').exists())
            definition['tests'][0]['host_flags'] = 'invalid string instead of argument list'
            (self.root / 'Build.json').write_text(json.dumps(definition))
            with self.assertRaisesRegex(RuntimeError, 'host test compiler flags'):
                toolchain.test_drivers(['abi-probe'])
        finally:
            toolchain.BUILD = previous

    def test_external_personality_requires_declared_audited_provider(self):
        symbol = '__ZN7Fixture10gMetaClassE'
        personality = {'IOClass': 'Fixture', 'IOProviderClass': 'IOService',
                       'CFBundleIdentifier': 'org.example.provider'}
        verify = toolchain.verify_matching_personality
        result = verify(personality, 'org.example.client', {'org.example.provider': '1.0'}, set(),
                        {'org.example.provider': {symbol}}, {'org.example.provider': {'Fixture'}})
        self.assertEqual(result['bundle_identifier'], 'org.example.provider')
        with self.assertRaisesRegex(RuntimeError, 'declared dependency'):
            verify(personality, 'org.example.client', {}, set(), {'org.example.provider': {symbol}}, {})
        with self.assertRaisesRegex(RuntimeError, 'missing from binary/provider'):
            verify(personality, 'org.example.client', {'org.example.provider': '1.0'}, set(), {}, {})
        with self.assertRaisesRegex(RuntimeError, 'layout contract'):
            verify(personality, 'org.example.client', {'org.example.provider': '1.0'}, set(),
                   {'org.example.provider': {symbol}}, {'org.example.provider': set()})

    def test_rejects_duplicate_and_path_executable(self):
        self.bundle()
        self.bundle('SecondSource', executable='OtherExecutable')
        with self.assertRaisesRegex(RuntimeError, 'duplicate Kext'):
            toolchain.driver_sources()
        (self.root / 'SecondSource/Info.plist').unlink()
        self.bundle('SecondSource', identifier='org.example.second', executable='../escape')
        with self.assertRaisesRegex(RuntimeError, 'invalid Kext identity'):
            toolchain.driver_sources()

    def test_c_only_module_without_platform_contract(self):
        path = self.bundle(executable='FixtureCOnly')
        (path / 'module.c').write_text('int fixture_value(void) { return 7; }\n')
        destination = self.root / 'artifacts/Extensions'
        result = subprocess.run(['python3', str(ROOT / 'scripts/toolchain.py'), 'build-drivers',
                                 '--drivers-root', str(self.root), '--driver-output', str(destination)],
                                cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads((destination.parent / 'verification-drivers.json').read_text())
        self.assertEqual(report['drivers'][0]['identifier'], 'org.example.fixture')
        self.assertEqual(report['provider_contracts'], [])
        self.assertEqual(report['drivers'][0]['imports'], [])
        self.assertTrue((destination / 'FixtureCOnly.kext/Contents/MacOS/FixtureCOnly').is_file())

    def test_cpp_module_uses_generic_personality_and_kernel_kpis(self):
        path = self.bundle(executable='FixtureCpp')
        plist_path = path / 'Info.plist'
        info = plistlib.loads(plist_path.read_bytes())
        info['OSBundleLibraries'] = {'com.apple.kpi.' + family: '8.0'
                                     for family in ('iokit', 'libkern', 'mach', 'unsupported', 'private')}
        info['IOKitPersonalities'] = {'Fixture': {'IOClass': 'ArbitraryFixtureService',
                                                 'IOProviderClass': 'IOResources', 'IOResourceMatch': 'IOKit'}}
        plist_path.write_bytes(plistlib.dumps(info))
        (path / 'module.cpp').write_text(
            '#include <IOKit/IOService.h>\n'
            'class ArbitraryFixtureService : public IOService {\n'
            ' OSDeclareDefaultStructors(ArbitraryFixtureService);\n};\n'
            'OSDefineMetaClassAndStructors(ArbitraryFixtureService, IOService);\n')
        destination = self.root / 'artifacts/Extensions'
        result = subprocess.run(['python3', str(ROOT / 'scripts/toolchain.py'), 'build-drivers',
                                 '--drivers-root', str(self.root), '--driver-output', str(destination)],
                                cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads((destination.parent / 'verification-drivers.json').read_text())
        self.assertEqual(report['provider_contracts'], [])
        self.assertEqual(report['drivers'][0]['unavailable_kpis'], [])

    def test_private_function_binding_requires_executable_mapped_bytes(self):
        data = bytearray(160)
        struct.pack_into('<8I', data, 0, 0xfeedfacf, 0x100000c, 0, 2, 1, 72, 0, 0)
        struct.pack_into('<II16sQQQQiiII', data, 32, 0x19, 72, b'__TEXT_EXEC',
                         0x1000, 160, 0, 160, 5, 5, 0, 0)
        data[128:136] = bytes.fromhex('1f2003d5c0035fd6')
        image = self.root / 'provider.macho'
        image.write_bytes(data)
        self.assertEqual(toolchain.provider_function_bytes(image, 0x1080, 8), data[128:136])
        for address, length in ((0x1081, 8), (0x2000, 8), (0x1080, 257), (0x109c, 8)):
            with self.assertRaises(RuntimeError):
                toolchain.provider_function_bytes(image, address, length)
        struct.pack_into('<i', data, 92, 1)  # read-only, non-executable segment
        image.write_bytes(data)
        with self.assertRaisesRegex(RuntimeError, 'not executable'):
            toolchain.provider_function_bytes(image, 0x1080, 8)

    def test_compiled_private_subclass_slots_reject_shift_and_pure_hook(self):
        text = """Vtable for 'DisplayFixture' (6 entries).
   0 | offset_to_top (0)
   1 | DisplayFixture RTTI
   2 | DisplayFixture::~DisplayFixture() [complete]
   3 | DisplayFixture::~DisplayFixture() [deleting]
   4 | bool DisplayFixture::start_hardware()
   5 | vcall_offset (0)

"""
        contract = {'slots': 3, 'bindings': {'2': 'DisplayFixture::start_hardware'}}
        self.assertEqual(toolchain.verify_compiler_vtable(text, 'DisplayFixture', contract)['slots'], 3)
        with self.assertRaisesRegex(RuntimeError, 'hook moved'):
            toolchain.verify_compiler_vtable(text, 'DisplayFixture', {'slots': 3, 'bindings': {'1': 'start_hardware'}})
        with self.assertRaisesRegex(RuntimeError, 'incompatible/abstract'):
            toolchain.verify_compiler_vtable(text.replace('start_hardware()', 'start_hardware() [pure]'),
                                            'DisplayFixture', contract)

    def test_host_test_selection_is_explicit_and_strict(self):
        from unittest.mock import patch
        tests = [{'name':'selected','sources':['fixture.cpp']},
                 {'name':'unrelated','sources':['missing-in-progress.cpp']}]
        (self.root/'Build.json').write_text(json.dumps({'tests':tests}))
        with patch.object(toolchain,'run') as run:
            toolchain.test_drivers(['selected'])
            self.assertEqual(run.call_count,2)
            self.assertIn(self.root/'fixture.cpp',run.call_args_list[0].args[0])
            for invalid in ([],['unknown'],['selected','selected']):
                with self.assertRaisesRegex(RuntimeError,'test selection'):
                    toolchain.test_drivers(invalid)
            self.assertEqual(run.call_count,2)

    def test_kernel_probe_rejects_changed_provider_signature(self):
        from unittest.mock import patch
        source = 'probe.cpp'
        required = '__ZN14FamilyProvider7performEj'
        changed = '__ZN14FamilyProvider7performEy'
        (self.root / 'provider.json').write_text(json.dumps({
            'classes': {'FamilyProvider': {}}, 'required_symbols': [required]}))
        (self.root / 'Build.json').write_text(json.dumps({'tests': [{
            'name': 'provider-probe', 'sources': ['host.cpp'],
            'kernel_compile': [source],
            'kernel_provider_imports': {source: ['provider.json']}}]}))
        with patch.object(toolchain, 'prepare_driver_sdk'), patch.object(toolchain, 'run'), \
             patch.object(toolchain.subprocess, 'check_output', return_value=required + '\n__ZN9IOService5startEPS_\n'):
            toolchain.test_drivers(['provider-probe'])
        with patch.object(toolchain, 'prepare_driver_sdk'), patch.object(toolchain, 'run'), \
             patch.object(toolchain.subprocess, 'check_output', return_value=changed + '\n'):
            with self.assertRaisesRegex(RuntimeError, 'unaudited kernel probe provider imports'):
                toolchain.test_drivers(['provider-probe'])

    def test_provider_abi_analysis_uses_private_output_and_atomic_publication(self):
        from unittest.mock import patch
        import hashlib
        build = self.root / 'build'
        tools = self.root / 'tools'
        project = tools / 'kernelcache_patcher'
        project.mkdir(parents=True)
        (project / 'CMakeLists.txt').write_text('fixture')
        patcher = build / 'kernelcache_patcher/kernelcache_patcher'
        patcher.parent.mkdir(parents=True)
        patcher.write_text('fixture')
        image = self.root / 'provider'
        image.write_bytes(b'provider')
        kernel = build / 'output' / (toolchain.KERNEL_FILE + '.unstripped')
        kernel.parent.mkdir(parents=True)
        kernel.write_bytes(b'kernel')
        sha = hashlib.sha256(image.read_bytes()).hexdigest()
        shared = build / 'drivers' / (sha + '.abi.json')
        shared.parent.mkdir(parents=True)
        shared.write_text('another writer has an incomplete report')
        contract = {'image_sha256':sha, 'classes':{}, 'required_symbols':[]}
        report = {'classes':[], 'symbols':{}}
        private = []

        def analyze(args, **kwargs):
            self.assertEqual(args[1], 'inspect-abi')
            output = Path(args[-1])
            self.assertNotEqual(output, shared)
            self.assertEqual(output.parent.parent, shared.parent)
            output.write_text(json.dumps(report))
            private.append(output.parent)

        with patch.object(toolchain, 'BUILD', build), patch.object(toolchain, 'TOOLS', tools), \
             patch.object(toolchain, 'NATIVE_KERNELCACHE', image), patch.object(toolchain, 'run', analyze):
            self.assertEqual(toolchain.verify_abi_contract(contract)['symbols'], {})
            self.assertEqual(toolchain.verify_abi_contract(contract)['symbols'], {})
            self.assertEqual(len(private), 1)
            kernel.write_bytes(b'changed kernel')
            self.assertEqual(toolchain.verify_abi_contract(contract)['symbols'], {})
            self.assertEqual(len(private), 2)
        self.assertEqual(json.loads(shared.read_text()), report)
        self.assertTrue(all(not directory.exists() for directory in private))

        def failed_analyze(args, **kwargs):
            output = Path(args[-1])
            private.append(output.parent)
            output.write_text('{truncated')
            raise RuntimeError('analysis failed')

        kernel.write_bytes(b'new kernel requiring analysis')
        with patch.object(toolchain, 'BUILD', build), patch.object(toolchain, 'TOOLS', tools), \
             patch.object(toolchain, 'NATIVE_KERNELCACHE', image), patch.object(toolchain, 'run', failed_analyze):
            with self.assertRaisesRegex(RuntimeError, 'analysis failed'):
                toolchain.verify_abi_contract(contract)
        self.assertEqual(json.loads(shared.read_text()), report)
        self.assertTrue(all(not directory.exists() for directory in private))


if __name__ == '__main__':
    unittest.main()
