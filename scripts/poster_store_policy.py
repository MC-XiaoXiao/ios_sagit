"""Pinned PosterFoundation compatibility for explicitly unprotected development Data.

The native None resource policy requires a file-protection value which APFS
without content protection does not provide. Retain its readability requirement;
leave directory checks, creation attributes and all protected policies intact.
This derivative must only accompany the project's no-SEP, legacy Data profile.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from apfs_sealed import BLOCK, SealedImage, digest, require
from bridge_trustcache import module_info
from keybag_policy import resign_cache

CACHE = '/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.54'
CACHE_BASE = 0x2484cc000
CACHE_SHA256 = 'e0e2f52373086b612c923a414b9fdb2e81909bcdd1621d3914d78416cb45e278'
ADDRESS = 0x249404eb8  # PFFileProtectionNoneResourceValues dispatch_once block


def prepare(source, output, development_unprotected_data=False):
    require(development_unprotected_data, 'explicit development-unprotected-data policy required')
    source, output = Path(source), Path(output)
    output.mkdir(parents=True, exist_ok=False)
    image = SealedImage(source)
    try:
        original = image.read_file(CACHE)
        require(digest(original).hex() == CACHE_SHA256, 'unsupported original PosterFoundation cache')
        offset = ADDRESS - CACHE_BASE
        require(original[offset:offset+4] == bytes.fromhex('44008052'), 'expected MOV W4,#2')
        patched = bytearray(original)
        patched[offset:offset+4] = bytes.fromhex('24008052')  # MOV W4,#1
        signature, cdhash = resign_cache(original, patched)
        sigoff = struct.unpack_from('<Q', original, 40)[0]
        patched[sigoff:] = signature
        for at in range(0, len(original), BLOCK):
            if original[at:at+BLOCK] != patched[at:at+BLOCK]:
                image.patch_file(CACHE, at, original[at:at+BLOCK], patched[at:at+BLOCK])
        hashes = image.rehash_file(CACHE)
        require(image.read_file(CACHE) == patched, 'cache logical readback mismatch')
        check_sig, check_hash = resign_cache(bytes(patched), patched)
        require(check_sig == signature and check_hash == cdhash, 'derived code signature mismatch')
        root_hash = image.reseal()
        image.verify_seal()
        changed = {p:b for p,b in image.changes.items() if b != image.raw[p*BLOCK:(p+1)*BLOCK]}
        before_hash, after_hash = hashlib.sha256(), hashlib.sha256()
        for at in range(0, len(image.raw), 1024*1024):
            before = image.raw[at:at+1024*1024]; after = bytearray(before)
            for p in range(at//BLOCK, (at+len(before))//BLOCK):
                if p in changed:
                    within = p*BLOCK-at; after[within:within+BLOCK] = changed[p]
            before_hash.update(before); after_hash.update(after)
        records = []
        delta = output/'sealed-blocks.bin'
        with delta.open('xb') as f:
            f.write(struct.pack('<8sQQ', b'APFSPAT1', len(image.raw), len(changed)))
            for p, after in sorted(changed.items()):
                before = image.raw[p*BLOCK:(p+1)*BLOCK]
                f.write(struct.pack('<Q', p)+before+after)
                records.append({'block':p, 'before_sha256':digest(before).hex(), 'after_sha256':digest(after).hex()})
        module = struct.pack('<I16sI',1,digest(cdhash)[:16],1)+cdhash+bytes([2,0])
        (output/'trust-cache.bin').write_bytes(module)
        descriptor = {'path':'usr/standalone/firmware/Cryptex1/SystemOS.dmg',
                      'delta':delta.name, 'source_sha256':before_hash.hexdigest(),
                      'derived_sha256':after_hash.hexdigest()}
        (output/'file-patch.json').write_text(json.dumps(descriptor,indent=2)+'\n')
        preserved = {}
        for suffix in ('15','40'):
            path = CACHE.rsplit('.',1)[0]+'.'+suffix
            preserved[path] = digest(image.read_file(path)).hex()
        manifest = {'schema':1, 'policy':'development-unprotected-poster-store',
                    'required_profile':['development-no-sep','development-legacy-data'],
                    'source':str(source.resolve()), **descriptor,
                    'image_bytes':len(image.raw), 'cache_path':CACHE,
                    'cache_source_sha256':CACHE_SHA256, 'cache_derived_sha256':digest(patched).hex(),
                    'address':hex(ADDRESS), 'before':'MOV W4,#2', 'after':'MOV W4,#1',
                    'retained_resource_requirement':'NSURLIsReadableKey=YES',
                    'omitted_resource_requirement':'NSURLFileProtectionKey=NSURLFileProtectionNone',
                    'creation_attributes_unchanged':True, 'protected_policies_unchanged':True,
                    'preserved_keybag_cache_hashes':preserved,
                    'source_root_hash':image.original_root_hash.hex(), 'derived_root_hash':root_hash.hex(),
                    'changed_file_hashes':hashes, 'blocks':records,
                    'delta_sha256':digest(delta.read_bytes()).hex(), 'trust_cache':module_info(module),
                    'cdhash':cdhash.hex(), 'apple_authenticated':False,
                    'installed':False, 'hardware_tested':False}
        (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        return manifest
    finally:
        image.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--development-unprotected-data',action='store_true')
    args = parser.parse_args()
    result = prepare(args.source,args.output,args.development_unprotected_data)
    print(json.dumps({k:result[k] for k in ('policy','source_sha256','derived_sha256','cdhash')},indent=2))
