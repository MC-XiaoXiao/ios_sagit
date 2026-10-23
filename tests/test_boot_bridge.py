"""Validate the trust boundaries that can turn a loader bug into DRAM corruption."""
import ctypes
import copy
import importlib.util
import json
import mmap
import plistlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_bridge', ROOT/'scripts/build-bridge.py')
bridge = importlib.util.module_from_spec(spec); spec.loader.exec_module(bridge)


def apple_nodes(blob):
    nodes = {}
    def walk(pos, path):
        np, nc = struct.unpack_from('<II', blob, pos); pos += 8; props = {}
        for _ in range(np):
            key = blob[pos:pos+32].split(b'\0')[0].decode(); size, = struct.unpack_from('<I', blob, pos+32)
            props[key] = blob[pos+36:pos+36+size]; pos += 36+((size+3)&~3)
        path += '/'+props['name'].rstrip(b'\0').decode(); nodes[path] = props
        for _ in range(nc): pos = walk(pos, path)
        return pos
    assert walk(0, '') == len(blob)
    return nodes


class BridgeTests(unittest.TestCase):
    @staticmethod
    def usb_resource_policy(mode='disabled-bypass', speed=3, otp=False):
        # Synthetic AP baselines exercise the producer; these are not a
        # production board policy or evidence of any live resource lease.
        clocks=[[i,0,0] for i in range(27)]
        for i in (0,2,3):clocks[i][1]=100000
        clocks[6][1]=1;clocks[7][1]=19200
        return {'version':2,'component':'\\_SB_.USB0','maximum_speed':speed,'core_clock_hz':60000000 if speed==3 else 120000000,
                'dma':{'mode':mode,'address_bits':32 if mode=='firmware-physical32' else 36,'smmu_base':'0x1680000','smmu_bytes':'0x10000',
                       'context_irqs':[396,397,398,399,400,401] if mode in ('active-context','fresh-context') else []},
                'phy':{'usb2':{'supplies':[[257,880000],[258,1200000],[268,1800000],[280,3088000]],
                               'differential_ref':False,'otp_trim':otp,
                               'device_init':[[0,128],[4,19],[0x18c,124],[0x2c,128],[0x184,10],[0x23c,240],[0x240,13]],
                               'host_init':[[0x210,99],[4,19],[0x18c,124],[0x2c,128],[0x184,10],[0x21c,140],[0x23c,5],[0x240,3],[0x218,255],[0x210,98]]},
                       **({'usb3':{'supplies':[[257,880000],[258,1200000]]}} if speed==4 else {})},
                'connector':{'sid':2,'adc_base':'0x4500','source_microamps':500000 if speed==3 else 1000000,'vconn':True},
                'regulator_limits':[[257,312000,1328000,880000],[258,312000,1328000,1200000],
                                    [268,1256000,2272000,1800000],[280,1664000,3704000,3088000]],
                'rpm_domains':[[0x1000+i,128,0] for i in range(4)],'rpm_clocks':clocks,
                'rpm_clock_limits':[[i,1 if i==6 else (4 if i==19 else 1000000),1 if i==6 else (4 if i==19 else 2000000)] for i in bridge.RPM_HANDOFF_CLOCK_LIMIT_IDS],
                'rpm_interconnect':[[i,1000000000 if i<0x2100 else 8000000000,100000000,0]
                                    for i in (0x2000,0x2001,0x2002,0x2100,0x2101,0x2102,0x2103,0x2104)]}

    def test_usb_resource_profile_builds_native_graph_and_live_capture_only(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic-usb-policy.json'
            for mode in ('active-context','fresh-context','architected-bypass','disabled-bypass','unmatched-bypass','firmware-physical32'):
                for speed in (3,4):
                    for otp in (False,True):
                        with self.subTest(mode=mode,speed=speed,otp=otp):
                            policy=self.usb_resource_policy(mode,speed,otp)
                            policy['rpm_interconnect'][3][2]=0x100000000
                            path.write_text(json.dumps(policy));profile=bridge.load_usb_resource_profile(path)
                            tree=bridge.parse_fdt(self.fdt);bridge.configure_usb_topology(tree,policy['component'],hardware_stop=True)
                            soc=next(n for n in tree['children'] if n['name'].startswith('soc'))
                            gcc=next(n for n in soc['children'] if n['name']=='clock-controller@100000')['props']
                            gcc['qcom,owned-clocks']=struct.pack('>2I',54,0)
                            gcc['qcom,owned-roots']=struct.pack('>2I',26,50000000)
                            spmi=next(n for n in soc['children'] if n['name']=='power-controller@800f000')['props']
                            spmi['qcom,owned-irqs']=struct.pack('>4I',0,8,7,3)
                            report=bridge.configure_usb_resources(tree,profile)
                            nodes=apple_nodes(bridge.apple_dt(tree));base='/device-tree/arm-io/'
                            usb=nodes[base+'usb@a800000'];phy=nodes[base+'phy@c012000']
                            personalities={}
                            for driver in ('QcomUSB','QcomInterconnect','QcomSPMI','QcomPMIC'):
                                info=plistlib.loads(next((ROOT/'drivers').glob('*/'+driver+'/Info.plist')).read_bytes())
                                personalities.update(info['IOKitPersonalities'])
                            for name,personality in [('usb@a800000','QcomUSB'),('usb@a800000','QcomUSBResources'),
                                                     ('phy@c012000','QcomUSBPHY'),('interconnect@1660000','QcomInterconnect'),
                                                     ('power-controller@800f000','QcomSPMI')]:
                                match=personalities[personality];accepted=match['IONameMatch']
                                if isinstance(accepted,str):accepted=[accepted]
                                compatible=nodes[base+name]['compatible'].split(b'\0')
                                self.assertEqual(match['IOProviderClass'],'AppleARMIODevice')
                                self.assertTrue(any(v.encode() in compatible for v in accepted))
                            self.assertEqual(personalities['QcomUSBHost']['IOProviderClass'],'QcomUSBController')
                            self.assertEqual(personalities['QcomUSBDevice']['IOProviderClass'],'QcomUSBController')
                            self.assertEqual(personalities['QcomUSBConnector']['IOProviderClass'],'QcomPMIC')
                            self.assertEqual(struct.unpack('<6Q',usb['reg']),(0xa800000,0xcd00,0xa8f8800,0x400,0x1680000,0x10000))
                            self.assertEqual(struct.unpack('<3I',usb['qcom,usb-controller-resources']),(policy['core_clock_hz'],policy['dma']['address_bits'],speed))
                            self.assertEqual(usb['qcom,usb-resource-handoff'],struct.pack('<I',1))
                            self.assertEqual(list(struct.iter_unpack('<2I',usb['interrupts'])),[(163,1)]+[(v,1) for v in policy['dma']['context_irqs']])
                            key='qcom,usb-firmware-physical32' if mode=='firmware-physical32' else 'qcom,usb-smmu-capture' if mode in ('active-context','fresh-context') else 'qcom,usb-dma-capture'
                            self.assertEqual(usb[key],struct.pack('<I',{'fresh-context':2,'disabled-bypass':2,'unmatched-bypass':3}.get(mode,1)))
                            for key in ('iommu-parent','qcom,usb-dma-window','qcom,usb-smmu-handoff','qcom,usb-smmu-bypass'):
                                self.assertNotIn(key,usb)
                            self.assertEqual(usb['usb-iort-component'],b'\\_SB_.USB0\0')
                            self.assertEqual(list(struct.iter_unpack('<2I',phy['qcom,usb-phy-supplies'])),[tuple(r) for r in policy['phy']['usb2']['supplies']])
                            self.assertEqual(list(struct.iter_unpack('<2I',phy['qcom,usb-phy-device-init'])),[tuple(r) for r in policy['phy']['usb2']['device_init']])
                            self.assertEqual(list(struct.iter_unpack('<2I',phy['qcom,usb-phy-host-init'])),[tuple(r) for r in policy['phy']['usb2']['host_init']])
                            self.assertEqual(phy['qcom,usb-phy-differential-ref'],bytes(4))
                            self.assertEqual(len(phy['reg']),48 if otp else 32)
                            if otp:self.assertEqual(struct.unpack_from('<2Q',phy['reg'],32),(0x784238,4))
                            self.assertEqual(nodes[base+'interconnect@1660000']['qcom,owned-qos-ports'],struct.pack('<I',2))
                            rpm=nodes[base+'power-controller@778000'];gcc=nodes[base+'clock-controller@100000'];spmi=nodes[base+'power-controller@800f000']
                            self.assertEqual(rpm['qcom,rpm-handoff-version'],struct.pack('<I',3))
                            self.assertEqual(rpm['qcom,rpm-usb-core-clock-hz'],struct.pack('<I',policy['core_clock_hz']))
                            self.assertEqual(rpm['qcom,rpm-transport-cold-handoff'],bytes(40))
                            self.assertNotIn('qcom,rpm-transport-handoff',rpm)
                            self.assertEqual(list(struct.iter_unpack('<4I',rpm['qcom,regulator-limits'])),[tuple(r) for r in policy['regulator_limits']])
                            self.assertEqual(list(struct.iter_unpack('<IIQQQ',rpm['qcom,rpm-interconnect-handoff']))[3],(0x2100,0,8000000000,0x100000000,0))
                            self.assertIn((54,0),list(struct.iter_unpack('<2I',gcc['qcom,owned-clocks'])))
                            self.assertIn((26,50000000),list(struct.iter_unpack('<2I',gcc['qcom,owned-roots'])))
                            self.assertEqual(list(struct.iter_unpack('<I',gcc['qcom,owned-domains'])),[(2,)])
                            self.assertEqual(struct.unpack('<4I',spmi['qcom,usb-connector']),(2,0x4500,policy['connector']['source_microamps'],1))
                            self.assertEqual(len(spmi['qcom,owned-irqs']),7*16)
                            if speed==4:
                                self.assertEqual(struct.unpack('<4Q',nodes[base+'phy@c010000']['reg']),(0xc010000,0x1000,0x1fcb244,4))
                                self.assertIn((120,125000000),list(struct.iter_unpack('<2I',gcc['qcom,owned-clocks'])))
                                self.assertIn((165,1200000),list(struct.iter_unpack('<2I',gcc['qcom,owned-roots'])))
                            else:self.assertNotIn(base+'phy@c010000',nodes)
                            self.assertFalse(report['DMA_or_resource_grants']);self.assertFalse(report['hardware_verified'])
                            self.assertFalse(report['build_executes_hardware'])
                            before=copy.deepcopy(tree);bridge.configure_usb_resources(tree,profile);self.assertEqual(tree,before)

    def test_usb_resource_context_irq_triggers_roundtrip(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic-usb-policy.json'
            for irqs in ([396,397],[[396,0],[397,0]],[[396,1],[397,1]],[[396,0],[397,1]],[[397,1]]):
                with self.subTest(irqs=irqs):
                    policy=self.usb_resource_policy('active-context');policy['dma']['context_irqs']=irqs
                    path.write_text(json.dumps(policy));profile=bridge.load_usb_resource_profile(path)
                    expected=tuple((v,0) if isinstance(v,int) else tuple(v) for v in irqs)
                    self.assertEqual(profile['dma'][4],expected)
                    tree=bridge.parse_fdt(self.fdt);bridge.configure_usb_topology(tree,policy['component'],hardware_stop=True)
                    bridge.configure_usb_resources(tree,profile)
                    usb=apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/usb@a800000']
                    self.assertEqual(list(struct.iter_unpack('<2I',usb['interrupts'])),[(163,1)]+[(irq,0 if flags else 1) for irq,flags in expected])
                    self.assertNotIn('qcom,usb-smmu-handoff',usb)
            malformed=[[[396,2]],[[396,-1]],[[396,True]],[[True,0]],[[396]],[[396,0,0]],
                       [[396,0],[396,1]],[[31,0]],[[1020,1]],[[163,1]],[[396,'edge']],['396,1']]
            for irqs in malformed:
                with self.subTest(invalid=irqs):
                    policy=self.usb_resource_policy('active-context');policy['dma']['context_irqs']=irqs
                    path.write_text(json.dumps(policy))
                    with self.assertRaises(ValueError):bridge.load_usb_resource_profile(path)

    def test_usb_resource_profile_rejects_unsafe_or_incomplete_policy(self):
        changes=[('version',lambda p:p.__setitem__('version',True)),('missing',lambda p:p.pop('rpm_clocks')),
                 ('unknown',lambda p:p.__setitem__('ready',True)),('component',lambda p:p.__setitem__('component','USB0')),
                 ('speed',lambda p:p.__setitem__('maximum_speed',5)),('clock',lambda p:p.__setitem__('core_clock_hz',19200000)),
                 ('CX off',lambda p:p['rpm_domains'][0].__setitem__(1,0)),('domain',lambda p:p['rpm_domains'][1].__setitem__(1,193)),
                 ('clock count',lambda p:p['rpm_clocks'].pop()),('clock duplicate',lambda p:p['rpm_clocks'][0].__setitem__(0,1)),
                 ('XO',lambda p:p['rpm_clocks'][6].__setitem__(1,0)),('PCNoC',lambda p:p['rpm_clocks'][7].__setitem__(1,19199)),
                 ('branch',lambda p:p['rpm_clocks'][9].__setitem__(1,2)),('bus mismatch',lambda p:p['rpm_interconnect'][0].__setitem__(2,1000)),
                 ('bandwidth',lambda p:p['rpm_interconnect'][-1].__setitem__(1,300000000)),
                 ('bus clock',lambda p:p['rpm_interconnect'][0].__setitem__(1,100000001)),
                 ('rail missing',lambda p:p['regulator_limits'].pop()),('rail duplicate',lambda p:p['regulator_limits'].append(p['regulator_limits'][0][:])),
                 ('rail unknown',lambda p:p['regulator_limits'][0].__setitem__(0,999)),
                 ('rail limit',lambda p:p['regulator_limits'][0].__setitem__(2,4000000)),
                 ('rail floor',lambda p:p['regulator_limits'][0].__setitem__(3,880001)),
                 ('supply duplicate',lambda p:p['phy']['usb2']['supplies'][0].__setitem__(0,258)),
                 ('supply voltage',lambda p:p['phy']['usb2']['supplies'][0].__setitem__(1,0)),
                 ('PHY flag',lambda p:p['phy']['usb2'].__setitem__('otp_trim',1)),
                 ('tuning range',lambda p:p['phy']['usb2']['device_init'][0].__setitem__(0,0x1000)),
                 ('device power',lambda p:p['phy']['usb2']['device_init'][0].__setitem__(0,0x210)),
                 ('tuning value',lambda p:p['phy']['usb2']['device_init'][0].__setitem__(1,256)),
                 ('host bracket',lambda p:p['phy']['usb2']['host_init'][0].__setitem__(1,98)),
                 ('connector sid',lambda p:p['connector'].__setitem__('sid',3)),('ADC',lambda p:p['connector'].__setitem__('adc_base',0x4501)),
                 ('source current',lambda p:p['connector'].__setitem__('source_microamps',1600000)),
                 ('source granule',lambda p:p['connector'].__setitem__('source_microamps',600000)),
                 ('DMA mode',lambda p:p['dma'].__setitem__('mode','global-bypass')),
                 ('DMA width',lambda p:p['dma'].__setitem__('address_bits',41)),('DMA SMMU',lambda p:p['dma'].__setitem__('smmu_base',0x1000000)),
                 ('DMA span',lambda p:p['dma'].__setitem__('smmu_bytes',0x20000)),('bypass IRQ',lambda p:p['dma'].__setitem__('context_irqs',[192]))]
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic-usb-policy.json'
            for label,change in changes:
                with self.subTest(case=label):
                    policy=self.usb_resource_policy();change(policy);path.write_text(json.dumps(policy))
                    with self.assertRaises(ValueError):bridge.load_usb_resource_profile(path)
            for mode,irqs in [('active-context',[]),('active-context',[163]),('active-context',[192,192]),('active-context',[31])]:
                policy=self.usb_resource_policy(mode);policy['dma']['context_irqs']=irqs;path.write_text(json.dumps(policy))
                with self.assertRaises(ValueError):bridge.load_usb_resource_profile(path)
            for change in (lambda p:p.__setitem__('core_clock_hz',60000000),lambda p:p['connector'].__setitem__('source_microamps',750000),lambda p:p['phy'].pop('usb3')):
                policy=self.usb_resource_policy(speed=4);change(policy);path.write_text(json.dumps(policy))
                with self.assertRaises(ValueError):bridge.load_usb_resource_profile(path)
            policy=self.usb_resource_policy();path.write_text(json.dumps(policy).replace('"version": 2','"version": 2, "version": 2',1))
            with self.assertRaises(ValueError):bridge.load_usb_resource_profile(path)

    def test_usb_core_clock_requires_its_persistent_active_cx_floor(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic-usb-cx-policy.json'
            for hz,minimum in ((60000000,64),(120000000,128),(150000000,256)):
                for cx in (16,32,48,64,128,192,256,320,384,512):
                    with self.subTest(core_hz=hz,cx=cx):
                        policy=self.usb_resource_policy(speed=3 if hz==60000000 else 4)
                        policy['core_clock_hz']=hz;policy['rpm_domains'][0][1]=cx
                        # Sleep remains explicit and may be off under the
                        # existing cold PM protocol; it cannot support active.
                        policy['rpm_domains'][0][2]=512 if cx<minimum else 0
                        path.write_text(json.dumps(policy))
                        if cx<minimum:
                            with self.assertRaisesRegex(ValueError,'persistent active CX floor'):
                                bridge.load_usb_resource_profile(path)
                        else:self.assertEqual(bridge.load_usb_resource_profile(path)['core_clock_hz'],hz)

    def test_usb_resource_conflicts_are_atomic_and_keep_other_owners(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic-usb-policy.json';path.write_text(json.dumps(self.usb_resource_policy()))
            profile=bridge.load_usb_resource_profile(path)
            original=bridge.parse_fdt(self.fdt);bridge.configure_usb_topology(original,profile['component'],hardware_stop=True)
            bridge.configure_usb_resources(original,profile)
            for case in range(21):
                with self.subTest(case=case):
                    tree=copy.deepcopy(original);soc=next(n for n in tree['children'] if n['name'].startswith('soc'))
                    nodes={n['name']:n for n in soc['children']}
                    chosen=next(n for n in tree['children'] if n['name']=='chosen')['props']
                    gcc=nodes['clock-controller@100000']['props'];rpm=nodes['power-controller@778000']['props']
                    spmi=nodes['power-controller@800f000']['props'];usb=nodes['usb@a800000']['props']
                    if case==0:chosen['qcom,usb-hardware-stop']=b'\1'+bytes(63)
                    elif case==1:chosen.pop('qcom,usb-firmware-stop')
                    elif case==2:chosen['usb-iort-component']=b'\\_SB_.OTHER\0'
                    elif case==3:tree['props']['compatible']=b'arm,virt\0'
                    elif case==4:gcc['qcom,owned-clocks']=struct.pack('>2I',32,1)
                    elif case==5:gcc['qcom,owned-resets']=b'\0'
                    elif case==6:rpm['qcom,regulator-limits']=struct.pack('>4I',257,312000,1328000,0)
                    elif case==7:rpm['qcom,rpm-clock-handoff']=bytes(27*16)
                    elif case==8:rpm['qcom,rpm-handoff-version']=struct.pack('>I',9)
                    elif case==9:spmi['qcom,owned-irqs']=struct.pack('>4I',2,0x13,7,3)
                    elif case==10:spmi['qcom,owned-irqs']=b''.join(struct.pack('>4I',0,i,0,1) for i in range(64))
                    elif case==11:usb['qcom,usb-dma-window']=bytes(32)
                    elif case==12:usb['iommu-parent']=struct.pack('>I',1)
                    elif case==13:usb['qcom,usb-dma-capture']=struct.pack('>I',1)
                    elif case==14:soc['children'].append({'name':'foreign@1680000','props':{'reg':struct.pack('>QQ',0x1680000,0x1000),'status':b'okay\0'},'children':[]})
                    elif case==15:clone=copy.deepcopy(nodes['clock-controller@100000']);clone['name']='duplicate-gcc';soc['children'].append(clone)
                    elif case==16:soc['props']['#address-cells']=struct.pack('>I',1)
                    elif case==17:soc['props']['ranges']=struct.pack('>3Q',0,4096,0x100000000)
                    elif case==18:soc['props']['ranges']=struct.pack('>3Q',0,0,0x100000)
                    elif case==19:spmi['reg']=spmi['reg'][:-16]+struct.pack('>2Q',0x800b000,0x3000)
                    elif case==20:rpm['qcom,rpm-usb-core-clock-hz']=struct.pack('>I',150000000)
                    before=copy.deepcopy(tree)
                    with self.assertRaises(ValueError):bridge.configure_usb_resources(tree,profile)
                    self.assertEqual(tree,before)

    def test_usb_gpu_share_one_rpm_policy_and_check_combined_demand(self):
        def gpu_policy(usb,average=100000000,peak=400000000):
            return {**{k:copy.deepcopy(usb[k]) for k in ('rpm_domains','rpm_clocks','rpm_clock_limits','rpm_interconnect')},
                    'version':2,'iova_first':0x10000000,'iova_bytes':0x4000000,
                    'average_bandwidth':average,'peak_bandwidth':peak,
                    'firmware_power':{'frequency_hz':257000000,'voltage_uv':800000},
                    'gfx_regulator':{'id':1,'sid':5,'minimum_uv':524000,'maximum_uv':1100000}}
        with tempfile.TemporaryDirectory() as directory:
            upath=Path(directory)/'synthetic-usb-policy.json';gpath=Path(directory)/'synthetic-gpu-policy.json'
            usb=self.usb_resource_policy();upath.write_text(json.dumps(usb));gpath.write_text(json.dumps(gpu_policy(usb)))
            profile=bridge.load_usb_resource_profile(upath)
            tree=bridge.parse_fdt(self.fdt);bridge.configure_usb_topology(tree,profile['component'],hardware_stop=True)
            bridge.configure_gpu_handoff(tree,gpath,cold_start=True)
            before=copy.deepcopy(tree)
            with self.assertRaises(ValueError):bridge.configure_usb_resources(tree,profile)
            self.assertEqual(tree,before)
            bridge.configure_usb_resources(tree,profile,bridge_consumer=True)
            rpm=apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/power-controller@778000']
            self.assertEqual(rpm['qcom,rpm-handoff-version'],struct.pack('<I',2))
            self.assertEqual(rpm['qcom,rpm-transport-handoff'],bytes(48))
            self.assertEqual(rpm['qcom,rpm-transport-cold-handoff'],bytes(40))
            # A later consumer cannot replace an already selected AP policy.
            conflicting=gpu_policy(usb);conflicting['rpm_clocks'][4][1]=19200;gpath.write_text(json.dumps(conflicting))
            before=copy.deepcopy(tree)
            with self.assertRaises(ValueError):bridge.configure_gpu_handoff(tree,gpath,cold_start=True)
            self.assertEqual(tree,before)
            conflicting=gpu_policy(usb);conflicting['rpm_clock_limits'][0][1]+=1;gpath.write_text(json.dumps(conflicting))
            with self.assertRaises(ValueError):bridge.configure_gpu_handoff(tree,gpath,cold_start=True)
            self.assertEqual(tree,before)
            for kind in ('bandwidth','BIMC'):
                usb=self.usb_resource_policy()
                if kind=='bandwidth':usb['rpm_interconnect'][-1][1]=500000000;average,peak=200000000,400000000
                else:usb['rpm_interconnect'][2][1]=100000000;average,peak=800000000,800000000
                upath.write_text(json.dumps(usb));gpath.write_text(json.dumps(gpu_policy(usb,average,peak)))
                profile=bridge.load_usb_resource_profile(upath)
                tree=bridge.parse_fdt(self.fdt);bridge.configure_usb_topology(tree,profile['component'],hardware_stop=True)
                bridge.configure_gpu_handoff(tree,gpath,cold_start=True);before=copy.deepcopy(tree)
                with self.assertRaises(ValueError):bridge.configure_usb_resources(tree,profile,bridge_consumer=True)
                self.assertEqual(tree,before)

    def test_usb_resource_runtime_requires_all_actual_native_versions(self):
        versions={'QcomUSB':'1.13.0','QcomRPM':'1.13.0','QcomGCC':'1.2.0','GenericClockController':'1.3.0',
                  'QcomPowerController':'1.2.0','QcomSPMI':'1.1.0','QcomPMIC':'1.1.0','QcomInterconnect':'1.0.0'}
        kexts=[{'CFBundleIdentifier':'org.darwin.driver.'+k,'CFBundleVersion':v} for k,v in versions.items()]
        kexts.append({'CFBundleIdentifier':'org.mi6.driver.GenericARMPlatform','CFBundleVersion':'1.1.0'})
        self.assertEqual(len(bridge.check_usb_resource_versions(kexts)),9)
        old_usb=[dict(k,CFBundleVersion='1.12.0') if k['CFBundleIdentifier']=='org.darwin.driver.QcomUSB' else k for k in kexts]
        with self.assertRaises(ValueError):bridge.check_usb_resource_versions(old_usb)
        old_rpm=[dict(k,CFBundleVersion='1.12.0') if k['CFBundleIdentifier']=='org.darwin.driver.QcomRPM' else k for k in kexts]
        with self.assertRaises(ValueError):bridge.check_usb_resource_versions(old_rpm)
        for identity,version in (('org.mi6.driver.GenericARMPlatform','1.0.0'),('org.darwin.driver.GenericClockController','1.2.0')):
            old_router=[dict(k,CFBundleVersion=version) if k['CFBundleIdentifier']==identity else k for k in kexts]
            with self.assertRaises(ValueError):bridge.check_usb_resource_versions(old_router)
        old_pmic=[dict(k,CFBundleVersion='1.0.0') if k['CFBundleIdentifier']=='org.darwin.driver.QcomPMIC' else k for k in kexts]
        with self.assertRaises(ValueError):bridge.check_usb_resource_versions(old_pmic)
        for i in range(len(kexts)):
            with self.subTest(bundle=kexts[i]['CFBundleIdentifier']):
                for changed in (kexts[:i]+kexts[i+1:],kexts+[kexts[i]],
                                [dict(k,CFBundleVersion='0.9.0') if n==i else k for n,k in enumerate(kexts)],
                                [dict(k,CFBundleVersion=1.6) if n==i else k for n,k in enumerate(kexts)]):
                    with self.assertRaises(ValueError):bridge.check_usb_resource_versions(changed)

    def test_pimem_requires_explicit_active_sleep_policy_for_usb_and_gpu(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic-policy.json'
            usb=self.usb_resource_policy();usb['rpm_clocks'][26]=[26,192000,9600]
            gpu={k:copy.deepcopy(usb[k]) for k in ('version','rpm_clocks','rpm_domains','rpm_clock_limits','rpm_interconnect')}
            gpu.update(iova_first=0x10000000,iova_bytes=0x4000000,average_bandwidth=100000000,peak_bandwidth=400000000)
            for owner,policy in (('USB',usb),('GPU',gpu)):
                def load(p):
                    path.write_text(json.dumps(p))
                    if owner=='USB':
                        profile=bridge.load_usb_resource_profile(path)
                        tree=bridge.parse_fdt(self.fdt);bridge.configure_usb_topology(tree,profile['component'],hardware_stop=True)
                        bridge.configure_usb_resources(tree,profile)
                    else:
                        tree=bridge.parse_fdt(self.fdt);bridge.configure_gpu_handoff(tree,path)
                    nodes=apple_nodes(bridge.apple_dt(tree))
                    return nodes['/device-tree/arm-io/power-controller@778000']['qcom,rpm-clock-handoff']
                records=list(struct.iter_unpack('<4I',load(policy)))
                self.assertEqual(len(records),27);self.assertEqual(records[-1],(26,192000,9600,0))
                for kind in ('missing','duplicate','unknown','wide-active','wide-sleep','boolean'):
                    with self.subTest(owner=owner,invalid=kind):
                        bad=copy.deepcopy(policy)
                        if kind=='missing':bad['rpm_clocks'].pop()
                        elif kind=='duplicate':bad['rpm_clocks'][-1][0]=25
                        elif kind=='unknown':bad['rpm_clocks'][-1][0]=27
                        elif kind=='wide-active':bad['rpm_clocks'][-1][1]=1<<31
                        elif kind=='wide-sleep':bad['rpm_clocks'][-1][2]=1<<31
                        else:bad['rpm_clocks'][-1][1]=True
                        with self.assertRaises(ValueError):load(bad)

    def test_qdss_state_policy_for_usb_and_gpu_preserves_exact_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic-state-policy.json'
            usb=self.usb_resource_policy()
            gpu={k:copy.deepcopy(usb[k]) for k in ('version','rpm_clocks','rpm_domains','rpm_clock_limits','rpm_interconnect')}
            gpu.update(iova_first=0x10000000,iova_bytes=0x4000000,average_bandwidth=100000000,peak_bandwidth=400000000)
            for owner,policy in (('USB',usb),('GPU',gpu)):
                def load(p):
                    path.write_text(json.dumps(p));tree=bridge.parse_fdt(self.fdt)
                    if owner=='USB':
                        profile=bridge.load_usb_resource_profile(path)
                        bridge.configure_usb_topology(tree,profile['component'],hardware_stop=True)
                        bridge.configure_usb_resources(tree,profile)
                    else:bridge.configure_gpu_handoff(tree,path)
                    records=apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/power-controller@778000']['qcom,rpm-clock-handoff']
                    return list(struct.iter_unpack('<4I',records))[19]
                for active in range(5):
                    for sleep in range(5):
                        with self.subTest(owner=owner,active=active,sleep=sleep):
                            policy['rpm_clocks'][19]=[19,active,sleep]
                            self.assertEqual(load(policy),(19,active,sleep,0))
                for value in (5,1<<31,0xffffffff,-1,True,'5',None,1<<32):
                    for field in (1,2):
                        with self.subTest(owner=owner,field=field,invalid=value):
                            bad=copy.deepcopy(policy);bad['rpm_clocks'][19][field]=value
                            with self.assertRaises(ValueError):load(bad)
                policy['rpm_clocks'][19]=[19,'0x4','3']
                self.assertEqual(load(policy),(19,4,3,0))

    def test_clock_limits_are_explicit_coherent_and_preserved_for_usb_and_gpu(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic-clock-limits.json'
            usb=self.usb_resource_policy();usb['rpm_clocks'][26]=[26,192000,9600]
            gpu={k:copy.deepcopy(usb[k]) for k in ('version','rpm_clocks','rpm_domains','rpm_clock_limits','rpm_interconnect')}
            gpu.update(iova_first=0x10000000,iova_bytes=0x4000000,average_bandwidth=100000000,peak_bandwidth=400000000)
            for owner,policy in (('USB',usb),('GPU',gpu)):
                def load(p):
                    path.write_text(json.dumps(p));tree=bridge.parse_fdt(self.fdt)
                    if owner=='USB':
                        profile=bridge.load_usb_resource_profile(path)
                        bridge.configure_usb_topology(tree,profile['component'],hardware_stop=True);bridge.configure_usb_resources(tree,profile)
                    else:bridge.configure_gpu_handoff(tree,path)
                    data=apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/power-controller@778000']['qcom,rpm-clock-limit-handoff']
                    return list(struct.iter_unpack('<4I',data))
                self.assertEqual(load(policy),[tuple(r)+(0,) for r in policy['rpm_clock_limits']])
                for active,sleep in ((0,0),(0xffffffff,0),(384000,192000)):
                    good=copy.deepcopy(policy);good['rpm_clock_limits'][-1]=[26,active,sleep]
                    self.assertEqual(load(good)[-1],(26,active,sleep,0))
                changes=[('legacy version',lambda p:p.__setitem__('version',1)),
                         ('missing field',lambda p:p.pop('rpm_clock_limits')),
                         ('missing row',lambda p:p['rpm_clock_limits'].pop()),
                         ('duplicate',lambda p:p['rpm_clock_limits'][-1].__setitem__(0,19)),
                         ('unsupported clock',lambda p:p['rpm_clock_limits'][-1].__setitem__(0,9)),
                         ('negative',lambda p:p['rpm_clock_limits'][-1].__setitem__(1,-1)),
                         ('boolean',lambda p:p['rpm_clock_limits'][-1].__setitem__(1,True)),
                         ('wide',lambda p:p['rpm_clock_limits'][-1].__setitem__(2,1<<32)),
                         ('request active',lambda p:p['rpm_clock_limits'][-1].__setitem__(1,191999)),
                         ('request sleep',lambda p:p['rpm_clock_limits'][-1].__setitem__(2,9599)),
                         ('XO',lambda p:p['rpm_clock_limits'][6].__setitem__(1,2)),
                         ('STATE',lambda p:p['rpm_clock_limits'][10].__setitem__(2,5)),
                         ('NoC active ceiling',lambda p:p['rpm_clock_limits'][0].__setitem__(1,999999)),
                         ('NoC sleep ceiling',lambda p:p['rpm_clock_limits'][0].__setitem__(2,999999))]
                for kind,change in changes:
                    with self.subTest(owner=owner,invalid=kind):
                        bad=copy.deepcopy(policy);change(bad)
                        with self.assertRaises(ValueError):load(bad)
                path.write_text(json.dumps(policy).replace('"rpm_clock_limits":','"rpm_clock_limits": [], "rpm_clock_limits":',1))
                with self.assertRaises(ValueError):
                    if owner=='USB':bridge.load_usb_resource_profile(path)
                    else:bridge.configure_gpu_handoff(bridge.parse_fdt(self.fdt),path)

    def test_complete_policy_runtime_gate_and_unchanged_cold_observer_compatibility(self):
        for version in ('1.4.0','1.5.0','1.7.0','1.8.0','1.9.0','1.10.0','1.11.0','2.0.0'):
            with self.subTest(version=version),mock.patch.object(bridge,'rpm_transport_version',return_value=version):
                if version in ('1.11.0','2.0.0'):self.assertEqual(bridge.require_gpu_rpm_transport(Path('unused')),version)
                else:
                    with self.assertRaises(ValueError):bridge.require_gpu_rpm_transport(Path('unused'))
                if version!='1.4.0':self.assertEqual(bridge.require_usb_rpm_transport(Path('unused')),version)
                else:
                    with self.assertRaises(ValueError):bridge.require_usb_rpm_transport(Path('unused'))
        kexts=[{'CFBundleIdentifier':'org.darwin.driver.'+k,'CFBundleVersion':v}
               for k,v in {'QcomUSB':'1.10.0','QcomRPM':'1.10.0','QcomGCC':'1.2.0','GenericClockController':'1.2.0',
                           'QcomPowerController':'1.2.0','QcomSPMI':'1.1.0','QcomPMIC':'1.1.0','QcomInterconnect':'1.0.0'}.items()]
        with self.assertRaises(ValueError):bridge.check_usb_resource_versions(kexts)

    def test_usb_resource_cli_rejects_conflicting_component_and_unsafe_gpu_mix(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic-usb-policy.json';path.write_text(json.dumps(self.usb_resource_policy()))
            base=['python3',str(ROOT/'scripts/build-bridge.py'),'--kernel',str(Path(directory)/'missing.kernel'),
                  '--output',str(Path(directory)/'out')]
            cases=[['--usb-resource-profile',str(path),'--usb-topology-component','\\_SB_.OTHER'],
                   ['--usb-hardware-stop','--usb-topology-component','\\_SB_.USB0',
                    '--gpu-handoff-profile',str(Path(directory)/'missing-gpu.json')]]
            for args in cases:
                result=subprocess.run(base+args,capture_output=True,text=True)
                self.assertEqual(result.returncode,2)
                self.assertNotIn('Traceback',result.stderr)
                self.assertFalse((Path(directory)/'out'/'XnuBridge.efi').exists())

    def test_usb_topology_reserves_only_an_empty_efi_record(self):
        tree = bridge.parse_fdt(self.fdt)
        report = bridge.configure_usb_topology(tree, '\\_SB_.USB0')
        nodes = apple_nodes(bridge.apple_dt(tree))
        chosen = nodes['/device-tree/chosen']
        self.assertEqual(chosen['usb-iort-component'], b'\\_SB_.USB0\0')
        self.assertEqual(chosen['qcom,usb-firmware-iort'], bytes(65536))
        self.assertEqual(chosen['qcom,usb-firmware-controller'], bytes(40))
        self.assertFalse(report['dma_ownership_granted'])
        self.assertFalse(report['hardware_verified'])
        self.assertNotIn('qcom,usb-dma-window', chosen)
        for value in ('USB0', '\\_SB_ USB0', '\\' + 'X'*64, '\\USBé', '', None):
            with self.assertRaises(ValueError): bridge.configure_usb_topology(tree, value)
        tree['props']['compatible'] = b'arm,virt\0'
        with self.assertRaises(ValueError): bridge.configure_usb_topology(tree, '\\_SB_.USB0')

    def test_no_sep_gate_checks_endpoint_before_lookup_and_preserves_targets(self):
        code = bytearray(0x11c)
        code[:40] = bytes.fromhex(
            'ff4302d1f44f07a9fd7b08a9fd030291f40300aabfc31eb8'
            '648d00941300805288da41b928030035')
        patched = bridge.patch_no_sep_keystore_gate(code)
        def branch_target(word, pc, bits, shift):
            imm = (word >> shift) & ((1 << bits)-1)
            if imm & (1 << (bits-1)): imm -= 1 << bits
            return pc + 4*imm
        old_call, zero, load, old_branch = struct.unpack_from('<4I',code,24)
        new_zero, new_load, new_branch, new_call = struct.unpack_from('<4I',patched,24)
        self.assertEqual((new_zero,new_load),(zero,load))
        self.assertEqual(branch_target(old_branch,36,19,5),branch_target(new_branch,32,19,5))
        self.assertEqual(branch_target(old_call,24,26,0),branch_target(new_call,36,26,0))
        self.assertEqual(patched[:24],code[:24])
        self.assertEqual(patched[40:],code[40:])
        for offset, mask in ((0,1),(27,4),(28,1),(32,1),(36,1)):
            invalid = bytearray(code); invalid[offset] ^= mask
            with self.assertRaises(ValueError): bridge.patch_no_sep_keystore_gate(invalid)

    def test_usb_rpm_snapshot_is_an_empty_observation_without_resource_grants(self):
        tree = bridge.parse_fdt(self.fdt)
        report = bridge.configure_usb_topology(tree, '\\_SB_.USB0', rpm_snapshot=True)
        chosen = apple_nodes(bridge.apple_dt(tree))['/device-tree/chosen']
        self.assertEqual(chosen['qcom,usb-firmware-rpm'], bytes(20544))
        self.assertFalse(report['rpm_snapshot']['resource_ownership_granted'])
        self.assertFalse(report['rpm_snapshot']['ACK_success_inferred'])
        self.assertNotIn('qcom,rpm-clock-handoff', chosen)
        self.assertNotIn('qcom,usb-smmu-handoff', chosen)

    def test_usb_npa_pin_covers_actual_code_protocol_and_all_relocations(self):
        source = ROOT/'boot/edk2-msm8998/Platforms/Msm8998Binaries/common/Binaries/NpaDxe/NpaDxe.efi'
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'pin.bin'
            report = bridge.pack_usb_npa_pin(source, output)
            pin = output.read_bytes(); data = source.read_bytes()
            magic, version, size, code, length, interface, interface_length, count, r0, r1 = struct.unpack_from('<10I', pin)
            self.assertEqual((magic,version,size,code,length,interface,interface_length,r0,r1),
                             (0x31495046,1,0x1b000,0x1000,0x17000,0x180f0,0x1b0,0,0))
            self.assertEqual(count, 212)
            offsets = list(struct.unpack_from('<'+str(count)+'I',pin,40))
            self.assertEqual(offsets, sorted(set(offsets)))
            self.assertTrue(all(offset % 8 == 0 for offset in offsets))
            self.assertEqual(pin[40+4*count:],data[code:code+length]+data[interface:interface+interface_length])
            self.assertFalse(report['firmware_callbacks_invoked'])
            corrupt = Path(directory)/'corrupt.efi'
            for pos in (0, 0x1000, 0x180f0, 0x1a000):
                bad = bytearray(data); bad[pos] ^= 1; corrupt.write_bytes(bad)
                with self.assertRaises(ValueError): bridge.pack_usb_npa_pin(corrupt, output)
                self.assertEqual(output.read_bytes(), pin)

    def test_usb_firmware_stop_also_observes_final_rpm_without_granting_dma(self):
        tree = bridge.parse_fdt(self.fdt)
        report = bridge.configure_usb_topology(tree, '\\_SB_.USB0', firmware_stop=True)
        chosen = apple_nodes(bridge.apple_dt(tree))['/device-tree/chosen']
        self.assertEqual(chosen['qcom,usb-firmware-stop'], bytes(64))
        self.assertEqual(chosen['qcom,usb-hardware-stop'], bytes(64))
        self.assertEqual(chosen['qcom,usb-firmware-rpm'], bytes(20544))
        self.assertTrue(report['rpm_snapshot']['enabled'])
        self.assertTrue(report['firmware_stop']['enabled'])
        self.assertFalse(report['firmware_stop']['hardware_halt_proven'])
        self.assertFalse(report['firmware_stop']['resource_ownership_granted'])
        self.assertFalse(report['dma_ownership_granted'])

    def test_usb_config_pin_covers_code_and_immutable_template(self):
        source = ROOT/'boot/edk2-msm8998/Platforms/Msm8998Binaries/common/Binaries/UsbConfigDxe/UsbConfigDxe.efi'
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'pin.bin'
            report = bridge.pack_usb_config_pin(source, output)
            pin = output.read_bytes(); data = source.read_bytes()
            header = struct.unpack_from('<10I', pin)
            self.assertEqual(header, (0x31495046,1,0x13000,0x1000,0x10000,0x11140,160,78,0,0))
            self.assertEqual(pin[40+4*78:], data[0x1000:0x11000]+data[0x11140:0x111e0])
            self.assertEqual(report['private_layout_revision'], '0x10003')
            self.assertFalse(report['build_executes_firmware'])
            bad = bytearray(data); bad[0x11140] ^= 1
            corrupt = Path(directory)/'corrupt.efi'; corrupt.write_bytes(bad)
            with self.assertRaises(ValueError): bridge.pack_usb_config_pin(corrupt, output)
            self.assertEqual(output.read_bytes(), pin)

    def test_usb_rpm_cold_handoff_has_one_kernel_or_bridge_consumer(self):
        tree = bridge.parse_fdt(self.fdt)
        report = bridge.configure_usb_rpm_cold(tree)
        nodes = apple_nodes(bridge.apple_dt(tree))
        rpm = nodes['/device-tree/arm-io/power-controller@778000']
        self.assertEqual(rpm['qcom,rpm-handoff-version'], struct.pack('<I',3))
        self.assertEqual(rpm['qcom,rpm-transport-cold-handoff'], bytes(40))
        self.assertFalse(report['resource_or_DMA_grants'])
        tree = bridge.parse_fdt(self.fdt)
        with self.assertRaises(ValueError): bridge.configure_usb_rpm_cold(tree, True)
        soc = next(n for n in tree['children'] if n['name'].startswith('soc'))
        props = next(n for n in soc['children'] if n['name']=='power-controller@778000')['props']
        props['qcom,rpm-handoff-version'] = struct.pack('>I',2)
        props['qcom,rpm-transport-handoff'] = bytes(48)
        bridge.configure_usb_rpm_cold(tree, True)
        self.assertEqual(props['qcom,rpm-handoff-version'], struct.pack('>I',2))
        with self.assertRaises(ValueError): bridge.configure_usb_rpm_cold(tree)

    def test_usb_hardware_stop_reserves_proof_without_enabling_usb(self):
        tree = bridge.parse_fdt(self.fdt)
        soc = next(n for n in tree['children'] if n['name'].startswith('soc'))
        soc['children'].append({'name':'usb@a800000','props':{'status':b'disabled\0',
            'qcom,usb-smmu-capture':struct.pack('>I',1),'qcom,usb-dma-capture':struct.pack('>I',1)},'children':[]})
        report = bridge.configure_usb_topology(tree, '\\_SB_.USB0', hardware_stop=True)
        nodes = apple_nodes(bridge.apple_dt(tree))
        chosen = nodes['/device-tree/chosen']
        for name, size in [('qcom,usb-hardware-stop',64),('qcom,usb-firmware-stop',64),('qcom,usb-firmware-rpm',20544)]:
            self.assertEqual(chosen[name],bytes(size))
        self.assertEqual(nodes['/device-tree/arm-io/usb@a800000']['status'],b'disabled\0')
        self.assertEqual(nodes['/device-tree/arm-io/usb@a800000']['qcom,usb-smmu-capture'],struct.pack('<I',1))
        self.assertEqual(nodes['/device-tree/arm-io/usb@a800000']['qcom,usb-dma-capture'],struct.pack('<I',1))
        soc['children'][-1]['props']['qcom,usb-dma-capture']=struct.pack('>I',2)
        nodes = apple_nodes(bridge.apple_dt(tree))
        self.assertEqual(nodes['/device-tree/arm-io/usb@a800000']['qcom,usb-dma-capture'],struct.pack('<I',2))
        self.assertEqual(nodes['/device-tree/arm-io/usb@a800000']['status'],b'disabled\0')
        self.assertTrue(report['hardware_stop']['enabled'])
        self.assertFalse(report['hardware_stop']['resource_or_DMA_mapping_grants'])
        self.assertFalse(report['hardware_stop']['build_executes_hardware'])

    def test_usb_glink_pin_covers_installed_transport_code_and_protocol(self):
        source = ROOT/'boot/edk2-msm8998/Platforms/Msm8998Binaries/common/Binaries/GlinkDxe/GlinkDxe.efi'
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)/'pin.bin'; report = bridge.pack_usb_glink_pin(source,out)
            data=out.read_bytes();original=source.read_bytes()
            self.assertEqual(struct.unpack_from('<10I',data),(0x31495046,1,0xc000,0x1000,0x8000,0x9090,0x78,83,0,0))
            self.assertEqual(data[40+4*83:],original[0x1000:0x9000]+original[0x9090:0x9108])
            self.assertFalse(report['build_executes_firmware'])

    def test_no_sep_uses_native_keystore_endpoint_policy_and_preserves_exec(self):
        tree = bridge.parse_fdt(self.fdt)
        tree['children'].append({'name':'filesystems', 'props':{}, 'children':[]})
        chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
        chosen['props']['bootargs'] = b'-v process-exec-trace=3 aks-endpoint=0\0'
        bridge.configure_no_sep(tree)
        bridge.configure_no_sep(tree)
        nodes = apple_nodes(bridge.apple_dt(tree))
        args = nodes['/device-tree/chosen']['bootargs'].rstrip(b'\0').split()
        self.assertEqual(args, [b'-v', b'process-exec-trace=3', b'aks-endpoint=0'])
        self.assertEqual(nodes['/device-tree/filesystems']['DoNotUseSEP'], b'')
        for value in (b'aks-endpoint=1', b'aks-endpoint=2', b'aks-endpoint=invalid'):
            chosen['props']['bootargs'] = value + b'\0'
            with self.assertRaises(ValueError): bridge.configure_no_sep(tree)
            self.assertEqual(chosen['props']['bootargs'], value + b'\0')
        chosen['props']['bootargs'] = b'x' * 1010 + b'\0'
        with self.assertRaises(ValueError): bridge.configure_no_sep(tree)

    def test_wifi_node_keeps_native_registers_interrupts_and_disabled_status(self):
        nodes = apple_nodes(bridge.apple_dt(bridge.parse_fdt(self.fdt)))
        wifi = next(p for p in nodes.values() if p.get('compatible') == b'qcom,wcn3990-wifi\0')
        self.assertEqual(wifi['status'], b'disabled\0')
        self.assertEqual(struct.unpack('<QQ', wifi['reg']), (0x18800000, 0x800000))
        # WCN3990 skips SPI419. Apple vectors include the SPI base of 32.
        expected = [(spi + 32, 1) for spi in (413,414,415,416,417,418,420,421,422,423,424,425)]
        self.assertEqual(list(struct.iter_unpack('<II', wifi['interrupts'])), expected)
        self.assertNotIn('memory-region', wifi)  # board/loader must supply the real reserved MSA

    def test_bluetooth_node_preserves_dedicated_uart_and_disabled_status(self):
        nodes = apple_nodes(bridge.apple_dt(bridge.parse_fdt(self.fdt)))
        bluetooth = next(p for p in nodes.values() if p.get('compatible') == b'qcom,wcn3990-bt\0')
        self.assertEqual(bluetooth['status'], b'disabled\0')
        self.assertEqual(struct.unpack('<QQ', bluetooth['reg']), (0x0c171000, 0x1000))
        self.assertEqual(struct.unpack('<II', bluetooth['interrupts']), (109 + 32, 1))
        self.assertEqual(struct.unpack('<I', bluetooth['current-speed']), (115200,))
        self.assertEqual(struct.unpack('<I', bluetooth['max-speed']), (3200000,))
        console = next(p for p in nodes.values() if p.get('reg') == struct.pack('<QQ', 0x0c1b0000, 0x1000))
        self.assertNotEqual(bluetooth['reg'], console['reg'])

    def test_display_handoff_keeps_runtime_resources_uncommitted(self):
        tree = bridge.parse_fdt(self.fdt)
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory)/'display.json'
            settings = {'version':1, 'compatible':'qcom,msm8998', 'physical_width_mm':64,
                        'physical_height_mm':114, 'refresh_hz':60, 'display_scale':3, 'display_rotation':0,
                        'display_uuid':'12345678-1234-5678-9012-123456789012'}
            profile.write_text(json.dumps(settings))
            bridge.configure_display_handoff(tree, profile)
            nodes = apple_nodes(bridge.apple_dt(tree))
            display = next(p for p in nodes.values() if p.get('compatible') == b'qcom,msm8998-dpu\0')
            self.assertEqual(display['qcom,display-handoff-version'], bytes(4))
            self.assertEqual(display['qcom,display-handoff-flags'], bytes(4))
            self.assertEqual(display['canvas-width'], bytes(4))
            self.assertEqual(display['canvas-height'], bytes(4))
            self.assertEqual(display['qcom,scanout-device-address'], bytes(8))
            self.assertEqual(display['display-physical-width-mm'], struct.pack('<I',64))
            chosen = nodes['/device-tree/chosen']
            self.assertEqual(chosen['display-scale'], struct.pack('<I', 3))
            self.assertEqual(chosen['display-rotation'], struct.pack('<I', 0))
            mmu_tree = bridge.parse_fdt(self.fdt)
            bridge.configure_display_handoff(mmu_tree, profile, smmu_handoff=True)
            mmu_display = apple_nodes(bridge.apple_dt(mmu_tree))['/device-tree/arm-io/display-controller@c901000']
            self.assertEqual(mmu_display['qcom,display-smmu-handoff'], bytes(4288))
            self.assertEqual(mmu_display['qcom,display-smmu-observed'], bytes(128))
            self.assertEqual(struct.unpack('<6Q', mmu_display['reg']),
                             (0xc901000, 0x8f000, 0xcd00000, 0x40000, 0xc8c0000, 0x40000))
            self.assertEqual(len(mmu_display['interrupts']), 21 * 8)
            self.assertEqual(struct.unpack_from('<4I', mmu_display['interrupts']), (115, 1, 295, 1))
            # iOS 18's getDisplayListNumber scans only arm-io's direct children.
            # DSI hosts and panel children must not count as extra displays.
            displays = [p for name,p in nodes.items()
                        if name.rsplit('/',1)[0] == '/device-tree/arm-io' and
                        p.get('device_type') in (b'display-subsystem\0', b'ext-display-subsystem\0')]
            self.assertEqual(displays, [display])
            for key, bad in (('refresh_hz', True), ('display_scale', 0),
                             ('display_scale', True), ('display_rotation', 45)):
                invalid = dict(settings, **{key: bad})
                profile.write_text(json.dumps(invalid))
                with self.assertRaises(ValueError): bridge.configure_display_handoff(tree,profile)
    def test_display_backlight_handoff_uses_pmi8998_and_runtime_commit(self):
        settings = json.loads((ROOT/'tools/display-handoff.json').read_text())
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory)/'display.json'
            profile.write_text(json.dumps(settings))
            tree = bridge.parse_fdt(self.fdt)
            report = bridge.configure_display_handoff(tree, profile)
            nodes = apple_nodes(bridge.apple_dt(tree))
            display = next(p for p in nodes.values() if p.get('compatible') == b'qcom,msm8998-dpu\0')
            spmi = next(p for p in nodes.values() if p.get('compatible') == b'qcom,spmi-pmic-arb\0')
            self.assertEqual(display['display-backlight'], struct.pack('<I', 0x101))
            self.assertEqual(struct.unpack('<8I', spmi['qcom,pmi8998-wled-handoff']),
                             (0, 0, 0x101, 3, 0xd800, 0xd900, 3, 1000))
            self.assertEqual(report['backlight_controller'], 'qcom,pmi8998-wled')
            self.assertFalse(report['hardware_boot_verified'])
            disabled = bridge.configure_display_handoff(tree, profile, disable_backlight=True)
            disabled_nodes = apple_nodes(bridge.apple_dt(tree))
            self.assertTrue(disabled['backlight_handoff_disabled'])
            self.assertIsNone(disabled['backlight_controller'])
            self.assertFalse(any('display-backlight' in p or 'qcom,pmi8998-wled-handoff' in p
                                 for p in disabled_nodes.values()))
            self.assertEqual(disabled_nodes['/device-tree/arm-io/display-controller@c901000']['canvas-width'], bytes(4))
            for key, value in [('compatible', 'qcom,pmi8994-wled'), ('spmi_sid', 2),
                               ('control_base', 0xd700), ('sink_base', 0xd800),
                               ('strings_mask', True), ('strings_mask', 16), ('sync_delay_us', 0)]:
                invalid = copy.deepcopy(settings); invalid['backlight'][key] = value
                profile.write_text(json.dumps(invalid))
                with self.assertRaises(ValueError):
                    bridge.configure_display_handoff(bridge.parse_fdt(self.fdt), profile)

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(); tmp = Path(cls.temp.name)
        subprocess.run(['clang', '-shared', '-fPIC', '-ffreestanding', '-fno-builtin', '-DBRIDGE_HOST_TEST', '-O2', str(ROOT/'boot/bridge/core.c'), str(ROOT/'boot/bridge/fdt.c'), '-o', str(tmp/'core.so')], check=True)
        cls.lib = ctypes.CDLL(str(tmp/'core.so'))
        cls.lib.macho_info.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        cls.lib.memmove.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.bridge_set_video.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
        cls.lib.fdt_get_video.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        cls.lib.fdt_find_arena.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_void_p]
        cls.lib.macho_load.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.bridge_prepare.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_void_p]
        cls.lib.bridge_set_platform_identity.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-i', str(ROOT/'device_trees'), '-o', str(tmp/'board.dtb'), str(ROOT/'device_trees/xiaomi-sagit.dts')], check=True)
        cls.fdt = (tmp/'board.dtb').read_bytes()
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def test_platform_identity_uses_firmware_values_and_rejects_partial_updates(self):
        original = bridge.apple_dt(bridge.parse_fdt(self.fdt))
        tree = ctypes.create_string_buffer(original)
        self.assertEqual(self.lib.bridge_set_platform_identity(tree, 0x12345678, 292), 0)
        chosen = apple_nodes(tree.raw[:len(original)])['/device-tree/chosen']
        self.assertEqual(chosen['unique-chip-id'], struct.pack('<Q', 0x12345678))
        self.assertEqual(chosen['chip-id'], struct.pack('<I', 292))
        updated = tree.raw
        self.assertEqual(self.lib.bridge_set_platform_identity(tree, 0x87654321, 292), -1)
        self.assertEqual(tree.raw, updated)
        for serial, chip in ((0, 292), (0xffffffff, 292), (1, 0), (1, 0xffffffff)):
            tree = ctypes.create_string_buffer(original)
            self.assertEqual(self.lib.bridge_set_platform_identity(tree, serial, chip), -1)
            self.assertEqual(tree.raw[:len(original)], original)
        for key, value in (('unique-chip-id', bytes(4)), ('chip-id', bytes(8))):
            parsed = bridge.parse_fdt(self.fdt)
            next(n for n in parsed['children'] if n['name'] == 'chosen')['props'][key] = value
            malformed = bridge.apple_dt(parsed)
            tree = ctypes.create_string_buffer(malformed)
            self.assertEqual(self.lib.bridge_set_platform_identity(tree, 1, 292), -1)
            self.assertEqual(tree.raw[:len(malformed)], malformed)
    def sample(self):
        b = bytearray(512)
        struct.pack_into('<8I', b, 0, 0xfeedfacf, 0x100000c, 0, 2, 2, 360, 0, 0)
        struct.pack_into('<II16sQQQQIIII', b, 32, 0x19, 72, b'__TEXT_EXEC', 0x100000, 64, 400, 16, 7, 5, 0, 0)
        struct.pack_into('<4I', b, 104, 5, 288, 6, 68)
        struct.pack_into('<Q', b, 104+272, 0x100000)
        b[400:416] = b'0123456789abcdef'
        return b
    def info(self, b):
        buf = ctypes.create_string_buffer(bytes(b)); info = (ctypes.c_uint64*3)()
        return self.lib.macho_info(buf, len(b), info), info, buf
    def test_segment_copy_and_zero_fill(self):
        overlap = ctypes.create_string_buffer(b'abcdefgh')
        self.lib.memmove(ctypes.byref(overlap, 2), overlap, 6)
        self.assertEqual(overlap.raw[:8], b'ababcdef')
        self.lib.memmove(overlap, ctypes.byref(overlap, 2), 6)
        self.assertEqual(overlap.raw[:6], b'abcdef')
        status, info, src = self.info(self.sample()); self.assertEqual(status, 0)
        arena = ctypes.create_string_buffer(0x100040)
        ctypes.memset(arena, 0xaa, len(arena))
        self.assertEqual(self.lib.macho_load(src, 512, info, arena, len(arena)), 0)
        self.assertEqual(arena.raw[0x100000:0x100040], b'0123456789abcdef'+bytes(48))
        self.assertEqual(self.lib.macho_load(src, 512, info, arena, 32), -1)
    def test_malformed_load_commands_and_segment_ranges(self):
        mutations = [(20, '<I', 0xffffffff), (36, '<I', 4), (32+48, '<Q', 65), (32+40, '<Q', 0xffffffffffffffff), (104+12, '<I', 67), (104+272, '<Q', 0x300000)]
        for offset, fmt, val in mutations:
            with self.subTest(offset=offset):
                b = self.sample(); struct.pack_into(fmt, b, offset, val)
                self.assertLess(self.info(b)[0], 0)
    def test_entry_must_be_file_backed_and_executable(self):
        b = self.sample(); struct.pack_into('<I', b, 32+60, 1)
        self.assertLess(self.info(b)[0], 0)
        b = self.sample(); struct.pack_into('<Q', b, 104+272, 0x100020)
        self.assertLess(self.info(b)[0], 0)
    def test_truncations_are_rejected(self):
        b = self.sample()
        for size in range(0, 416): self.assertLess(self.info(b[:size])[0], 0)
    def test_standard_fdt_converts_addresses_and_phandle(self):
        nodes = apple_nodes(bridge.apple_dt(bridge.parse_fdt(self.fdt), bytes(range(64))))
        soc = nodes['/device-tree/arm-io']
        self.assertIn(b'arm-io,sdm835\0', soc['compatible'])
        self.assertEqual(struct.unpack('<QQQ', soc['ranges']), (0, 0, 1<<32))
        uart = nodes['/device-tree/arm-io/serial@c1b0000']
        self.assertEqual(struct.unpack('<QQ', uart['reg']), (0xc1b0000, 0x1000))
        self.assertEqual(struct.unpack('<II', uart['interrupts']), (146, 1))
        timer=nodes['/device-tree/timer']
        self.assertEqual(struct.unpack('<8I',timer['interrupts']),(17,1,18,1,19,1,16,1))
        gic = nodes['/device-tree/arm-io/interrupt-controller@17a00000']
        self.assertEqual(struct.unpack('<I', gic['#interrupt-cells']), (2,))
        self.assertIn(b'ARM,gicv3\0', gic['compatible'])
        ufs = nodes['/device-tree/arm-io/storage@1da4000']
        self.assertEqual(struct.unpack('<II', ufs['interrupts']), (297, 1))
        secure = nodes['/device-tree/arm-io/secure-firmware']
        self.assertEqual(secure['compatible'], b'qcom,scm-msm8998\0')
        self.assertEqual(secure['method'], b'smc\0')
        self.assertNotIn('reg', secure)
        chosen = nodes['/device-tree/chosen']
        self.assertEqual(chosen['amfi-only-platform-code'], struct.pack('<I', 1))
        self.assertEqual(nodes['/device-tree/arm-io/power-controller@800f000']['qcom,pmic-rtc'],
                         struct.pack('<II', 0, 0x6000))
        self.assertEqual(struct.unpack('<I', chosen['nvram-bank-size']), (0x4000,))
        self.assertEqual(struct.unpack('<I', chosen['nvram-bank-count']), (1,))
        self.assertEqual(struct.unpack('<I', chosen['nvram-current-bank']), (0,))
        self.assertEqual(len(chosen['nvram-proxy-data']), 0x4000)
        self.assertEqual(chosen['nvram-proxy-data'][4:10], b'2nvram')
        self.assertEqual(chosen['serial-device'], uart['phandle'])
        self.assertEqual(chosen['serial-device'], uart['AAPL,phandle'])
        self.assertEqual(chosen['random-seed'], bytes(range(64)))
        self.assertEqual(struct.unpack('<I', nodes['/device-tree/cpus/cpu@100']['reg']), (0x100,))
        self.assertEqual(struct.unpack('<I', nodes['/device-tree/cpus/cpu@0']['cluster-type']), (ord('E'),))
        self.assertEqual(struct.unpack('<I', nodes['/device-tree/cpus/cpu@100']['cluster-type']), (ord('P'),))
        self.assertEqual(len(nodes['/device-tree/memory']['reg']), 16)

    def test_iphone_product_identity_survives_apple_dt_conversion(self):
        nodes = apple_nodes(bridge.apple_dt(bridge.parse_fdt(self.fdt)))
        # IODTPlatformExpert::getProductName/getMachineName use root model;
        # product/product-name is descriptive and does not drive hw.product.
        self.assertEqual(nodes['/device-tree']['model'], b'iPhoneMi6,1\0')
        self.assertEqual(nodes['/device-tree']['compatible'], b'xiaomi,sagit\0qcom,msm8998\0')
        self.assertEqual(nodes['/device-tree']['target-type'], b'SDM835\0')
        self.assertEqual(nodes['/device-tree/product']['product-name'], b'Xiaomi Mi 6\0')

    def test_bad_fdt_header_is_rejected(self):
        for b in [b'', bytes(40), self.fdt[:100]]:
            with self.assertRaises(ValueError): bridge.parse_fdt(b)
    def test_native_cpu_limit_replaces_existing_limit(self):
        for count in (1, 2, 4, 8):
            tree = bridge.parse_fdt(self.fdt)
            chosen = next(n for n in tree['children'] if n['name'] == 'chosen')['props']
            chosen['bootargs'] += b' cpus=1 cpus=4\0'
            # The property must be a single string, like real bootargs.
            chosen['bootargs'] = chosen['bootargs'].replace(b'\0', b' ').rstrip() + b'\0'
            bridge.configure_cpu_count(tree, count)
            args = chosen['bootargs'].rstrip(b'\0').decode().split()
            self.assertEqual([a for a in args if a.startswith('cpus=')], [f'cpus={count}'])
            self.assertIn('serial=3', args)
            nodes = apple_nodes(bridge.apple_dt(tree))
            self.assertEqual(sum(p.get('device_type') == b'cpu\0' for p in nodes.values()), 8)
    def test_cpu_limit_rejects_invalid_count_and_mask(self):
        tree = bridge.parse_fdt(self.fdt)
        for count in (0, 9, -1, True, '8'):
            with self.assertRaises(ValueError): bridge.configure_cpu_count(tree, count)
        chosen = next(n for n in tree['children'] if n['name'] == 'chosen')['props']
        chosen['bootargs'] = b'cpumask=0x11\0'
        with self.assertRaises(ValueError): bridge.configure_cpu_count(tree, 2)
    def test_default_uses_four_cpus_with_eight_cpu_topology(self):
        tree = bridge.parse_fdt(self.fdt)
        chosen = next(n for n in tree['children'] if n['name'] == 'chosen')['props']
        self.assertEqual([a for a in chosen['bootargs'].rstrip(b'\0').decode().split() if a.startswith('cpus=')], ['cpus=4'])
        nodes = apple_nodes(bridge.apple_dt(tree))
        self.assertEqual(sum(p.get('device_type') == b'cpu\0' for p in nodes.values()), 8)
    def test_gpio_pmic_interrupt_domains_and_button_cells(self):
        nodes = apple_nodes(bridge.apple_dt(bridge.parse_fdt(self.fdt)))
        gpio = nodes['/device-tree/arm-io/gpio-controller@3400000']
        spmi = nodes['/device-tree/arm-io/power-controller@800f000']
        self.assertEqual(struct.unpack('<QQ', gpio['reg']), (0x3400000, 0xc00000))
        self.assertEqual(struct.unpack('<II', gpio['interrupts']), (240, 1))
        self.assertEqual(struct.unpack('<II', spmi['interrupts']), (358, 1))
        self.assertEqual(struct.unpack('<I', gpio['#interrupt-cells']), (2,))
        self.assertEqual(struct.unpack('<I', spmi['#interrupt-cells']), (4,))
        self.assertNotEqual(gpio['AAPL,phandle'], spmi['AAPL,phandle'])
        self.assertEqual(struct.unpack('<12I', spmi['qcom,owned-irqs']),
                         (0, 8, 0, 3, 0, 0xc5, 0, 3, 0, 8, 1, 3))
        self.assertEqual(struct.unpack('<15I', spmi['qcom,button-map']),
                         (0x30, 0, 0, 0, 15, 0xe9, 1, 0, 6, 15, 0xea, 0, 0, 1, 15))
        # Cascaded GPIO/SPMI trigger flags retain Linux's five polarities;
        # only GIC interrupt specifiers become Apple's edge/level cells.
        tree = bridge.parse_fdt(self.fdt)
        soc = next(n for n in tree['children'] if n['name'].startswith('soc@'))
        soc['children'].append({'name': 'gpio-client', 'props': {
            'interrupt-parent': struct.pack('>I', 0x100),
            'interrupts': struct.pack('>II', 124, 3)}, 'children': []})
        child = apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/gpio-client']
        self.assertEqual(child['interrupts'], struct.pack('<II', 124, 3))
    def test_apfs_root_matches_system_volume_without_macos_uuid_resolver(self):
        tree = bridge.parse_fdt(self.fdt)
        system = 'C1FFCBC9-9565-4E1F-8686-2000A9397B25'
        data = '33888466-84EE-4238-A837-230EA4F61F00'
        bridge.configure_apfs_root(tree, {'system_uuid': system, 'data_uuid': data})
        props = next(n for n in tree['children'] if n['name'] == 'chosen')['props']
        match = ET.fromstring(props['root-matching'].rstrip(b'\0'))
        self.assertEqual([e.text for e in match.iter('string')], ['IOMedia', system])
        self.assertEqual(props['boot-uuid'], system.encode() + b'\0')
        self.assertNotIn(data.encode(), props['root-matching'])
        with self.assertRaises(ValueError):
            bridge.configure_apfs_root(tree, {'system_uuid': system, 'data_uuid': system})
    def test_panic_ram_is_zeroed_and_excluded_from_vm_allocations(self):
        libc = ctypes.CDLL(None)
        libc.posix_memalign.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t]
        libc.free.argtypes = [ctypes.c_void_p]
        arena = ctypes.c_void_p()
        self.assertEqual(libc.posix_memalign(ctypes.byref(arena), 0x200000, 0x4000000), 0)
        try:
            ctypes.memset(arena, 0xaa, 0x4000000)
            kernel = ctypes.create_string_buffer(bytes(self.sample()))
            source_tree = bridge.parse_fdt(self.fdt)
            soc = next(n for n in source_tree['children'] if n['name'].startswith('soc'))
            fingerprint = next(n for n in soc['children'] if n['name']=='fingerprint')
            # Firmware properties exceed the former 1 MiB DT bound. Their
            # entire lifetime must be inside the arena, below topOfKernelData.
            fpc_packet=b'FPC'+bytes(700000)
            goodix_packet=b'Goodix'+bytes(1800000)
            fingerprint['props']['fpc-firmware']=fpc_packet
            fingerprint['props']['goodix-firmware']=goodix_packet
            chosen = next(n for n in source_tree['children'] if n['name'] == 'chosen')
            memory_map = next(n for n in chosen['children'] if n['name'] == 'memory-map')
            memory_map['props']['TrustCache'] = bytes(16)
            from bridge_trustcache import handoff
            module = struct.pack('<I16sI', 1, bytes(16), 1) + bytes(22)
            tcdata = handoff([module]); trust_cache = ctypes.create_string_buffer(tcdata)
            dt = bridge.apple_dt(source_tree)
            tree = ctypes.create_string_buffer(dt)
            ba = ctypes.c_void_p(); info = (ctypes.c_uint64*3)()
            self.assertEqual(self.lib.bridge_prepare(arena, 0x4000000, kernel, 512, tree, 0x400001, trust_cache, len(tcdata), 0x100, ctypes.byref(ba), info), -3)
            self.assertEqual(self.lib.bridge_prepare(arena, 0x4000000, kernel, 512, tree, len(dt), trust_cache, len(tcdata), 0x100, ctypes.byref(ba), info), 0)
            args = ctypes.string_at(ba, 1152)
            virt, phys, size, top = struct.unpack_from('<4Q', args, 8)
            dtva, dtlen = struct.unpack_from('<QI', args, 96)
            nodes = apple_nodes(ctypes.string_at(phys + dtva - virt, dtlen))
            # The proxy is an inline property in the copied, pinned device tree.
            # Destroy the source to prove no pointer into the loader is retained.
            ctypes.memset(tree, 0, len(dt))
            self.assertLess(phys + dtva - virt + dtlen, top)
            copied = apple_nodes(ctypes.string_at(phys + dtva - virt, dtlen))
            self.assertEqual(copied['/device-tree/arm-io/fingerprint']['fpc-firmware'],fpc_packet)
            self.assertEqual(copied['/device-tree/arm-io/fingerprint']['goodix-firmware'],goodix_packet)
            self.assertEqual(struct.unpack('<III',copied['/device-tree/arm-io/fingerprint']['reset-gpios']),(0x100,37,1))
            self.assertEqual(copied['/device-tree/chosen']['nvram-proxy-data'],
                             nodes['/device-tree/chosen']['nvram-proxy-data'])
            self.assertEqual(len(copied['/device-tree/chosen']['nvram-proxy-data']), 0x4000)
            tcbase, tclen = struct.unpack('<QQ', copied['/device-tree/chosen/memory-map']['TrustCache'])
            ctypes.memset(trust_cache, 0, len(tcdata))
            self.assertEqual(ctypes.string_at(tcbase, tclen), tcdata)
            self.assertEqual(tcbase % 0x4000, 0)
            self.assertGreaterEqual(tcbase, phys + dtva - virt + dtlen)
            self.assertLessEqual(tcbase + tclen, ba.value)
            base, length = struct.unpack('<QQ', nodes['/device-tree/pram']['reg'])
            self.assertEqual(length, 0x100000)
            self.assertEqual(base % 0x4000, 0)
            self.assertGreaterEqual(base, ba.value + 1152)
            self.assertEqual(base + length, top)
            self.assertLessEqual(top, phys + size)
            self.assertEqual(ctypes.string_at(base, length), bytes(length))
            self.assertEqual(struct.unpack('<I', nodes['/device-tree/chosen']['embedded-panic-log-size']), (length,))
            self.assertEqual(struct.unpack('<QQ', nodes['/device-tree/chosen/memory-map']['PanicLog']), (base, length))
        finally:
            libc.free(arena)

    def test_large_arena_advertises_only_the_reserved_capacity(self):
        capacity = 1024 * 1024 * 1024
        # Reserve virtual space without faulting in a GiB of host RAM.
        with mmap.mmap(-1, capacity + 0x200000) as storage:
            start = ctypes.addressof(ctypes.c_char.from_buffer(storage))
            arena = (start + 0x1fffff) & ~0x1fffff
            kernel = ctypes.create_string_buffer(bytes(self.sample()))
            tree = ctypes.create_string_buffer(bridge.apple_dt(bridge.parse_fdt(self.fdt)))
            ba = ctypes.c_void_p(); info = (ctypes.c_uint64 * 3)()
            self.assertEqual(self.lib.bridge_prepare(arena, capacity, kernel, 512, tree,
                len(tree)-1, None, 0, 0, ctypes.byref(ba), info), 0)
            args = ctypes.string_at(ba, 1152)
            virt, phys, size, top = struct.unpack_from('<4Q', args, 8)
            dtva, dtlen = struct.unpack_from('<QI', args, 96)
            nodes = apple_nodes(ctypes.string_at(phys + dtva - virt, dtlen))
            self.assertEqual((phys, size), (arena, capacity))
            self.assertEqual(struct.unpack('<Q', args[1144:1152])[0], capacity)
            self.assertEqual(nodes['/device-tree/memory']['reg'], struct.pack('<QQ', arena, capacity))
            self.assertEqual(nodes['/device-tree/chosen']['dram-size'], struct.pack('<Q', capacity))
            self.assertLess(top, arena + capacity)
            self.assertEqual(self.lib.bridge_prepare(arena, capacity + 0x200000, kernel, 512,
                tree, len(tree)-1, None, 0, 0, ctypes.byref(ba), info), -2)
    def test_display_handoff_keeps_u64_addresses_and_le32_mode_cells(self):
        tree = bridge.parse_fdt(self.fdt)
        soc = next(n for n in tree['children'] if n['name'].startswith('soc'))
        display = next(n for n in soc['children'] if n['name'] == 'display-controller@c901000')
        display['props'].update({'qcom,display-handoff-version': struct.pack('>I', 1),
                                 'display-width': struct.pack('>I', 1080),
                                 'canvas-width': struct.pack('>I', 1080),
                                 'canvas-height': struct.pack('>I', 1920),
                                 'framebuffer-address': struct.pack('>Q', 0x123456000),
                                 'framebuffer-size': struct.pack('>Q', 0x1000000),
                                 'qcom,scanout-device-address': struct.pack('>Q', 0x90000000)})
        props = apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/display-controller@c901000']
        self.assertEqual(struct.unpack('<QQ', props['reg']), (0xc901000, 0x8f000))
        self.assertEqual(struct.unpack('<Q', props['framebuffer-address']), (0x123456000,))
        self.assertEqual(struct.unpack('<Q', props['qcom,scanout-device-address']), (0x90000000,))
        self.assertEqual(struct.unpack('<I', props['display-width']), (1080,))
        # Original IOMFB copies these CFData bytes directly into UInt32 fields.
        self.assertEqual(struct.unpack('<I', props['canvas-width']), (1080,))
        self.assertEqual(struct.unpack('<I', props['canvas-height']), (1920,))
        self.assertEqual(struct.unpack('<I', props['qcom,display-handoff-version']), (1,))
    def test_clock_handoff_cells_are_little_endian(self):
        tree = bridge.parse_fdt(self.fdt)
        soc = next(n for n in tree['children'] if n['name'].startswith('soc'))
        gcc = next(n for n in soc['children'] if n['name'] == 'clock-controller@100000')
        gcc['props'].update({'qcom,gcc-handoff-version': struct.pack('>I', 1),
                             'qcom,owned-roots': struct.pack('>II', 162, 240000000),
                             'qcom,owned-clocks': struct.pack('>II', 109, 0),
                             'qcom,owned-domains': struct.pack('>I', 1)})
        props = apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/clock-controller@100000']
        self.assertEqual(struct.unpack('<QQ', props['reg']), (0x100000, 0x94000))
        self.assertEqual(struct.unpack('<II', props['qcom,owned-roots']), (162, 240000000))
        self.assertEqual(struct.unpack('<II', props['qcom,owned-clocks']), (109, 0))
        self.assertEqual(struct.unpack('<I', props['qcom,owned-domains']), (1,))
        self.assertEqual(struct.unpack('<I', props['qcom,gcc-handoff-version']), (1,))
        # Synthetic policy exercises serialization only; it is never a boot
        # profile and does not claim observed/ACKed physical RPM state.
        policy = {'version':2,'iova_first':0x10000000,'iova_bytes':0x4000000,
                  'average_bandwidth':100000000,'peak_bandwidth':400000000,
                  'rpm_domains':[[0x1000+i,128,0] for i in range(4)],
                  'rpm_clocks':[[i,0,0] for i in range(27)],
                  'rpm_clock_limits':[[i,1 if i==6 else (4 if i==19 else 1000000),1 if i==6 else (4 if i==19 else 2000000)] for i in bridge.RPM_HANDOFF_CLOCK_LIMIT_IDS],
                  'rpm_interconnect':[[i,1000000000,0,0] for i in (0x2000,0x2001,0x2002,0x2100,0x2101,0x2102,0x2103,0x2104)]}
        for i in (0,2,3): policy['rpm_clocks'][i][1] = 100000
        policy['rpm_clocks'][6][1] = 1; policy['rpm_clocks'][7][1] = 19200
        for row in policy['rpm_interconnect']:
            if row[0] < 0x2100 or row[0] == 0x2104: row[2] = 100000000
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory)/'synthetic-policy.json'; profile.write_text(json.dumps(policy))
            report = bridge.configure_gpu_handoff(tree, profile)
            nodes = apple_nodes(bridge.apple_dt(tree))
            gpu = nodes['/device-tree/arm-io/gpu@5000000']
            self.assertEqual(struct.unpack('<6Q',gpu['reg']), (0x5000000,0x40000,0x5040000,0x10000,0x1000000,0x80000))
            self.assertEqual(gpu['qcom,gpu-handoff'],bytes(40))
            soc=next(n for n in tree['children'] if n['name']=='soc' or n['name'].startswith('soc@'))
            gpu_node=next(n for n in soc['children'] if n['name']=='gpu@5000000')
            firmware_record=(1,31,0x120000000,0x130000000,0x140000000,4096,4096,130,300000000,800000,0,0,0)
            gpu_node['props']['qcom,gpu-firmware-handoff']=struct.pack('>II3Q8I',*firmware_record)
            gpu=apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/gpu@5000000']
            self.assertEqual(struct.unpack('<II3Q8I',gpu['qcom,gpu-firmware-handoff']),firmware_record)
            self.assertEqual(struct.unpack_from('<3Q',gpu['qcom,adreno-smmu-handoff'],32), (0,0x10000000,0x14000000))
            self.assertEqual(struct.unpack('<IIQQII',gpu['qcom,gpu-noc-handoff']), (0,0,100000000,400000000,1,0))
            self.assertFalse(report['hardware_boot_verified'])
            policy['firmware_power']={'frequency_hz':257000000,'voltage_uv':800000}
            policy['gfx_regulator']={'id':1,'sid':5,'minimum_uv':524000,'maximum_uv':1100000}
            profile.write_text(json.dumps(policy))
            bridge.configure_gpu_handoff(tree,profile,cold_start=True)
            rpm=apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/power-controller@778000']
            self.assertEqual(rpm['qcom,rpm-handoff-version'],struct.pack('<I',2))
            self.assertEqual(rpm['qcom,rpm-transport-handoff'],bytes(48))
            policy['firmware_power']['frequency_hz']=710000097
            profile.write_text(json.dumps(policy))
            with self.assertRaises(ValueError): bridge.configure_gpu_handoff(tree,profile,cold_start=True)
            policy['firmware_power']['frequency_hz']=300000000
            profile.write_text(json.dumps(policy))
            with self.assertRaises(ValueError): bridge.configure_gpu_handoff(tree,profile,cold_start=True)
            bridge.configure_gpu_handoff(tree,profile)
            rpm=apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/power-controller@778000']
            self.assertNotIn('qcom,rpm-transport-handoff',rpm)
            mdt = Path(directory)/'zap.mdt'; segment = mdt.with_suffix('.b02'); packed = Path(directory)/'gpu-zap.bin'
            mdt.write_bytes(b'original signed metadata'); segment.write_bytes(b'original segment')
            firmware = bridge.pack_gpu_zap(mdt, packed)
            wrapped = packed.read_bytes()
            self.assertEqual(struct.unpack_from('<5I',wrapped), (0x50415a51,1,2,len(wrapped),16))
            for at,identity,original in ((32,0xffffffff,mdt.read_bytes()),(48,2,segment.read_bytes())):
                kind,offset,size,reserved = struct.unpack_from('<4I',wrapped,at)
                self.assertEqual((kind,size,reserved),(identity,len(original),0))
                self.assertEqual(wrapped[offset:offset+size],original)
            self.assertFalse(firmware['signature_verified'])
            self.assertFalse(firmware['firmware_bytes_rewritten'])
            original = {}
            for i,name in enumerate(('a530_pm4.fw','a530_pfp.fw','a540_gpmu.fw2')):
                original[i] = struct.pack('<4I',3,2,i,0x12345678)
                (Path(directory)/name).write_bytes(original[i])
            packed_microcode = Path(directory)/'gpu-firmware.bin'
            microcode_report = bridge.pack_gpu_firmware(Path(directory),packed_microcode)
            microcode = packed_microcode.read_bytes()
            self.assertEqual(struct.unpack_from('<5I',microcode),(0x57464751,1,3,len(microcode),16))
            for i in range(3):
                kind,offset,size,reserved = struct.unpack_from('<4I',microcode,32+16*i)
                self.assertEqual((kind,reserved),(i,0))
                self.assertEqual(microcode[offset:offset+size],original[i])
            self.assertFalse(microcode_report['hardware_boot_verified'])
            self.assertFalse(microcode_report['firmware_bytes_rewritten'])
            invalid = subprocess.run(['python3',str(ROOT/'scripts/build-bridge.py'),'--gpu-smmu-init',
                '--output',str(Path(directory)/'invalid-smmu')],capture_output=True,text=True)
            self.assertEqual(invalid.returncode,2)
            self.assertIn('--gpu-smmu-init requires --gpu-firmware',invalid.stderr)
            invalid = subprocess.run(['python3',str(ROOT/'scripts/build-bridge.py'),'--gpu-cold-start',
                '--output',str(Path(directory)/'invalid-cold')],capture_output=True,text=True)
            self.assertEqual(invalid.returncode,2)
            self.assertIn('--gpu-cold-start requires --gpu-smmu-init',invalid.stderr)
            policy['firmware_power']={'frequency_hz':300000000,'voltage_uv':800000}
            profile.write_text(json.dumps(policy));bridge.configure_gpu_handoff(tree,profile)
            props=apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/gpu@5000000']
            self.assertEqual(struct.unpack('<4I',props['qcom,gpu-firmware-power']),(1,300000000,800000,0))
            policy['firmware_power']['voltage_uv']=800001;profile.write_text(json.dumps(policy))
            with self.assertRaises(ValueError):bridge.configure_gpu_handoff(tree,profile)
            policy.pop('firmware_power');policy.pop('gfx_regulator');profile.write_text(json.dumps(policy));bridge.configure_gpu_handoff(tree,profile)
            self.assertNotIn('qcom,gpu-firmware-power',apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/gpu@5000000'])
            policy['zap_memory']=['0x95600000','0x100000'];profile.write_text(json.dumps(policy))
            bridge.configure_gpu_handoff(tree,profile)
            props=apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/gpu@5000000']
            self.assertEqual(struct.unpack('<QQ',props['qcom,gpu-zap-memory']),(0x95600000,0x100000))
            policy.pop('zap_memory');profile.write_text(json.dumps(policy));bridge.configure_gpu_handoff(tree,profile)
            self.assertNotIn('qcom,gpu-zap-memory',apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/gpu@5000000'])
            policy['rpm_clocks'][2][1] += 1; profile.write_text(json.dumps(policy))
            with self.assertRaises(ValueError): bridge.configure_gpu_handoff(tree,profile)
    def test_power_controllers_match_and_convert_resources(self):
        import plistlib
        tree = bridge.parse_fdt(self.fdt)
        soc = next(n for n in tree['children'] if n['name'].startswith('soc'))
        paths = {'QcomRPM':'power-controller@778000', 'QcomSPMI':'power-controller@800f000',
                 'QcomMMCC':'clock-controller@c8c0000', 'QcomGPUCC':'clock-controller@5065000',
                 'QcomOSM':'performance-controller@179c0000'}
        for module, name in paths.items():
            node = next(n for n in soc['children'] if n['name']==name)
            personality = next(iter(plistlib.loads(next((ROOT/'drivers').glob('*/'+module+'/Info.plist')).read_bytes())['IOKitPersonalities'].values()))
            matches = personality['IONameMatch']
            if isinstance(matches,str): matches=[matches]
            self.assertTrue(set(matches) & set(node['props']['compatible'].rstrip(b'\0').decode().split('\0')))
            self.assertEqual(personality['IOProviderClass'],'AppleARMIODevice')
        rpm = next(n for n in soc['children'] if n['name']==paths['QcomRPM'])
        spmi = next(n for n in soc['children'] if n['name']==paths['QcomSPMI'])
        mmcc = next(n for n in soc['children'] if n['name']==paths['QcomMMCC'])
        rpm['props']['qcom,regulator-limits']=struct.pack('>4I',1,320000,1000000,500000)
        rpm['props']['qcom,rpm-handoff-version']=struct.pack('>I',1)
        domains=[(0x1000+i,192,64,0) for i in range(4)]
        rpm['props']['qcom,rpm-domain-handoff']=b''.join(struct.pack('>4I',*v) for v in domains)
        clocks=[(i,19200,19200,0) for i in range(27)]
        rpm['props']['qcom,rpm-clock-handoff']=b''.join(struct.pack('>4I',*v) for v in clocks)
        clock_limits=[(i,1000000,2000000,0) for i in bridge.RPM_HANDOFF_CLOCK_LIMIT_IDS]
        rpm['props']['qcom,rpm-clock-limit-handoff']=b''.join(struct.pack('>4I',*v) for v in clock_limits)
        buses=[(0x2100+i,0,1<<40,(1<<32)+i,0) for i in range(8)]
        rpm['props']['qcom,rpm-interconnect-handoff']=b''.join(struct.pack('>IIQQQ',*v) for v in buses)
        spmi['props']['qcom,pm8005-regulators']=struct.pack('>5I',1,1,320000,1000000,800000)
        mmcc['props']['qcom,supply-points']=struct.pack('>5I',43,300000000,2,0x1000,192)
        nodes=apple_nodes(bridge.apple_dt(tree))
        self.assertEqual(struct.unpack('<4Q',nodes['/device-tree/arm-io/'+paths['QcomRPM']]['reg']),
                         (0x778000,0x7000,0x17911000,0x1000))
        self.assertEqual(len(nodes['/device-tree/arm-io/'+paths['QcomSPMI']]['reg']),80)
        self.assertEqual(struct.unpack('<4I',nodes['/device-tree/arm-io/'+paths['QcomRPM']]['qcom,regulator-limits']),(1,320000,1000000,500000))
        props=nodes['/device-tree/arm-io/'+paths['QcomRPM']]
        self.assertEqual(list(struct.iter_unpack('<4I',props['qcom,rpm-domain-handoff'])),domains)
        self.assertEqual(list(struct.iter_unpack('<4I',props['qcom,rpm-clock-handoff'])),clocks)
        self.assertEqual(list(struct.iter_unpack('<4I',props['qcom,rpm-clock-limit-handoff'])),clock_limits)
        self.assertEqual(list(struct.iter_unpack('<IIQQQ',props['qcom,rpm-interconnect-handoff'])),buses)
        for key,(fmt,_,_) in bridge.HANDOFF_RECORDS.items():
            if key.startswith('qcom,rpm-'):continue
            values=tuple((1<<36)+i if c=='Q' else i for i,c in enumerate(fmt.replace('8I','I'*8).replace('16I','I'*16).replace('3Q','QQQ')))
            rpm['props'][key]=struct.pack('>'+fmt,*values)
            actual=apple_nodes(bridge.apple_dt(tree))['/device-tree/arm-io/'+paths['QcomRPM']][key]
            self.assertEqual(struct.unpack('<'+fmt,actual),values)
            rpm['props'][key]=b'bad'
            with self.assertRaisesRegex(ValueError,'invalid handoff'):bridge.apple_dt(tree)
            del rpm['props'][key]
        self.assertEqual(struct.unpack('<5I',nodes['/device-tree/arm-io/'+paths['QcomSPMI']]['qcom,pm8005-regulators']),(1,1,320000,1000000,800000))
        self.assertEqual(struct.unpack('<5I',nodes['/device-tree/arm-io/'+paths['QcomMMCC']]['qcom,supply-points']),(43,300000000,2,0x1000,192))
    def test_fdt_arena_excludes_reserved_memory_and_loader(self):
        tmp = Path(self.temp.name)
        dts = '/dts-v1/; /memreserve/ 0xa0000000 0x02000000; / { #address-cells=<2>; #size-cells=<2>; memory@a0000000 { device_type="memory"; reg=<0 0xa0000000 0 0x40000000>; }; reserved-memory { #address-cells=<2>; #size-cells=<2>; ranges; secure@a2000000 { reg=<0 0xa2000000 0 0x02000000>; }; }; };'
        (tmp/'firmware.dts').write_text(dts)
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(tmp/'firmware.dtb'), str(tmp/'firmware.dts')], check=True)
        buf = ctypes.create_string_buffer((tmp/'firmware.dtb').read_bytes()); arena = ctypes.c_uint64()
        self.assertEqual(self.lib.fdt_find_arena(buf, 0x10000000, 0xa5000000, 0xa6000000, ctypes.byref(arena)), 0)
        self.assertEqual(arena.value, 0xa6000000)
        self.assertLess(self.lib.fdt_find_arena(buf, 0x40000000, 0xa5000000, 0xa6000000, ctypes.byref(arena)), 0)
        self.assertLess(self.lib.fdt_find_arena(buf, 0x10000001, 0, 0, ctypes.byref(arena)), 0)
    def test_fdt_reserved_ranges_above_4g(self):
        tmp = Path(self.temp.name)
        (tmp/'high.dts').write_text('/dts-v1/; / { #address-cells=<2>; #size-cells=<2>; memory@100000000 { device_type="memory"; reg=<1 0 0 0x40000000>; }; chosen { linux,initrd-start=<1 0>; linux,initrd-end=<1 0x02000000>; }; };')
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(tmp/'high.dtb'), str(tmp/'high.dts')], check=True)
        buf = ctypes.create_string_buffer((tmp/'high.dtb').read_bytes()); arena = ctypes.c_uint64()
        self.assertEqual(self.lib.fdt_find_arena(buf, 0x10000000, 0x80080000, 0x81000000, ctypes.byref(arena)), 0)
        self.assertEqual(arena.value, 0x102000000)
    def test_boot_video_geometry_and_physical_address_flag(self):
        ba = ctypes.create_string_buffer(1152)
        self.assertEqual(self.lib.bridge_set_video(ba, 0x9d400000, 1080*1920*4, 1080, 1920, 4320), 0)
        self.assertEqual(struct.unpack_from('<6Q', ba.raw, 40), (0x9d400001, 0, 4320, 1080, 1920, 32))
        for base, size, width, height, stride in [(0, 100, 8, 16, 32), (0x1001, 512, 8, 16, 32), (0x1000, 511, 8, 16, 32), (0x1000, 512, 8, 16, 31), (0x1000, 512, 9, 16, 32)]:
            self.assertLess(self.lib.bridge_set_video(ba, base, size, width, height, stride), 0)
    def test_firmware_simple_framebuffer_is_reserved_and_forwarded(self):
        tmp = Path(self.temp.name)
        (tmp/'video.dts').write_text('/dts-v1/; / { #address-cells=<2>; #size-cells=<2>; memory@a0000000 { device_type="memory"; reg=<0 0xa0000000 0 0x40000000>; }; framebuffer@a0000000 { compatible="simple-framebuffer"; reg=<0 0xa0000000 0 0x800000>; width=<1080>; height=<1920>; stride=<4320>; format="x8r8g8b8"; }; };')
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(tmp/'video.dtb'), str(tmp/'video.dts')], check=True)
        buf = ctypes.create_string_buffer((tmp/'video.dtb').read_bytes()); ba = ctypes.create_string_buffer(1152); arena = ctypes.c_uint64()
        self.assertEqual(self.lib.fdt_get_video(buf, ba), 0)
        self.assertEqual(struct.unpack_from('<6Q', ba.raw, 40), (0xa0000001, 0, 4320, 1080, 1920, 32))
        self.assertEqual(self.lib.fdt_find_arena(buf, 0x10000000, 0x80080000, 0x81000000, ctypes.byref(arena)), 0)
        self.assertEqual(arena.value, 0xa0800000)
    def test_watchdog_programming(self):
        src = '''#include <stdint.h>
#include <assert.h>
static unsigned int count, offsets[6], values[6];
#define WDT_WRITE(off,val) do {offsets[count]=off;values[count++]=val;}while(0)
#include "watchdog.h"
int main(void) {
 assert(bridge_watchdog_arm(5)==0 && count==6);
 assert(offsets[0]==8 && values[0]==0);
 assert(offsets[2]==16 && values[2]==0xfffff);
 assert(offsets[3]==20 && values[3]==32765*5);
 assert(offsets[4]==8 && values[4]==3);
 count=0; assert(bridge_watchdog_arm(0)==0 && count==1 && values[0]==0);
 count=0; assert(bridge_watchdog_arm(31)==0 && count==6 && values[3]==32765*31);
 count=0; assert(bridge_watchdog_arm(32)<0 && count==0);
}'''
        tmp = Path(self.temp.name); (tmp/'wdt.c').write_text(src)
        subprocess.run(['clang', '-Wall', '-Werror', '-I'+str(ROOT/'boot/bridge'), str(tmp/'wdt.c'), '-o', str(tmp/'wdt')], check=True)
        subprocess.run([str(tmp/'wdt')], check=True)

if __name__ == '__main__': unittest.main()
