"""Build the pinned MKB public-query development policy and sealed block delta."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from apfs_sealed import BLOCK, SealedImage, digest, require
from bridge_trustcache import module_info

ROOT = Path(__file__).resolve().parents[1]
CACHE_PATH = '/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.15'
CACHE_BASE = 0x1aee94000
CACHE_SHA256 = 'ca183cdfd136cd3e09aa302bc333233077c19c27d877bc6a8be2b9c5389bd148'
CAVE = 0x1b59ad160
CAVE_SIZE = 0xea0
HOOKS = {
    'device_lock_state': (0x1b5982484, 0xd10183ff),
    'device_unlocked_since_boot': (0x1b5982ad8, 0xd10143ff),
    'user_unlocked_since_boot': (0x1b5982bec, 0xd10103ff),
}
AKS_CACHE_PATH = '/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.40'
AKS_CACHE_BASE = 0x218754000
AKS_CACHE_SHA256 = 'bedf001c833f614cd4d635979577bdfce7805efa0c69b3958b4280e4d80c8788'
AKS_CAVE = 0x21aea9200
AKS_CAVE_SIZE = 0x200
AKS_NATIVE_STATE = 0x21ae5d148
AKS_HOOKS = {
    'device_state': (0x21ae5eaa8, 0xaa0103e2),
    'extended_device_state': (0x21ae5eab4, 0xaa0103e2),
}


def compile_policy(output, aks=False):
    output = Path(output)
    inputs = {}
    stem = 'keybag_aks_policy' if aks else 'keybag_data_policy'
    cave, capacity = (AKS_CAVE, AKS_CAVE_SIZE) if aks else (CAVE, CAVE_SIZE)
    for suffix in ('S',):
        source = ROOT / f'tools/displayctl/{stem}.{suffix}'
        inputs[str(source.relative_to(ROOT))] = digest(source.read_bytes()).hex()
        subprocess.run(['clang', '--target=aarch64-none-elf', '-march=armv8-a', '-Oz',
                        '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                        '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-mgeneral-regs-only',
                        '-c', str(source), '-o', str(output / f'policy-{suffix}.o')], check=True)
    linker = (('keybag_aks_native_state = ' + hex(AKS_NATIVE_STATE) + '; ' if aks else '') +
              'SECTIONS { . = ' + hex(cave) + '; .text : { *(.text*) *(.rodata*) } '
              '/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n')
    (output / 'policy.ld').write_text(linker + '\n')
    elf = output / 'policy.elf'
    entry = 'keybag_aks_device_state' if aks else 'keybag_device_lock_state'
    subprocess.run(['ld.lld', '--no-relax', '-e', entry,
                    '-T', str(output / 'policy.ld'), str(output / 'policy-S.o'), '-o', str(elf)], check=True)
    subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', str(elf),
                    str(output / 'policy.bin')], check=True)
    with (output / 'policy-disassembly.txt').open('w') as f:
        subprocess.run(['llvm-objdump', '-d', str(elf)], stdout=f, check=True)
    listing = subprocess.check_output(['llvm-nm', '--defined-only', str(elf)], text=True)
    addresses = {row.split()[2]: int(row.split()[0], 16) for row in listing.splitlines() if len(row.split()) == 3}
    require(not subprocess.check_output(['llvm-nm', '--undefined-only', str(elf)]).strip(), 'unresolved policy symbol')
    blob = (output / 'policy.bin').read_bytes()
    require(0 < len(blob) <= capacity, 'policy does not fit verified zero padding')
    return blob, addresses, inputs


def resign_cache(original, modified):
    """Retain the native cache CodeDirectory format, verify and refresh all pages."""
    off, capacity = struct.unpack_from('<QQ', original, 40)
    require(len(original) == len(modified) and off + capacity == len(original), 'cache signature bounds')
    sig = bytearray(original[off:])
    magic, length, count = struct.unpack_from('>3I', sig)
    require(magic == 0xfade0cc0 and length <= capacity and count == 3, 'unsupported cache signature')
    slots = dict(struct.unpack_from('>II', sig, 12 + 8 * i) for i in range(count))
    require(set(slots) == {0, 2, 65536}, 'unsupported cache signing slots')
    at = slots[0]
    cdlen = struct.unpack_from('>I', sig, at + 4)[0]
    cd = bytearray(sig[at:at + cdlen])
    magic, _, version, flags, hashes, ident, special, count, limit = struct.unpack_from('>9I', cd)
    require((magic, version, flags, special, limit) == (0xfade0c02, 0x20400, 2, 2, off),
            'expected native ad-hoc shared-cache CodeDirectory')
    hs, ht, platform, page = struct.unpack_from('4B', cd, 36)
    require((hs, ht, platform, page) == (32, 2, 0, 14) and hashes + count * hs == cdlen,
            'unsupported cache hashing format')
    require(count == (limit + (1 << page) - 1) >> page, 'cache code slot count mismatch')
    req = slots[2]
    reqsize = struct.unpack_from('>I', sig, req + 4)[0]
    require(digest(sig[req:req + reqsize]) == cd[hashes - 64:hashes - 32] and
            cd[hashes - 32:hashes] == bytes(32), 'cache special-slot mismatch')
    require(sig[slots[65536]:slots[65536] + 8] == struct.pack('>II', 0xfade0b01, 8),
            'refuse to retain a nonempty CMS identity')
    old = b''.join(digest(original[p:min(p + (1 << page), limit)]) for p in range(0, limit, 1 << page))
    require(cd[hashes:] == old, 'original cache code-page mismatch')
    name = b'org.darwin.development.shared-cache\0'
    require(len(name) <= hashes - special * hs - ident, 'development signing identifier too long')
    cd[ident:hashes - special * hs] = name.ljust(hashes - special * hs - ident, b'\0')
    cd[hashes:] = b''.join(digest(modified[p:min(p + (1 << page), limit)]) for p in range(0, limit, 1 << page))
    sig[at:at + cdlen] = cd
    return bytes(sig), digest(cd)[:20]


def prepare(source, output):
    source, output = Path(source), Path(output)
    output.mkdir(parents=True, exist_ok=False)
    image = SealedImage(source)
    try:
        cache_records, hooks, inputs, cdhashes = [], [], {}, []
        changed_hashes = 0
        configs = [(CACHE_PATH, CACHE_BASE, CACHE_SHA256, CAVE, CAVE_SIZE, HOOKS, False),
                   (AKS_CACHE_PATH, AKS_CACHE_BASE, AKS_CACHE_SHA256, AKS_CAVE, AKS_CAVE_SIZE, AKS_HOOKS, True)]
        for path, base, source_hash, cave, capacity, entries, aks in configs:
            directory = output / 'aks' if aks else output
            directory.mkdir(exist_ok=True)
            original = image.read_file(path)
            require(digest(original).hex() == source_hash, 'unsupported original cache: expected pinned 22H374')
            blob, symbols, source_inputs = compile_policy(directory, aks=aks)
            inputs.update(source_inputs)
            cave_offset = cave - base
            require(original[cave_offset:cave_offset + capacity] == bytes(capacity), 'padding is not empty')
            patched = bytearray(original)
            patched[cave_offset:cave_offset + len(blob)] = blob
            for name, (addr, before) in entries.items():
                target = symbols[('keybag_aks_' if aks else 'keybag_') + name]
                offset = addr - base
                require(struct.unpack_from('<I', original, offset)[0] == before, 'native entry instruction changed')
                displacement = target - addr
                require(displacement % 4 == 0 and -(1 << 27) <= displacement < (1 << 27), 'branch out of range')
                after = 0x14000000 | ((displacement >> 2) & 0x03ffffff)
                struct.pack_into('<I', patched, offset, after)
                hooks.append({'cache_path': path, 'api': name, 'address': hex(addr), 'target': hex(target),
                              'before': hex(before), 'after': hex(after)})
            signature, cdhash = resign_cache(original, patched)
            sigoff = struct.unpack_from('<Q', original, 40)[0]
            patched[sigoff:] = signature
            for p in range(0, len(original), BLOCK):
                if original[p:p + BLOCK] != patched[p:p + BLOCK]:
                    image.patch_file(path, p, original[p:p + BLOCK], patched[p:p + BLOCK])
            changed_hashes += image.rehash_file(path)
            require(image.read_file(path) == patched, 'derived cache logical readback mismatch')
            verify_sig, verify_hash = resign_cache(bytes(patched), patched)
            require(verify_sig == signature and verify_hash == cdhash, 'derived cache signature mismatch')
            cdhashes.append(cdhash)
            cache_records.append({'path': path, 'source_sha256': source_hash,
                                  'derived_sha256': digest(patched).hex(), 'cdhash': cdhash.hex(),
                                  'payload_bytes': len(blob)})
        root_hash = image.reseal()
        image.verify_seal()
        changed = {p: d for p, d in image.changes.items() if d != image.raw[p * BLOCK:(p + 1) * BLOCK]}
        old_hash, new_hash = hashlib.sha256(), hashlib.sha256()
        for p in range(0, len(image.raw), 1024 * 1024):
            old = image.raw[p:p + 1024 * 1024]
            new = bytearray(old)
            for block in range(p // BLOCK, (p + len(old)) // BLOCK):
                if block in changed:
                    start = block * BLOCK - p
                    new[start:start + BLOCK] = changed[block]
            old_hash.update(old)
            new_hash.update(new)
        records = []
        with (output / 'sealed-blocks.bin').open('xb') as f:
            f.write(struct.pack('<8sQQ', b'APFSPAT1', len(image.raw), len(changed)))
            for block, after in sorted(changed.items()):
                before = image.raw[block * BLOCK:(block + 1) * BLOCK]
                f.write(struct.pack('<Q', block) + before + after)
                records.append({'block': block, 'before_sha256': digest(before).hex(), 'after_sha256': digest(after).hex()})
        hashes = sorted(cdhashes)
        module = struct.pack('<I16sI', 1, digest(b''.join(hashes))[:16], len(hashes)) + b''.join(h + bytes([2, 0]) for h in hashes)
        (output / 'trust-cache.bin').write_bytes(module)
        manifest = {'schema': 1, 'policy': 'development-no-sep-data-availability',
                    'source': str(source.resolve()), 'source_sha256': old_hash.hexdigest(),
                    'derived_sha256': new_hash.hexdigest(), 'image_bytes': len(image.raw),
                    'volume_uuid': image.volume_uuid, 'cache_path': CACHE_PATH,
                    'cache_source_sha256': CACHE_SHA256, 'cache_derived_sha256': cache_records[0]['derived_sha256'],
                    'cdhash': cache_records[0]['cdhash'], 'caches': cache_records,
                    'trust_cache': module_info(module), 'inputs': inputs,
                    'hooks': hooks, 'payload_bytes': sum(c['payload_bytes'] for c in cache_records), 'changed_file_hashes': changed_hashes,
                    'source_root_hash': image.original_root_hash.hex(), 'derived_root_hash': root_hash.hex(),
                    'verified_tree_nodes': len(image.nodes), 'blocks': records,
                    'delta_sha256': digest((output / 'sealed-blocks.bin').read_bytes()).hex(),
                    'apple_authenticated': False, 'installed': False, 'hardware_tested': False}
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        return manifest
    finally:
        image.close()
