import copy
import hashlib
import hmac
import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'integrity', Path(__file__).parents[1] / 'scripts/kernelcache_integrity.py')
integrity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(integrity)


def fixture():
    data = bytearray(256)
    data[128:192] = bytes(range(64))
    value = hmac.digest(bytes(32), data[128:192], 'sha256')
    data[32:64] = value
    image = SimpleNamespace(sections=[
        dict(name='__text', segment='__TEXT_EXEC', vm=128, size=64),
        dict(name='__fips_hmacs', segment='__TEXT', vm=32, size=32)],
        offset=lambda address, size=1: address)
    return data, {integrity.CORECRYPTO: image}, value


class IntegrityTests(unittest.TestCase):
    def test_code_and_digest_corruption_are_detected_independently(self):
        data, images, _ = fixture()
        integrity.audit(data, images)
        for offset in (32, 128, 191):
            bad = bytearray(data)
            bad[offset] ^= 1
            with self.assertRaisesRegex(ValueError, 'HMAC mismatch'):
                integrity.audit(bad, images)

    def test_truncated_duplicate_missing_and_overlapping_sections_are_rejected(self):
        data, images, _ = fixture()
        original = images[integrity.CORECRYPTO]
        variants = []
        for index, field, value in [(1, 'size', 31), (0, 'size', 0),
                                    (0, 'vm', 240), (1, 'vm', 128)]:
            image = copy.deepcopy(original)
            image.sections[index][field] = value
            variants.append(image)
        image = copy.deepcopy(original)
        image.sections.append(image.sections[1].copy())
        variants.append(image)
        image = copy.deepcopy(original)
        image.sections.pop()
        variants.append(image)
        for image in variants:
            with self.subTest(sections=image.sections), self.assertRaises(ValueError):
                integrity.audit(data, {integrity.CORECRYPTO: image})

    def test_finalizer_changes_only_digest_and_is_idempotent(self):
        data, images, original = fixture()
        data[128] ^= 1  # Model a linker/ISA change after Apple's HMAC was set.
        before = bytes(data)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'cache'
            path.write_bytes(data)
            context = SimpleNamespace(context=lambda p: (p.read_bytes(), None, images))
            with patch.dict('sys.modules', armv8_compat=context), \
                    patch.object(integrity, 'source_module', return_value=(original, 64)):
                report = integrity.update(path, Path('provider'))
                after = path.read_bytes()
                self.assertEqual(after[:32], before[:32])
                self.assertEqual(after[64:], before[64:])
                self.assertTrue(report['modules'][0]['changed'])
                integrity.audit(after, images)
                self.assertFalse(integrity.update(path, Path('provider'))['modules'][0]['changed'])
                self.assertEqual(path.read_bytes(), after)

    def test_arbitrary_digest_is_not_silently_repaired(self):
        data, images, original = fixture()
        data[32] ^= 1
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'cache'
            path.write_bytes(data)
            context = SimpleNamespace(context=lambda p: (p.read_bytes(), None, images))
            with patch.dict('sys.modules', armv8_compat=context), \
                    patch.object(integrity, 'source_module', return_value=(original, 64)), \
                    self.assertRaisesRegex(ValueError, 'unexpected embedded HMAC'):
                integrity.update(path, Path('provider'))
            self.assertEqual(path.read_bytes(), data)

    def test_unaudited_source_is_rejected_before_write(self):
        data, images, _ = fixture()
        with tempfile.TemporaryDirectory() as directory:
            path, source = Path(directory) / 'cache', Path(directory) / 'provider'
            path.write_bytes(data)
            source.write_bytes(b'unsupported provider')
            context = SimpleNamespace(context=lambda p: (p.read_bytes(), None, images))
            with patch.dict('sys.modules', armv8_compat=context), \
                    self.assertRaisesRegex(ValueError, 'unaudited source provider'):
                integrity.update(path, source)
            self.assertEqual(path.read_bytes(), data)

    def test_collections_without_corecrypto_are_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'cache'
            path.write_bytes(b'other collection')
            context = SimpleNamespace(context=lambda p: (p.read_bytes(), None, {}))
            with patch.dict('sys.modules', armv8_compat=context):
                self.assertEqual(integrity.update(path, Path('unused')), {'modules': []})
            self.assertEqual(hashlib.sha256(path.read_bytes()).digest(),
                             hashlib.sha256(b'other collection').digest())


if __name__ == '__main__':
    unittest.main()
