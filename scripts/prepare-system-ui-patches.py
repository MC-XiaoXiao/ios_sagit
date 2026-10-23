#!/usr/bin/env python3
"""Prepare the pinned 22H374 ProgressUI resources and OBDevice fallback.

Reads a freshly exported SystemOS image without modifying it. Re-signs the
changed shared-cache member, verifies its code pages, rehashes/reseals the
Cryptex, and emits a same-size APFSPAT1 delta for the existing COW installer.
The resulting engineering signature must accompany the emitted trust cache.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from apfs_sealed import BLOCK, SealedImage, digest, require
from bridge_trustcache import module_info
from keybag_policy import resign_cache

ROOT = Path(__file__).resolve().parents[1]
CACHE = '/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.09'
CACHE_BASE = 0x19add0000
CACHE_SHA256 = '6061adda538cef653e81e88d1e3c12ef855e008d2d209f2f8f87a3bd3bd94807'
FRAMEWORK = 0x19b938000
FRAMEWORK_UUID = 'c0c3892b9f1538c8baed7401cde5b767'
HOOK = 0x19b94f4f4
HOOK_BEFORE = bytes.fromhex('c08b26b0')
CAVE = 0x19b993880
PADDING_START = 0x19b993860
PADDING_END = 0x19b994000
SYMBOLS = {
    'ob_get_type': 0x19b9933c0,
    'ob_get_template': 0x19b992f40,
    'ob_template_for_width': 0x19b94f524,
    'ob_set_template': 0x19b9922e0,
    'ob_home_button_key': 0x1e8ac8648,
    'ob_initializer_resume': HOOK + 4,
}
LOGOS = {
    'apple-logo': 'fd92fa7f1582d54590255a338e5e179873b2cf971ccfada61a06dc136897ba62',
    'apple-logo-black': '7e9be353e5697acaf02e00d4290d613fb8cde1f64cd235034388db1b44bde441',
}


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def compile_fallback(output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'tools/displayctl/onboarding_template_fallback.S'
    linker = '\n'.join(f'{k} = {v:#x};' for k, v in SYMBOLS.items())
    linker += (f'\nSECTIONS {{ . = {CAVE:#x}; .text : {{ *(.text*) }} '
               '/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n')
    (output / 'fallback.ld').write_text(linker)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-march=armv8-a', '-c',
                    str(source), '-o', str(output/'fallback.o')], check=True)
    subprocess.run(['ld.lld', '--no-relax', '-T', str(output/'fallback.ld'),
                    '-e', 'onboarding_template_fallback', str(output/'fallback.o'),
                    '-o', str(output/'fallback.elf')], check=True)
    require(not subprocess.check_output(['llvm-nm', '--undefined-only', str(output/'fallback.elf')]).strip(),
            'unresolved fallback symbol')
    subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text',
                    str(output/'fallback.elf'), str(output/'fallback.bin')], check=True)
    with (output/'fallback-disassembly.txt').open('w') as f:
        subprocess.run(['llvm-objdump', '-d', str(output/'fallback.elf')], stdout=f, check=True)
    blob = (output/'fallback.bin').read_bytes()
    require(0 < len(blob) <= PADDING_END-CAVE, 'fallback exceeds audited padding')
    return blob


def branch_instruction(origin, destination):
    displacement = destination-origin
    require(displacement % 4 == 0 and -(1 << 27) <= displacement < (1 << 27),
            'fallback branch out of range')
    return struct.pack('<I', 0x14000000 | ((displacement >> 2) & 0x3ffffff))


def check_framework(original):
    start = FRAMEWORK-CACHE_BASE
    h = original[start:start+32]
    require(struct.unpack_from('<II', h) == (0xfeedfacf, 0x100000c), 'wrong native framework header')
    pos, count, uuid = start+32, struct.unpack_from('<I', h, 16)[0], None
    text_end = None
    for _ in range(count):
        command, size = struct.unpack_from('<II', original, pos)
        require(size >= 8 and pos+size <= start+32+struct.unpack_from('<I', h, 20)[0],
                'invalid framework load command')
        if command == 0x1b:
            uuid = original[pos+8:pos+24].hex()
        if command == 0x19 and original[pos+8:pos+24].rstrip(b'\0') == b'__TEXT':
            base, length = struct.unpack_from('<QQ', original, pos+24)
            text_end = base+length
        pos += size
    require(uuid == FRAMEWORK_UUID and text_end == PADDING_START, 'framework identity/layout changed')
    require(original[HOOK-CACHE_BASE:HOOK-CACHE_BASE+4] == HOOK_BEFORE, 'initializer hook changed')
    require(original[PADDING_START-CACHE_BASE:PADDING_END-CACHE_BASE] == bytes(PADDING_END-PADDING_START),
            'audited cache padding is occupied')


def prepare(source, output, logo_directory):
    source, output, logo_directory = Path(source), Path(output), Path(logo_directory)
    output.mkdir(parents=True, exist_ok=False)
    blob = compile_fallback(output)
    image = SealedImage(source)
    try:
        original = image.read_file(CACHE)
        require(digest(original).hex() == CACHE_SHA256, 'unsupported or already-modified 22H374 cache .09')
        check_framework(original)
        patched = bytearray(original)
        patched[CAVE-CACHE_BASE:CAVE-CACHE_BASE+len(blob)] = blob
        hook_after = branch_instruction(HOOK, CAVE)
        patched[HOOK-CACHE_BASE:HOOK-CACHE_BASE+4] = hook_after
        signature, cdhash = resign_cache(original, patched)
        signature_offset = struct.unpack_from('<Q', original, 40)[0]
        patched[signature_offset:] = signature
        for at in range(0, len(original), BLOCK):
            if original[at:at+BLOCK] != patched[at:at+BLOCK]:
                image.patch_file(CACHE, at, original[at:at+BLOCK], patched[at:at+BLOCK])
        changed_file_hashes = image.rehash_file(CACHE)
        require(image.read_file(CACHE) == patched, 'derived cache readback mismatch')
        verified_signature, verified_hash = resign_cache(bytes(patched), patched)
        require(verified_signature == signature and verified_hash == cdhash, 'derived code pages do not verify')
        root_hash = image.reseal()
        image.verify_seal()
        changed = {p: data for p, data in image.changes.items()
                   if data != image.raw[p*BLOCK:(p+1)*BLOCK]}
        before_hash, after_hash = hashlib.sha256(), hashlib.sha256()
        for at in range(0, len(image.raw), 1024*1024):
            before = image.raw[at:at+1024*1024]
            after = bytearray(before)
            for p in range(at//BLOCK, (at+len(before))//BLOCK):
                if p in changed:
                    within = p*BLOCK-at
                    after[within:within+BLOCK] = changed[p]
            before_hash.update(before)
            after_hash.update(after)
        records = []
        delta, reverse = output/'sealed-blocks.bin', output/'sealed-blocks-reverse.bin'
        with delta.open('xb') as forward, reverse.open('xb') as backward:
            header = struct.pack('<8sQQ', b'APFSPAT1', len(image.raw), len(changed))
            forward.write(header)
            backward.write(header)
            for p, after in sorted(changed.items()):
                before = image.raw[p*BLOCK:(p+1)*BLOCK]
                forward.write(struct.pack('<Q', p)+before+after)
                backward.write(struct.pack('<Q', p)+after+before)
                records.append({'block': p, 'before_sha256': digest(before).hex(), 'after_sha256': digest(after).hex()})
        descriptor = {'path': 'usr/standalone/firmware/Cryptex1/SystemOS.dmg', 'delta': delta.name,
                      'source_sha256': before_hash.hexdigest(), 'derived_sha256': after_hash.hexdigest()}
        write_json(output/'file-patch.json', descriptor)
        write_json(output/'rollback-file-patch.json', dict(descriptor, delta=reverse.name,
                   source_sha256=after_hash.hexdigest(), derived_sha256=before_hash.hexdigest()))
        module = struct.pack('<I16sI', 1, digest(cdhash)[:16], 1)+cdhash+bytes([2, 0])
        (output/'trust-cache.bin').write_bytes(module)
        resource_records = []
        overlay = output/'overlay/System/Library/PrivateFrameworks/ProgressUI.framework'
        overlay.mkdir(parents=True)
        for stem, expected in LOGOS.items():
            logo = logo_directory/(stem+'@2x~ipad.png')
            raw = logo.read_bytes()
            require(digest(raw).hex() == expected, 'unexpected original ProgressUI resource: '+str(logo))
            require(raw.startswith(b'\x89PNG\r\n\x1a\n'), 'expected native PNG')
            target = overlay/(stem+'@3x~iphone.png')
            target.write_bytes(raw)
            resource_records.append({'source': str(logo.resolve()), 'path': str(target.relative_to(output/'overlay')),
                                     'bytes': len(raw), 'sha256': expected, 'byte_identical_to_native_ipad': True})
        preserved = {}
        for suffix in ('15', '40', '54'):
            path = CACHE.rsplit('.', 1)[0]+'.'+suffix
            preserved[path] = digest(image.read_file(path)).hex()
        manifest = {'schema': 1, 'build': '22H374', 'source': str(source.resolve()), **descriptor,
                    'image_bytes': len(image.raw), 'cache_path': CACHE, 'framework_uuid': FRAMEWORK_UUID,
                    'cache_source_sha256': CACHE_SHA256, 'cache_derived_sha256': digest(patched).hex(),
                    'hook': hex(HOOK), 'hook_before': HOOK_BEFORE.hex(), 'hook_after': hook_after.hex(),
                    'cave': hex(CAVE), 'payload_bytes': len(blob), 'payload_sha256': digest(blob).hex(),
                    'fallback_source_sha256': digest((ROOT/'tools/displayctl/onboarding_template_fallback.S').read_bytes()).hex(),
                    'fallback_condition': 'phone type 1, template 0, finite positive real screen bounds',
                    'fallback_action': 'native templateTypeForBoundsWidth: with min(width,height)',
                    'physical_resolution_scale_and_ppi_unchanged': True,
                    'source_root_hash': image.original_root_hash.hex(), 'derived_root_hash': root_hash.hex(),
                    'changed_file_hashes': changed_file_hashes, 'blocks': records,
                    'delta_sha256': digest(delta.read_bytes()).hex(), 'cdhash': cdhash.hex(),
                    'trust_cache': module_info(module), 'preserved_cache_hashes': preserved,
                    'resources': resource_records, 'apple_authenticated': False,
                    'installed': False, 'hardware_tested': False}
        write_json(output/'manifest.json', manifest)
        return manifest
    finally:
        image.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--logo-directory', type=Path, required=True)
    args = parser.parse_args()
    result = prepare(args.source, args.output, args.logo_directory)
    print(json.dumps({k: result[k] for k in ('source_sha256', 'derived_sha256', 'cdhash', 'payload_bytes')}, indent=2))
