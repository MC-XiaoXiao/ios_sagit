"""Upload development inputs to the bridge's USB service while its UI remains active."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import zlib

from project.device.fastboot import Fastboot


def upload(client: Fastboot, serial: str, kind: str, path: Path) -> dict:
    data = path.read_bytes()
    limit = min(client.limit(serial, 'max-download-size'), 64 * 1024 * 1024)
    if not data or len(data) > limit:
        raise ValueError(f'{kind}: input must contain 1..{limit} bytes')
    if kind == 'dt' and len(data) > 4 * 1024 * 1024:
        raise ValueError('dt: send the unpacked Apple DT, at most 4 MiB')
    if kind == 'ramdisk' and (len(data) < 16384 or len(data) % 16384):
        raise ValueError('ramdisk: image size must be a multiple of 16 KiB')
    crc = f'{zlib.crc32(data):08x}'
    client.run('stage', str(path.resolve()), serial=serial)
    client.run('oem', 'bridge', 'load', kind, crc, serial=serial)
    actual = client.getvar(serial, f'bridge-crc32:{kind}')
    if actual.lower() != crc:
        raise ValueError(f'{kind}: committed CRC differs: {actual} != {crc}')
    return {'kind': kind, 'path': str(path.resolve()), 'bytes': len(data), 'crc32': crc}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    parser.add_argument('--timeout', type=float, default=90)
    for kind in ('kernel', 'dt', 'ramdisk', 'trustcache'):
        parser.add_argument('--' + kind, type=Path)
    parser.add_argument('--reset', action='store_true', help='restore bundled inputs before uploads')
    parser.add_argument('--boot', action='store_true', help='boot after all requested uploads pass CRC verification')
    parser.add_argument('--flash-boot', type=Path, help='persist an Android v0 boot image; firmware verifies readback')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    if not any((args.kernel, args.dt, args.ramdisk, args.trustcache, args.reset, args.boot, args.flash_boot)):
        parser.error('specify payloads, --reset, --boot or --flash-boot')
    client = Fastboot(timeout=args.timeout)
    report = {'success': False, 'inputs': [], 'commands': client.commands}
    try:
        serial = client.select(args.serial, wait=True)
        if client.getvar(serial, 'bridge-service') != '1':
            raise ValueError('device is not running the bridge Fastboot service')
        report['serial'] = serial
        if args.reset:
            client.run('oem', 'bridge', 'reset', serial=serial)
        for kind in ('kernel', 'dt', 'ramdisk', 'trustcache'):
            if path := getattr(args, kind):
                report['inputs'].append(upload(client, serial, kind, path))
                print(f'{kind}: CRC32 verified', flush=True)
        if args.flash_boot:
            from project.build.boot_format import parse_image
            parse_image(args.flash_boot.read_bytes())
            client.flash_boot(serial, args.flash_boot)
            report['boot_flashed'] = True
        if args.boot:
            client.run('continue', serial=serial)
            report['boot_requested'] = True
        report['success'] = True
        return 0
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        report['error'] = str(error)
        print(str(error), flush=True)
        return 1
    finally:
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    raise SystemExit(main())
