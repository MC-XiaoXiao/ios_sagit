#!/usr/bin/env python3
"""Offline APFS block-delta validation and installation; never builds a full image.

Only the project's unmounted, unencrypted containers with System/Data are supported.
GPT is read and checked, never modified. The old block zero is backed up before
writing free blocks; the new block zero is published only after fsync/readback.
"""
import argparse
import hashlib
import importlib.util
import json
import mmap
import os
from pathlib import Path
import struct
import stat
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('apfs_storage', ROOT/'scripts/apfs-storage.py')
storage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(storage)


def write_json(path, obj):
    with tempfile.NamedTemporaryFile(mode='w',dir=path.parent,prefix=path.name+'.',delete=False) as f:
        temporary=Path(f.name)
        f.write(json.dumps(obj, indent=2)+'\n');f.flush();os.fsync(f.fileno())
    temporary.replace(path)
    sync_dir(path.parent)


def sync_dir(path):
    fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY)
    try:os.fsync(fd)
    finally:os.close(fd)


class Delta:
    def __init__(self, path):
        self.path = path
        self.report = json.loads(Path(str(path)+'.json').read_text())
        self.file = path.open('rb')
        try:
            self.raw = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
            if len(self.raw) < 24 or self.raw[:8] != b'APFSCOW1':
                raise ValueError('invalid block delta')
            self.size, self.count = struct.unpack_from('<QQ', self.raw, 8)
            if self.size != self.report['container_bytes'] or self.size % 4096 or self.count != self.report['changed_blocks'] or len(self.raw) != 24+self.count*4104:
                raise ValueError('delta geometry/report mismatch')
            self.sha = storage.sha(path)
            if self.sha != self.report['delta_sha256']:
                raise ValueError('delta checksum mismatch')
            previous = -1
            ranges = self.report.get('write_ranges')
            if ranges is not None:
                end = 1
                for span in ranges:
                    start, count = span['start_block'], span['block_count']
                    if not isinstance(start, int) or not isinstance(count, int) or start < end or count <= 0 or start+count > self.size//4096:
                        raise ValueError('invalid sparse COW write ranges')
                    end = start+count
                if sum(span['block_count'] for span in ranges) != self.count-1:
                    raise ValueError('sparse COW write range count mismatch')
            for block, data in self.records():
                if block <= previous or block >= self.size//4096 or (block and block < self.report['metadata_start_block']):
                    raise ValueError('unsorted, duplicate, live or out-of-bounds delta block')
                if block and ranges is not None and not any(span['start_block'] <= block < span['start_block']+span['block_count'] for span in ranges):
                    raise ValueError('delta block outside sparse COW write ranges')
                previous = block
            if self.count < 2 or struct.unpack_from('<Q', self.raw, 24)[0] != 0 or self.raw[64:68] != b'NXSB':
                raise ValueError('missing primary superblock commit')
        except Exception:
            if hasattr(self,"raw"): self.raw.close()
            self.file.close()
            raise

    def records(self):
        for i in range(self.count):
            off = 24+i*4104
            yield struct.unpack_from('<Q', self.raw, off)[0], self.raw[off+8:off+4104]

    def runs(self):
        start=None;data=bytearray()
        for block,payload in self.records():
            if start is not None and (block!=start+len(data)//4096 or len(data)==1024*1024):
                yield start,bytes(data);start=None;data.clear()
            if start is None:start=block
            data.extend(payload)
        if start is not None:yield start,bytes(data)



def source_identity(report):
    kind = report.get('source_identity_kind', 'full-container-sha256')
    if kind == 'apfs-checkpoint-structures-sha256':
        return kind, report['source_structure_sha256']
    if kind != 'full-container-sha256':
        raise ValueError('unsupported source identity kind')
    return kind, report['source_sha256']


def check_source(fd, offset, delta, same_offline_session):
    """Reuse the plan's full hash only while its source remains offline/unchanged."""
    kind, expected = source_identity(delta.report)
    if kind == 'apfs-checkpoint-structures-sha256':
        if delta.report.get('repair') not in ('checkpoint-area', 'system-overlay', 'data-overlay'):
            raise ValueError('structural identity requires a native-state COW plan')
        manifest = hashlib.sha256(); previous = -1
        records = delta.report['source_structures']
        if not records or records[0]['block'] != 0:
            raise ValueError('structural identity is missing the original primary')
        for record in records:
            block, digest = record['block'], record['sha256']
            if not isinstance(block, int) or block <= previous or block >= delta.size//4096 or len(bytes.fromhex(digest)) != 32:
                raise ValueError('invalid structural source manifest')
            previous = block
            data = os.pread(fd, 4096, offset+block*4096)
            if len(data) != 4096 or hashlib.sha256(data).hexdigest() != digest:
                raise ValueError(f'source structure changed at block {block}')
            manifest.update(struct.pack('<Q', block)); manifest.update(bytes.fromhex(digest))
        if manifest.hexdigest() != expected:
            raise ValueError('structural source manifest digest mismatch')
        return kind
    if same_offline_session:
        if 'source_hash_reused_from' in delta.report:
            raise ValueError('same-session reuse requires a plan that hashed the complete source')
    elif hash_range(fd, offset, delta.size) != delta.report['source_sha256']:
        raise ValueError('source changed since delta planning')
    if hashlib.sha256(os.pread(fd, 4096, offset)).hexdigest() != delta.report['source_superblock_sha256']:
        raise ValueError('source superblock mismatch')
    return 'same-offline-session-superblock' if same_offline_session else 'full-container-sha256'

def validate(args, delta):
    source = Path(delta.report['source']).resolve()
    source_offset=delta.report.get('source_offset',0)
    with source.open('rb') as f:
        identity_check = check_source(f.fileno(), source_offset, delta, args.same_offline_session)
    work = Path(tempfile.mkdtemp(prefix='apfs-cow-', dir=ROOT/'build'))
    mount = work/'view'; mount.mkdir()
    log = args.output.with_suffix('.apfsck.log')
    helper = ROOT/'build/apfs/apfs-cow-view'
    with (work/'fuse.log').open('w') as fuse_log:
        process = subprocess.Popen([str(helper),str(source),str(args.delta.resolve()),str(mount),str(source_offset)], stdout=fuse_log, stderr=subprocess.STDOUT)
        try:
            for _ in range(100):
                if (mount/'container.apfs').exists(): break
                if process.poll() is not None: raise RuntimeError((work/'fuse.log').read_text())
                time.sleep(.05)
            else: raise RuntimeError('read-only FUSE view did not become ready')
            checker = ROOT/'tools/src/apfsprogs/apfsck/apfsck'
            # Native-state plans preserve volume crash flags and
            # pending free queues. Check their integrity without declaring a
            # clean unmount or changing the preserved volume superblocks.
            crash_flags = delta.report.get('repair') not in ('checkpoint-area', 'system-overlay', 'data-overlay')
            checker_args = [str(checker), *(['-c'] if crash_flags else []), *(['-m'] if args.metadata_only else []), '-w', str(mount/'container.apfs')]
            with log.open('w') as out:
                result = subprocess.run(checker_args,stdout=out,stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError('independent apfsck failed: '+str(log))
            write_json(args.output, {'schema':1, 'delta_sha256':delta.sha,
                'source_sha256':delta.report.get('source_sha256'),
                'source_identity_kind':source_identity(delta.report)[0],
                'source_structure_sha256':delta.report.get('source_structure_sha256'),
                'source_identity_check':identity_check,'apfsck_clean':not args.metadata_only,'metadata_only':args.metadata_only,'unclean_unmount_flags_checked':crash_flags,
                'apfsck_exit_code':0,'apfsck_sha256':storage.sha(checker),'log':str(log),
                'full_image_created':False,'device_written':False})
        finally:
            if os.path.ismount(mount): subprocess.run(['fusermount3','-u',str(mount)],check=True)
            if process.poll() is None: process.terminate()
            process.wait(timeout=10)
    print('Independent apfsck accepted the composed container; no full image created.')


def hash_range(fd, offset, size):
    h = hashlib.sha256()
    for pos in range(0,size,4*1024*1024):
        n = min(4*1024*1024,size-pos)
        data = os.pread(fd,n,offset+pos)
        if len(data)!=n: raise OSError('short device read')
        h.update(data)
    return h.hexdigest()


def plan_device(args):
    storage.assert_unmounted(args.device)
    with args.device.open('rb') as f:
        sector,size=storage.geometry(f.fileno(),args.sector_size)
        primary,_,parts,_=storage.read_gpt(f.fileno(),sector,size)
        if str(uuid.UUID(bytes_le=primary['raw'][56:72])).upper()!=args.disk_guid.upper():
            raise ValueError('unexpected disk GUID')
        matches=[p for p in parts if p['name']=='ios-apfs' and p['guid']==args.partition_guid.upper()]
        if len(matches)!=1:raise ValueError('expected ios-apfs GPT entry not found')
        part=matches[0];offset=part['first_lba']*sector;length=(part['last_lba']-part['first_lba']+1)*sector
    repair = args.command == 'plan-checkpoints'
    system_only = args.command == 'plan-system-device'
    data_only = args.command == 'plan-data-device'
    operation = 'checkpoint-plan' if repair else ('system-update-plan' if system_only else ('data-update-plan' if data_only else 'update-plan'))
    command=[str(ROOT/'build/apfs/apfs-image'),operation,'--source',str(args.device.resolve()),
        '--source-offset',str(offset),'--source-size',str(length),
        '--output',str(args.output.resolve())]
    if not repair:
        if getattr(args,'overlay',None) is not None: command += ['--overlay',str(args.overlay.resolve())]
        if getattr(args,'development_setup_complete',False): command += ['--development-setup-complete']
        if system_only and args.file_block_patch is not None: command += ['--file-block-patch',str(args.file_block_patch.resolve())]
        if not system_only and not data_only:
            if args.volume_group is not None: command += ['--volume-group', args.volume_group]
            if args.add_volumes is not None: command += ['--add-volumes', str(args.add_volumes.resolve())]
            if args.source_identity is not None: command += ['--source-identity', str(args.source_identity.resolve())]
    subprocess.run(command,check=True)


def apply(args, delta):
    checked = json.loads(args.validation.read_text())
    if checked.get('metadata_only') or not checked.get('apfsck_clean') or checked['apfsck_exit_code'] != 0 or checked['delta_sha256'] != delta.sha or source_identity(checked) != source_identity(delta.report):
        raise ValueError('independent validation is absent or belongs to another delta')
    storage.assert_unmounted(args.device)
    fd = os.open(args.device,os.O_RDWR|os.O_EXCL)
    try:
        sector,size = storage.geometry(fd,args.sector_size)
        primary,backup,parts,_ = storage.read_gpt(fd,sector,size)
        disk_guid = str(uuid.UUID(bytes_le=primary['raw'][56:72])).upper()
        if disk_guid != args.disk_guid.upper(): raise ValueError('unexpected disk GUID')
        matches = [p for p in parts if p['guid']==args.partition_guid.upper() and p['name']=='ios-apfs']
        if len(matches)!=1: raise ValueError('expected ios-apfs GPT entry not found')
        part = matches[0]; offset=part['first_lba']*sector
        if (part['last_lba']-part['first_lba']+1)*sector != delta.size: raise ValueError('partition size mismatch')
        entry=primary['table'][part['index']*128:(part['index']+1)*128]
        if str(uuid.UUID(bytes_le=entry[:16])).upper()!=storage.APFS_GUID: raise ValueError('wrong partition type')
        print('Checking source identity before writing…',flush=True)
        identity_check = check_source(fd, offset, delta, args.same_offline_session)
        old_primary=os.pread(fd,4096,offset)
        if hashlib.sha256(old_primary).hexdigest()!=delta.report['source_superblock_sha256']:
            raise ValueError('source superblock mismatch')
        args.backup.mkdir(parents=True,exist_ok=False)
        sync_dir(args.backup.parent)
        with (args.backup/'primary-before.bin').open('xb') as f:
            f.write(old_primary);f.flush();os.fsync(f.fileno())
        sync_dir(args.backup)
        state={'device':str(args.device),'disk_guid':disk_guid,'partition':part,
            'partition_byte_offset':offset,'delta_sha256':delta.sha,'source_sha256':delta.report.get('source_sha256'),
            'source_identity_kind':source_identity(delta.report)[0],
            'source_structure_sha256':delta.report.get('source_structure_sha256'),
            'source_identity_check':identity_check,
            'primary_before_sha256':hashlib.sha256(old_primary).hexdigest(),
            'primary_after_sha256':hashlib.sha256(delta.raw[32:4128]).hexdigest(),
            'changed_blocks':delta.count,'gpt_changed':False,'commit_started':False,'committed':False}
        write_json(args.backup/'transaction.json',state)
        # These are original free blocks or proven inactive checkpoint/IP slots.
        # Failure here leaves block zero and every old live block untouched.
        for block,data in delta.records():
            if block: storage.write_at(fd,data,offset+block*4096)
        os.fsync(fd)
        for block,data in delta.records():
            if block and os.pread(fd,4096,offset+block*4096)!=data: raise OSError('COW block readback failed before commit')
        if os.pread(fd,4096,offset)!=old_primary: raise OSError('old primary changed before commit')
        state['commit_started']=True;write_json(args.backup/'transaction.json',state)
        storage.write_at(fd,delta.raw[32:4128],offset)
        os.fsync(fd)
        if os.pread(fd,4096,offset)!=delta.raw[32:4128]: raise OSError('primary commit readback failed; use saved primary before any boot')
        p,b,_,_=storage.read_gpt(fd,sector,size)
        if p!=primary or b!=backup: raise OSError('unexpected GPT change')
        state.update(committed=True,changed_blocks_readback_verified=True)
        write_json(args.backup/'transaction.json',state)
        print(f'Committed {delta.count} blocks ({delta.count*4096} bytes); GPT and old file extents retained.',flush=True)
    finally: os.close(fd)


def verify_blocks(args,delta):
    transaction=json.loads((args.backup/'transaction.json').read_text())
    if not transaction.get('committed') or transaction['delta_sha256']!=delta.sha:
        raise ValueError('missing committed transaction for this delta')
    direct=stat.S_ISBLK(args.device.stat().st_mode)
    fd=os.open(args.device,os.O_RDONLY|(os.O_DIRECT if direct else 0))
    try:
        # GPT parsing uses a separate buffered descriptor because its table
        # byte length is not universally aligned for direct I/O.
        with args.device.open('rb') as f:
            sector,size=storage.geometry(f.fileno(),4096 if not direct else None)
            primary,_,parts,_=storage.read_gpt(f.fileno(),sector,size)
        if str(uuid.UUID(bytes_le=primary['raw'][56:72])).upper()!=transaction['disk_guid'] or transaction['partition'] not in parts:
            raise ValueError('transaction target identity changed')
        offset=transaction['partition_byte_offset']
        with mmap.mmap(-1,1024*1024) as buffer:
            for block,data in delta.runs():
                view=memoryview(buffer)[:len(data)]
                try:
                    if os.preadv(fd,[view],offset+block*4096)!=len(data) or view!=data:
                        raise OSError('physical changed-block readback mismatch')
                finally:view.release()
        write_json(args.output,{'delta_sha256':delta.sha,'changed_blocks':delta.count,
            'changed_bytes':delta.count*4096,'all_changed_blocks_verified':True,
            'host_cache_bypassed':direct,'device':str(args.device)})
        print('All changed blocks match the delta; host cache bypass:',direct)
    finally:os.close(fd)


def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='command',required=True)
    v=sub.add_parser('validate');v.add_argument('--delta',type=Path,required=True);v.add_argument('--output',type=Path,required=True)
    v.add_argument('--metadata-only',action='store_true',help='diagnostic metadata pass; omits compressed payload checks and cannot authorize apply')
    d=sub.add_parser('plan-device');d.add_argument('--device',type=Path,required=True);d.add_argument('--overlay',type=Path)
    d.add_argument('--output',type=Path,required=True);d.add_argument('--disk-guid',required=True);d.add_argument('--partition-guid',required=True);d.add_argument('--sector-size',type=int)
    d.add_argument('--volume-group');d.add_argument('--add-volumes',type=Path);d.add_argument('--source-identity',type=Path)
    d.add_argument('--development-setup-complete',action='store_true',help='seed Setup completion preferences in Data')
    s=sub.add_parser('plan-system-device',help='path-copy System files while preserving all other native volume trees')
    s.add_argument('--device',type=Path,required=True);s.add_argument('--overlay',type=Path)
    s.add_argument('--file-block-patch',type=Path,help='same-size file patch descriptor; unchanged extents are retained')
    s.add_argument('--output',type=Path,required=True);s.add_argument('--disk-guid',required=True);s.add_argument('--partition-guid',required=True);s.add_argument('--sector-size',type=int)
    n=sub.add_parser('plan-data-device',help='path-copy Setup completion preferences while preserving other native volume trees')
    n.add_argument('--device',type=Path,required=True);n.add_argument('--development-setup-complete',action='store_true',required=True)
    n.add_argument('--output',type=Path,required=True);n.add_argument('--disk-guid',required=True);n.add_argument('--partition-guid',required=True);n.add_argument('--sector-size',type=int)
    c=sub.add_parser('plan-checkpoints',help='relocate undersized checkpoint rings, preserving all native volume trees')
    c.add_argument('--device',type=Path,required=True);c.add_argument('--output',type=Path,required=True)
    c.add_argument('--disk-guid',required=True);c.add_argument('--partition-guid',required=True);c.add_argument('--sector-size',type=int)
    a=sub.add_parser('apply');a.add_argument('--delta',type=Path,required=True);a.add_argument('--validation',type=Path,required=True)
    a.add_argument('--device',type=Path,required=True);a.add_argument('--disk-guid',required=True);a.add_argument('--partition-guid',required=True)
    a.add_argument('--backup',type=Path,required=True);a.add_argument('--sector-size',type=int)
    b=sub.add_parser('verify-blocks');b.add_argument('--delta',type=Path,required=True);b.add_argument('--device',type=Path,required=True)
    b.add_argument('--backup',type=Path,required=True);b.add_argument('--output',type=Path,required=True)
    for command in (v, a):
        command.add_argument('--same-offline-session', action='store_true',
            help='reuse the plan full-source hash: caller confirms no boot, writes or source replacement since planning')
    args=p.parse_args()
    if args.command in ('plan-device', 'plan-system-device', 'plan-data-device', 'plan-checkpoints'):plan_device(args);return
    delta=Delta(args.delta)
    try:
        if args.command=='validate':validate(args,delta)
        elif args.command=='verify-blocks':verify_blocks(args,delta)
        else:apply(args,delta)
    finally:delta.raw.close();delta.file.close()


if __name__=='__main__':main()
