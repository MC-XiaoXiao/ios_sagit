import sys
from pathlib import Path
import struct
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from bridge_amfi import patch_latch_function


class AMFIPlatformTests(unittest.TestCase):
    def test_only_hardware_latch_branch_changes(self):
        code = bytearray(0x258)
        code[:20] = bytes.fromhex('ff4301d1f65702a9f44f03a9fd7b04a9fd030191')
        code[28:36] = bytes.fromhex('1f050071a1000054')
        struct.pack_into('<I', code, 0x30, 0x14000023)
        result = patch_latch_function(code)
        self.assertEqual(result[:0x20], code[:0x20])
        self.assertEqual(result[0x24:], code[0x24:])
        branch = struct.unpack_from('<I', result, 0x20)[0]
        self.assertEqual(0x20 + (branch & 0x3ffffff)*4, 0xbc)
        for offset in (0, 28, 32, 48):
            altered = code.copy(); altered[offset] ^= 1
            with self.assertRaises(ValueError): patch_latch_function(altered)
        with self.assertRaises(ValueError): patch_latch_function(code[:-1])


if __name__ == '__main__': unittest.main()
