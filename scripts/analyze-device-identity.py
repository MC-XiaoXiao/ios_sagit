#!/usr/bin/env python3
"""Verify iOS 18 device classification offline using original AArch64 code."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

from apfs_sealed import SealedImage, require

ROOT = Path(__file__).resolve().parents[1]
CACHE = '/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.09'
REGIONS = {
    'classifier.bin': (0x19cef0394, 0x2a4,
        '1dfe8b99fafa46fd8a0f7af3ee0aa45774ed8b7ba7b54e4019007ec4a1501e8d'),
    'class-strings.bin': (0x19cf54cae, 0x65,
        '67cb381787dc17705b2fc16233091b952e42f440f7cf7f2989008d8886497003'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
        default=ROOT/'build/apfs/firmware/094-33089-038.dmg')
    parser.add_argument('--dtb', type=Path, help='built standard DTB; defaults to compiling sagit DTS')
    parser.add_argument('--output', type=Path, default=ROOT/'build/device-identity')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Invalidate a prior success report before doing any new verification.
    report_path = out/'verification.json'
    report_path.unlink(missing_ok=True)
    image = SealedImage(args.source)
    try:
        header = image.read_file(CACHE, 0, 4096)
        require(header[:16] == b'dyld_v1   arm64\0', 'expected arm64 shared cache')
        offset, count = struct.unpack_from('<II', header, 16)
        require(0 < count <= (4096-offset)//32, 'invalid cache mappings')
        mappings = [struct.unpack_from('<QQQII', header, offset+32*i) for i in range(count)]
        for name, (address, size, expected) in REGIONS.items():
            matches = [(base, fileoff) for base, length, fileoff, _, _ in mappings
                       if base <= address and address+size <= base+length]
            require(len(matches) == 1, 'native code mapping missing or ambiguous')
            base, fileoff = matches[0]
            data = image.read_file(CACHE, fileoff+address-base, size)
            require(hashlib.sha256(data).hexdigest() == expected,
                    'unsupported MobileGestalt bytes: expected pinned 22H374 '+name)
            (out/name).write_bytes(data)
    finally:
        image.close()

    spec = importlib.util.spec_from_file_location('identity_bridge', ROOT/'scripts/build-bridge.py')
    bridge = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bridge)
    dtb = args.dtb.resolve() if args.dtb else out/'xiaomi-sagit.identity.dtb'
    if not args.dtb:
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-i', str(ROOT/'device_trees'),
                        '-o', str(dtb), str(ROOT/'device_trees/xiaomi-sagit.dts')], check=True)
    tree = bridge.parse_fdt(dtb.read_bytes())
    product = tree['props']['model']
    require(product.endswith(b'\0') and b'\0' not in product[:-1] and len(product) <= 63,
            'expected a single XNU product string shorter than 63 bytes')
    require(product.startswith(b'iPhone'), 'candidate product must enter the native iPhone branch')
    # Read model back from the actual Apple DT representation handed to XNU.
    apple = bridge.apple_dt(tree)
    nprops, _ = struct.unpack_from('<II', apple)
    pos, props = 8, {}
    for _ in range(nprops):
        key = apple[pos:pos+32].split(b'\0')[0].decode('ascii')
        length, = struct.unpack_from('<I', apple, pos+32)
        props[key] = apple[pos+36:pos+36+length]
        pos += 36+((length+3)&~3)
    require(props['model'] == product, 'Apple DT changed the product identity')
    (out/'product.bin').write_bytes(product)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-c',
                    str(ROOT/'tests/device_identity_native.S'), '-o', 'native-classification.o'],
                   cwd=out, check=True)
    subprocess.run(['ld.lld', '-T', str(ROOT/'tests/device_identity_native.ld'),
                    'native-classification.o', '-o', 'native-classification.elf'], cwd=out, check=True)
    # Guest RAM covers the unmodified library's original VAs. Only touched
    # pages consume host RAM. No MMIO, device passthrough, or networking is used.
    result = subprocess.run(['qemu-system-aarch64', '-M', 'virt', '-cpu', 'cortex-a57',
        '-m', '8G', '-nographic', '-monitor', 'none', '-serial', 'none', '-nic', 'none',
        '-semihosting-config', 'enable=on,target=native', '-kernel', 'native-classification.elf'],
        cwd=out, timeout=20, capture_output=True)
    log = result.stdout+result.stderr
    (out/'native-classification.log').write_bytes(log)
    require(result.returncode == 0 and b'NATIVE_CLASSIFICATION_OK: 7 cases\n' in log,
            'original classifier failed; see native-classification.log')
    report = {
        'firmware': 'iPad7,11/12 iOS 18.7.10 22H374',
        'source_image': str(args.source.resolve()), 'cache_path': CACHE,
        'native_regions': {name: {'address': hex(address), 'bytes': size, 'sha256': sha}
                           for name, (address, size, sha) in REGIONS.items()},
        'device_tree': str(dtb), 'dtb_sha256': hashlib.sha256(dtb.read_bytes()).hexdigest(),
        'apple_dt_model': product[:-1].decode('ascii'),
        'classification_cases': [
            {'product': value, 'class_number': number} for value, number in [
                ('Xiaomi Mi 6 (sagit)', 2), (product[:-1].decode('ascii'), 1),
                ('iPhone9,1', 1), ('iPad7,11', 3), ('iPod9,1', 2),
                ('Unrecognized', 2), ('iphoneMi6,1', 2)]],
        'native_classifier_code_modified': False,
        'environment_models': ['strncmp', 'disabled logging', 'stack guard',
                               'preinitialized native hw.product once-cache'],
        'public_MGCopyAnswer_executed': False, 'persistent_cache_tested': False,
        'hardware_tested': False, 'device_installed': False, 'status': 'passed',
    }
    report_path.write_text(json.dumps(report, indent=2)+'\n')
    print('Original MobileGestalt classifier: 7 cases passed; '+str(report_path))


if __name__ == '__main__':
    main()
