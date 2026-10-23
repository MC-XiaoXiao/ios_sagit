"""Trust-cache formats are parsed before the loader hands them to AMFI."""
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
import bridge_trustcache as tc


def module(version=1):
    width = {0: 20, 1: 22, 2: 24}[version]
    return struct.pack('<I16sI', version, bytes(16), 2) + bytes(width) + b'\x11'*20 + bytes(width-20)


def signed_image():
    directory = bytearray(44)
    struct.pack_into('>III', directory, 0, 0xfade0c02, len(directory), 0x20001)
    directory[36:40] = bytes((32, 2, 0, 12))
    signature = struct.pack('>IIIII', 0xfade0cc0, 20+len(directory), 1, 0, 20) + directory
    image = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 1, 16, 0, 0)
    image += struct.pack('<4I', 0x1d, 16, 48, len(signature)) + signature
    return image, bytes(directory)


class TrustCacheTests(unittest.TestCase):
    def test_detached_payload_must_match_before_trusting_its_directory(self):
        from detached_codesign import sign_data
        with tempfile.TemporaryDirectory() as temporary:
            payload = Path(temporary)/'cache.plist'; payload.write_bytes(b'changed service cache')
            signature = payload.with_suffix('.plist.sig')
            signature.write_bytes(sign_data(payload.read_bytes(), 'org.mi6.test.cache'))
            data, files = tc.development_module([signature])
            self.assertEqual(tc.module_info(data)['entries'], 1)
            self.assertEqual(files[0]['payload_sha256'], hashlib.sha256(payload.read_bytes()).hexdigest())
            payload.write_bytes(b'corrupted service cache')
            with self.assertRaises(ValueError): tc.development_module([signature])

    def test_module_versions_and_offset_handoff(self):
        modules = [module(v) for v in (0, 1, 2)]
        data = tc.handoff(modules)
        count, a, b, c = struct.unpack_from('<4I', data)
        self.assertEqual((count, a, b, c), (3, 16, 16+len(modules[0]), 16+len(modules[0])+len(modules[1])))
        self.assertEqual(data[c:], modules[2])
        for m in modules:
            self.assertEqual(tc.raw_module(m), m)
            with self.assertRaises(ValueError): tc.raw_module(m[:-1])
        with self.assertRaises(ValueError): tc.handoff([])

    def test_unsorted_and_malformed_modules_rejected(self):
        m = module()
        with self.assertRaises(ValueError): tc.module_info(m[:24] + m[46:] + m[24:46])
        with self.assertRaises(ValueError): tc.module_info(bytes(23))

    def test_im4p_payload_not_the_der_wrapper_is_handed_off(self):
        def field(tag, data): return bytes((tag, len(data))) + data
        m = module()
        fields = field(0x16, b'IM4P') + field(0x16, b'trst') + field(0x16, b'1') + field(4, m)
        wrapped = field(0x30, fields)
        self.assertEqual(tc.raw_module(wrapped), m)
        with self.assertRaises(ValueError): tc.raw_module(wrapped[:-1])
        with self.assertRaises(ValueError): tc.raw_module(wrapped+b'\0')

    def test_development_cache_contains_actual_code_directory_hash(self):
        image, directory = signed_image()
        expected = hashlib.sha256(directory).digest()[:20]
        self.assertEqual(tc.code_directory_entries(image), [(expected, 2)])
        with tempfile.TemporaryDirectory() as temporary:
            p = Path(temporary)/'executable'; p.write_bytes(image)
            data, files = tc.development_module([p])
            self.assertEqual(data[24:], expected + b'\x02\0')
            self.assertEqual(files[0]['sha256'], hashlib.sha256(image).hexdigest())
            self.assertEqual(tc.development_module([p])[0], data)
        damaged = bytearray(image)
        struct.pack_into('<I', damaged, 44, 0xffffffff)
        with self.assertRaises(ValueError): tc.code_directory_entries(damaged)

    def test_fat_slice_bounds_are_checked(self):
        image, _ = signed_image()
        fat = struct.pack('>7I', 0xcafebabe, 1, 0x100000c, 0, 28, len(image), 0) + image
        self.assertEqual(tc.code_directory_entries(fat), tc.code_directory_entries(image))
        with self.assertRaises(ValueError): tc.code_directory_entries(fat[:-1])


if __name__ == '__main__': unittest.main()
