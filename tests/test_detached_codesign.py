import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from detached_codesign import sign_data, verify_data


class DetachedSignatureTests(unittest.TestCase):
    def test_all_pages_and_last_partial_page_are_authenticated(self):
        payload = bytes(range(251)) * 70
        signature = sign_data(payload, 'org.mi6.test.data')
        self.assertEqual(len(verify_data(payload, signature)[0][0]), 20)
        for offset in (0, 4095, 4096, 16384, len(payload) - 1):
            changed = bytearray(payload); changed[offset] ^= 1
            with self.assertRaisesRegex(ValueError, 'page hash mismatch'):
                verify_data(changed, signature)
        with self.assertRaises(ValueError):
            verify_data(payload + b'x', signature)

    def test_signature_corruption_is_rejected(self):
        payload = b'engineering plist'
        signature = sign_data(payload, 'org.mi6.test.data')
        for offset in (0, 7, 11, 19, 23, 35, 47, 57, len(signature) - 1):
            changed = bytearray(signature); changed[offset] ^= 1
            with self.assertRaises(ValueError):
                verify_data(payload, changed)


if __name__ == '__main__':
    unittest.main()
