#!/usr/bin/env python3
"""Finalize corecrypto's module HMAC after relocation and ISA lowering.

The original POST code and algorithm self-tests remain enabled. Only the
expected digest is updated for the final linked executable section.
"""
import argparse
import hashlib
import hmac
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace

CORECRYPTO = 'com.apple.kec.corecrypto'
# Audited provider: module layout, zero-key SHA256 mechanism and hardware POST.
# A new provider requires an independent audit before enabling finalization.
PROVIDER_SHA256 = 'aa65c36c486a8b5cb1d7a2d565d1d6b3cc228b3c172f8c69c5da664ee19832cf'


def need(ok, message):
    if not ok:
        raise ValueError('corecrypto integrity: ' + message)


def digest_region(data, image):
    texts = [s for s in image.sections if s['name'] == '__text'
             and s['segment'] in ('__TEXT', '__TEXT_EXEC')]
    macs = [s for s in image.sections if s['name'] == '__fips_hmacs']
    need(len(texts) == len(macs) == 1, 'expected one __text and one __fips_hmacs section')
    text, mac = texts[0], macs[0]
    need(text['size'] > 0 and text['size'] % 4 == 0, 'invalid executable section size')
    need(mac['segment'] == '__TEXT' and mac['size'] == 32, 'unsupported HMAC layout')
    start = image.offset(text['vm'], text['size'])
    destination = image.offset(mac['vm'], 32)
    need(0 <= start <= len(data) - text['size'] and 0 <= destination <= len(data) - 32,
         'section outside file')
    need(destination + 32 <= start or start + text['size'] <= destination,
         'digest overlaps hashed code')
    value = hmac.digest(bytes(32), data[start:start + text['size']], 'sha256')
    return destination, value, text['size']


def source_module(source):
    data = source.read_bytes()
    need(hashlib.sha256(data).hexdigest() == PROVIDER_SHA256, 'unaudited source provider')
    spec = importlib.util.spec_from_file_location('integrity_inventory',
                                                Path(__file__).with_name('analyze-kernelcache.py'))
    inventory = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inventory)
    metadata, kexts = inventory.inspect(source)
    matches = [k for k in kexts if k['CFBundleIdentifier'] == CORECRYPTO]
    need(len(matches) == 1, 'source module is missing or duplicated')
    def offset(address, size=1):
        for segment in metadata['segments']:
            vm = int(segment['vmaddr'], 16)
            if vm <= address and size <= segment['filesize'] - (address - vm):
                return segment['fileoff'] + address - vm
        raise ValueError('corecrypto integrity: unmapped source section')
    start = offset(matches[0]['_PrelinkExecutableLoadAddr'], 32)
    header = struct.unpack_from('<8I', data, start)
    pos, end = start + 32, start + 32 + header[5]
    need(end <= len(data), 'truncated source commands')
    sections = []
    for _ in range(header[4]):
        need(pos + 8 <= end, 'truncated source command')
        command, length = struct.unpack_from('<II', data, pos)
        need(length >= 8 and length % 8 == 0 and pos + length <= end, 'invalid source command')
        if command == 0x19:
            need(length >= 72, 'truncated source segment')
            segment = struct.unpack_from('<II16sQQQQiiII', data, pos)
            need(length == 72 + segment[9] * 80, 'invalid source section count')
            for i in range(segment[9]):
                sec = struct.unpack_from('<16s16sQQIIIIIIII', data, pos + 72 + i * 80)
                sections.append(dict(name=sec[0].rstrip(b'\0').decode(),
                                     segment=sec[1].rstrip(b'\0').decode(), vm=sec[2], size=sec[3]))
        pos += length
    need(pos == end, 'source command count mismatch')
    destination, value, size = digest_region(data, SimpleNamespace(sections=sections, offset=offset))
    need(hmac.compare_digest(data[destination:destination + 32], value), 'source HMAC mismatch')
    return value, size


def audit(data, images, source=None):
    image = images.get(CORECRYPTO)
    if image is None:
        return {'modules': []}
    destination, value, size = digest_region(data, image)
    need(hmac.compare_digest(data[destination:destination + 32], value), 'HMAC mismatch')
    if source is not None:
        _, original_size = source_module(source)
        need(original_size == size, 'hashed section size differs from source')
    return {'modules': [{'bundle': CORECRYPTO, 'algorithm': 'HMAC-SHA256',
                         'hashed_bytes': size, 'hmac_offset': destination, 'hmac': value.hex()}]}


def update(path, source):
    # Read structure without requiring the stale digest to pass the final gate.
    import armv8_compat
    data, _, images = armv8_compat.context(path)
    if CORECRYPTO not in images:
        return {'modules': []}
    original_value, original_size = source_module(source)
    destination, value, size = digest_region(data, images[CORECRYPTO])
    need(original_size == size, 'hashed section size differs from source')
    old = data[destination:destination + 32]
    need(old in (original_value, value), 'unexpected embedded HMAC before finalization')
    result = bytearray(data)
    result[destination:destination + 32] = value
    report = audit(result, images)
    report['modules'][0].update(previous_hmac=old.hex(), changed=old != value)
    path.write_bytes(result)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    parser.add_argument('--source-kernelcache', type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(update(args.kernelcache, args.source_kernelcache), sort_keys=True))
    except (ValueError, OSError, KeyError, struct.error) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
