#!/usr/bin/env python3
"""Flash Mi 6 EDK2/XNU using USB Fastboot; optionally reboot and start XNU."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from fastboot_usb import Fastboot
from mi6_boot import FIRMWARE_NAME, FIRMWARE_NAMES, MAGIC, check_efi, parse_image, unpack_payload

ROOT = Path(__file__).resolve().parents[1]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, help='Android v0 boot image to flash to boot')
    parser.add_argument('--payload', type=Path, help='EFI to launch once through EDK2 USB Fastboot')
    parser.add_argument('--boot', action='store_true', help='reboot after flashing; start a supplied EFI once')
    parser.add_argument('--serial', help='Fastboot device serial; otherwise select the single connected device')
    parser.add_argument('--firmware-serial', default='edk2-fastboot1', help='EDK2 serial after reboot')
    parser.add_argument('--fastboot', default='fastboot', help='Android platform-tools fastboot executable')
    parser.add_argument('--timeout', type=float, default=90, help='command/re-enumeration deadline in seconds')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/fastboot/report.json')
    args = parser.parse_args(argv)
    if not args.image and not args.payload:
        parser.error('provide --image or --payload')
    if args.payload and not args.boot:
        parser.error('--payload requires --boot')
    if not 1 <= args.timeout <= 600:
        parser.error('--timeout must be 1..600 seconds')
    report = {'success': False, 'transport': 'usb-fastboot', 'commands': [],
              'flash_completed': False, 'reboot_accepted': False, 'efi_start_accepted': False,
              'xnu_boot_verified': False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    client = Fastboot(args.fastboot, args.timeout, report['commands'])
    try:
        # Validate every local input before issuing any mutating USB command.
        autoboot = False
        if args.image:
            data = args.image.read_bytes()
            image = parse_image(data)
            report.update(image=str(args.image.resolve()), image_sha256=hashlib.sha256(data).hexdigest())
            if image.ramdisk and (image.ramdisk.startswith(MAGIC) or
                                  image.header[48:64].rstrip(b'\0') in FIRMWARE_NAMES):
                firmware = image.header[48:64].rstrip(b'\0')
                if firmware not in FIRMWARE_NAMES:
                    raise ValueError('payload image is missing the matching EDK2 loader marker')
                metadata, _ = unpack_payload(image.ramdisk)
                if firmware != FIRMWARE_NAME and metadata['autoboot'] and metadata['delay_seconds'] == 0:
                    raise ValueError('zero-delay payload requires rebuilt EDK2 firmware')
                autoboot = metadata['autoboot']
                report['payload'] = metadata
        if args.payload:
            payload = args.payload.read_bytes()
            check_efi(payload)
            report['efi_sha256'] = hashlib.sha256(payload).hexdigest()
            if autoboot:
                raise ValueError('autoboot image already starts its EFI; omit --payload')
        serial = client.select(args.serial)
        report.update(serial=serial, product=client.require_sagit(serial))
        if args.image:
            print(f'Flashing boot on {serial} (sagit)...', flush=True)
            client.flash_boot(serial, args.image)
            report['flash_completed'] = True
            if args.boot:
                client.run('reboot', serial=serial)
                report['reboot_accepted'] = True
                if args.payload:
                    serial = client.select(args.firmware_serial, wait=True)
        if args.payload:
            crc = client.launch_efi(serial, args.payload)
            report.update(efi_start_accepted=True, downloaded_crc32=f'{crc:08x}')
        report['success'] = True
        if autoboot and args.boot:
            if report['payload']['delay_seconds']:
                print('Flash completed; reboot accepted. The bundled EFI will start after the on-screen countdown.')
            else:
                print('Flash completed; reboot accepted. EDK2 will start the bundled EFI immediately.')
        elif args.payload:
            print('EFI download verified; start accepted by EDK2. Check the device screen for boot progress.')
        else:
            print('Flash completed.' + (' Reboot accepted.' if args.boot else ' Device remains in Fastboot.'))
        return 0
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        report['error'] = str(error)
        print(f'flash: {error}')
        return 1
    finally:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    raise SystemExit(main())
