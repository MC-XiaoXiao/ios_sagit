"""Native userspace service patching and display policy."""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import struct
from project.build import context as build_state


def patch_no_sep_service_readiness(image):
    """Use native port death instead of retaining requests for a dead server.

    No FairPlay request, key, result or daemon code is replaced. KeepAlive
    still starts the original job; exit invalidates its old Mach endpoints.
    """
    expected = '75d3014dcecf67a9129240a469d08d202cecede2ba66704bbf09c8b4c5ab0fae'
    if hashlib.sha256(image).hexdigest() != expected:
        raise ValueError('native FairPlay launchd reference hash mismatch')
    job = plistlib.loads(image)
    endpoints = ('com.apple.fairplayd.versioned', 'com.apple.fairplayd')
    if (job.get('Label') != 'com.apple.fairplayd.A2' or job.get('Program') != '/usr/sbin/fairplayd.A2'
            or job.get('KeepAlive') is not True
            or any(job.get('MachServices', {}).get(name) is not True for name in endpoints)
            or job.get('CFBundleInfoDictionaryVersion') != '6.0'):
        raise ValueError('unsupported native FairPlay launchd configuration')
    # This descriptive bundle-format key is not a launchd job setting. Omit
    # it to retain the file's existing logical size and APFS allocation.
    del job['CFBundleInfoDictionaryVersion']
    readiness = {'ResetAtClose': True}
    for name in endpoints:
        job['MachServices'][name] = readiness
    encoded = plistlib.dumps(job, fmt=plistlib.FMT_BINARY, sort_keys=False)
    padding = len(image) - len(encoded)
    if padding < 0:
        raise ValueError('readiness plist exceeds original allocation')
    # Padding before the offset table leaves every object/reference intact;
    # the trailer remains the final32 bytes, as required by the native parser.
    table, = struct.unpack_from('>Q', encoded, len(encoded)-8)
    result = bytearray(encoded[:table] + bytes(padding) + encoded[table:])
    struct.pack_into('>Q', result, len(result)-8, table+padding)
    if len(result) != len(image) or plistlib.loads(result) != job:
        raise ValueError('readiness plist failed allocation-preserving round trip')
    return bytes(result)


def patch_launchd_cache_readiness(data):
    """Reuse a native binary-plist readiness object; change only two references.

    iOS18 boots from the cached LaunchDaemons dictionary. Updating an individual
    job plist alone does not change that cached configuration.
    """
    if len(data) < 40 or len(data) > 16*1024*1024 or data[:8] != b'bplist00':
        raise ValueError('unsupported launchd cache format')
    width, refs, count, top, table = struct.unpack('>6xBBQQQ', data[-32:])
    if width not in (1,2,4,8) or refs not in (1,2,4,8) or not 0 <= top < count or table+count*width != len(data)-32:
        raise ValueError('invalid launchd cache trailer')
    offsets = [int.from_bytes(data[table+i*width:table+(i+1)*width], 'big') for i in range(count)]
    if any(not 8 <= offset < table for offset in offsets):
        raise ValueError('invalid launchd cache object offset')
    def header(reference):
        if not 0 <= reference < count: raise ValueError('invalid launchd cache reference')
        pos = offsets[reference]; kind, length = data[pos] >> 4, data[pos] & 15; pos += 1
        if length == 15:
            if pos >= table or data[pos] >> 4 != 1 or data[pos] & 15 > 3:
                raise ValueError('invalid launchd cache object length')
            size = 1 << (data[pos] & 15); pos += 1
            if pos+size > table: raise ValueError('truncated launchd cache object length')
            length = int.from_bytes(data[pos:pos+size], 'big'); pos += size
        return kind, pos, length
    def string(reference):
        kind, pos, length = header(reference)
        size = length * (2 if kind == 6 else 1)
        if kind not in (5,6) or pos+size > table:
            raise ValueError('invalid launchd cache dictionary key')
        return data[pos:pos+size].decode('utf-16-be' if kind == 6 else 'ascii')
    def items(reference):
        kind, pos, length = header(reference)
        if kind != 13 or length > count or pos+2*length*refs > table:
            raise ValueError('invalid launchd cache dictionary')
        keys = [int.from_bytes(data[pos+i*refs:pos+(i+1)*refs], 'big') for i in range(length)]
        pos += length*refs
        result = [(string(key), int.from_bytes(data[pos+i*refs:pos+(i+1)*refs], 'big'), pos+i*refs)
                  for i,key in enumerate(keys)]
        if len({name for name,_,_ in result}) != length:
            raise ValueError('duplicate launchd cache key')
        return result
    selected = top
    for name in ('LaunchDaemons', '/System/Library/LaunchDaemons/com.apple.fairplayd.A2.plist', 'MachServices'):
        matches = [entry for entry in items(selected) if entry[0] == name]
        if len(matches) != 1: raise ValueError('missing native FairPlay cached job')
        selected = matches[0][1]
    choices = []
    for index,offset in enumerate(offsets):
        if data[offset] >> 4 != 13: continue
        entries = items(index)
        if len(entries) == 1 and entries[0][0] == 'ResetAtClose':
            value = entries[0][1]
            if value < count and data[offsets[value]] == 9: choices.append(index)
    if not choices: raise ValueError('no reusable native ResetAtClose dictionary')
    original = plistlib.loads(data)
    job = original['LaunchDaemons']['/System/Library/LaunchDaemons/com.apple.fairplayd.A2.plist']
    if job.get('Program') != '/usr/sbin/fairplayd.A2' or job.get('Label') != 'com.apple.fairplayd.A2' or job.get('KeepAlive') is not True:
        raise ValueError('unsupported native cached FairPlay job')
    result, patches = bytearray(data), []
    for name in ('com.apple.fairplayd.versioned', 'com.apple.fairplayd'):
        entry = next((entry for entry in items(selected) if entry[0] == name), None)
        if entry is None or job['MachServices'].get(name) is not True:
            raise ValueError('unexpected cached FairPlay endpoint policy')
        _,reference,pos = entry
        if reference >= count or data[offsets[reference]] != 9:
            raise ValueError('cached FairPlay endpoint is not a boolean')
        replacement = choices[0].to_bytes(refs, 'big')
        patches.append({'endpoint': name, 'offset': pos, 'before': data[pos:pos+refs].hex(), 'after': replacement.hex()})
        result[pos:pos+refs] = replacement
        job['MachServices'][name] = {'ResetAtClose': True}
    if plistlib.loads(result) != original or len(result) != len(data):
        raise ValueError('cached readiness policy changed unrelated configuration')
    return bytes(result), patches


def prepare_no_sep_service_readiness(source, output, resource_fork=None, compression_attribute=None,
                                     cache=None, cache_signature=None):
    from project.build import native as _component_native
    original = source.read_bytes()
    patched = patch_no_sep_service_readiness(original)
    relative = Path('System/Library/LaunchDaemons/com.apple.fairplayd.A2.plist')
    destination = output/'overlay'/relative
    if destination.exists() and destination.read_bytes() != patched:
        raise ValueError('refusing to replace a different prepared service policy')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(patched)
    report = {'schema_version': 1, 'runtime_build': '22H374', 'development_only': True,
        'installed': False, 'hardware_tested': False, 'source': str(source.resolve()),
        'source_sha256': hashlib.sha256(original).hexdigest(), 'path': str(destination.resolve()),
        'overlay': str((output/'overlay').resolve()), 'bytes': len(patched),
        'sha256': hashlib.sha256(patched).hexdigest(), 'policy': 'native-mach-reset-at-close',
        'endpoints': ['com.apple.fairplayd.versioned', 'com.apple.fairplayd'],
        'removed_descriptive_metadata': ['CFBundleInfoDictionaryVersion'],
        'daemon_code_modified': False, 'crypto_success_fabricated': False,
        'setup_completion_written': False,
        'reference': 'https://github.com/apple-oss-distributions/launchd/blob/main/man/launchd.plist.5',
        'inputs': {str(build_state.SELF.relative_to(build_state.ROOT)): _component_native.digest(build_state.SELF)}}
    if resource_fork is not None:
        fork, chunks = replace_zlib_resource_chunks(resource_fork.read_bytes(),
            compression_attribute.read_bytes(), original, patched)
        (output/'resource-fork.bin').write_bytes(fork)
        report['resource_fork'] = {'path': str((output/'resource-fork.bin').resolve()),
            'bytes': len(fork), 'sha256': hashlib.sha256(fork).hexdigest(), 'changed_chunks': chunks}
    if bool(cache) != bool(cache_signature):
        raise ValueError('both launchd cache and detached signature are required')
    if cache is not None:
        from project.firmware.codesign import HEADER, sign_data, verify_data
        original_cache, original_signature = cache.read_bytes(), cache_signature.read_bytes()
        declared, = struct.unpack_from('>I', original_signature, 4)
        if declared > len(original_signature) or any(original_signature[declared:]):
            raise ValueError('unsupported launchd cache signature padding')
        verify_data(original_cache, original_signature[:declared])
        modified, patches = patch_launchd_cache_readiness(original_cache)
        fields = HEADER.unpack_from(original_signature, 20)
        identifier = original_signature[20+fields[5]:20+fields[4]-1].decode('utf8')
        signature = sign_data(modified, identifier)
        entries = verify_data(modified, signature)
        if len(signature) != declared: raise ValueError('launchd signature allocation changed')
        signature += bytes(len(original_signature)-len(signature))
        for relative,payload in [('System/Library/xpc/launchd.plist', modified),
                                 ('System/Library/xpc/launchd.plist.sig', signature)]:
            target = output/'overlay'/relative; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(payload)
        module = struct.pack('<I16sI', 1, hashlib.sha256(signature).digest()[:16], len(entries))
        module += b''.join(cdhash+bytes((kind,0)) for cdhash,kind in entries)
        (output/'trust-cache.bin').write_bytes(module)
        report['launchd_cache'] = {'source': str(cache.resolve()), 'source_sha256': _component_native.digest(cache),
            'sha256': hashlib.sha256(modified).hexdigest(), 'bytes': len(modified), 'patches': patches,
            'signature_source_sha256': _component_native.digest(cache_signature), 'signature_sha256': hashlib.sha256(signature).hexdigest(),
            'signature_bytes': len(signature), 'cdhash': entries[0][0].hex(),
            'signature_identity_preserved': True, 'trust_cache': str((output/'trust-cache.bin').resolve())}
    (output/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def patch_migration_unlock(image):
    """Skip only negative MKB lock-state results in the pinned native service.

    Preserve stash-load and lock-state calls, their errors, normal state0/3
    handling and positive locked states. No keybag result is rewritten.
    """
    expected = '4275137397d7df5d4c0f46d90eb5141ed3bb7f6759c41cc5c5ca019d5f14ee92'
    if hashlib.sha256(image).hexdigest() != expected:
        raise ValueError('native datamigrator reference hash mismatch')
    original = bytes.fromhex('750800341f0c00712008005400080034')
    if image[0xd038:0xd048] != original:
        raise ValueError('native datamigrator unlock branch mismatch')
    patched = bytearray(image)
    # cmp w0,#3; ccmp w0,#0,#4,ne; b.le original_epilogue.
    # If state==3, CCMP supplies Z=1; otherwise compare signed state to0.
    patched[0xd040:0xd048] = bytes.fromhex('0418407a0d080054')
    return bytes(patched)


def replace_zlib_resource_chunks(fork, attribute, original, replacement):
    """Keep native fork allocation, chunk offsets and Resource Manager map intact.

    Only changed logical chunks are encoded again. Refuse any that exceed their
    original compressed slot; padding is outside the descriptor's exact length.
    """
    import zlib
    if (len(attribute) != 16 or struct.unpack('<4sIQ', attribute) !=
            (b'fpmc', 4, len(original)) or len(original) != len(replacement) or not original):
        raise ValueError('unsupported compression attribute or logical allocation')
    if len(fork) < 264:
        raise ValueError('truncated resource fork')
    base, map_offset, data_size, map_size = struct.unpack_from('>4I', fork)
    count = (len(original) + 65535) // 65536
    if (base != 256 or map_size != 50 or map_offset + map_size != len(fork) or
            base + data_size != map_offset or 264 + count * 8 > map_offset or
            struct.unpack_from('>I', fork, base)[0] != data_size - 4 or
            struct.unpack_from('<I', fork, base + 4)[0] != count):
        raise ValueError('unsupported resource fork layout')

    def decode(compressed, length):
        if compressed[:1] == b'\xff':
            result = compressed[1:]
        else:
            if (len(compressed) < 3 or compressed[0] != 0x78 or compressed[1] & 0x20 or
                    int.from_bytes(compressed[:2], 'big') % 31):
                raise ValueError('invalid zlib chunk header')
            decoder = zlib.decompressobj(-15)
            result = decoder.decompress(compressed[2:], length + 1)
            if not decoder.eof or decoder.unconsumed_tail:
                raise ValueError('invalid or oversized deflate chunk')
        if len(result) != length:
            raise ValueError('decoded chunk size mismatch')
        return result

    result, changed, previous = bytearray(fork), [], 264 + count * 8
    for index in range(count):
        descriptor = 264 + index * 8
        relative, size = struct.unpack_from('<2I', fork, descriptor)
        start, end = 260 + relative, 260 + relative + size
        if size == 0 or start < previous or end > map_offset:
            raise ValueError('invalid or overlapping compressed chunk range')
        previous = end
        before, after = original[index * 65536:(index + 1) * 65536], replacement[index * 65536:(index + 1) * 65536]
        if decode(fork[start:end], len(before)) != before:
            raise ValueError('resource fork does not match the original service')
        if before == after:
            continue
        # decmpfs zlib chunks omit the Adler-32 trailer; native descriptors
        # supply the exact compressed length independently of the next offset.
        encoded = zlib.compress(after, 9)[:-4]
        if len(encoded) > size:
            raise ValueError('replacement compressed chunk exceeds its original allocation')
        result[start:end] = encoded + bytes(size - len(encoded))
        struct.pack_into('<I', result, descriptor + 4, len(encoded))
        if decode(encoded, len(after)) != after:
            raise ValueError('replacement chunk failed round trip')
        changed.append({'index': index, 'offset': start, 'old_bytes': size, 'new_bytes': len(encoded)})
    return bytes(result), changed


def prepare_display_services(source, output, resource_fork=None, compression_attribute=None):
    from project.build import native as _component_native
    from project.firmware.macho import resign_macho, signature_location, signature_blobs
    original = source.read_bytes()
    patched = patch_migration_unlock(original)
    signed, signature = resign_macho(patched, 'org.darwin.development.datamigrator')
    off, _, _, _ = signature_location(original)
    before, _ = signature_blobs(original[off:])
    after, _ = signature_blobs(signed[off:])
    if len(signed) != len(original) or any(before[k] != after[k] for k in (2,5,7)):
        raise ValueError('native service allocation or preserved policy changed')
    if signed[:0xd040] != original[:0xd040] or signed[0xd048:off] != original[0xd048:off]:
        raise ValueError('unrelated native service code changed')
    relative = Path('System/Library/PrivateFrameworks/DataMigration.framework/XPCServices/com.apple.datamigrator.xpc/com.apple.datamigrator')
    destination = output / 'overlay' / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    report = {'schema_version': 1, 'runtime_build': '22H374', 'development_only': True,
        'installed': False, 'hardware_tested': False, 'source': str(source.resolve()),
        'source_sha256': hashlib.sha256(original).hexdigest(), 'path': str(destination.resolve()),
        'overlay': str((output/'overlay').resolve()),
        'bytes': len(signed), 'sha256': hashlib.sha256(signed).hexdigest(), 'signature': signature,
        'code_patch': {'vm_address': '0x10000d040', 'file_offset': 0xd040,
            'before': original[0xd040:0xd048].hex(), 'after': signed[0xd040:0xd048].hex(),
            'effect': 'Skip PreBoard unlock wait only when original MKB lock-state query is negative; preserve normal0/3 and positive locked states'},
        'keybag_operations_implemented': False, 'keybag_success_fabricated': False,
        'migration_plugins_skipped': False, 'setup_completion_written': False,
        'inputs': {str(path.relative_to(build_state.ROOT)): _component_native.digest(path) for path in
            (build_state.SELF, build_state.ROOT/'tools/project/firmware/macho.py')}}
    if destination.exists() and destination.read_bytes() != signed:
        raise ValueError('refusing to replace a different prepared service')
    destination.write_bytes(signed)
    destination.chmod(0o755)
    if resource_fork is not None:
        original_fork = resource_fork.read_bytes()
        attribute = compression_attribute.read_bytes()
        fork, chunks = replace_zlib_resource_chunks(original_fork, attribute, original, signed)
        fork_path = output / 'resource-fork.bin'
        fork_path.write_bytes(fork)
        report['resource_fork'] = {'path': str(fork_path.resolve()), 'bytes': len(fork),
            'source_sha256': hashlib.sha256(original_fork).hexdigest(),
            'sha256': hashlib.sha256(fork).hexdigest(), 'changed_chunks': chunks,
            'decmpfs_sha256': hashlib.sha256(attribute).hexdigest(),
            'allocation_changed': False, 'filesystem_metadata_changed': False,
            'chunk_offsets_changed': False, 'resource_map_changed': False}
    (output/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def patch_system_app_install_wait(image):
    """Fail missing-app placeholders promptly and retain native install timeout."""
    if hashlib.sha256(image).hexdigest() != '36bedfaefa65bf10b6e166a8feef36669246a3a8008756d4e140ebc781585515':
        raise ValueError('native system-app migrator reference hash mismatch')
    if (image[0x4890:0x48a4].hex() != '0808008b08f17dd3691640f9290540f9280d00f9' or
            image[0x7638:0x7640].hex() != 'ff8302d1fc6f04a9'):
        raise ValueError('native system-app migration deadline calculation mismatch')
    patched = bytearray(image)
    # Preserve the original count-dependent dispatch_group_wait deadline.
    # A zero deadline lets migration finish while native installs are pending.
    # Return false before creating an install coordinator or localized on-disk
    # placeholder. The native caller logs failure and retains the missing app
    # and App Store request; this does not report an installed application.
    patched[0x7638:0x7640] = bytes.fromhex('00008052c0035fd6')
    return bytes(patched)


def patch_springboard_snapshot_wait(image):
    """Defer launch-image discovery and capture through the native defaults path."""
    if hashlib.sha256(image).hexdigest() != 'd3fe19db1b7a97ab9d5299d18fd9903865094ca159ec2cbb111b905516afed10':
        raise ValueError('native SpringBoard migrator reference hash mismatch')
    if (image[0xb418:0xb420].hex() != 'ff4301d1f65702a9' or
            image[0xb578:0xb588].hex() != 'f40300aa130440f90a090094f50300aa' or
            image[0xb618:0xb624].hex() != 'e10300aae00314aa61080094'):
        raise ValueError('native SpringBoard launch-image path mismatch')
    patched = bytearray(image)
    # The launch-image-only discovery method returns nil without entering UIKit
    # application queries. Its caller retains nil and continues other migration.
    patched[0xb418:0xb420] = bytes.fromhex('000080d2c0035fd6')
    # Keep the capture method's real prologue/epilogue. Zero only the three
    # objects released on exit (x19,x20,SP+0x28), then branch to its existing
    # NSUserDefaults code at0xb624. That code requests post-migration capture.
    patched[0xb578:0xb588] = bytes.fromhex('140080d2130080d2ff1700f928000014')
    return bytes(patched)


def prepare_system_app_migrator(source, output, resource_fork=None, compression_attribute=None):
    return prepare_native_migration_bundle(source, output, patch_system_app_install_wait,
        'systemappmigrator', 'SystemAppMigrator.migrator/SystemAppMigrator', [(0x7638, 8)],
        'Fail missing-app placeholder materialization; retain the native install-group deadline and error paths',
        {'app_install_success_fabricated': False, 'native_install_wait_preserved': True,
         'missing_app_placeholder_materialization_skipped': True},
        resource_fork, compression_attribute)


def prepare_springboard_migrator(source, output, resource_fork=None, compression_attribute=None):
    return prepare_native_migration_bundle(source, output, patch_springboard_snapshot_wait,
        'springboardmigrator', 'SpringBoard.migrator/SpringBoard', [(0xb418, 8), (0xb578, 16)],
        'Defer launch-image discovery and capture; execute the original post-migration capture request',
        {'snapshot_success_fabricated': False, 'launch_images_deferred': True,
         'original_snapshot_timeout_seconds': 300, 'post_migration_capture_flag_retained': True},
        resource_fork, compression_attribute)


def prepare_native_migration_bundle(source, output, patch, identifier, relative, patches, effect,
                                    properties, resource_fork=None, compression_attribute=None):
    from project.build import native as _component_native
    from project.firmware.macho import resign_macho, signature_location, signature_blobs
    original = source.read_bytes()
    edited = patch(original)
    signed, signature = resign_macho(edited, 'org.darwin.development.' + identifier)
    off, _, _, _ = signature_location(original, allow_bundle=True)
    before, _ = signature_blobs(original[off:]); after, _ = signature_blobs(signed[off:])
    cursor = 0
    for offset, size in patches:
        if (not cursor <= offset < offset + size <= off or
                signed[cursor:offset] != original[cursor:offset] or
                signed[offset:offset+size] != edited[offset:offset+size]):
            raise ValueError('unrelated native migration bundle code changed')
        cursor = offset + size
    if (len(signed) != len(original) or before[2] != after[2] or
            set(after) != {0, 2} or signed[cursor:off] != original[cursor:off]):
        raise ValueError('unrelated native migration bundle code or signing policy changed')
    destination = output/'overlay/System/Library/DataClassMigrators'/relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and destination.read_bytes() != signed:
        raise ValueError('refusing to replace a different prepared migration bundle')
    destination.write_bytes(signed); destination.chmod(0o755)
    report = {'schema_version': 1, 'runtime_build': '22H374', 'development_only': True,
        'installed': False, 'hardware_tested': False, 'source': str(source.resolve()),
        'source_sha256': hashlib.sha256(original).hexdigest(), 'path': str(destination.resolve()),
        'bytes': len(signed), 'sha256': hashlib.sha256(signed).hexdigest(), 'signature': signature,
        'code_patches': [{'vm_address': hex(offset), 'file_offset': offset, 'before': original[offset:offset+size].hex(),
            'after': signed[offset:offset+size].hex()} for offset, size in patches],
        'effect': effect,
        'migration_plugins_skipped': False,
        'setup_completion_written': False, 'keybag_operations_implemented': False,
        **properties,
        'inputs': {str(path.relative_to(build_state.ROOT)): _component_native.digest(path) for path in (build_state.SELF, build_state.ROOT/'tools/project/firmware/macho.py')}}
    if len(report['code_patches']) == 1:
        report['code_patch'] = dict(report['code_patches'][0], effect=effect)
    if resource_fork is not None:
        original_fork, attribute = resource_fork.read_bytes(), compression_attribute.read_bytes()
        fork, chunks = replace_zlib_resource_chunks(original_fork, attribute, original, signed)
        path = output/'resource-fork.bin'; path.write_bytes(fork)
        report['resource_fork'] = {'path': str(path.resolve()), 'bytes': len(fork),
            'source_sha256': hashlib.sha256(original_fork).hexdigest(), 'sha256': hashlib.sha256(fork).hexdigest(),
            'decmpfs_sha256': hashlib.sha256(attribute).hexdigest(), 'changed_chunks': chunks,
            'allocation_changed': False, 'filesystem_metadata_changed': False}
    (output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def verify_display_readback(prefix):
    """Export actual DPU captures, comparing pixels only when a software source exists."""
    import zlib
    prefix = prefix.resolve()
    text = Path(str(prefix) + '-validation.txt').read_text()
    native_window = bool(re.search(r'^NATIVE_WINDOW_BEGIN native_content_only=1 ', text, re.M))
    size = re.search(r'^SWAP_TEST_SIZE result=0x0 width=(\d+) height=(\d+)$', text, re.M)
    layout = re.search(r'^SWAP_TEST_REFERENCE saved=1 bytes=(\d+) row=(\d+) ', text, re.M)
    if native_window:
        initial = json.loads(Path(str(prefix) + '-before-scanout.json').read_text())
        width, height = initial.get('Width', 0), initial.get('Height', 0)
        if not (0 < width <= 4096 and 0 < height <= 4096):
            raise ValueError('invalid native window geometry')
        allocation = source_stride = 0
    elif not size or not layout:
        raise ValueError('capture lacks a successful native size query and software source export')
    else:
        width, height = map(int, size.groups())
        allocation, source_stride = map(int, layout.groups())
    if not native_window and not (0 < width <= 4096 and 0 < height <= 4096 and width * 4 <= source_stride <= 65536
            and allocation == source_stride * height and allocation <= 32 * 1024 * 1024):
        raise ValueError('invalid software source geometry')

    def visible(path, stride):
        data = path.read_bytes()
        if len(data) != stride * height:
            raise ValueError('truncated or oversized framebuffer: ' + str(path))
        return data, b''.join(data[y * stride:y * stride + width * 4] for y in range(height))

    def png(path, bgra):
        # Standard-library RGB encoder; no image-generation or host imaging
        # dependency. Discard only alpha, which is not a panel color channel.
        filtered = bytearray()
        for y in range(height):
            row = bgra[y * width * 4:(y + 1) * width * 4]
            rgb = bytearray(width * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
            filtered.extend(b'\0' + rgb)
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 2, 0, 0, 0)) +
                         chunk(b'IDAT', zlib.compress(filtered)) + chunk(b'IEND', b''))

    source = None
    report = {'mode': 'native-window' if native_window else 'software-source',
              'size': [width, height], 'captures': {}, 'optical_presentation_verified': False}
    if not native_window:
        source_raw, source = visible(Path(str(prefix) + '-source.bgra'), source_stride)
        png(Path(str(prefix) + '-source.png'), source)
        report.update(source_size=[width, height], source_stride=source_stride,
            source_raw_sha256=hashlib.sha256(source_raw).hexdigest(),
            source_visible_bgra_sha256=hashlib.sha256(source).hexdigest())
    for phase in ('before', 'on', 'after'):
        stem = str(prefix) + '-' + phase + '-scanout'
        metadata = json.loads(Path(stem + '.json').read_text())
        stride = metadata.get('RowBytes', 0)
        if (metadata.get('Version') != 1 or metadata.get('PixelFormat') != 0x42475241 or
                (metadata.get('Width'), metadata.get('Height')) != (width, height) or
                not width * 4 <= stride <= 65536 or stride * height > 16 * 1024 * 1024):
            raise ValueError('incompatible scanout geometry: ' + phase)
        raw, pixels = visible(Path(stem + '.bgra'), stride)
        if metadata.get('Bytes') != len(raw):
            raise ValueError('scanout metadata byte count differs: ' + phase)
        stable = (metadata['SourceAddress'] & 0xffffffff) == (metadata['SourceAddressAfter'] & 0xffffffff)
        matches = sum(pixels[i:i + 4] == source[i:i + 4] for i in range(0, len(source), 4)) if source is not None else None
        image = Path(str(prefix) + '-' + phase + '.png')
        png(image, pixels)
        report['captures'][phase] = {'raw_sha256': hashlib.sha256(raw).hexdigest(),
            'visible_bgra_sha256': hashlib.sha256(pixels).hexdigest(),
            'source_address_stable': stable, 'matching_bgra_pixels': matches,
            'total_pixels': width * height, 'pixels_match_source': matches == width * height and stable if source is not None else None,
            'png': str(image), 'metadata': metadata}
    for name in ('END', 'WAIT'):
        result = re.search(r'^SWAP_TEST_' + name + r' result=(0x[0-9a-f]+)', text, re.M)
        report['native_swap_' + name.lower()] = result[1] if result else None
    flags = re.search(r'^SWAP_TEST_WAIT result=0x[0-9a-f]+ flags=(\d+) ', text, re.M)
    report['native_swap_wait_flags'] = int(flags[1]) if flags else None
    retire = re.search(r'^SWAP_RETIREMENT_WAIT result=(0x[0-9a-f]+) flags=0 first_token=(\d+) replacement_token=(\d+) elapsed_ms=([0-9.]+)$', text, re.M)
    if retire:
        report['native_retirement_wait'] = {'result': retire[1], 'flags': 0,
            'first_token': int(retire[2]), 'replacement_token': int(retire[3]), 'elapsed_ms': float(retire[4])}
    report['scanout_pixels_verified'] = report['captures']['after']['pixels_match_source']
    report['native_window_captured'] = native_window and all(c['source_address_stable'] for c in report['captures'].values())
    report['scanout_changed'] = report['captures']['before']['raw_sha256'] != report['captures']['after']['raw_sha256']
    path = Path(str(prefix) + '-readback.json')
    path.write_text(json.dumps(report, indent=2) + '\n')
    if native_window:
        return {'report': str(path), 'native_window_captured': report['native_window_captured'],
                'scanout_changed': report['scanout_changed'], 'software_source_submitted': False,
                'size': [width, height], 'optical_presentation_verified': False}
    return {'report': str(path), 'scanout_pixels_verified': report['scanout_pixels_verified'],
            'scanout_changed': report['scanout_changed'], 'matching_bgra_pixels': report['captures']['after']['matching_bgra_pixels'],
            'total_pixels': width * height, 'native_swap_end': report['native_swap_end'],
            'native_swap_wait': report['native_swap_wait'], 'native_swap_wait_flags': report['native_swap_wait_flags'],
            'native_retirement_wait': report.get('native_retirement_wait'), 'optical_presentation_verified': False}
