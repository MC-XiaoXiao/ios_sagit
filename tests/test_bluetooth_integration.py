"""Verify Apple matching, reference provenance and the observed iOS 18 boundary."""
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import copy
import struct
import tempfile
import unittest
import zipfile
from unittest.mock import patch
from test_boot_bridge import apple_nodes, bridge
import toolchain

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ROOT / 'drivers'


class BluetoothIntegrationTests(unittest.TestCase):
    def test_reference_sources_are_pinned(self):
        reference = ROOT / 'docs/bluetooth/reference'
        manifest = json.loads((reference / 'sources.json').read_text())
        for name, record in manifest.items():
            with self.subTest(name=name):
                self.assertEqual(hashlib.sha256((reference / name).read_bytes()).hexdigest(), record['sha256'])
                self.assertIn('/'+record['ref']+'/', record['url'])
                self.assertRegex(record['ref'], r'^(v[0-9.]+|[0-9a-f]{40})$')
        uart = (reference / 'msm8998.dtsi').read_text()
        self.assertIn('blsp1_uart3: serial@c171000', uart)
        self.assertIn('GIC_SPI 109', uart)
        self.assertIn('"qcom,wcn3990-bt"', (reference / 'msm8998-mtp.dtsi').read_text())

    def test_apple_model_matches_the_actual_kernel(self):
        model = json.loads((ROOT / 'docs/bluetooth/ios18-model.json').read_text())
        spec = importlib.util.spec_from_file_location('bluetooth_inventory', ROOT / 'scripts/analyze-kernelcache.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        metadata, kexts = module.inspect(ROOT / 'build/ios18-kext-analysis' / model['kernel']['file'])
        self.assertEqual(metadata['sha256'], model['kernel']['sha256'])
        by_id = {k['CFBundleIdentifier']: k for k in kexts}
        for observed in model['kernel_kexts']:
            for key, value in observed.items():
                self.assertEqual(by_id[observed['CFBundleIdentifier']][key], value)
        for absent in model['absent_kernel_bundles']:
            self.assertNotIn(absent, by_id)
        native = by_id['com.apple.driver.AppleBluetooth']['IOKitPersonalities']['AppleBluetooth']
        self.assertEqual(native['IOClass'], 'BTReset')
        self.assertEqual(native['IOProviderClass'], 'IOService')

    def test_user_space_evidence_and_explicit_boundary(self):
        model = json.loads((ROOT / 'docs/bluetooth/ios18-model.json').read_text())['user_space']
        for path, record in model['files'].items():
            extracted = ROOT / 'build/bluetooth/reference' / Path(path).name
            if not extracted.exists():
                self.skipTest('optional extracted daemon evidence is not present')
            self.assertEqual(hashlib.sha256(extracted.read_bytes()).hexdigest(), record['sha256'])
        launch = model['files']['/System/Library/LaunchDaemons/com.apple.bluetoothd.plist']['plist']
        self.assertEqual(launch['ProgramArguments'], ['/usr/sbin/bluetoothd'])
        self.assertEqual(launch['UserName'], 'mobile')
        self.assertFalse(model['bluetooth_dext_present'])
        self.assertIn('/dev/cu.bluetooth', model['observations']['bluetoothd']['transport_strings'])
        self.assertIn('AppleBluetoothModule', model['observations']['bluetoothd']['transport_strings'])

    def test_personality_and_collection_use_apple_matching(self):
        info = plistlib.loads((DRIVERS / 'bluetooth/QcomBluetooth/Info.plist').read_bytes())
        self.assertEqual(info['CFBundleIdentifier'], 'org.darwin.driver.QcomBluetooth')
        self.assertEqual(info['CFBundlePackageType'], 'KEXT')
        for personality in info['IOKitPersonalities'].values():
            self.assertEqual(personality['IOProviderClass'], 'AppleARMIODevice')
            self.assertEqual(personality['IONameMatch'], 'qcom,wcn3990-bt')
            self.assertEqual(personality['IOClass'], 'QcomBluetooth')
            self.assertEqual(personality['IOUserClientClass'], 'QcomBluetoothUserClient')
        self.assertNotIn('com.apple.iokit.IOBluetoothFamily', info['OSBundleLibraries'])
        config = json.loads((DRIVERS / 'Build.json').read_text())
        collection = config['collections']['bluetooth']
        self.assertIn(info['CFBundleIdentifier'], collection['roots'])
        self.assertEqual(collection['tests'], ['gcc','gpio','bluetooth', 'bluetooth-firmware','bluetooth-runtime','bluetooth-bootstrap'])
        self.assertIn('com.apple.iokit.IOSerialFamily', info['OSBundleLibraries'])
        self.assertFalse(any('AppleBluetoothModule' in k for k in collection['roots']))

    def test_unified_build_entry(self):
        help_text = subprocess.check_output(['python3', str(ROOT / 'scripts/toolchain.py'), '--help'], text=True)
        self.assertIn('build-bluetooth-client', help_text)
        self.assertIn('prepare-bluetooth', help_text)
        targets = subprocess.check_output(['make', '-n', 'bluetooth-driver', 'bluetooth-client', 'test-bluetooth'], cwd=ROOT, text=True)
        self.assertIn('scripts/toolchain.py build-drivers', targets)
        self.assertIn('scripts/toolchain.py build-bluetooth-client', targets)
        self.assertNotIn('/Project_iOS2ARM/', targets)

    def test_native_serial_layout_and_collection_evidence(self):
        contract=json.loads((DRIVERS/'abi-22H374/apple-serial-abi.json').read_text())
        self.assertEqual(contract['classes']['IOSerialDriverSync']['instance_size'],136)
        self.assertEqual(contract['classes']['IOSerialDriverSync']['vtable_slots'],180)
        self.assertEqual(contract['classes']['IOSerialStreamSync']['instance_size'],152)
        self.assertEqual(contract['classes']['IOSerialStreamSync']['vtable_slots'],181)
        self.assertEqual(contract['image_sha256'],json.loads((ROOT/'docs/bluetooth/ios18-model.json').read_text())['kernel']['sha256'])
        config=json.loads((DRIVERS/'Build.json').read_text())
        self.assertIn('abi-22H374/apple-serial-abi.json',config['vtable_contracts']['QcomBluetooth']['native_abi_contracts'])
        slots=config['vtable_contracts']['QcomBluetooth']['additional_classes']['QcomBluetoothSerial']
        self.assertEqual(slots['slots'],180)

    def test_native_bluetool_contract_and_factory_evidence_are_pinned(self):
        evidence=json.loads((ROOT/'docs/bluetooth/native-bootstrap-evidence.json').read_text())
        for name,record in evidence['sources'].items():
            source=ROOT/'build/bluetooth/reference'/name
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),record['sha256'])
        for function in evidence['functions']:
            source=ROOT/'build/bluetooth/reference'/function['image']
            self.assertEqual(toolchain.provider_function_bytes(source,int(function['address'],0),32).hex(),function['entry_bytes'])
        self.assertEqual(evidence['xpc']['request']['kMsgId'],'RunBuiltinScript')
        self.assertFalse(evidence['native_chipset_policy_ported'])
        self.assertFalse(evidence['hardware_tested'])

    def test_native_daemon_h4_posix_configuration_preserves_launchd_services(self):
        source=ROOT/'build/bluetooth/reference/com.apple.bluetoothd.plist'
        if not source.exists():self.skipTest('pinned native daemon plist absent')
        before=plistlib.loads(source.read_bytes())
        after=plistlib.loads(toolchain.prepare_bluetooth_daemon(source))
        self.assertEqual(after['EnvironmentVariables']['BT_HCI_TRANSPORT'],
                         'H4:POSIX:com.apple.uart.bluetooth,3200000')
        del after['EnvironmentVariables']['BT_HCI_TRANSPORT']
        self.assertEqual(after,before)
        with tempfile.TemporaryDirectory() as directory:
            invalid=Path(directory)/'modified.plist';invalid.write_bytes(source.read_bytes()+b' ')
            with self.assertRaises(ValueError):toolchain.prepare_bluetooth_daemon(invalid)


class BluetoothBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='bluetooth-offline-',dir=ROOT/'build')
        self.root=Path(self.temp.name)
        self.patch=bytearray(600)
        struct.pack_into('<I',self.patch,0,((len(self.patch)-4)<<8)|1)
        struct.pack_into('<HH',self.patch,16,1,3)
        def tag(identifier,data):return struct.pack('<HHII',identifier,len(data),0,0)+data
        self.rf=tag(40,b'RF calibration stays unchanged')
        payload=tag(2,b'\x00\x01\x02\x03\x04\x06')+tag(17,b'\x02\x55\x00\x99')+tag(27,b'\x20\xa5')+self.rf
        self.nvm=struct.pack('<I',(len(payload)<<8)|2)+payload
        self.archive=self.root/'assets.zip'
        with zipfile.ZipFile(self.archive,'w') as z:
            z.writestr('firmware/patch.tlv',self.patch);z.writestr('firmware/nvm.bin',self.nvm)
        sha=lambda data:hashlib.sha256(data).hexdigest()
        self.profile={'version':1,'archive_sha256':sha(self.archive.read_bytes()),
            'controller':{'product':1,'soc':4,'rom':3},
            'local_address':'06:04:03:02:01:00',
            'rails':[{'id':257,'microvolts':1000000,'minimum':800000,'maximum':1200000,'boot_floor':0}],
            'pins':[{'pin':p,'configuration':8} for p in range(45,49)],
            'patch':{'file':'firmware/patch.tlv','sha256':sha(self.patch)},
            'nvm':{'file':'firmware/nvm.bin','sha256':sha(self.nvm)}}
        self.policy=self.root/'profile.json';self.output=self.root/'prepared'

    def tearDown(self):self.temp.cleanup()

    def prepare(self):
        self.policy.write_text(json.dumps(self.profile))
        return toolchain.prepare_bluetooth_firmware(self.archive,self.policy,self.output)

    def tree(self):
        nodes=[{'name':n,'props':{'compatible':c.encode()+b'\0'},'children':[]} for n,c in
               [('bluetooth','qcom,wcn3990-bt'),('gcc','qcom,gcc-msm8998'),
                ('gpio','qcom,msm8998-pinctrl'),('rpm','qcom,glink-rpm')]]
        nodes[0]['props']['status']=b'disabled\0'
        return {'name':'','props':{},'children':[{'name':'soc','props':{},'children':nodes}]}

    def test_prepare_preserves_calibration_and_records_transport_edits(self):
        manifest=self.prepare()
        nvm=(self.output/'nvm.bin').read_bytes()
        differences=[i for i,(a,b) in enumerate(zip(self.nvm,nvm)) if a!=b]
        self.assertEqual(len(differences),3)
        self.assertEqual(nvm[-len(self.rf):],self.rf)
        self.assertEqual(len(manifest['nvm_transport_changes']),3)
        self.assertEqual(toolchain.prepare_bluetooth_transport_nvm(nvm),(nvm,[]))
        config=struct.unpack('<26I',(self.output/'configuration.bin').read_bytes())
        self.assertEqual(config[:6],(1,104,1,4,3,1))
        self.assertEqual(config[6:8],(257,1000000))
        self.assertEqual(config[18:22],(8,8,8,8))
        self.assertFalse(list(self.output.glob('*.pending')))

    def test_bridge_merges_permissions_and_keeps_opaque_little_endian_data(self):
        self.prepare();tree=self.tree()
        result=toolchain.configure_bluetooth_firmware(tree,self.output)
        self.assertFalse(result['hardware_tested'])
        nodes=apple_nodes(bridge.apple_dt(tree))
        bt=nodes['/device-tree/arm-io/bluetooth']
        self.assertEqual(bt['status'],b'okay\0')
        self.assertEqual(bt['qcom,bluetooth-config'],(self.output/'configuration.bin').read_bytes())
        self.assertEqual(bt['bluetooth-nvm'],(self.output/'nvm.bin').read_bytes())
        self.assertEqual(bt['transport-encoding'],struct.pack('<I',1))
        self.assertEqual(bt['local-mac-address'],bytes.fromhex('060403020100'))
        gpio=nodes['/device-tree/arm-io/gpio']
        self.assertEqual(struct.unpack('<4I',gpio['qcom,owned-gpios']),(45,46,47,48))
        self.assertEqual(list(struct.iter_unpack('<II',gpio['qcom,owned-pin-functions'])),[(p,4) for p in range(45,49)])
        gcc=nodes['/device-tree/arm-io/gcc']
        self.assertEqual(list(struct.iter_unpack('<II',gcc['qcom,owned-clocks'])),[(37,0),(53,0)])
        self.assertEqual(struct.unpack('<II',gcc['qcom,owned-roots']),(14,51200000))
        before=copy.deepcopy(tree);toolchain.configure_bluetooth_firmware(tree,self.output)
        self.assertEqual(tree,before)

    def test_invalid_policy_never_publishes_assets(self):
        original=copy.deepcopy(self.profile)
        for change in [lambda p:p.update(archive_sha256='0'*64),
                       lambda p:p['controller'].update(rom=4),
                       lambda p:p['pins'][0].update(configuration=4),
                       lambda p:p['rails'][0].update(id=4095),
                       lambda p:p['rails'][0].update(boot_floor=999999),
                       lambda p:p['patch'].update(file='../patch.tlv'),
                       lambda p:p.update(local_address='00:11:22:33:44:55'),
                       lambda p:p.update(local_address='01:04:03:02:01:00'),
                       lambda p:p['controller'].update(soc=True)]:
            self.profile=copy.deepcopy(original);change(self.profile)
            with self.assertRaises(ValueError):self.prepare()
            self.assertFalse(self.output.exists())

    def test_tamper_or_conflicting_policy_leaves_device_tree_unchanged(self):
        self.prepare()
        for key,value in [('qcom,owned-roots',struct.pack('>II',14,7372800)),
                          ('qcom,owned-clocks',b'bad')]:
            tree=self.tree();tree['children'][0]['children'][1]['props'][key]=value
            before=copy.deepcopy(tree)
            with self.assertRaises(ValueError):toolchain.configure_bluetooth_firmware(tree,self.output)
            self.assertEqual(tree,before)
        nvm=self.output/'nvm.bin';nvm.write_bytes(nvm.read_bytes()+b'x')
        tree=self.tree();before=copy.deepcopy(tree)
        with self.assertRaises(ValueError):toolchain.configure_bluetooth_firmware(tree,self.output)
        self.assertEqual(tree,before)

    def test_duplicate_or_missing_transport_tags_are_rejected(self):
        for payload in [b'',struct.pack('<HHII',17,1,0,0)+b'\x80',
                        struct.pack('<HHII',17,3,0,0)+b'\x80\x00\x11']:
            with self.assertRaises(ValueError):
                toolchain.prepare_bluetooth_transport_nvm(struct.pack('<I',(len(payload)<<8)|2)+payload)
        with self.assertRaises(ValueError):toolchain.prepare_bluetooth_transport_nvm(self.nvm[:-1])

    def test_optional_native_launchd_is_prepared_but_never_installed(self):
        source=ROOT/'build/bluetooth/reference/com.apple.bluetoothd.plist'
        if not source.exists():self.skipTest('pinned native daemon plist absent')
        self.policy.write_text(json.dumps(self.profile))
        manifest=toolchain.prepare_bluetooth_firmware(self.archive,self.policy,self.output,source)
        self.assertIn('com.apple.bluetoothd.plist',manifest['files'])
        report=toolchain.configure_bluetooth_firmware(self.tree(),self.output)
        self.assertTrue(report['native_launchd_prepared'])
        self.assertFalse(report['native_launchd_installed'])

    def test_conflicting_transport_encoding_is_atomic(self):
        self.prepare();tree=self.tree()
        tree['children'][0]['children'][0]['props']['transport-encoding']=struct.pack('>I',7)
        before=copy.deepcopy(tree)
        with self.assertRaises(ValueError):toolchain.configure_bluetooth_firmware(tree,self.output)
        self.assertEqual(tree,before)


class BluetoothStackTests(unittest.TestCase):
    def setUp(self):
        self.reference=ROOT/'build/bluetooth/reference'
        if not (self.reference/'BlueTool').exists():self.skipTest('pinned native images absent')
        if not (ROOT/'build/output/bluetoothbootstrap').exists():self.skipTest('cross-linked backend absent')
        self.temp=tempfile.TemporaryDirectory(prefix='bluetooth-stack-offline-',dir=ROOT/'build')
        self.output=Path(self.temp.name)/'stack'

    def tearDown(self):
        if hasattr(self,'temp'):self.temp.cleanup()

    def test_stages_native_ipc_without_modifying_original_images(self):
        originals={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in self.reference.iterdir() if p.name in
            ('BlueTool','bluetoothd','com.apple.BlueTool.plist','com.apple.bluetoothd.plist')}
        manifest=toolchain.prepare_bluetooth_stack(self.reference,self.output)
        self.assertFalse(manifest['hardware_tested']);self.assertFalse(manifest['installed'])
        self.assertEqual(toolchain.verify_bluetooth_stack(self.output)['status'],'passed')
        for p,sha in originals.items():self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(),sha)
        blue=plistlib.loads((self.output/'root/System/Library/LaunchDaemons/com.apple.BlueTool.plist').read_bytes())
        self.assertEqual(blue['MachServices'],{'com.apple.BlueTool':True})
        self.assertEqual(blue['UserName'],'mobile')
        self.assertEqual(blue['ProgramArguments'],['/usr/libexec/bluetoothbootstrap','-R'])
        self.assertEqual(manifest['backend_signature']['entitlements'],{'org.darwin.bluetooth.bootstrap':True})

    def test_modified_backend_and_launchd_contract_are_rejected(self):
        toolchain.prepare_bluetooth_stack(self.reference,self.output)
        manifest_path=self.output/'manifest.json'
        manifest=json.loads(manifest_path.read_text())
        name='root/System/Library/LaunchDaemons/com.apple.BlueTool.plist'
        path=self.output/name;blue=plistlib.loads(path.read_bytes());blue['UserName']='root'
        path.write_bytes(plistlib.dumps(blue))
        with self.assertRaises(ValueError):toolchain.verify_bluetooth_stack(self.output)
        # A matching file hash cannot authorize a changed native IPC contract.
        manifest['files'][name]={'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):toolchain.verify_bluetooth_stack(self.output)
        toolchain.prepare_bluetooth_stack(self.reference,self.output)
        path=self.output/'root/usr/libexec/bluetoothbootstrap'
        raw=bytearray(path.read_bytes());raw[4096]^=1;path.write_bytes(raw)
        with self.assertRaises(ValueError):toolchain.verify_bluetooth_stack(self.output)

    def test_preparation_refuses_stale_sources_before_publication(self):
        original=toolchain.digest
        def changed(path):
            return '0'*64 if Path(path)==ROOT/'drivers/bluetooth/userspace/BlueToolService.cpp' else original(path)
        with patch.object(toolchain,'digest',side_effect=changed):
            with self.assertRaises(ValueError):toolchain.prepare_bluetooth_stack(self.reference,self.output)
        self.assertFalse(self.output.exists())

    def test_manifest_cannot_claim_unperformed_native_or_hardware_validation(self):
        toolchain.prepare_bluetooth_stack(self.reference,self.output)
        path=self.output/'manifest.json';manifest=json.loads(path.read_text())
        for key in ('hardware_tested','installed','native_chipset_policy_ported','native_stack_runtime_verified'):
            modified=copy.deepcopy(manifest);modified[key]=True;path.write_text(json.dumps(modified))
            with self.subTest(key=key),self.assertRaises(ValueError):toolchain.verify_bluetooth_stack(self.output)


if __name__ == '__main__':
    unittest.main()
