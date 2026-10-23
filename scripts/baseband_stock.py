"""Read pinned sagit NON-HLOS files with mtools; never mount or execute them."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def monolithic(mdt, fetch):
    """Reconstruct actual ELF file offsets from matching MDT/split segments."""
    if len(mdt) < 52 or mdt[:7] != b'\x7fELF\1\1\1' or struct.unpack_from('<H', mdt, 18)[0] != 164:
        raise ValueError('expected ELF32 Hexagon metadata')
    phoff, = struct.unpack_from('<I', mdt, 28)
    ehsize, phsize, count = struct.unpack_from('<3H', mdt, 40)
    if ehsize != 52 or phsize != 32 or phoff != 52 or not 2 <= count <= 96 or 52 + count * 32 > len(mdt):
        raise ValueError('invalid MDT program headers')
    segments, end = [], 0
    for i in range(count):
        header = struct.unpack_from('<8I', mdt, 52 + i * 32)
        offset, length = header[1], header[4]
        if offset + length > 64 * 1024 * 1024:
            raise ValueError('split firmware exceeds bounded file layout')
        if not length: continue
        data = fetch(i)
        if len(data) != length: raise ValueError('split segment length differs from MDT: ' + str(i))
        for old_offset, old in segments:
            if offset < old_offset + len(old) and old_offset < offset + length:
                raise ValueError('overlapping split firmware ranges')
        segments.append((offset, data)); end = max(end, offset + length)
    first = struct.unpack_from('<8I', mdt, 52)
    second = struct.unpack_from('<8I', mdt, 84)
    if (first[1] or (second[6] >> 24) & 7 != 2 or len(mdt) != first[4] + second[4] or
            fetch(0) != mdt[:first[4]] or fetch(1) != mdt[first[4]:]):
        raise ValueError('MDT header/hash do not match split firmware')
    output = bytearray(end)
    for offset, data in segments: output[offset:offset + len(data)] = data
    return bytes(output)


def prepare(root=ROOT, output=None):
    root = Path(root).resolve()
    pin_path = root / 'docs/baseband/stock-firmware-source.json'
    pin = json.loads(pin_path.read_text())
    archive = root / 'firmware/sagit-stock' / pin['url'].rsplit('/', 1)[-1]
    raw = archive.read_bytes()
    if len(raw) != pin['bytes'] or digest(raw) != pin['sha256']:
        raise ValueError('stock firmware archive differs from pinned extraction')
    output = Path(output or root / 'build/baseband/stock-files').resolve()
    if output.is_relative_to(root / 'firmware') or output == root or output in archive.parents or output == pin_path:
        raise ValueError('output may not overwrite input sources')
    if output.exists():
        encoded = (output / 'manifest.json').read_bytes()
        if digest(encoded) != pin['staged_manifest_sha256']: raise ValueError('stock output manifest differs from pinned extraction')
        report = json.loads(encoded)
        if report['archive_sha256'] != pin['sha256']: raise ValueError('different existing stock output')
        for name, row in report['files'].items():
            data = (output / name).read_bytes()
            if len(data) != row['bytes'] or digest(data) != row['sha256']: raise ValueError('changed staged stock file: ' + name)
        return report
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sagit-stock-', dir=output.parent) as temporary:
        stage = Path(temporary) / 'files'; stage.mkdir()
        source = Path(temporary) / 'NON-HLOS.bin'
        with zipfile.ZipFile(archive) as z:
            member = 'firmware-update/NON-HLOS.bin'
            record = pin['extracted'][member]
            info = z.getinfo(member)
            if info.file_size != record['bytes'] or info.file_size > 256 * 1024 * 1024 or info.flag_bits & 1:
                raise ValueError('unexpected NON-HLOS archive entry')
            firmware = z.read(member)
            if digest(firmware) != record['sha256']: raise ValueError('NON-HLOS differs from its pinned member')
            source.write_bytes(firmware)
        # mtools always addresses this regular input image explicitly. No
        # mount, block device, update-binary or updater-script is executed.
        def read_file(name):
            if not re.fullmatch(r'[a-z0-9_.]+', name): raise ValueError('noncanonical firmware member')
            return subprocess.run(['mtype', '-i', str(source), '::/image/' + name], check=True, capture_output=True).stdout
        subprocess.run(['mcopy', '-i', str(source), '-s', '::/image/modem_pr', str(stage)], check=True, capture_output=True)
        for name in ('mba.mbn', 'modemr.jsn', 'modemuw.jsn'):
            (stage / name).write_bytes(read_file(name))
        mdt = read_file('modem.mdt')
        modem = monolithic(mdt, lambda i: read_file(f'modem.b{i:02d}'))
        (stage / 'modem.mbn').write_bytes(modem)
        (stage / 'modem.mdt').write_bytes(mdt)
        files = {}
        for p in sorted(stage.rglob('*')):
            if p.is_symlink(): raise ValueError('unexpected symbolic link in FAT extraction')
            if not p.is_file(): continue
            name = p.relative_to(stage).as_posix()
            if not re.fullmatch(r'[a-zA-Z0-9_./-]+', name) or any(s in ('.', '..') for s in name.split('/')):
                raise ValueError('noncanonical extracted file path')
            data = p.read_bytes(); files[name] = {'bytes': len(data), 'sha256': digest(data)}
        old = json.loads((root / 'docs/baseband/firmware-sources.json').read_text())
        matches = {name: all(files[name][k] == old['files'][name][k] for k in ('bytes', 'sha256'))
                   for name in ('mba.mbn', 'modem.mbn', 'modemr.jsn', 'modemuw.jsn')}
        remote = {name: row for name, row in files.items() if name.startswith('modem_pr/') or name.endswith('.jsn')}
        declared_sectors = struct.unpack_from('<H', firmware, 19)[0] or struct.unpack_from('<I', firmware, 32)[0]
        report = {'schema': 1, 'status': 'pinned-stock-files-read', 'archive_sha256': pin['sha256'],
            'source_evidence': str(pin_path.relative_to(root)), 'source_kind': pin['source_kind'],
            'original_xiaomi_package_verified': False, 'files': files, 'matches_existing_boot_inputs': matches,
            'remote_file_count': len(remote), 'remote_file_bytes': sum(r['bytes'] for r in remote.values()),
            'maximum_remote_path_bytes': max(len('/readonly/firmware/image/' + p) for p in remote),
            'dynamic_module_so_directory_present': any(p.startswith('modem_pr/so/') for p in remote),
            'fat_declared_bytes': declared_sectors * struct.unpack_from('<H', firmware, 11)[0],
            'non_hlos_bytes': len(firmware), 'absent_tail_was_not_zero_filled': True,
            'files_in_driver_catalog': False, 'hardware_tested': False}
        encoded = (json.dumps(report, indent=2) + '\n').encode()
        if digest(encoded) != pin['staged_manifest_sha256']: raise ValueError('fresh stock manifest differs from pinned extraction')
        (stage / 'manifest.json').write_bytes(encoded)
        stage.rename(output)
        return report


if __name__ == '__main__':
    report = prepare()
    print(json.dumps({k: report[k] for k in ('status', 'matches_existing_boot_inputs', 'remote_file_count',
        'remote_file_bytes', 'maximum_remote_path_bytes', 'dynamic_module_so_directory_present', 'hardware_tested')}, indent=2))
