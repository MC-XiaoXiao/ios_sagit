"""Read telephony evidence directly from the original iPad sealed Cryptex.

No files inside the image are changed. Split-cache VM addresses remain unslid;
this is an evidence reader, not a Mach-O rebinder or a runnable dylib extractor.
Cache structures follow Apple's dyld_cache_format.h (dyld-1285.19).
"""
import argparse
import ctypes
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import zipfile

from apfs_sealed import SealedImage, BLOCK, require, u64


def digest(data):
    return hashlib.sha256(data).hexdigest()


def cryptex_provenance(root, image, source_hash):
    """Tie the APFS seal to the root-hash IM4P in the chosen IPSW manifest.

    This verifies consistency with the supplied IPSW, not Apple's signature or
    device personalization. The 208-byte cssy payload layout is matched exactly.
    """
    archive = root / 'firmware/iPad_10.2_18.7.10_22H374_Restore.ipsw'
    with zipfile.ZipFile(archive) as firmware:
        raw_manifest = firmware.read('BuildManifest.plist')
        manifest = plistlib.loads(raw_manifest)
        require(manifest['ProductBuildVersion'] == '22H374', 'wrong iPad build')
        candidates = [i['Manifest']['Cryptex1,SystemVolume'] for i in manifest['BuildIdentities']
                      if i['Manifest'].get('Cryptex1,SystemOS', {}).get('Info', {}).get('Path') == '094-33089-038.dmg.aea']
        require(candidates and len({(c['Info']['Path'], c['Digest']) for c in candidates}) == 1,
                'ambiguous Cryptex root-hash identity')
        entry = candidates[0]
        blob = firmware.read(entry['Info']['Path'])
    require(entry['Info']['HashMethod'] == 'sha2-384' and hashlib.sha384(blob).digest() == entry['Digest'],
            'Cryptex root-hash blob differs from BuildManifest')

    def der(data, at):
        require(at + 2 <= len(data), 'short DER')
        tag, length, at = data[at], data[at + 1], at + 2
        if length & 128:
            n = length & 127
            require(1 <= n <= 4 and at + n <= len(data), 'DER length')
            length = int.from_bytes(data[at:at + n], 'big'); at += n
        require(length <= len(data) - at, 'DER bounds')
        return tag, data[at:at + length], at + length

    tag, body, end = der(blob, 0)
    require(tag == 0x30 and end == len(blob), 'expected IM4P sequence')
    values, at = [], 0
    while at < len(body):
        tag, value, at = der(body, at); values.append((tag, value))
    require(len(values) == 4 and values[:3] == [(0x16, b'IM4P'), (0x16, b'cssy'), (0x16, b'0')]
            and values[3][0] == 4, 'unexpected root-hash IM4P')
    payload = values[3][1]
    require(len(payload) == 208 and payload[:16] == struct.pack('<4I', 2, 0, 1, 32),
            'unsupported cssy root-hash format')
    require(payload[16:48] == image.original_root_hash, 'APFS seal differs from IPSW root-hash payload')
    prepared_path = root / 'build/apfs/prepared-firmware.json'
    prepared = json.loads(prepared_path.read_text())
    require(prepared['build'] == '22H374' and
            prepared['components']['Cryptex1,SystemOS']['sha256'] == source_hash,
            'decrypted source differs from the prepared iPad firmware')
    return {'ipsw': str(archive.relative_to(root)), 'build_manifest_sha256': digest(raw_manifest),
            'root_hash_member': entry['Info']['Path'], 'root_hash_im4p_sha384': entry['Digest'].hex(),
            'apfs_seal_sha256': image.original_root_hash.hex(), 'volume_uuid': image.volume_uuid,
            'seal_matches_manifest_payload': True, 'source_matches_prepared_firmware': True,
            'preparation_record_sha256': digest(prepared_path.read_bytes()),
            'apple_signature_verified': False, 'device_personalization_verified': False}


class Files:
    def __init__(self, image, decoder):
        self.image = image
        self.decoder = ctypes.CDLL(str(decoder)).zbm_decompress
        self.decoder.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                 ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
        self.decoder.restype = ctypes.c_int

    def stream(self, owner, offset, size, total):
        require(0 <= offset <= offset + size <= total, 'stream bounds')
        out, covered = bytearray(), 0
        for logical, length, physical in sorted(self.image.extents[owner]):
            require(logical == covered and physical * BLOCK + length <= len(self.image.raw),
                    'sparse or invalid stream')
            covered += length
            a, b = max(offset, logical), min(offset + size, logical + length)
            if a < b:
                start = physical * BLOCK + a - logical
                out += self.image.raw[start:start + b - a]
        require(total <= covered < total + BLOCK and len(out) == size, 'stream coverage')
        return bytes(out)

    @lru_cache(maxsize=128)
    def info(self, path):
        oid = self.image.paths[path]
        attrs = {}
        for _, k, v, _ in self.image.records[oid]:
            if u64(k) >> 60 == 4:
                length, = struct.unpack_from('<H', k, 8)
                require(len(k) == 10 + length and k[-1] == 0 and len(v) >= 4,
                        'xattr bounds')
                flags, size = struct.unpack_from('<HH', v)
                require(size == len(v) - 4, 'xattr size')
                attrs[k[10:-1].decode()] = (flags, v[4:])
        if 'com.apple.decmpfs' not in attrs:
            return {'plain': True, **self.image.file_info(path)}
        flags, header = attrs['com.apple.decmpfs']
        require(flags == 2 and len(header) == 16 and header[:4] == b'fpmc',
                'expected inline decmpfs header')
        method, total = struct.unpack_from('<IQ', header, 4)
        require(method == 14 and 0 < total <= 512 * 1024 * 1024,
                'only bounded LZBITMAP resource forks are supported')
        flags, fork = attrs['com.apple.ResourceFork']
        require(flags == 1 and len(fork) == 48, 'expected external resource fork')
        owner, size = struct.unpack_from('<QQ', fork)
        count = (total + 65535) // 65536
        table = self.stream(owner, 0, (count + 1) * 4, size)
        offsets = struct.unpack('<' + 'I' * (count + 1), table)
        require(offsets[0] == len(table) and offsets[-1] == size, 'compression table coverage')
        require(all(a < b <= size and b - a <= 65537 for a, b in zip(offsets, offsets[1:])),
                'compression chunk bounds')
        return {'plain': False, 'owner': owner, 'size': total, 'fork_size': size, 'offsets': offsets}

    @lru_cache(maxsize=64)
    def chunk(self, path, index):
        info = self.info(path)
        offsets = info['offsets']
        raw = self.stream(info['owner'], offsets[index], offsets[index + 1] - offsets[index],
                          info['fork_size'])
        size = min(65536, info['size'] - index * 65536)
        if raw[0] == 0xff:
            require(len(raw) == size + 1, 'stored chunk size')
            return raw[1:]
        output = ctypes.create_string_buffer(size)
        written = ctypes.c_size_t()
        result = self.decoder(output, size, raw, len(raw), ctypes.byref(written))
        require(result == 0 and written.value == size, 'LZBITMAP decode failed')
        return output.raw

    def read(self, path, offset, size):
        info = self.info(path)
        require(0 <= offset <= offset + size <= info['size'], 'file bounds')
        if info['plain']:
            return self.stream(info['owner'], offset, size, info['size'])
        out = bytearray()
        while len(out) < size:
            at = offset + len(out)
            take = min(size - len(out), 65536 - at % 65536)
            out += self.chunk(path, at // 65536)[at % 65536:at % 65536 + take]
        return bytes(out)


class Cache:
    prefix = '/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64'

    def __init__(self, files):
        self.files, self.maps, self.members = files, [], []
        header = files.read(self.prefix, 0, 552)
        self.add(self.prefix, header[88:104])
        require(header[:16] == b'dyld_v1   arm64\0', 'wrong cache architecture/version')
        suboffset, subcount = struct.unpack_from('<II', header, 392)
        require(subcount <= 128, 'too many subcaches')
        table = files.read(self.prefix, suboffset, subcount * 56)
        for i in range(subcount):
            uuid, _, suffix = struct.unpack_from('<16sQ32s', table, 56 * i)
            require(b'\0' in suffix, 'unterminated subcache suffix')
            suffix = suffix.split(b'\0')[0].decode()
            require(re.fullmatch(r'\.\d+(?:\.dyld(?:data|readonly|linkedit))?', suffix),
                    'unexpected subcache suffix')
            self.add(self.prefix + suffix, uuid)
        self.maps.sort()
        require(all(a[0] + a[1] <= b[0] for a, b in zip(self.maps, self.maps[1:])),
                'overlapping cache mappings')
        imageoffset, imagecount = struct.unpack_from('<II', header, 448)
        require(imagecount <= 20000, 'too many cache images')
        table = files.read(self.prefix, imageoffset, imagecount * 32)
        self.images = {}
        for i in range(imagecount):
            address, _, _, pathoff, _ = struct.unpack_from('<QQQII', table, i * 32)
            raw = files.read(self.prefix, pathoff, min(1024, files.info(self.prefix)['size'] - pathoff))
            require(b'\0' in raw, 'unterminated image path')
            path = raw.split(b'\0')[0].decode()
            require(path not in self.images, 'duplicate image path')
            self.images[path] = address

    def add(self, path, uuid):
        h = self.files.read(path, 0, 552)
        require(h[:16] == b'dyld_v1   arm64\0' and h[88:104] == uuid, 'subcache UUID mismatch')
        offset, count = struct.unpack_from('<II', h, 16)
        require(0 < count < 16, 'invalid cache mappings')
        raw = self.files.read(path, offset, count * 32)
        for i in range(count):
            address, size, fileoff, _, _ = struct.unpack_from('<QQQII', raw, i * 32)
            require(size > 0 and fileoff + size <= self.files.info(path)['size'], 'mapping bounds')
            self.maps.append((address, size, path, fileoff))
        self.members.append({'path': path, 'uuid': uuid.hex(), 'header_sha256': digest(h),
                             'bytes': self.files.info(path)['size']})

    def read(self, address, size):
        require(0 <= size <= 64 * 1024 * 1024, 'VM read size')
        for base, length, path, offset in self.maps:
            if base <= address and address + size <= base + length:
                return self.files.read(path, offset + address - base, size)
        raise ValueError('VM range not in a single cache mapping: ' + hex(address))

    def library(self, path):
        base = self.images[path]
        h = self.read(base, 32)
        magic, cpu, _, _, count, size, _, _ = struct.unpack('<8I', h)
        require(magic == 0xfeedfacf and cpu == 0x100000c and 0 < count <= size // 8,
                'unexpected cached Mach-O')
        data = self.read(base, 32 + size)
        deps, sections, segments, symbols, exports, uuid = [], [], [], None, None, None
        pos = 32
        for _ in range(count):
            cmd, length = struct.unpack_from('<II', data, pos)
            require(length >= 8 and pos + length <= len(data), 'Mach-O command bounds')
            if cmd in (12, 24, 0x80000018, 0x8000001f, 0x80000023):
                require(length >= 24, 'short dylib command')
                off, = struct.unpack_from('<I', data, pos + 8)
                require(24 <= off < length and b'\0' in data[pos + off:pos + length], 'dylib name')
                deps.append(data[pos + off:pos + length].split(b'\0')[0].decode())
            elif cmd == 0x19:
                require(length >= 72, 'short segment')
                name, va, vs, fo, fs, _, _, n, _ = struct.unpack_from('<16s4Q4I', data, pos + 8)
                require(length == 72 + n * 80, 'section command size')
                segments.append((va, vs, fo, fs, name.rstrip(b'\0').decode()))
                for j in range(n):
                    sn, sg, address, sz = struct.unpack_from('<16s16sQQ', data, pos + 72 + j * 80)
                    if sn.rstrip(b'\0') in (b'__cstring', b'__oslogstring'):
                        require(va <= address <= address + sz <= va + vs, 'section VM bounds')
                        sections.append((sg.rstrip(b'\0').decode(), sn.rstrip(b'\0').decode(), address, sz))
            elif cmd == 2:
                require(length == 24 and symbols is None, 'symbol command')
                symbols = struct.unpack_from('<4I', data, pos + 8)
            elif cmd == 0x80000033:
                require(length == 16, 'export command')
                exports = struct.unpack_from('<II', data, pos + 8)
            elif cmd == 0x80000022:
                require(length == 48, 'dyld info command')
                exports = struct.unpack_from('<II', data, pos + 40)
            elif cmd == 0x1b:
                require(length == 24, 'UUID command')
                uuid = data[pos + 8:pos + 24].hex()
            pos += length
        require(pos == len(data), 'command count mismatch')

        def file_read(off, n):
            for va, _, fo, fs, _ in segments:
                if fo <= off <= off + n <= fo + fs:
                    return self.read(va + off - fo, n)
            raise ValueError('Mach-O file range is not mapped')

        imports = []
        if symbols:
            off, n, strings, strlen = symbols
            require(n < 1000000, 'too many symbols')
            entries, names = file_read(off, n * 16), file_read(strings, strlen)
            for i in range(n):
                name, flags, _, _, _ = struct.unpack_from('<IBBHQ', entries, i * 16)
                if flags & 0xe0 or flags & 0x0e or not flags & 1 or not name:
                    continue
                stop = names.find(b'\0', name)
                require(name < strlen and stop >= name, 'symbol name')
                imports.append(names[name:stop].decode())
        exported = export_trie(file_read(*exports), base) if exports and exports[1] else []
        keywords = re.compile(r'(baseband|telephony|qmi|transport|pcie|usb|ioreg|ioservice|io.?connect|'
                              r'io.?call|mhi|ipcrtr|qrtr|smem|hsic|applebb|modem)', re.I)
        selected = []
        for segment, section, address, sz in sections:
            raw = self.read(address, sz)
            values, start = [], 0
            for value in raw.split(b'\0'):
                if len(value) <= 4096:
                    text = value.decode('utf8', errors='replace')
                    if keywords.search(text):
                        values.append({'address': hex(address + start), 'text': text})
                start += len(value) + 1
            selected.append({'segment': segment, 'section': section, 'address': hex(address),
                             'bytes': sz, 'sha256': digest(raw), 'matches': values})
        return {'path': path, 'address': hex(base), 'uuid': uuid,
                'commands_sha256': digest(data), 'dependencies': deps,
                'imports': imports, 'export_count': len(exported),
                'exports': [v for v in exported if keywords.search(v['name'])],
                'string_sections': selected}


def export_trie(data, base):
    result, seen = [], set()

    def uleb(pos, end):
        value = 0
        for shift in range(0, 64, 7):
            require(pos < end, 'truncated ULEB')
            byte = data[pos]; pos += 1
            require(shift != 63 or byte <= 1, 'ULEB overflow')
            value |= (byte & 127) << shift
            if not byte & 128:
                return value, pos
        raise ValueError('ULEB too long')

    def walk(pos, name, depth):
        require(pos not in seen and depth <= 128 and len(name) <= 4096, 'export trie cycle/depth')
        seen.add(pos)
        n, pos = uleb(pos, len(data))
        end = pos + n
        require(end < len(data), 'export terminal bounds')
        if n:
            flags, at = uleb(pos, end)
            entry = {'name': name, 'flags': flags}
            if flags & 8:
                ordinal, at = uleb(at, end)
                require(b'\0' in data[at:end], 'reexport name')
                entry.update(ordinal=ordinal, reexport=data[at:end].split(b'\0')[0].decode())
            else:
                address, at = uleb(at, end)
                entry['address'] = hex(address if flags & 3 == 2 else base + address)
            result.append(entry)
        count = data[end]; pos = end + 1
        for _ in range(count):
            stop = data.find(b'\0', pos)
            require(stop >= pos, 'export edge name')
            edge = data[pos:stop].decode()
            child, pos = uleb(stop + 1, len(data))
            walk(child, name + edge, depth + 1)
    if data:
        walk(0, '', 0)
    return result


def analyze(root):
    source = root / 'build/apfs/firmware/094-33089-038.dmg'
    decoder = root / 'build/baseband/reference/libzbitmap.so'
    decoder.parent.mkdir(parents=True, exist_ok=True)
    csource = root / 'tools/src/apfsprogs/lib/libzbitmap.c'
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-I', str(root / 'tools/src/apfsprogs/include'),
                    str(csource), '-o', str(decoder)], check=True)
    image = SealedImage(source)
    try:
        cache = Cache(Files(image, decoder))
        chosen = ('libTelephonyIOKitDynamic.dylib', 'libTelephonyBasebandDynamic.dylib',
                  'libTelephonyUtilDynamic.dylib', 'libBasebandManager.dylib',
                  'libBasebandManagerICE.dylib', 'libBasebandCommandDriversQMI.dylib',
                  'libBasebandCommandDrivers.dylib', 'libQMIParserDynamic.dylib',
                  'CoreTelephony.framework/CoreTelephony',
                  'AppleBasebandLink.framework/AppleBasebandLink')
        libraries = [cache.library(p) for p in cache.images if p.endswith(chosen)]
        require(len(libraries) == len(chosen), 'runtime library selection incomplete')
        with source.open('rb') as handle:
            sourcehash = hashlib.file_digest(handle, 'sha256').hexdigest()
        report = {'schema': 1, 'build': '22H374', 'product': 'iPad_10.2',
                  'source': str(source.relative_to(root)), 'source_sha256': sourcehash,
                  'input_provenance': cryptex_provenance(root, image, sourcehash),
                  'read_only': True, 'unslid_addresses': True,
                  'dyld_format_reference': 'https://github.com/apple-oss-distributions/dyld/blob/dyld-1285.19/include/mach-o/dyld_cache_format.h',
                  'lzbitmap_decoder_source': str(csource.relative_to(root)),
                  'lzbitmap_decoder_source_sha256': digest(csource.read_bytes()),
                  'cache_members': cache.members, 'libraries': libraries,
                  'limitations': ['Strings and exports are evidence, not a proven Apple control ABI.',
                                  'No code was executed from the firmware or on a device.']}
        target = root / 'docs/baseband/ipad-runtime-libraries.json'
        pending = target.with_suffix('.json.pending')
        pending.write_text(json.dumps(report, indent=2) + '\n')
        pending.replace(target)
        return {'report': str(target), 'cache_members': len(cache.members), 'libraries': len(libraries)}
    finally:
        image.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    print(json.dumps(analyze(args.root), indent=2))
