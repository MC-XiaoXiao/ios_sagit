"""Fail-closed ISA classification and branch-range invariants."""
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest import mock
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location('armv8_compat', Path(__file__).parents[1]/'scripts/armv8_compat.py')
isa = importlib.util.module_from_spec(spec); spec.loader.exec_module(isa)


class Armv8Tests(unittest.TestCase):
    def test_context_skips_codeless_personalities_but_checks_every_code_image(self):
        kernel = mock.Mock()
        kernel.offset.return_value = 0x100
        image = mock.Mock()
        verifier = SimpleNamespace(Image=mock.Mock(side_effect=[kernel, image]),
            inventory=SimpleNamespace(inspect=mock.Mock(return_value=({}, [
                {'CFBundleIdentifier': 'org.example.events', 'IOKitPersonalities': {}},
                {'CFBundleIdentifier': 'org.example.code', '_PrelinkExecutableLoadAddr': 0x1234}]))),
            check_kcgen_walk=mock.Mock())
        loader = mock.Mock()
        spec = SimpleNamespace(loader=loader)
        path = mock.Mock(); path.read_bytes.return_value = b'image'
        with mock.patch.object(isa.importlib.util, 'spec_from_file_location', return_value=spec), \
             mock.patch.object(isa.importlib.util, 'module_from_spec', return_value=verifier):
            data, actual, images = isa.context(path)
        self.assertIs(actual, kernel)
        self.assertEqual(images, {'org.example.code': image})
        kernel.offset.assert_called_once_with(0x1234)
        verifier.check_kcgen_walk.assert_called_once_with(data, kernel, images)

    def test_baseline_decoder_distinguishes_crypto_and_extensions(self):
        baseline, extended = isa.Decoder(), isa.Decoder(True)
        for word in (0x4e284a00, 0x5e280800, 0x1ac14000, 0xd503201f):
            self.assertIsNotNone(baseline.decode(word), hex(word))
        for word in (0xce7c83bb, 0xce000000, 0xf8210062, 0xc8a17c62, 0xf8bfc061):
            self.assertIsNone(baseline.decode(word), hex(word))
            self.assertIsNotNone(extended.decode(word), hex(word))
            self.assertIsNotNone(isa.instruction_kind(word))

    def test_unimplemented_extensions_do_not_become_nops(self):
        # SM4E, CASP and an SVE instruction need distinct safe implementations.
        for word in (0xcec08420, 0x48207c82, 0x2518e3e0):
            with self.assertRaisesRegex(ValueError, 'no safe ARMv8 lowering'):
                isa.helper_assembly(word)

    def test_all_generated_helpers_use_baseline_instructions(self):
        assembler, baseline = isa.Assembler(), isa.Decoder()
        words = (0xce608000, 0xce608400, 0xcec08000, 0xce608800,
                 0xce000000, 0xce200000, 0xce608c00, 0xce800000)
        words += tuple((size<<30)|0x38200000|(op<<12)|0xc00000|0x10000|0x62
                       for size in range(4) for op in range(9))
        words += tuple((size<<30)|0x08e0fc00|0x10000|0x62 for size in range(4))
        words += tuple((size<<30)|0x38bfc061 for size in range(4))
        for word in words:
            body = isa.helper(word, 0x100000, 0x200004, assembler)
            for insn, in struct.iter_unpack('<I', body):
                self.assertIsNotNone(baseline.decode(insn), f'{word:#x}: {insn:#x}')

    def test_branch_signed_limits_and_alignment(self):
        for delta in (-(1<<27), -4, 0, 4, (1<<27)-4):
            word, = struct.unpack('<I', isa.branch(0x10000000, 0x10000000+delta))
            immediate = word & 0x3ffffff
            if immediate & (1<<25): immediate -= 1<<26
            self.assertEqual(immediate*4, delta)
        for delta in (-(1<<27)-4, 2, 1<<27):
            with self.assertRaises(ValueError): isa.branch(0x10000000, 0x10000000+delta)

    def test_constants_require_complete_match_and_correct_bundle(self):
        pattern = bytes(range(15,-1,-1))
        self.assertEqual(isa.known_data(pattern[:-1], 'com.apple.kec.corecrypto'), [])
        self.assertEqual(isa.known_data(pattern, 'org.example.driver'), [])
        spans = isa.known_data(pattern, 'com.apple.kec.corecrypto')
        self.assertEqual(spans, [(0,16,'byte-reversal-vector')])

    def test_libm_literals_require_complete_unchanged_pool(self):
        # Native absolute-value mask at 0xfffffff005bae55c, including padding.
        pool = bytes.fromhex('ffffffffffffff7f000000000000000000000000')
        self.assertEqual(isa.known_data(pool, 'com.apple.kec.Libm'),
                         [(0, 20, 'kMathAbsoluteValueMask')])
        self.assertEqual(isa.known_data(pool[:-4], 'com.apple.kec.Libm'), [])
        self.assertEqual(isa.known_data(pool[:-1] + b'\x01', 'com.apple.kec.Libm'), [])
        self.assertEqual(isa.known_data(pool, 'org.example.driver'), [])

    def test_byte_dispatch_requires_bounded_index_and_valid_targets(self):
        pool = bytearray(0x240)
        struct.pack_into('<5I', pool, 0, 0x10000210, 0x92401cd1, 0x78b17a11, 0x8b110211, 0xd61f0220)
        struct.pack_into('<I', pool, 0x20, 0xd503249f)
        struct.pack_into('<I', pool, 0x30, 0xd503249f)
        for n in range(256): struct.pack_into('<h', pool, 0x40 + n * 2, -16 if n % 2 else -32)
        bundle = 'org.example.unrelated-driver'
        expected = [(0x40, 512, 'byte-opcode-signed16-dispatch')]
        self.assertEqual(isa.known_data(pool, bundle), expected)
        self.assertEqual(isa.known_data(pool[:-2], bundle), [])
        changed = bytearray(pool)
        struct.pack_into('<h', changed, 0x40, 4)
        self.assertEqual(isa.known_data(changed, bundle), [])
        struct.pack_into('<I', pool, 4, 0xd503201f)
        self.assertEqual(isa.known_data(pool, bundle), [])

    def test_complete_mathematical_bitmask_pool(self):
        pool = b''.join(((1 << n) - 1).to_bytes(8, 'little') for n in range(65))
        self.assertEqual(isa.known_data(pool, 'org.example.driver'), [(0, 520, 'low-bit-masks')])
        self.assertEqual(isa.known_data(pool[:-8], 'org.example.driver'), [])


if __name__ == '__main__': unittest.main()
