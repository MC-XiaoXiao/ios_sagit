"""Execute the public no-SEP query code at actual shared-cache addresses."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from apfs_sealed import SealedImage, digest
from keybag_policy import CACHE_PATH, CAVE, HOOKS, compile_policy, resign_cache
from keybag_policy import AKS_CAVE, AKS_HOOKS, AKS_NATIVE_STATE

try:
    import unicorn
    from unicorn import arm64_const as reg
except ImportError:
    unicorn = None


@unittest.skipUnless(unicorn, 'ARM64 execution tests require unicorn')
class PublicQueryExecution(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.blob, cls.symbols, _ = compile_policy(Path(cls.temp.name))
        cls.aks_temp = tempfile.TemporaryDirectory()
        cls.aks_blob, cls.aks_symbols, _ = compile_policy(Path(cls.aks_temp.name), aks=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()
        cls.aks_temp.cleanup()

    def test_no_sep_public_queries_with_slide_and_user_ids(self):
        for name, (addr, _) in HOOKS.items():
            for slide in (0, 0x16000000):
                for argument in (0, 1, 501, 0xffffffff):
                    with self.subTest(api=name, slide=slide, argument=argument):
                        cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
                        for page in {CAVE & ~4095, addr & ~4095}:
                            cpu.mem_map(page + slide, 4096)
                        cpu.mem_map(0x2000000, 4096)
                        cpu.mem_write(CAVE + slide, self.blob)
                        target = self.symbols['keybag_' + name] + slide
                        branch = 0x14000000 | (((target - (addr + slide)) >> 2) & 0x3ffffff)
                        cpu.mem_write(addr + slide, struct.pack('<I', branch))
                        cpu.reg_write(reg.UC_ARM64_REG_X0, argument)
                        cpu.reg_write(reg.UC_ARM64_REG_X30, 0x2000000)
                        cpu.reg_write(reg.UC_ARM64_REG_SP, 0x1000000)
                        preserved = {getattr(reg, f'UC_ARM64_REG_X{i}'): i * 12345 for i in range(19, 30)}
                        for r, value in preserved.items():
                            cpu.reg_write(r, value)
                        cpu.emu_start(addr + slide, 0x2000000, count=8)
                        self.assertEqual(cpu.reg_read(reg.UC_ARM64_REG_PC), 0x2000000)
                        self.assertEqual(cpu.reg_read(reg.UC_ARM64_REG_X0), 3 if name.startswith('device_lock_state') else 1)
                        self.assertEqual(cpu.reg_read(reg.UC_ARM64_REG_SP), 0x1000000)
                        for r, value in preserved.items():
                            self.assertEqual(cpu.reg_read(r), value)

    def test_aks_record_bounds_and_native_fallback(self):
        for name, (address, _) in AKS_HOOKS.items():
            for slide in (0, 0x16000000):
                for bag, output in ((0, 0x3000010), (0, 0), (1, 0x3000010), (0xffffffff, 0x3000010)):
                    with self.subTest(api=name, slide=slide, bag=bag, output=output):
                        self.check_aks_call(name, address, slide, bag, output)

    def check_aks_call(self, name, address, slide, bag, output):
        cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        for page in {AKS_CAVE & ~4095, address & ~4095, AKS_NATIVE_STATE & ~4095}:
            cpu.mem_map(page + slide, 4096)
        cpu.mem_map(0x2000000, 4096)
        cpu.mem_map(0x3000000, 4096)
        cpu.mem_write(0x3000000, bytes([0xa5]) * 128)
        cpu.mem_write(AKS_CAVE + slide, self.aks_blob)
        target = self.aks_symbols['keybag_aks_' + name] + slide
        branch = 0x14000000 | (((target - (address + slide)) >> 2) & 0x3ffffff)
        cpu.mem_write(address + slide, struct.pack('<I', branch))
        cpu.reg_write(reg.UC_ARM64_REG_X0, bag)
        cpu.reg_write(reg.UC_ARM64_REG_X1, output)
        cpu.reg_write(reg.UC_ARM64_REG_X19, 0x12345678)
        cpu.reg_write(reg.UC_ARM64_REG_X30, 0x2000000)
        supported = bag == 0 and output != 0
        stop = 0x2000000 if supported else AKS_NATIVE_STATE + slide
        cpu.emu_start(address + slide, stop, count=32)
        self.assertEqual(cpu.reg_read(reg.UC_ARM64_REG_PC), stop)
        self.assertEqual(cpu.reg_read(reg.UC_ARM64_REG_X19), 0x12345678)
        expected = bytearray([0xa5] * 128)
        if supported:
            self.assertEqual(cpu.reg_read(reg.UC_ARM64_REG_X0), 0)
            expected[16:82] = bytes(66)
            struct.pack_into('<II', expected, 16, 2, 3)
            struct.pack_into('<I', expected, 36, 4)
        else:
            self.assertEqual(cpu.reg_read(reg.UC_ARM64_REG_X0), bag)
            self.assertEqual(cpu.reg_read(reg.UC_ARM64_REG_X1), 17 if name == 'device_state' else 35)
            self.assertEqual(cpu.reg_read(reg.UC_ARM64_REG_X2), output)
        self.assertEqual(cpu.mem_read(0x3000000, 128), expected)

SOURCE = ROOT / 'build/apfs/firmware/094-33089-038.dmg'


@unittest.skipUnless(SOURCE.exists(), 'requires supplied firmware fixture')
class SealedDerivative(unittest.TestCase):
    def test_source_seal_and_changed_file_reseal(self):
        image = SealedImage(SOURCE)
        try:
            original = image.read_file(CACHE_PATH)
            offset = CAVE - 0x1aee94000
            with self.assertRaisesRegex(ValueError, 'original bytes'):
                image.patch_file(CACHE_PATH, offset, b'bad!', b'test')
            image.patch_file(CACHE_PATH, offset, bytes(4), b'test')
            self.assertEqual(image.rehash_file(CACHE_PATH), 1)
            new_root = image.reseal()
            self.assertNotEqual(new_root, image.original_root_hash)
            image.verify_seal()
            root = image.vmap[image.root_oid]
            corrupt = bytearray(image.block(root))
            corrupt[100] ^= 1
            image.changes[root] = bytes(corrupt)
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                image.verify_seal()
            signature, cdhash = resign_cache(original, original)
            modified = bytearray(original)
            modified[-len(signature):] = signature
            self.assertEqual(resign_cache(modified, modified), (signature, cdhash))
            modified[5000] ^= 1
            with self.assertRaisesRegex(ValueError, 'code-page mismatch'):
                resign_cache(modified, modified)
        finally:
            image.close()


if __name__ == '__main__':
    unittest.main()
