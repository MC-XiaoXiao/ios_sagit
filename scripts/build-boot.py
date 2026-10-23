#!/usr/bin/env python3
"""Pack EDK2 and a self-contained XnuBridge into a flashable Mi 6 boot image."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from mi6_boot import package, parse_image, unpack_payload

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--edk2', type=Path, default=ROOT / 'build/edk2-sagit/boot-sagit.img')
    parser.add_argument('--bridge', type=Path, help='self-contained ARM64 XnuBridge.efi, including its kernel')
    parser.add_argument('--autoboot', action='store_true', help='start the bundled EFI on every power-on')
    parser.add_argument('--boot-delay', type=int, default=5, help='volume-key recovery countdown, 1..30 seconds')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/boot/boot-sagit.img')
    args = parser.parse_args()
    try:
        edk2 = args.edk2.read_bytes()
        efi = args.bridge.read_bytes() if args.bridge else None
        result = package(edk2, efi, autoboot=args.autoboot, delay=args.boot_delay)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(result)
        report = {'product': 'sagit', 'format': 'MI6BOOT1', 'autoboot': args.autoboot,
                  'edk2': str(args.edk2.resolve()), 'edk2_sha256': hashlib.sha256(edk2).hexdigest(),
                  'bridge': str(args.bridge.resolve()) if args.bridge else None,
                  'image_bytes': len(result), 'image_sha256': hashlib.sha256(result).hexdigest(),
                  'payload': unpack_payload(parse_image(result).ramdisk)[0] if efi else None,
                  'hardware_tested': False}
        args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'{args.output}: {len(result)} bytes; autoboot={args.autoboot}')
        return 0
    except (OSError, ValueError) as error:
        parser.exit(1, f'build-boot: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
