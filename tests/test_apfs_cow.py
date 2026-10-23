"""Installer boundary/failure tests. Filesystem validity is tested by Go/apfsck."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from test_apfs_storage import module, disk, ROOT, storage

cow=module('apfs_cow',ROOT/'scripts/apfs-cow.py')


class COWInstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        device=self.root/'disk.img';disk(device,4096)
        source=self.root/'source';base=bytearray(2*1024*1024);base[32:36]=b'NXSB';source.write_bytes(base)
        report={'container_bytes':len(base),'image_sha256':storage.sha(source),'system_uuid':'fixture','data_uuid':'fixture'}
        with device.open('rb') as f:plan=storage.make_plan(f.fileno(),4096,device.stat().st_size,report)
        # This fixture tests the installer, not APFS encoding/checking.
        storage.install(device,plan,source,self.root/'gpt-backup',Path('/usr/bin/true'))
        primary=bytearray(base[:4096]);primary[16]=2;tail=b'new free block'.ljust(4096,b'\0')
        raw=b'APFSCOW1'+struct.pack('<QQ',len(base),2)+struct.pack('<Q',0)+primary+struct.pack('<Q',64)+tail
        delta=self.root/'test.delta';delta.write_bytes(raw)
        metadata={'container_bytes':len(base),'changed_blocks':2,'metadata_start_block':64,'source_sha256':storage.sha(source),
            'source_superblock_sha256':hashlib.sha256(base[:4096]).hexdigest(),'delta_sha256':storage.sha(delta)}
        Path(str(delta)+'.json').write_text(json.dumps(metadata))
        validation=self.root/'validated.json';validation.write_text(json.dumps({'apfsck_clean':True,'apfsck_exit_code':0,'delta_sha256':storage.sha(delta),'source_sha256':storage.sha(source)}))
        self.delta=cow.Delta(delta)
        self.args=argparse.Namespace(device=device,delta=delta,validation=validation,backup=self.root/'cow-backup',
            disk_guid=plan['disk_guid'],partition_guid=plan['apfs']['guid'],sector_size=4096,same_offline_session=False)
        self.offset=plan['apfs']['first_lba']*4096
        self.before=device.read_bytes()

    def tearDown(self):
        self.delta.raw.close();self.delta.file.close();self.tmp.cleanup()

    def test_commit_writes_only_planned_blocks_and_keeps_gpt(self):
        cow.apply(self.args,self.delta)
        expected=bytearray(self.before)
        for block,data in self.delta.records():expected[self.offset+block*4096:self.offset+(block+1)*4096]=data
        self.assertEqual(self.args.device.read_bytes(),expected)
        self.assertEqual((self.args.backup/'primary-before.bin').read_bytes(),self.before[self.offset:self.offset+4096])
        self.assertTrue(json.loads((self.args.backup/'transaction.json').read_text())['committed'])
        self.args.output=self.root/'physical.json'
        cow.verify_blocks(self.args,self.delta)
        self.assertTrue(json.loads(self.args.output.read_text())['all_changed_blocks_verified'])

    def test_changed_device_or_wrong_identity_never_writes(self):
        self.args.disk_guid='wrong'
        with self.assertRaisesRegex(ValueError,'GUID'):cow.apply(self.args,self.delta)
        self.assertEqual(self.args.device.read_bytes(),self.before)
        with self.args.device.open('rb') as f:
            p,_,_,_=storage.read_gpt(f.fileno(),4096,len(self.before))
        import uuid
        self.args.disk_guid=str(uuid.UUID(bytes_le=p['raw'][56:72])).upper()
        with self.args.device.open('r+b') as f:f.seek(self.offset+10000);f.write(b'x')
        changed=self.args.device.read_bytes()
        with self.assertRaisesRegex(ValueError,'source changed since delta planning'):cow.apply(self.args,self.delta)
        self.assertEqual(self.args.device.read_bytes(),changed)
        self.assertFalse(self.args.backup.exists())

    def test_failed_tail_readback_keeps_old_primary(self):
        original=os.pread
        def read(fd,n,offset):
            data=original(fd,n,offset)
            if n==4096 and offset==self.offset+64*4096:return bytes(4096)
            return data
        with patch.object(cow.os,'pread',side_effect=read):
            with self.assertRaisesRegex(OSError,'before commit'):cow.apply(self.args,self.delta)
        actual=self.args.device.read_bytes()
        self.assertEqual(actual[self.offset:self.offset+4096],self.before[self.offset:self.offset+4096])
        self.assertFalse(json.loads((self.args.backup/'transaction.json').read_text())['commit_started'])

    def test_corrupted_delta_is_rejected(self):
        with self.args.delta.open('r+b') as f:f.seek(-1,2);f.write(b'x')
        with self.assertRaisesRegex(ValueError,'checksum'):cow.Delta(self.args.delta)
        self.assertEqual(self.args.device.read_bytes(),self.before)

    def test_metadata_only_diagnostic_cannot_authorize_install(self):
        report=json.loads(self.args.validation.read_text());report['metadata_only']=True
        self.args.validation.write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError,'independent validation is absent'):
            cow.apply(self.args,self.delta)
        self.assertEqual(self.args.device.read_bytes(),self.before)
        self.assertFalse(self.args.backup.exists())

    def test_sparse_checkpoint_writes_must_match_declared_ranges(self):
        metadata = self.delta.report.copy()
        metadata['write_ranges'] = [{'start_block': 64, 'block_count': 1}]
        report = Path(str(self.args.delta)+'.json')
        report.write_text(json.dumps(metadata))
        checked = cow.Delta(self.args.delta)
        checked.raw.close(); checked.file.close()
        for ranges, message in [
            ([{'start_block': 63, 'block_count': 1}], 'outside sparse'),
            ([{'start_block': 64, 'block_count': 2}], 'count mismatch'),
            ([{'start_block': 0, 'block_count': 1}], 'invalid sparse'),
            ([{'start_block': 64, 'block_count': -1}], 'invalid sparse'),
        ]:
            with self.subTest(ranges=ranges):
                metadata['write_ranges'] = ranges
                report.write_text(json.dumps(metadata))
                with self.assertRaisesRegex(ValueError, message): cow.Delta(self.args.delta)
        self.assertEqual(self.args.device.read_bytes(),self.before)

    def structural_identity(self):
        records = []
        digest = hashlib.sha256()
        with self.args.device.open('rb') as f:
            for block in (0, 1):
                checksum = hashlib.sha256(os.pread(f.fileno(), 4096, self.offset+block*4096)).hexdigest()
                records.append({'block': block, 'sha256': checksum})
                digest.update(struct.pack('<Q', block)); digest.update(bytes.fromhex(checksum))
        self.delta.report.pop('source_sha256')
        self.delta.report.update(repair='checkpoint-area', source_identity_kind='apfs-checkpoint-structures-sha256',
            source_structures=records, source_structure_sha256=digest.hexdigest())
        self.args.validation.write_text(json.dumps({'apfsck_clean':True,'apfsck_exit_code':0,
            'delta_sha256':self.delta.sha,'source_identity_kind':'apfs-checkpoint-structures-sha256',
            'source_structure_sha256':digest.hexdigest()}))

    def test_checkpoint_repair_checks_structures_without_reading_entire_partition(self):
        self.structural_identity()
        with patch.object(cow, 'hash_range', side_effect=AssertionError('full partition read requested')):
            cow.apply(self.args,self.delta)
        state = json.loads((self.args.backup/'transaction.json').read_text())
        self.assertTrue(state['committed'])
        self.assertEqual(state['source_identity_kind'],'apfs-checkpoint-structures-sha256')
        self.assertIsNone(state['source_sha256'])

    def test_changed_structure_rejects_checkpoint_repair_before_any_write(self):
        self.structural_identity()
        with self.args.device.open('r+b') as f:
            f.seek(self.offset+4096+20); f.write(b'x')
        changed = self.args.device.read_bytes()
        with patch.object(cow, 'hash_range', side_effect=AssertionError('full partition read requested')):
            with self.assertRaisesRegex(ValueError,'source structure changed at block 1'):
                cow.apply(self.args,self.delta)
        self.assertEqual(self.args.device.read_bytes(),changed)
        self.assertFalse(self.args.backup.exists())

    def test_system_overlay_uses_structural_identity_and_rejects_stale_source(self):
        self.structural_identity()
        self.delta.report['repair'] = 'system-overlay'
        with self.args.device.open('r+b') as f:
            f.seek(self.offset+4096+20); f.write(b'x')
        changed = self.args.device.read_bytes()
        with patch.object(cow, 'hash_range', side_effect=AssertionError('full partition read requested')):
            with self.assertRaisesRegex(ValueError,'source structure changed at block 1'):
                cow.apply(self.args,self.delta)
        self.assertEqual(self.args.device.read_bytes(),changed)
        self.assertFalse(self.args.backup.exists())

    def test_data_overlay_uses_the_same_structural_identity(self):
        self.structural_identity()
        self.delta.report['repair'] = 'data-overlay'
        with patch.object(cow, 'hash_range', side_effect=AssertionError('full partition read requested')):
            cow.apply(self.args,self.delta)
        state = json.loads((self.args.backup/'transaction.json').read_text())
        self.assertTrue(state['committed'])
        self.assertEqual(state['source_identity_kind'],'apfs-checkpoint-structures-sha256')


if __name__=='__main__':unittest.main()
