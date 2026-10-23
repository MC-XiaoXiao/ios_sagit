#!/usr/bin/env python3
"""Plan and install an independently checked APFS image inside the userdata extent.

Input is an entire GPT UFS LUN exported by EDK2's LinuxSimpleMassStorage.
Writes GPT metadata, the new ios-apfs extent and the old userdata signature
area. Remaining userdata is left unformatted; this is a destructive split.
"""
import argparse
import errno
import fcntl
import hashlib
import json
import mmap
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
APFS_GUID = '7C3457EF-0000-11AA-AA11-00306543ECAC'


def sha(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def geometry(fd, requested=None):
    st = os.fstat(fd)
    if stat.S_ISREG(st.st_mode):
        if requested not in (512, 4096):
            raise ValueError('disk images require --sector-size 512 or 4096')
        return requested, st.st_size
    if not stat.S_ISBLK(st.st_mode):
        raise ValueError('expected a regular GPT disk image or a block device')
    size = struct.unpack('<Q', fcntl.ioctl(fd, 0x80081272, bytes(8)))[0]
    sector = struct.unpack('<I', fcntl.ioctl(fd, 0x1268, bytes(4)))[0]
    if sector not in (512, 4096) or requested not in (None, sector):
        raise ValueError('unsupported or mismatched logical sector size')
    return sector, size


def header(fd, sector, lba, total):
    raw = os.pread(fd, sector, sector*lba)
    if len(raw) != sector or raw[:8] != b'EFI PART':
        raise ValueError('missing GPT header; export the entire UFS LUN, not userdata alone')
    rev, length, checksum, reserved = struct.unpack_from('<4I', raw, 8)
    if rev != 0x10000 or not 92 <= length <= sector or reserved:
        raise ValueError('unsupported GPT header')
    check = bytearray(raw[:length]); struct.pack_into('<I', check, 16, 0)
    if zlib.crc32(check) != checksum:
        raise ValueError('GPT header CRC mismatch')
    current, alternate, first, last = struct.unpack_from('<4Q', raw, 24)
    entries_lba, count, stride, crc = struct.unpack_from('<Q3I', raw, 72)
    if current != lba or not 0 < alternate < total or not 1 < first <= last < total-1:
        raise ValueError('invalid GPT geometry')
    if not 1 <= count <= 16384 or stride != 128:
        raise ValueError('unsupported GPT entry geometry')
    size = count*stride
    stop = entries_lba+(size+sector-1)//sector
    if not (1 < entries_lba < stop <= first or last < entries_lba < stop <= total-1):
        raise ValueError('GPT table overlaps usable space')
    table = os.pread(fd, size, entries_lba*sector)
    if len(table) != size or zlib.crc32(table) != crc:
        raise ValueError('GPT table CRC mismatch')
    return {'raw': raw, 'table': table, 'table_lba': entries_lba, 'current': current,
            'alternate': alternate, 'first': first, 'last': last, 'count': count}


def read_gpt(fd, sector, size):
    if size % sector:
        raise ValueError('disk size is not sector aligned')
    total = size//sector
    primary, backup = header(fd, sector, 1, total), header(fd, sector, total-1, total)
    if (primary['alternate'] != total-1 or backup['alternate'] != 1 or
            primary['table'] != backup['table'] or primary['raw'][40:72] != backup['raw'][40:72]):
        raise ValueError('primary and backup GPT disagree')
    partitions, free = [], []
    for i in range(primary['count']):
        entry = primary['table'][i*128:(i+1)*128]
        if entry[:16] == bytes(16):
            free.append(i); continue
        first, last, flags = struct.unpack_from('<3Q', entry, 32)
        if not primary['first'] <= first <= last <= primary['last']:
            raise ValueError('partition outside usable GPT space')
        partitions.append({'index': i, 'first_lba': first, 'last_lba': last,
                           'name': entry[56:128].decode('utf-16-le').rstrip('\0'),
                           'guid': str(uuid.UUID(bytes_le=entry[16:32])).upper()})
    ordered = sorted(partitions, key=lambda p:p['first_lba'])
    if any(a['last_lba'] >= b['first_lba'] for a,b in zip(ordered, ordered[1:])):
        raise ValueError('overlapping GPT partitions')
    return primary, backup, partitions, free


def make_plan(fd, sector, size, image_report):
    primary, backup, parts, free = read_gpt(fd, sector, size)
    users = [p for p in parts if p['name'] == 'userdata']
    if len(users) != 1 or not free or any(p['name'] == 'ios-apfs' for p in parts):
        raise ValueError('need exactly one userdata, a free GPT slot, and no existing ios-apfs')
    original = users[0]
    align = 1024*1024//sector
    count = (image_report['container_bytes']+sector*align-1)//(sector*align)*align
    first = (original['last_lba']+1-count)//align*align
    last = first+count-1
    if first-original['first_lba'] < align or last > original['last_lba']:
        raise ValueError('userdata is too small for the image and a 1 MiB remainder')
    return {'schema': 1, 'sector_size': sector, 'disk_bytes': size,
            'gpt_sha256': hashlib.sha256(primary['raw']+primary['table']+backup['raw']).hexdigest(),
            'disk_guid': str(uuid.UUID(bytes_le=primary['raw'][56:72])).upper(),
            'userdata': original, 'userdata_new_last_lba': first-1,
            'apfs': {'index': free[0], 'first_lba': first, 'last_lba': last,
                     'name': 'ios-apfs', 'type_guid': APFS_GUID, 'guid': str(uuid.uuid4()).upper()},
            'container_bytes': image_report['container_bytes'], 'image_sha256': image_report['image_sha256'],
            'system_uuid': image_report['system_uuid'], 'data_uuid': image_report['data_uuid'],
            'destructive_userdata': True, 'userdata_remainder_formatted': False,
            'hardware_boot_verified': False}


def rewrite_header(raw, table):
    out = bytearray(raw)
    struct.pack_into('<I', out, 88, zlib.crc32(table))
    struct.pack_into('<I', out, 16, 0)
    struct.pack_into('<I', out, 16, zlib.crc32(out[:struct.unpack_from('<I', out, 12)[0]]))
    return out


def write_at(fd, data, offset):
    while data:
        n = os.pwrite(fd, data, offset)
        if not n: raise OSError('short disk write')
        offset += n; data = data[n:]


def assert_unmounted(device):
    # Walk sysfs descendants: catches mounted child partitions and dm holders.
    st = device.stat()
    if not stat.S_ISBLK(st.st_mode): return
    node = Path('/sys/dev/block')/f'{os.major(st.st_rdev)}:{os.minor(st.st_rdev)}'
    base = node.resolve(); devs = {st.st_rdev}
    for p in base.glob('*/dev'):
        major, minor = map(int, p.read_text().strip().split(':')); devs.add(os.makedev(major,minor))
    for d in devs:
        sys = Path('/sys/dev/block')/f'{os.major(d)}:{os.minor(d)}'
        if any((sys/'holders').iterdir()): raise ValueError('device or partition has active holders')
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        major, minor = map(int,line.split()[2].split(':'))
        if os.makedev(major,minor) in devs: raise ValueError('device or child partition is mounted')
    for line in Path('/proc/swaps').read_text().splitlines()[1:]:
        p=Path(line.split()[0])
        if p.exists() and p.stat().st_rdev in devs: raise ValueError('device is active swap')


def direct_readback(device, plan):
    """Hash physical block reads, bypassing the host's buffered-write cache."""
    fd=os.open(device,os.O_RDONLY | os.O_DIRECT)
    buffer=mmap.mmap(-1,4*1024*1024)
    try:
        view=memoryview(buffer);h=hashlib.sha256();offset=plan['apfs']['first_lba']*plan['sector_size']
        remaining=plan['container_bytes'];completed=0
        while remaining:
            length=min(len(buffer),remaining)
            n=os.preadv(fd,[view[:length]],offset)
            if n!=length: raise OSError('short direct readback')
            h.update(view[:n]);offset+=n;remaining-=n;completed+=n
            if completed%(256*1024*1024)==0:
                print(f'Direct readback {completed//(1024*1024)} / {plan["container_bytes"]//(1024*1024)} MiB',flush=True)
        if h.hexdigest()!=plan['image_sha256']: raise ValueError('direct device SHA-256 mismatch')
        return {'device':str(device),'sha256':h.hexdigest(),'bytes':completed,'host_cache_bypassed':True,'passed':True}
    finally:
        if 'view' in locals(): view.release()
        buffer.close();os.close(fd)


def install(device, plan, image, backup_dir, checker):
    if sha(image) != plan['image_sha256'] or image.stat().st_size != plan['container_bytes']:
        raise ValueError('image differs from the reviewed plan')
    print('Checking final APFS image with independent apfsck...',flush=True)
    subprocess.run([str(checker), '-c', '-w', str(image)], check=True)
    print('Independent APFS check passed.',flush=True)
    assert_unmounted(device)
    fd = os.open(device, os.O_RDWR | os.O_EXCL)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        sector,size = geometry(fd,plan['sector_size'])
        primary,backup,parts,_ = read_gpt(fd,sector,size)
        fingerprint=hashlib.sha256(primary['raw']+primary['table']+backup['raw']).hexdigest()
        if fingerprint != plan['gpt_sha256'] or size != plan['disk_bytes']:
            raise ValueError('disk changed since planning')
        # Recompute all boundaries from the current disk rather than trusting edited JSON.
        check = make_plan(fd,sector,size,plan)
        for key in ('userdata','userdata_new_last_lba','container_bytes'):
            if check[key]!=plan[key]: raise ValueError('invalid plan geometry')
        if any(check['apfs'][key]!=plan['apfs'][key] for key in ('index','first_lba','last_lba','name','type_guid')):
            raise ValueError('invalid target partition')
        uuid.UUID(plan['apfs']['guid'])
        backup_dir.mkdir(parents=True,exist_ok=False)
        saved=[]
        for name,lba,data in [('protective-mbr',0,os.pread(fd,sector,0)),('primary-header',1,primary['raw']),
                              ('primary-table',primary['table_lba'],primary['table']),
                              ('backup-table',backup['table_lba'],backup['table']),('backup-header',backup['current'],backup['raw'])]:
            (backup_dir/(name+'.bin')).write_bytes(data);saved.append({'file':name+'.bin','byte_offset':lba*sector,'sha256':hashlib.sha256(data).hexdigest()})
        (backup_dir/'restore.json').write_text(json.dumps({'disk_guid':plan['disk_guid'],'disk_bytes':size,'ranges':saved},indent=2)+'\n')
        for p in backup_dir.iterdir():
            with p.open('rb') as f: os.fsync(f.fileno())
        directory_fd=os.open(backup_dir,os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(directory_fd)
        finally: os.close(directory_fd)
        # Image goes first; a power failure cannot expose a half-written APFS GPT entry.
        start=plan['apfs']['first_lba']*sector
        with image.open('rb') as src:
            offset=start
            while data:=src.read(4*1024*1024):
                write_at(fd,data,offset);offset+=len(data)
                if (offset-start)%(256*1024*1024)==0:
                    print(f'Written {(offset-start)//(1024*1024)} / {plan["container_bytes"]//(1024*1024)} MiB',flush=True)
        os.fsync(fd)
        print('APFS image written and flushed; verifying complete readback...',flush=True)
        h=hashlib.sha256();remaining=plan['container_bytes'];offset=start
        while remaining:
            data=os.pread(fd,min(4*1024*1024,remaining),offset)
            if not data: raise OSError('short readback')
            h.update(data);offset+=len(data);remaining-=len(data)
            if (offset-start)%(256*1024*1024)==0:
                print(f'Read back {(offset-start)//(1024*1024)} / {plan["container_bytes"]//(1024*1024)} MiB',flush=True)
        if h.hexdigest()!=plan['image_sha256']: raise ValueError('APFS image readback mismatch')
        # User explicitly chose to erase userdata. Remove the old ext4/f2fs
        # primary signatures; a smaller GPT entry must not expose the old filesystem.
        write_at(fd,bytes(1024*1024),plan['userdata']['first_lba']*sector)
        os.fsync(fd)
        table=bytearray(primary['table']);u=plan['userdata']['index']*128
        struct.pack_into('<Q',table,u+40,plan['userdata_new_last_lba'])
        a=plan['apfs'];e=bytearray(128);e[:16]=uuid.UUID(APFS_GUID).bytes_le;e[16:32]=uuid.UUID(a['guid']).bytes_le
        struct.pack_into('<3Q',e,32,a['first_lba'],a['last_lba'],0);name='ios-apfs'.encode('utf-16-le');e[56:56+len(name)]=name
        table[a['index']*128:(a['index']+1)*128]=e
        for hdr in (backup,primary):
            write_at(fd,table,hdr['table_lba']*sector);os.fsync(fd)
            write_at(fd,rewrite_header(hdr['raw'],table),hdr['current']*sector);os.fsync(fd)
        read_gpt(fd,sector,size)
        installed=dict(plan)
        installed['image_readback_verified']=True
        installed['userdata_primary_signatures_cleared']=True
        if stat.S_ISBLK(os.fstat(fd).st_mode):
            try: fcntl.ioctl(fd,0x125f)  # BLKRRPART requires CAP_SYS_ADMIN even with a device ACL.
            except OSError as e:
                if e.errno not in (errno.EPERM,errno.EACCES,errno.EBUSY): raise
                installed['kernel_partition_refresh_required']=True
                print('GPT and APFS readback passed; reconnect UAS to refresh host partition nodes.')
        (backup_dir/'installed.json').write_text(json.dumps(installed,indent=2)+'\n')
        print('APFS SHA-256 readback and both GPT copies verified.',flush=True)
    finally: os.close(fd)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('plan');p.add_argument('--device',type=Path,required=True);p.add_argument('--image-report',type=Path,required=True);p.add_argument('--sector-size',type=int,choices=(512,4096));p.add_argument('--output',type=Path,required=True)
    p=sub.add_parser('install');p.add_argument('--device',type=Path,required=True);p.add_argument('--plan',type=Path,required=True);p.add_argument('--image',type=Path,required=True);p.add_argument('--backup',type=Path,required=True);p.add_argument('--erase-userdata',action='store_true',required=True);p.add_argument('--checker',type=Path,default=ROOT/'tools/src/apfsprogs/apfsck/apfsck')
    p=sub.add_parser('verify-device-hash');p.add_argument('--device',type=Path,required=True);p.add_argument('--plan',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.command=='plan':
        with args.device.open('rb') as f:
            sector,size=geometry(f.fileno(),args.sector_size); plan=make_plan(f.fileno(),sector,size,json.loads(args.image_report.read_text()))
        args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(plan,indent=2)+'\n');print(json.dumps(plan,indent=2))
    elif args.command=='install': install(args.device,json.loads(args.plan.read_text()),args.image,args.backup,args.checker)
    else:
        result=direct_readback(args.device,json.loads(args.plan.read_text()))
        args.output.write_text(json.dumps(result,indent=2)+'\n');print('Direct physical readback SHA-256 passed.',flush=True)


if __name__=='__main__':
    try: main()
    except (ValueError,OSError,subprocess.CalledProcessError) as e:
        print(f'apfs-storage: {e}',file=sys.stderr);sys.exit(1)
