import hashlib
import importlib.util
from pathlib import Path
import tempfile
import struct
import unittest
import zlib
import plistlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('display_service_toolchain', ROOT/'scripts/toolchain.py')
toolchain = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolchain)


class DisplayServicePolicyTests(unittest.TestCase):
    def test_compressed_patch_preserves_offsets_and_unchanged_chunks(self):
        original = b'A' * 65536 + b'B' * 45000
        chunks = [zlib.compress(original[:65536], 1)[:-4], zlib.compress(original[65536:], 1)[:-4]]
        end = 280 + sum(map(len, chunks))
        fork = bytearray(end + 50)
        struct.pack_into('>4I', fork, 0, 256, end, end - 256, 50)
        struct.pack_into('>I', fork, 256, end - 260)
        struct.pack_into('<I', fork, 260, 2)
        offset = 280
        for index, chunk in enumerate(chunks):
            struct.pack_into('<2I', fork, 264 + 8 * index, offset - 260, len(chunk))
            fork[offset:offset + len(chunk)] = chunk
            offset += len(chunk)
        attribute = struct.pack('<4sIQ', b'fpmc', 4, len(original))
        replacement = b'C' * 65536 + original[65536:]
        updated, changed = toolchain.replace_zlib_resource_chunks(bytes(fork), attribute, original, replacement)
        self.assertEqual(len(updated), len(fork))
        self.assertEqual(updated[:264], fork[:264])
        self.assertEqual(updated[end:], fork[end:])
        self.assertEqual(updated[280 + len(chunks[0]):], fork[280 + len(chunks[0]):])
        self.assertEqual([chunk['index'] for chunk in changed], [0])
        for index in range(2):
            self.assertEqual(updated[264 + 8 * index:268 + 8 * index], fork[264 + 8 * index:268 + 8 * index])
        with self.assertRaisesRegex(ValueError, 'original service'):
            toolchain.replace_zlib_resource_chunks(bytes(fork), attribute, b'D' * len(original), replacement)
        with self.assertRaisesRegex(ValueError, 'allocation'):
            toolchain.replace_zlib_resource_chunks(bytes(fork), attribute, original,
                bytes(range(256)) * 256 + original[65536:])
        invalid = bytearray(fork)
        struct.pack_into('<I', invalid, 272, 0)
        with self.assertRaisesRegex(ValueError, 'range'):
            toolchain.replace_zlib_resource_chunks(bytes(invalid), attribute, original, replacement)

    def test_unrelated_or_already_patched_input_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'reference hash'):
            toolchain.patch_no_sep_service_readiness(bytes(577))
        with self.assertRaisesRegex(ValueError, 'reference hash'):
            toolchain.patch_migration_unlock(bytes(255360))
        with self.assertRaisesRegex(ValueError, 'reference hash'):
            toolchain.patch_system_app_install_wait(bytes(152304))
        with self.assertRaisesRegex(ValueError, 'reference hash'):
            toolchain.patch_springboard_snapshot_wait(bytes(194032))

    @unittest.skipUnless((ROOT/'build/display-runtime/migration-reference/com.apple.fairplayd.A2.plist').exists(),
                         'requires supplied original iOS18 FairPlay job')
    def test_no_sep_readiness_preserves_native_job_and_allocation(self):
        source = ROOT/'build/display-runtime/migration-reference/com.apple.fairplayd.A2.plist'
        original = source.read_bytes()
        with tempfile.TemporaryDirectory() as directory:
            report = toolchain.prepare_no_sep_service_readiness(source, Path(directory))
            patched = Path(report['path']).read_bytes()
            before, after = plistlib.loads(original), plistlib.loads(patched)
            self.assertEqual(len(patched), len(original))
            self.assertEqual(after['MachServices']['com.apple.unfreed'], {'HostSpecialPort': 17})
            self.assertIs(after['MachServices']['com.apple.fairplayd.xpc'], True)
            for name in report['endpoints']:
                self.assertEqual(after['MachServices'][name], {'ResetAtClose': True})
                after['MachServices'][name] = True
            after['CFBundleInfoDictionaryVersion'] = '6.0'
            self.assertEqual(after, before)
            self.assertFalse(report['daemon_code_modified'])
            self.assertFalse(report['crypto_success_fabricated'])
            self.assertFalse(report['setup_completion_written'])
            with self.assertRaisesRegex(ValueError, 'reference hash'):
                toolchain.patch_no_sep_service_readiness(patched)

    def test_cached_readiness_changes_only_named_native_endpoints(self):
        ready = {'ResetAtClose': True}
        job = {'Program': '/usr/sbin/fairplayd.A2', 'Label': 'com.apple.fairplayd.A2',
               'KeepAlive': True, 'MachServices': {'com.apple.fairplayd': True,
               'com.apple.fairplayd.versioned': True, 'com.apple.unfreed': {'HostSpecialPort': 17}}}
        path = '/System/Library/LaunchDaemons/com.apple.fairplayd.A2.plist'
        cache = {'LaunchDaemons': {path: job, 'other': {'MachServices': {'other': ready}}},
                 'AppExtensions': {'untouched': [1,2,3]}}
        original = plistlib.dumps(cache, fmt=plistlib.FMT_BINARY)
        patched, changes = toolchain.patch_launchd_cache_readiness(original)
        decoded = plistlib.loads(patched)
        self.assertEqual(len(patched), len(original))
        allowed = set()
        for change in changes: allowed.update(range(change['offset'], change['offset']+len(bytes.fromhex(change['before']))))
        self.assertTrue(all(a == b or i in allowed for i,(a,b) in enumerate(zip(original,patched))))
        for name in ('com.apple.fairplayd', 'com.apple.fairplayd.versioned'):
            self.assertEqual(decoded['LaunchDaemons'][path]['MachServices'][name], ready)
            decoded['LaunchDaemons'][path]['MachServices'][name] = True
        self.assertEqual(decoded, cache)
        with self.assertRaises(ValueError): toolchain.patch_launchd_cache_readiness(patched)
        cache['LaunchDaemons'].pop('other')
        with self.assertRaisesRegex(ValueError, 'no reusable'):
            toolchain.patch_launchd_cache_readiness(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))

    @unittest.skipUnless((ROOT/'build/display-runtime/migration-reference/SpringBoardMigrator').exists(),
                         'requires supplied original iOS18 SpringBoard migration bundle')
    def test_snapshot_deferral_preserves_native_capture_and_completion_policy(self):
        import sys
        sys.path.insert(0, str(ROOT/'scripts'))
        from macho_development_sign import signature_location, signature_blobs, verify_macho
        from bridge_trustcache import code_directory_entries
        source = ROOT/'build/display-runtime/migration-reference/SpringBoardMigrator'
        original = source.read_bytes()
        with tempfile.TemporaryDirectory() as directory:
            report = toolchain.prepare_springboard_migrator(source, Path(directory))
            patched = Path(report['path']).read_bytes()
            off, _, _, _ = signature_location(original, allow_bundle=True)
            self.assertEqual(len(patched), len(original))
            self.assertEqual(patched[:0xb418], original[:0xb418])
            self.assertEqual(patched[0xb418:0xb420].hex(), '000080d2c0035fd6')
            self.assertEqual(patched[0xb420:0xb578], original[0xb420:0xb578])
            self.assertEqual(patched[0xb578:0xb588].hex(), '140080d2130080d2ff1700f928000014')
            self.assertEqual(patched[0xb588:off], original[0xb588:off])
            # Branch lands in the original defaults path; all exit releases
            # receive nil and the original stack frame is restored.
            branch = struct.unpack_from('<I', patched, 0xb584)[0]
            self.assertEqual(0xb584 + (branch & 0x03ffffff) * 4, 0xb624)
            a, _ = signature_blobs(original[off:]); b, _ = signature_blobs(patched[off:])
            self.assertEqual(a[2], b[2]); self.assertEqual(set(b), {0, 2})
            self.assertTrue(report['post_migration_capture_flag_retained'])
            self.assertFalse(report['snapshot_success_fabricated'])
            self.assertFalse(report['migration_plugins_skipped'])
            self.assertFalse(report['setup_completion_written'])
            self.assertEqual(verify_macho(patched), report['signature'])
            self.assertEqual(code_directory_entries(patched), [(bytes.fromhex(report['signature']['cdhash']), 2)])
            with self.assertRaises(ValueError): toolchain.patch_springboard_snapshot_wait(patched)

    @unittest.skipUnless((ROOT/'build/display-runtime/migration-reference/SystemAppMigrator').exists(),
                         'requires supplied original iOS18 migration bundle')
    def test_system_app_native_timeout_and_bundle_signature(self):
        import sys
        sys.path.insert(0, str(ROOT/'scripts'))
        from macho_development_sign import signature_location, signature_blobs, verify_macho, resign_macho
        from bridge_trustcache import code_directory_entries
        source = ROOT/'build/display-runtime/migration-reference/SystemAppMigrator'
        original = source.read_bytes()
        with tempfile.TemporaryDirectory() as directory:
            report = toolchain.prepare_system_app_migrator(source, Path(directory))
            patched = Path(report['path']).read_bytes()
            off, _, _, _ = signature_location(original, allow_bundle=True)
            self.assertEqual(len(patched), len(original))
            self.assertEqual(patched[:0x4894], original[:0x4894])
            self.assertEqual(patched[0x4894:0x4898].hex(), '080080d2')
            self.assertEqual(patched[0x4898:0x7638], original[0x4898:0x7638])
            self.assertEqual(patched[0x7638:0x7640].hex(), '00008052c0035fd6')
            self.assertEqual(patched[0x7640:off], original[0x7640:off])
            a, _ = signature_blobs(original[off:]); b, _ = signature_blobs(patched[off:])
            self.assertEqual(a[2], b[2]); self.assertEqual(set(b), {0, 2})
            from detached_codesign import HEADER
            self.assertEqual(HEADER.unpack_from(b[0])[-1], 0)  # MH_BUNDLE is not a main executable.
            self.assertFalse(report['app_install_success_fabricated'])
            self.assertTrue(report['missing_app_placeholder_materialization_skipped'])
            self.assertFalse(report['migration_plugins_skipped'])
            self.assertFalse(report['signature']['cms_present'])
            self.assertFalse(report['signature']['platform_identity_claimed'])
            self.assertEqual(code_directory_entries(patched), [(bytes.fromhex(report['signature']['cdhash']), 2)])
            self.assertEqual(verify_macho(patched), report['signature'])
            with self.assertRaises(ValueError): toolchain.patch_system_app_install_wait(patched)
            with self.assertRaises(ValueError): resign_macho(original, 'org.mi6.development.fixture')

    @unittest.skipUnless((ROOT/'build/display-runtime/migration-reference/datamigrator').exists(),
                         'requires supplied original iOS18 service')
    def test_native_patch_scope_and_signing_policy(self):
        import sys
        sys.path.insert(0, str(ROOT/'scripts'))
        from macho_development_sign import signature_location, signature_blobs, verify_macho
        from bridge_trustcache import code_directory_entries
        source = ROOT/'build/display-runtime/migration-reference/datamigrator'
        original = source.read_bytes()
        self.assertEqual(hashlib.sha256(original).hexdigest(),
                         '4275137397d7df5d4c0f46d90eb5141ed3bb7f6759c41cc5c5ca019d5f14ee92')
        with tempfile.TemporaryDirectory() as directory:
            report = toolchain.prepare_display_services(source, Path(directory))
            patched = Path(report['path']).read_bytes()
            off, _, _, _ = signature_location(original)
            self.assertEqual(patched[:0xd040], original[:0xd040])
            self.assertEqual(patched[0xd048:off], original[0xd048:off])
            self.assertEqual(patched[0xd040:0xd048].hex(), '0418407a0d080054')
            before, _ = signature_blobs(original[off:])
            after, _ = signature_blobs(patched[off:])
            for slot in (2, 5, 7): self.assertEqual(after[slot], before[slot])
            self.assertEqual(len(patched), len(original))
            signed = verify_macho(patched)
            self.assertFalse(signed['cms_present'])
            self.assertFalse(signed['platform_identity_claimed'])
            self.assertEqual(code_directory_entries(patched), [(bytes.fromhex(signed['cdhash']), 2)])
            with self.assertRaises(ValueError): toolchain.patch_migration_unlock(patched)
            changed = bytearray(patched); changed[0xd040] ^= 1
            with self.assertRaises(ValueError): verify_macho(changed)

            storage = ROOT/'build/display-runtime/migration-policy/source-storage'
            if (storage/'attribute-0.bin').exists():
                fork, chunks = toolchain.replace_zlib_resource_chunks(
                    (storage/'attribute-0.bin').read_bytes(),
                    (storage/'attribute-1.bin').read_bytes(), original, patched)
                self.assertEqual([chunk['index'] for chunk in chunks], [0, 3])
                self.assertEqual(len(fork), 72043)


if __name__ == '__main__': unittest.main()
