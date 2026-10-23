"""Validate firmware/development trust caches and encode XNU's iBoot handoff.

The offset-table ABI is defined in bsd/sys/trust_caches.h. IM4P and module
parsing follows the reference project's tools/macos-kernel-collection/trustcache.py.
No executable is modified or re-signed here.
"""
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import uuid
import zipfile
from detached_codesign import verify_data


def der_item(data, pos, tag):
    if pos + 2 > len(data) or data[pos] != tag:
        raise ValueError('unexpected or truncated DER field')
    length = data[pos + 1]; pos += 2
    if length & 128:
        count = length & 127
        if not 0 < count <= 8 or pos + count > len(data) or data[pos] == 0:
            raise ValueError('invalid DER length')
        length = int.from_bytes(data[pos:pos + count], 'big'); pos += count
        if length < 128:
            raise ValueError('non-minimal DER length')
    end = pos + length
    if end > len(data):
        raise ValueError('truncated DER payload')
    return data[pos:end], end


def raw_module(data):
    if data.startswith(b'0'):
        sequence, end = der_item(data, 0, 0x30)
        if end != len(data):
            raise ValueError('trailing IM4P bytes')
        marker, pos = der_item(sequence, 0, 0x16)
        kind, pos = der_item(sequence, pos, 0x16)
        _, pos = der_item(sequence, pos, 0x16)
        data, pos = der_item(sequence, pos, 4)
        # FourCC names vary for OS, restore and Cryptex components. The raw
        # module grammar below, rather than an image-type allowlist, identifies
        # a valid trust cache supplied explicitly by the caller.
        if marker != b'IM4P' or len(kind) != 4 or pos != len(sequence):
            raise ValueError('not a trust-cache IM4P')
    module_info(data)
    return data


def module_info(data):
    if len(data) < 24:
        raise ValueError('truncated trust-cache header')
    version, identifier, count = struct.unpack_from('<I16sI', data)
    width = {0: 20, 1: 22, 2: 24}.get(version)
    if width is None or 24 + count * width != len(data):
        raise ValueError('unsupported or truncated trust-cache module')
    hashes = [data[pos:pos + 20] for pos in range(24, len(data), width)]
    if hashes != sorted(hashes):
        raise ValueError('trust-cache hashes are not sorted')
    return {'version': version, 'uuid': str(uuid.UUID(bytes=identifier)),
            'entries': count, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def handoff(modules):
    if not 1 <= len(modules) <= 64:
        raise ValueError('expected 1..64 trust-cache modules')
    cursor = 4 + 4 * len(modules); offsets = []
    for module in modules:
        module_info(module)
        offsets.append(cursor); cursor += len(module)
    if cursor >= 1 << 32:
        raise ValueError('trust cache exceeds offset ABI')
    return struct.pack('<' + 'I' * (1 + len(offsets)), len(offsets), *offsets) + b''.join(modules)


def firmware_static(report_path):
    report = json.loads(report_path.read_text())
    with zipfile.ZipFile(report['ipsw']) as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
        if (manifest['ProductVersion'], manifest['ProductBuildVersion']) != (report['product_version'], report['build']):
            raise ValueError('firmware identity differs from prepared filesystem')
        identity = next(b for b in manifest['BuildIdentities']
                        if b['Info']['DeviceClass'] == report['board'] and b['Info']['RestoreBehavior'] == 'Erase')
        component = identity['Manifest']['StaticTrustCache']
        data = archive.read(component['Info']['Path'])
        digest = component['Digest']
        algorithm = {20: 'sha1', 32: 'sha256', 48: 'sha384'}.get(len(digest))
        if not algorithm or hashlib.new(algorithm, data).digest() != digest:
            raise ValueError('StaticTrustCache BuildManifest digest mismatch')
    return raw_module(data), {'ipsw': report['ipsw'], 'board': report['board'],
                              'zip_entry': component['Info']['Path'], 'manifest_digest': digest.hex()}


def arm64_slices(data):
    if len(data) < 8:
        raise ValueError('truncated Mach-O')
    magic, count = struct.unpack_from('>II', data)
    if magic not in (0xcafebabe, 0xcafebabf):
        return [data]
    width = 32 if magic == 0xcafebabf else 20
    if 8 + count * width > len(data):
        raise ValueError('truncated FAT table')
    result = []
    for pos in range(8, 8 + count * width, width):
        cpu, = struct.unpack_from('>I', data, pos)
        start, size = struct.unpack_from('>QQ' if width == 32 else '>II', data, pos + 8)
        if start < 8 + count * width or start + size > len(data):
            raise ValueError('FAT slice out of bounds')
        if cpu == 0x100000c:
            result.append(data[start:start + size])
    if not result:
        raise ValueError('no arm64 Mach-O slice')
    return result


def code_directory_entries(data):
    result = set()
    for image in arm64_slices(data):
        if len(image) < 32:
            raise ValueError('truncated Mach-O header')
        magic, cpu, _, _, count, command_bytes = struct.unpack_from('<6I', image)
        if magic != 0xfeedfacf or cpu != 0x100000c or 32 + command_bytes > len(image):
            raise ValueError('expected arm64 Mach-O')
        pos = 32; signature = None
        for _ in range(count):
            if pos + 8 > 32 + command_bytes:
                raise ValueError('truncated load command')
            cmd, size = struct.unpack_from('<II', image, pos)
            if size < 8 or size % 8 or pos + size > 32 + command_bytes:
                raise ValueError('invalid load command')
            if cmd == 0x1d:
                if size != 16 or signature is not None:
                    raise ValueError('invalid LC_CODE_SIGNATURE')
                start, length = struct.unpack_from('<II', image, pos + 8)
                if not start or start + length > len(image):
                    raise ValueError('signature out of bounds')
                signature = image[start:start + length]
            pos += size
        if pos != 32 + command_bytes or signature is None or len(signature) < 12:
            raise ValueError('missing embedded code signature')
        magic, length, count = struct.unpack_from('>III', signature)
        if magic != 0xfade0cc0 or length > len(signature) or 12 + 8 * count > length:
            raise ValueError('invalid signature superblob')
        directories = 0
        for slot in range(count):
            kind, start = struct.unpack_from('>II', signature, 12 + 8 * slot)
            if kind != 0 and not 0x1000 <= kind < 0x1006:
                continue
            if start < 12 + 8 * count or start + 44 > length:
                raise ValueError('CodeDirectory out of bounds')
            magic, size = struct.unpack_from('>II', signature, start)
            hash_type = signature[start + 37]
            algorithm = {1: 'sha1', 2: 'sha256', 3: 'sha256', 4: 'sha384'}.get(hash_type)
            if magic != 0xfade0c02 or size < 44 or start + size > length or algorithm is None:
                raise ValueError('unsupported CodeDirectory')
            ident, = struct.unpack_from('>I', signature, start+20)
            if 0 < ident < size and signature[start+ident:start+size].startswith((b'org.mi6.development.',b'org.darwin.development.')):
                from macho_development_sign import verify_macho
                verify_macho(image)
            result.add((hashlib.new(algorithm, signature[start:start + size]).digest()[:20], hash_type))
            directories += 1
        if not directories:
            raise ValueError('missing CodeDirectory')
    return sorted(result)


def development_module(paths):
    entries = set(); files = []
    for path in paths:
        candidates = sorted(path.rglob('*')) if path.is_dir() else [path]
        for candidate in candidates:
            if candidate.is_symlink() or not candidate.is_file():
                continue
            with candidate.open('rb') as source:
                magic = source.read(4)
            if magic == b'\xfa\xde\x0c\xc0':
                payload = candidate.with_suffix('')
                if candidate.suffix != '.sig' or not payload.is_file() or payload.is_symlink():
                    raise ValueError(f'detached signature needs a regular companion file: {candidate}')
                data = candidate.read_bytes()
                hashes = verify_data(payload.read_bytes(), data)
                entries.update(hashes)
                files.append({'path': str(candidate.resolve()), 'sha256': hashlib.sha256(data).hexdigest(),
                    'payload': str(payload.resolve()), 'payload_sha256': hashlib.sha256(payload.read_bytes()).hexdigest(),
                    'cdhashes': [h.hex() for h, _ in hashes]})
                continue
            if magic not in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
                if not path.is_dir():
                    raise ValueError(f'not a signed Mach-O: {candidate}')
                continue
            data = candidate.read_bytes(); hashes = code_directory_entries(data)
            entries.update(hashes)
            files.append({'path': str(candidate.resolve()), 'sha256': hashlib.sha256(data).hexdigest(),
                          'cdhashes': [h.hex() for h, _ in hashes]})
    if not entries:
        raise ValueError('no signed executables for development trust cache')
    body = b''.join(h + bytes((kind, 0)) for h, kind in sorted(entries))
    identifier = hashlib.sha256(body).digest()[:16]
    return struct.pack('<I16sI', 1, identifier, len(entries)) + body, files
