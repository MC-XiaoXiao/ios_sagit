#!/usr/bin/env python3
"""Independently inspect the V40 Android boot image, firmware contents and isolation."""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import struct
import subprocess
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/edk2-judypn'
REPO = ROOT / 'boot/edk2-sdm845'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def snapshot():
    base = ROOT / 'boot/edk2-msm8998'
    record = {'files': {}, 'heads': {}}
    for repo in [base, base / 'Common/edk2', base / 'Common/edk2-platforms',
                 base / 'GPLDrivers/Library/simple-init']:
        if not (repo / '.git').exists():
            continue
        record['heads'][str(repo.relative_to(ROOT))] = git(repo, 'rev-parse', 'HEAD')
        names = subprocess.check_output(['git', '-C', str(repo), 'ls-files', '-z']).decode().split('\0')
        for name in names:
            p = repo / name
            if name and p.is_file():
                record['files'][str(p.relative_to(ROOT))] = sha(p.read_bytes())
    extra = ['scripts/build-sagit.sh', 'scripts/edk2-host-gcc.sh', 'scripts/edk2-target-gcc.sh',
             'tools/device-profiles/sagit.json', 'build/edk2-sagit/boot-sagit.img',
             'build/edk2-sagit/uefi-sagit.fd']
    extra += [str(p.relative_to(ROOT)) for p in (ROOT / 'tools/patches/edk2-sagit').glob('*.patch')]
    for name in extra:
        p = ROOT / name
        if p.is_file():
            record['files'][name] = sha(p.read_bytes())
    return record


def align(size, unit):
    return (size + unit - 1) // unit * unit


def files_in_fv(data):
    assert data[40:44] == b'_FVH', 'Invalid FV signature'
    length, = struct.unpack_from('<Q', data, 32)
    header_size, = struct.unpack_from('<H', data, 48)
    assert length <= len(data) and header_size >= 56
    offset = align(header_size, 8)
    files = {}
    while offset + 24 <= length and data[offset:offset + 24] != b'\xff' * 24:
        header = data[offset:offset + 24]
        size = int.from_bytes(header[20:23], 'little')
        skip = 24
        if size == 0xffffff:
            size, = struct.unpack_from('<Q', data, offset + 24)
            skip = 32
        assert size >= skip and offset + size <= length, 'Invalid FFS size'
        files[str(uuid.UUID(bytes_le=header[:16]))] = data[offset + skip:offset + size]
        offset = align(offset + size, 8)
    return files


def sections(data):
    offset = 0
    while offset + 4 <= len(data):
        size = int.from_bytes(data[offset:offset + 3], 'little')
        kind = data[offset + 3]
        skip = 4
        if size == 0xffffff:
            size, = struct.unpack_from('<I', data, offset + 4)
            skip = 8
        assert size >= skip and offset + size <= len(data), 'Invalid section size'
        yield kind, data[offset:offset + size], skip
        offset = align(offset + size, 4)


def pe_info(data):
    assert data[:2] == b'MZ', 'Missing EFI PE/COFF image'
    pe, = struct.unpack_from('<I', data, 60)
    assert data[pe:pe + 4] == b'PE\0\0'
    machine, = struct.unpack_from('<H', data, pe + 4)
    subsystem, = struct.unpack_from('<H', data, pe + 24 + 68)
    assert machine == 0xaa64, f'Unexpected EFI architecture: {machine:x}'
    return {'machine': 'AARCH64', 'subsystem': subsystem, 'sha256': sha(data)}


def verify():
    image = (BUILD / 'boot-judypn.img').read_bytes()
    assert image[:8] == b'ANDROID!'
    kernel_size, _, ramdisk_size, _, second_size, _, _, page, version, os_version = struct.unpack_from('<10I', image, 8)
    assert version == 1 and page == 2048 and kernel_size > 0
    recovery_size, recovery_offset, header_size = struct.unpack_from('<IQI', image, 1632)
    assert (recovery_size, recovery_offset, header_size) == (0, 0, 1648)
    cursor = page
    parts = []
    digest = hashlib.sha1()
    for size in (kernel_size, ramdisk_size, second_size, recovery_size):
        part = image[cursor:cursor + size]
        assert len(part) == size
        digest.update(part)
        digest.update(struct.pack('<I', size))
        parts.append(part)
        cursor += align(size, page)
    assert cursor == len(image), 'Truncated or unexpected trailing boot data'
    assert image[576:608] == digest.digest().ljust(32, b'\0'), 'Android boot image ID mismatch'
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    payload = stream.decompress(parts[0]) + stream.flush()
    assert stream.eof, 'Truncated gzip kernel'
    dtb = (REPO / 'Platform/LG/sdm845/FdtBlob_compat/judypn.dtb').read_bytes()
    assert stream.unused_data == dtb, 'Boot image does not contain the pinned V40 DTB'
    assert dtb[:4] == bytes.fromhex('d00dfeed')
    assert struct.unpack_from('>I', dtb, 4)[0] == len(dtb)
    shim = (REPO / 'tools/BootShim/BootShim.bin').read_bytes()
    firmware = (BUILD / 'uefi-judypn.fd').read_bytes()
    assert payload == shim + firmware, 'BootShim/FD payload mismatch'
    assert shim[56:60] == b'ARM\x64' and shim[64:72] == b'EDK2-MSM'
    assert struct.unpack_from('<QQ', shim, 72) == (0xce000000, 0x700000)
    assert len(firmware) == 0x700000
    outer = files_in_fv(firmware)
    nested = outer['9e21fd93-9c72-4c15-8c4b-e77f1db2d792']
    guided = next(raw for kind, raw, skip in sections(nested) if kind == 2)
    assert str(uuid.UUID(bytes_le=guided[4:20])) == 'ee4e5898-3914-4259-9d6e-dc7bd79403cf'
    data_offset, = struct.unpack_from('<H', guided, 20)
    expanded = lzma.decompress(guided[data_offset:])
    fv = next(raw[skip:] for kind, raw, skip in sections(expanded) if kind == 0x17)
    contents = files_in_fv(fv)
    wanted = {'QcomFastboot': 'a78dafb8-467e-4fd0-b117-03c2f8ced2cc',
              'AndroidFastbootApp': '9588502a-5370-11e3-8631-d7c5951364c8'}
    probe_inf = REPO / 'Silicon/Qualcomm/QcomPkg/Drivers/QcomFastboot/TestPayload/Probe.inf'
    wanted['FastbootPayloadProbe'] = next(line.split('=', 1)[1].strip().lower()
                                         for line in probe_inf.read_text().splitlines() if 'FILE_GUID' in line)
    modules = {}
    for name, guid in wanted.items():
        pe = next(raw[skip:] for kind, raw, skip in sections(contents[guid]) if kind == 0x10)
        modules[name] = pe_info(pe)
        if name != 'QcomFastboot':
            assert modules[name]['subsystem'] == 10
        if name == 'AndroidFastbootApp':
            assert b'run-efi\0' in pe and b'crc32\0' in pe
        if name == 'QcomFastboot':
            assert b'edk2-judypn\0' in pe and b'boot_a\0' in pe and b'boot_b\0' in pe
    for name in ('AndroidFastbootApp', 'FastbootPayloadProbe'):
        pe_info((BUILD / (name + '.efi')).read_bytes())
    # Check that this build used the framebuffer library and disabled GENI UART.
    build_log = (BUILD / 'logs/build.log').read_text(errors='replace')
    assert 'USE_UART=0' in build_log
    assert '/QcomGeniSerialPortLib/' not in build_log
    before_path = BUILD / 'mi6-before.json'
    isolation = {'baseline_available': before_path.exists()}
    if before_path.exists():
        before = json.loads(before_path.read_text())
        changed = [name for name, value in before['files'].items()
                   if not (ROOT / name).is_file() or sha((ROOT / name).read_bytes()) != value]
        changed_heads = [name for name, value in before['heads'].items()
                         if git(ROOT / name, 'rev-parse', 'HEAD') != value]
        isolation.update(checked_files=len(before['files']), changed_files=changed,
                         changed_heads=changed_heads, mi6_unchanged=not changed and not changed_heads)
        assert isolation['mi6_unchanged'], f'Mi 6 baseline changed: {changed + changed_heads}'
    for rel in ('Common/edk2', 'Common/edk2-platforms', 'GPLDrivers/Library/SimpleInit'):
        path = REPO / rel
        assert not path.is_symlink() and path.resolve().is_relative_to(REPO)
        git_dir = Path(git(path, 'rev-parse', '--absolute-git-dir'))
        assert not (git_dir / 'objects/info/alternates').exists()
    report = {'success': True, 'device': 'LG V40 ThinQ', 'codename': 'judypn',
              'hardware_tested': False, 'uart_enabled': False,
              'boot_image': {'size': len(image), 'sha256': sha(image), 'header_version': version,
                             'page_size': page, 'kernel_size': kernel_size, 'os_version_field': os_version},
              'firmware': {'size': len(firmware), 'sha256': sha(firmware), 'base': '0xce000000',
                           'ffs_files': len(contents), 'modules': modules},
              'dtb': {'sha256': sha(dtb), 'size': len(dtb)}, 'isolation': isolation}
    (BUILD / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Verified Android boot v1, V40 DTB, BootShim, compressed FV and {len(modules)} AARCH64 modules')
    if isolation.get('mi6_unchanged'):
        print(f'Mi 6 unchanged: {isolation["checked_files"]} source/tool/artifact hashes match')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--snapshot-mi6', action='store_true')
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    if args.snapshot_mi6:
        path = BUILD / 'mi6-before.json'
        if not path.exists():
            path.write_text(json.dumps(snapshot(), indent=2) + '\n')
    else:
        verify()


if __name__ == '__main__':
    main()
