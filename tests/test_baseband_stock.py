"""Pinned regular-file extraction and ELF reconstruction; no device access."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from baseband_stock import monolithic, prepare


class StockFirmware(unittest.TestCase):
    def test_split_elf_uses_actual_file_offsets(self):
        header = bytearray(148); header[:7] = b'\x7fELF\1\1\1'
        struct.pack_into('<H', header, 18, 164); struct.pack_into('<I', header, 28, 52)
        struct.pack_into('<3H', header, 40, 52, 32, 3)
        struct.pack_into('<8I', header, 52, 0, 0, 0, 0, 148, 148, 0, 0)
        struct.pack_into('<8I', header, 84, 0, 4096, 0, 0, 4, 4, 2 << 24, 0)
        struct.pack_into('<8I', header, 116, 1, 8192, 0, 0, 3, 16, 7, 0)
        pieces = {0: bytes(header), 1: b'HASH', 2: b'xyz'}
        mdt = bytes(header) + pieces[1]
        result = monolithic(mdt, pieces.__getitem__)
        self.assertEqual(len(result), 8195)
        self.assertEqual(result[:148], header)
        self.assertEqual(result[4096:4100], b'HASH')
        self.assertEqual(result[8192:], b'xyz')
        self.assertFalse(any(result[148:4096]))
        with self.assertRaisesRegex(ValueError, 'length'):
            monolithic(mdt, lambda i: b'x' if i == 2 else pieces[i])
        bad = bytearray(mdt); struct.pack_into('<I', bad, 120, 4098)
        overlap = {**pieces, 0: bytes(bad[:148])}
        with self.assertRaisesRegex(ValueError, 'overlapping'): monolithic(bad, overlap.__getitem__)
        bad = bytearray(mdt); bad[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'header/hash'): monolithic(bad, pieces.__getitem__)

    def test_real_archive_matches_existing_boot_and_complete_mcfg_files(self):
        expected = json.loads((ROOT / 'build/baseband/stock-files/manifest.json').read_text())
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'files'
            actual = prepare(ROOT, output)
            self.assertEqual(actual, expected)
            self.assertTrue(all(actual['matches_existing_boot_inputs'].values()))
            self.assertEqual(actual['remote_file_count'], 113)
            self.assertEqual(actual['remote_file_bytes'], 4692735)
            self.assertLess(actual['maximum_remote_path_bytes'], 128)
            self.assertFalse(actual['dynamic_module_so_directory_present'])
            self.assertFalse(actual['files_in_driver_catalog'])
            self.assertFalse(actual['original_xiaomi_package_verified'])
            self.assertFalse(actual['hardware_tested'])
            self.assertEqual(prepare(ROOT, output), expected)
            # MCFG's own index uses the same short directory names as FAT.
            config = output / 'modem_pr/mcfg/configs'
            for line in (config / 'mcfg_sw/mbn_sw.txt').read_text().splitlines():
                self.assertTrue((config / line).is_file(), line)
            changed = output / 'modemr.jsn'; changed.write_bytes(changed.read_bytes() + b' ')
            with self.assertRaisesRegex(ValueError, 'changed staged stock file'): prepare(ROOT, output)

    def test_output_is_not_an_input_location(self):
        with self.assertRaisesRegex(ValueError, 'input sources'): prepare(ROOT, ROOT / 'firmware/cannot-replace')
        with self.assertRaisesRegex(ValueError, 'input sources'): prepare(ROOT, ROOT)


if __name__ == '__main__':
    unittest.main()
