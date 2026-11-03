"""Only launch a staged EFI after one exact firmware CRC acknowledgement."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zlib

from project.device.fastboot import Fastboot


class Firmware(Fastboot):
    def require_edk2(self, serial):
        pass

    def limit(self, serial, name):
        return 65536

    def run(self, *words, **kwargs):
        self.sent.append(words)
        return self.reply if words == ('oem', 'crc32') else ''


class FastbootCRCTests(unittest.TestCase):
    def test_exact_crc_formats(self):
        data = b'checked EFI payload'
        expected = f'{zlib.crc32(data):08x}'
        replies = [f'(bootloader) crc32={expected}\nOKAY\n',
                   f'(bootloader) {expected}\nOKAY\n', f'{expected}\n']
        with tempfile.TemporaryDirectory() as directory, patch('project.device.fastboot.check_efi'):
            path = Path(directory) / 'test.efi'
            path.write_bytes(data)
            for reply in replies:
                with self.subTest(reply=reply):
                    device = Firmware()
                    device.reply, device.sent = reply, []
                    self.assertEqual(device.launch_efi('test', path), zlib.crc32(data))
                    self.assertEqual(device.sent[-1], ('oem', 'run-efi'))
            for reply in ['deadbeef\n', f'crc32={expected}BAD\n', f'{expected}\n{expected}\n']:
                with self.subTest(rejected=reply):
                    device = Firmware()
                    device.reply, device.sent = reply, []
                    with self.assertRaises(ValueError):
                        device.launch_efi('test', path)
                    self.assertNotIn(('oem', 'run-efi'), device.sent)


if __name__ == '__main__':
    unittest.main()
