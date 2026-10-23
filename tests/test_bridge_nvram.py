"""Check the proxy against the actual XNU CHRP checksum implementation."""
import ctypes
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('bridge_nvram', ROOT/'scripts/bridge_nvram.py')
nvram = importlib.util.module_from_spec(spec); spec.loader.exec_module(nvram)


class NVRAMTests(unittest.TestCase):
    def test_explicit_boot_args_seed_preserves_headers_and_validates_checksum(self):
        original = nvram.empty_bank(0x4000)
        seeded = nvram.with_boot_args(original, b'-v wdt_test=no_user_checkin')
        self.assertEqual(seeded[:16], original[:16])
        self.assertEqual(seeded[32:48], original[32:48])
        self.assertEqual(seeded[48:90].split(b'\0')[0], b'boot-args=-v wdt_test=no_user_checkin')
        self.assertEqual(seeded[0x2000:], original[0x2000:])
        buf = ctypes.create_string_buffer(seeded)
        self.assertEqual(self.lib.image_adler(buf, len(seeded)), struct.unpack_from('<I', seeded, 16)[0])
        with self.assertRaises(ValueError): nvram.with_boot_args(seeded, b'new')
        for bad in (b'a\0b', b'x'*1024):
            with self.assertRaises(ValueError): nvram.with_boot_args(original, bad)

    @classmethod
    def setUpClass(cls):
        source = (ROOT/'kernel/iokit/Kernel/IONVRAMCHRPHandler.cpp').read_text()
        def function(name):
            start = source.index(name + '(')
            start = source.rfind('static ', 0, start)
            brace = source.index('{', start); end = brace + 1; depth = 1
            while depth:
                depth += (source[end] == '{') - (source[end] == '}'); end += 1
            return source[start:end]
        # Compile the kernel's functions verbatim, independent of Python's zlib.
        code = '#include <stdint.h>\n#include <stddef.h>\n'
        code += source[source.index('typedef struct chrp_nvram_header'):source.index('} apple_nvram_header_t;') + len('} apple_nvram_header_t;')]
        code += function('adler32') + '\n' + function('chrp_checksum')
        code += '\nuint32_t image_adler(const uint8_t *b, size_t n) { return adler32(b+20,n-20); }'
        code += '\nuint8_t image_checksum(const void *b) { return chrp_checksum(b); }'
        cls.temp = tempfile.TemporaryDirectory(); path = Path(cls.temp.name)
        (path/'check.c').write_text(code)
        subprocess.run(['clang', '-shared', '-fPIC', '-Wall', '-Werror', str(path/'check.c'), '-o', str(path/'check.so')], check=True)
        cls.lib = ctypes.CDLL(str(path/'check.so'))
        cls.lib.image_adler.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.image_adler.restype = ctypes.c_uint32
        cls.lib.image_checksum.argtypes = [ctypes.c_void_p]
        cls.lib.image_checksum.restype = ctypes.c_uint8

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def test_snapshot_matches_kernel_checksums_and_partition_walk(self):
        for size in (0x4000, 0x10000):
            data = nvram.empty_bank(size); buf = ctypes.create_string_buffer(data)
            self.assertEqual(self.lib.image_adler(buf, size), struct.unpack_from('<I', data, 16)[0])
            pos, names = 0, []
            while pos < size:
                sig, checksum, blocks, name = struct.unpack_from('<BBH12s', data, pos)
                self.assertEqual(self.lib.image_checksum(ctypes.byref(buf, pos)), checksum)
                self.assertGreater(blocks, 0)
                names.append(name.rstrip(b'\0')); pos += blocks*16
            self.assertEqual(pos, size)
            self.assertEqual(names, [b'2nvram', b'2common', b'2system'])
            self.assertEqual(nvram.validate_bank(data, size)[b'2common'], (48, 0x1fd0))
            self.assertEqual(data[48:0x2000], bytes(0x1fd0))

    def test_corrupt_checksum_adler_bounds_and_missing_partition_rejected(self):
        for offset, value, repair_adler in ((1, 0, False), (40, 0x7f, True), (0x3000, 1, False), (34, 0xff, True)):
            data = bytearray(nvram.empty_bank(0x4000)); data[offset] = value
            if repair_adler: struct.pack_into('<I', data, 16, zlib.adler32(data[20:]))
            with self.assertRaises(ValueError): nvram.validate_bank(data, 0x4000)
        with self.assertRaises(ValueError): nvram.validate_bank(nvram.empty_bank(0x4000), 0x8000)
        # Even a correctly rechecksummed image must reject an oversized partition
        # and a missing common region, rather than relying only on Adler32.
        for missing in (False, True):
            data = bytearray(nvram.empty_bank(0x4000))
            if missing: data[36:48] = b'other'.ljust(12, b'\0')
            else: struct.pack_into('<H', data, 34, 0xffff)
            data[33] = nvram.checksum(data[32:48])
            struct.pack_into('<I', data, 16, zlib.adler32(data[20:]))
            with self.assertRaises(ValueError): nvram.validate_bank(data, 0x4000)
        for size in (0, 0x2000, 0x4001, 0x10010):
            with self.assertRaises(ValueError): nvram.empty_bank(size)

    def test_handoff_requires_complete_consistent_bank_parameters(self):
        props = {key: struct.pack('>I', value) for key, value in
                 (('nvram-bank-size', 0x4000), ('nvram-bank-count', 1), ('nvram-current-bank', 0))}
        complete = nvram.chosen_properties(props)
        self.assertNotIn('nvram-proxy-data', props)
        self.assertEqual(len(complete['nvram-proxy-data']), 0x4000)
        self.assertEqual(nvram.chosen_properties({}), {})
        for changed in ({'nvram-bank-count': struct.pack('>I', 0)}, {'nvram-current-bank': struct.pack('>I', 1)}, {'nvram-bank-size': b'\0'}, {'nvram-proxy-data': bytes(0x4000)}):
            with self.assertRaises(ValueError): nvram.chosen_properties(dict(props, **changed))
        del props['nvram-current-bank']
        with self.assertRaises(ValueError): nvram.chosen_properties(props)
