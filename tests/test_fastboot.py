"""Validate the boot-image parser, actual UEFI payload loader and USB flashing gates."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import textwrap
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import mi6_boot as boot
from fastboot_usb import Fastboot

spec = importlib.util.spec_from_file_location('mi6_flash', ROOT / 'scripts/flash.py')
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


def efi_fixture() -> bytes:
    data = bytearray(4096)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<H', data, 132, 0xaa64)
    struct.pack_into('<H', data, 152, 0x20b)
    struct.pack_into('<H', data, 220, 10)
    data[1024:2048] = bytes(range(256)) * 4
    return bytes(data)


def edk2_fixture() -> bytes:
    header = bytearray(4096)
    header[:8] = b'ANDROID!'
    struct.pack_into('<10I', header, 8, 37, 0x80080000, 0, 0x82000000,
                     0, 0, 0x81e00000, 4096, 0, 0x1800014)
    header[48:64] = boot.FIRMWARE_NAME.ljust(16, b'\0')
    return bytes(header) + b'firmware-and-appended-DTB'.ljust(4096, b'\0')


class ImageTests(unittest.TestCase):
    def test_autoboot_round_trip_keeps_firmware(self):
        image = boot.package(edk2_fixture(), efi_fixture(), autoboot=True, delay=3)
        parsed = boot.parse_image(image)
        self.assertEqual(parsed.kernel, boot.parse_image(edk2_fixture()).kernel)
        metadata, efi = boot.unpack_payload(parsed.ramdisk)
        self.assertEqual(efi, efi_fixture())
        self.assertTrue(metadata['autoboot'])
        self.assertEqual(metadata['delay_seconds'], 3)

    def test_recovery_image_drops_existing_autoboot_payload(self):
        source = boot.package(edk2_fixture(), efi_fixture(), autoboot=True)
        recovery = boot.package(source, None, autoboot=False)
        self.assertFalse(boot.parse_image(recovery).ramdisk)

    def test_old_firmware_and_non_arm64_payload_rejected(self):
        old = bytearray(edk2_fixture()); old[48:64] = bytes(16)
        with self.assertRaisesRegex(ValueError, 'rebuild'):
            boot.package(bytes(old), efi_fixture(), autoboot=True)
        efi = bytearray(efi_fixture()); struct.pack_into('<H', efi, 132, 0x8664)
        with self.assertRaisesRegex(ValueError, 'ARM64'):
            boot.package(edk2_fixture(), bytes(efi), autoboot=True)

    def test_corrupt_and_truncated_images_rejected(self):
        image = boot.package(edk2_fixture(), efi_fixture(), autoboot=True)
        with self.assertRaises(ValueError): boot.parse_image(image[:-1])
        with self.assertRaises(ValueError): boot.parse_image(image + b'signature')
        payload = bytearray(boot.parse_image(image).ramdisk); payload[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'CRC32'): boot.unpack_payload(payload)
        payload = bytearray(boot.parse_image(image).ramdisk); payload[48] = 1
        with self.assertRaises(ValueError): boot.unpack_payload(payload)

    def test_bounds_and_missing_bridge_rejected(self):
        for delay in (0, 31):
            with self.assertRaises(ValueError): boot.pack_payload(efi_fixture(), autoboot=True, delay=delay)
        with self.assertRaises(ValueError): boot.package(edk2_fixture(), None, autoboot=True)
        image = bytearray(edk2_fixture()); struct.pack_into('<I', image, 8, 0xffffffff)
        with self.assertRaises(ValueError): boot.parse_image(image)
        with mock.patch.object(boot, 'MAX_IMAGE', 8192):
            with self.assertRaisesRegex(ValueError, '64 MiB'):
                boot.package(edk2_fixture(), efi_fixture(), autoboot=True)

    def test_actual_edk2_loader_with_actual_lzma_decoder(self):
        edk2 = ROOT / 'boot/edk2-msm8998/Common/edk2'
        library = edk2 / 'MdeModulePkg/Library/LzmaCustomDecompressLib'
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            image, efi, executable = tmp / 'boot.img', tmp / 'bridge.efi', tmp / 'loader'
            image.write_bytes(boot.package(edk2_fixture(), efi_fixture(), autoboot=True, delay=1))
            efi.write_bytes(efi_fixture())
            command = ['clang', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                       '-fno-omit-frame-pointer', '-g', '-include', 'Uefi.h',
                       '-fshort-wchar', '-DMDE_CPU_X64', '-Wno-macro-redefined',
                       '-Wno-incompatible-pointer-types', '-I' + str(edk2 / 'MdePkg/Include'),
                       '-I' + str(edk2 / 'MdeModulePkg/Include'),
                       '-I' + str(library),
                       '-I' + str(edk2 / 'MdePkg/Include/X64'), str(ROOT / 'tests/edk2_sagit_boot_test.c'),
                       str(library / 'LzmaDecompress.c'), str(library / 'Sdk/C/LzmaDec.c'),
                       '-lz', '-o', str(executable)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(executable), str(image), str(efi)], check=True)


class UsbTests(unittest.TestCase):
    def test_storage_start_and_flush_do_not_open_uart(self):
        spec = importlib.util.spec_from_file_location('mi6_usb_storage', ROOT / 'scripts/usb-storage.py')
        storage = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(storage)
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(command='start', serial='edk2-fastboot1', recovery_seconds=5,
                                   device=Path('/dev/fixture'), payload=Path('/tmp/fixture.efi'),
                                   disk_guid=None, output=Path(directory) / 'report.json')
            with mock.patch.object(storage, 'open_uart', side_effect=AssertionError('UART was opened')), \
                    mock.patch.object(storage, 'Fastboot') as client, \
                    mock.patch.object(storage, 'exported_disk', return_value='fixture-guid'), \
                    mock.patch.object(storage, 'flush_host') as flush_host:
                client.return_value.select.return_value = args.serial
                storage.usb_only(args)
                client.return_value.launch_efi.assert_called_once_with(args.serial, args.payload)
                args.command = 'flush'
                storage.usb_only(args)
                flush_host.assert_called_once_with(args.device)
            report = json.loads(args.output.read_text())
            self.assertIsNone(report['uart'])
            self.assertTrue(report['host_flushed'])
            self.assertFalse(report['device_flushed'])

    def test_flash_cli_with_fake_usb_only_fastboot(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            executable, state = tmp / 'fastboot', tmp / 'state.json'
            executable.write_text('#!' + sys.executable + '\n' + textwrap.dedent('''\
                import json, pathlib, sys, zlib
                path = pathlib.Path(__file__).with_name('state.json')
                state = json.loads(path.read_text()) if path.exists() else {'serial':'stock','commands':[]}
                args = sys.argv[1:]
                state['commands'].append(args[:])
                if args[:1] == ['-s']: args = args[2:]
                if args == ['devices']: print(state['serial']+'\tfastboot')
                elif args[:1] == ['getvar']:
                    values = {'product':'sagit','version-bootloader':'edk2-fastboot-1',
                              'partition-size:boot':'0x04000000','max-download-size':'0x04000000'}
                    print('(bootloader) '+args[1]+': '+values[args[1]], file=sys.stderr)
                elif args[:4] == ['-S','0','flash','boot']: state['flashed'] = args[4]
                elif args == ['reboot']: state['serial'] = 'edk2-fastboot1'
                elif args[:1] == ['stage']: state['crc'] = zlib.crc32(pathlib.Path(args[1]).read_bytes())
                elif args == ['oem','crc32']: print('(bootloader) crc32=%08x' % state['crc'], file=sys.stderr)
                elif args == ['oem','run-efi']: state['started'] = True
                else: raise SystemExit('unexpected command '+repr(args))
                path.write_text(json.dumps(state))
                '''))
            executable.chmod(0o755)
            image, payload, report = tmp / 'boot.img', tmp / 'bridge.efi', tmp / 'report.json'
            payload.write_bytes(efi_fixture())
            for auto in (False, True):
                with self.subTest(autoboot=auto):
                    state.unlink(missing_ok=True)
                    image.write_bytes(boot.package(edk2_fixture(), efi_fixture() if auto else None, autoboot=auto))
                    args = ['--image', str(image), '--boot', '--fastboot', str(executable), '--output', str(report)]
                    if not auto: args += ['--payload', str(payload)]
                    self.assertEqual(flash.main(args), 0)
                    result = json.loads(report.read_text())
                    self.assertTrue(result['flash_completed'])
                    self.assertTrue(result['reboot_accepted'])
                    self.assertFalse(result['xnu_boot_verified'])
                    commands = json.loads(state.read_text())['commands']
                    writes = [row for row in commands if 'flash' in row]
                    self.assertEqual(len(writes), 1)
                    self.assertEqual(writes[0][2:6], ['-S', '0', 'flash', 'boot'])
                    self.assertEqual(result['efi_start_accepted'], not auto)

    def test_multiple_devices_require_selection(self):
        client = Fastboot()
        with mock.patch.object(client, 'devices', return_value=['a', 'b']):
            with self.assertRaisesRegex(ValueError, 'multiple'): client.select()
            self.assertEqual(client.select('b'), 'b')

    def test_getvar_accepts_fastboot_stderr(self):
        client = Fastboot()
        result = subprocess.CompletedProcess([], 0, '', '(bootloader) partition-size:boot: 0x04000000\nFinished.\n')
        with mock.patch('fastboot_usb.subprocess.run', return_value=result):
            self.assertEqual(client.limit('device', 'partition-size:boot'), 64 << 20)

    def test_wrong_product_never_flashes(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'boot.img'; image.write_bytes(edk2_fixture())
            def run(client, *words, **kwargs):
                if words == ('devices',): return 'device\tfastboot\n'
                if words == ('getvar', 'product'): return 'product: judypn\n'
                self.fail(f'unexpected USB command: {words}')
            with mock.patch.object(Fastboot, 'run', run):
                result = flash.main(['--image', str(image), '--output', str(Path(directory) / 'report.json')])
            self.assertEqual(result, 1)

    def test_partition_or_download_limit_prevents_flash(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'image'; path.write_bytes(b'x' * 4096)
            client = Fastboot()
            with mock.patch.object(client, 'limit', side_effect=[8192, 2048]), mock.patch.object(client, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'limit'): client.flash_boot('device', path)
                run.assert_not_called()

    def test_download_crc_failure_prevents_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bridge.efi'; path.write_bytes(efi_fixture())
            client = Fastboot()
            with mock.patch.object(client, 'require_edk2'), mock.patch.object(client, 'limit', return_value=64 << 20), \
                    mock.patch.object(client, 'run', side_effect=['', 'crc32=bad0bad0']) as run:
                with self.assertRaisesRegex(ValueError, 'CRC32'): client.launch_efi('device', path)
                self.assertEqual(run.call_count, 2)
                self.assertEqual(run.call_args_list[0].args[0], 'stage')

    def test_crc_checked_before_start_without_uart(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bridge.efi'; path.write_bytes(efi_fixture())
            client = Fastboot(); crc = zlib.crc32(path.read_bytes())
            with mock.patch.object(client, 'require_edk2'), mock.patch.object(client, 'limit', return_value=64 << 20), \
                    mock.patch.object(client, 'run', side_effect=['', f'crc32={crc:08x}', '']) as run:
                self.assertEqual(client.launch_efi('device', path), crc)
                self.assertEqual(run.call_args_list[-1].args, ('oem', 'run-efi'))

    def test_timeout_records_uncertain_outcome_without_retry(self):
        client = Fastboot()
        with mock.patch('fastboot_usb.subprocess.run', side_effect=subprocess.TimeoutExpired('fastboot', 1)) as run:
            with self.assertRaises(TimeoutError): client.run('flash', 'boot', 'image', serial='device')
            self.assertEqual(run.call_count, 1)
        self.assertIn('unknown', client.commands[0]['error'])


if __name__ == '__main__':
    unittest.main()
