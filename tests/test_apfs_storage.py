import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


storage = module('apfs_storage', ROOT/'scripts/apfs-storage.py')
bridge = module('apfs_bridge', ROOT/'scripts/build-bridge.py')


def disk(path, sector):
    total = 16384; table = bytearray(128*128)
    def part(i, name, start, end):
        base=i*128
        table[base:base+16]=uuid.uuid4().bytes_le
        table[base+16:base+32]=uuid.uuid4().bytes_le
        struct.pack_into('<3Q', table, base+32, start, end, 0)
        n=name.encode('utf-16-le');table[base+56:base+56+len(n)]=n
    first=2+(len(table)+sector-1)//sector;last=total-first
    part(0,'boot',first,first+127);part(1,'userdata',first+128,last)
    guid=uuid.uuid4().bytes_le
    def hdr(current,alternate,entries):
        raw=bytearray(sector);raw[:8]=b'EFI PART'
        struct.pack_into('<4I4Q',raw,8,0x10000,92,0,0,current,alternate,first,last)
        raw[56:72]=guid;struct.pack_into('<Q3I',raw,72,entries,128,128,zlib.crc32(table))
        return storage.rewrite_header(raw,table)
    with path.open('wb') as f:
        f.truncate(total*sector)
        for lba,data in [(1,hdr(1,total-1,2)),(2,table),(total-1-len(table)//sector,table),
                         (total-1,hdr(total-1,1,total-1-len(table)//sector)),(first,b'BOOT SENTINEL')]:
            f.seek(lba*sector);f.write(data)
    return first


class APFSStorageTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def fixture(self,sector):
        device=self.root/'disk.img';first=disk(device,sector)
        image=self.root/'apfs.img';image.write_bytes(b'APFS TEST PAYLOAD'+bytes(2*1024*1024-17))
        report={'container_bytes':image.stat().st_size,'image_sha256':storage.sha(image),
                'system_uuid':str(uuid.uuid4()).upper(),'data_uuid':str(uuid.uuid4()).upper()}
        with device.open('rb') as f: plan=storage.make_plan(f.fileno(),sector,device.stat().st_size,report)
        return device,image,plan,first
    def test_install_is_bounded_and_both_gpts_are_valid(self):
        for sector in (512,4096):
            with self.subTest(sector=sector):
                case=self.root/str(sector);case.mkdir();previous=self.root;self.root=case
                device,image,plan,first=self.fixture(sector)
                storage.install(device,plan,image,case/'backup',Path('/usr/bin/true'))
                with device.open('rb') as f:
                    _,_,parts,_=storage.read_gpt(f.fileno(),sector,device.stat().st_size)
                    self.assertEqual(os.pread(f.fileno(),13,first*sector),b'BOOT SENTINEL')
                    apfs=next(p for p in parts if p['name']=='ios-apfs')
                    self.assertEqual(os.pread(f.fileno(),image.stat().st_size,apfs['first_lba']*sector),image.read_bytes())
                    user=next(p for p in parts if p['name']=='userdata')
                    self.assertLess(user['last_lba'],apfs['first_lba'])
                self.assertTrue((case/'backup/restore.json').exists());self.root=previous
                verified=storage.direct_readback(device,plan)
                self.assertTrue(verified['host_cache_bypassed'])
                self.assertEqual(verified['sha256'],storage.sha(image))
    def test_crc_failure_is_rejected(self):
        device,_,_,_=self.fixture(512)
        with device.open('r+b') as f:f.seek(530);f.write(b'\xff')
        with device.open('rb') as f:
            with self.assertRaisesRegex(ValueError,'CRC'):storage.read_gpt(f.fileno(),512,device.stat().st_size)
    def test_tampered_target_and_changed_image_never_write(self):
        device,image,plan,_=self.fixture(512);before=storage.sha(device)
        plan['apfs']['first_lba']=0
        with self.assertRaisesRegex(ValueError,'target'):
            storage.install(device,plan,image,self.root/'backup',Path('/usr/bin/true'))
        self.assertEqual(storage.sha(device),before)
        image.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'image differs'):
            storage.install(device,plan,image,self.root/'backup',Path('/usr/bin/true'))
        self.assertEqual(storage.sha(device),before)
    def test_oversized_image_never_gets_a_plan(self):
        device,image,plan,_=self.fixture(512);plan['container_bytes']=device.stat().st_size
        with device.open('rb') as f:
            with self.assertRaisesRegex(ValueError,'too small'):
                storage.make_plan(f.fileno(),512,device.stat().st_size,plan)
    def test_boot_uuid_overrides_stale_disk_indices(self):
        tree={'children':[{'name':'chosen','props':{'bootargs':b'-v rd=disk0s1s1 rootdev=md0 boot-uuid=old\0','root-matching':b'old'}}]}
        report={'system_uuid':str(uuid.uuid4()),'data_uuid':str(uuid.uuid4())}
        bridge.configure_apfs_root(tree,report);p=tree['children'][0]['props']
        self.assertEqual(p['boot-uuid'],report['system_uuid'].upper().encode()+b'\0')
        self.assertIn(report['system_uuid'].upper().encode(),p['root-matching'])
        self.assertNotIn(b'old',p['root-matching']);self.assertNotIn(b'disk0',p['bootargs'])
        self.assertIn(b'rd=uuid',p['bootargs'])


if __name__=='__main__':unittest.main()
