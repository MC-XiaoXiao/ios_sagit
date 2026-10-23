#!/usr/bin/env python3
"""Start UFS export, control the exporter, or passively await its exact firmware."""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import termios
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))
from fastboot_usb import Fastboot
spec = importlib.util.spec_from_file_location('apfs_storage', ROOT/'scripts/apfs-storage.py')
storage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(storage)


def open_uart(path):
    fd = os.open(path, os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX|fcntl.LOCK_NB)
        attr = termios.tcgetattr(fd)
        attr[0] = attr[1] = attr[3] = 0
        attr[2] = termios.CS8|termios.CREAD|termios.CLOCAL
        attr[4] = attr[5] = termios.B115200
        attr[6][termios.VMIN] = attr[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attr)
        return fd
    except Exception:
        os.close(fd)
        raise


def exchange(fd, command, expected, timeout=5):
    # Clear an unfinished line and pace individual bytes: the target UART can
    # drop a burst while servicing USB storage requests.
    pending = ('\r'+command+'\r').encode('ascii') if command else b''
    end, next_tx = time.monotonic()+timeout, time.monotonic()
    received = bytearray()
    while time.monotonic() < end:
        if pending and time.monotonic() >= next_tx:
            count = os.write(fd, pending[:1])
            pending = pending[count:]
            next_tx = time.monotonic()+0.05
        if select.select([fd], [], [], 0.05)[0]:
            received.extend(os.read(fd, 65536))
            if expected.encode() in received:
                return bytes(received)
            if b'[mi6-storage] ERROR:' in received:
                raise RuntimeError(received.decode('utf8','replace'))
    raise TimeoutError(f'No exporter acknowledgement for {command!r}: '+received.decode('utf8','replace'))


def flush_host(device):
    storage.assert_unmounted(device)
    fd = os.open(device, os.O_RDWR|os.O_EXCL)
    try:
        storage.geometry(fd)
        os.fsync(fd)
    finally:
        os.close(fd)


def wait_firmware(fd, serial, seconds, uart):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            probe = subprocess.run(['fastboot', '-s', serial, 'getvar', 'product'],
                                   capture_output=True, text=True, timeout=min(3, max(0.1, end - time.monotonic())))
        except subprocess.TimeoutExpired:
            probe = None
        if probe is not None and probe.returncode == 0 and 'product: sagit' in probe.stdout + probe.stderr:
            return True
        until = min(end, time.monotonic() + 2)
        while time.monotonic() < until:
            if select.select([fd], [], [], min(0.2, max(0, until - time.monotonic())))[0]:
                uart.extend(os.read(fd, 65536))
    return False


def exported_disk(args):
    with args.device.open('rb') as device:
        sector, size = storage.geometry(device.fileno())
        primary, _, _, _ = storage.read_gpt(device.fileno(), sector, size)
        guid = str(uuid.UUID(bytes_le=primary['raw'][56:72])).upper()
        if args.disk_guid and guid != args.disk_guid.upper():
            raise ValueError('unexpected exported GPT identity')
    return guid


def usb_only(args):
    report = {'command': args.command, 'transport': 'usb', 'uart': None,
              'device': str(args.device), 'host_flushed': False, 'device_flushed': False}
    client = Fastboot(timeout=args.recovery_seconds, commands=report.setdefault('commands', []))
    try:
        if args.command == 'wait':
            serial = client.select(args.serial, wait=True)
            client.require_edk2(serial)
            report['firmware_recovered'] = True
        else:
            if args.command == 'start':
                serial = client.select(args.serial, wait=True)
                client.launch_efi(serial, args.payload)
            deadline = time.monotonic() + (60 if args.command == 'start' else 0)
            while True:
                try:
                    report['exported_disk_guid'] = exported_disk(args)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError('expected exported UFS device did not enumerate')
                    time.sleep(0.2)
            report['exporter_ready'] = True
            if args.command == 'flush':
                flush_host(args.device)
                report['host_flushed'] = True
                print('Host block device flushed. Restart the phone with its power button.')
        report['success'] = True
    except Exception as error:
        report['success'] = False
        report['error'] = str(error)
        raise
    finally:
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['start', 'status', 'reboot', 'wait', 'flush'])
    parser.add_argument('--no-uart', action='store_true', help='use USB only; flush then restart physically')
    parser.add_argument('--uart', type=Path,
                        default=Path('/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'))
    parser.add_argument('--serial', default='edk2-fastboot1')
    parser.add_argument('--payload', type=Path, default=ROOT/'build/usb-storage/LinuxSimpleMassStorage.efi')
    parser.add_argument('--device', type=Path,
                        default=Path('/dev/disk/by-id/usb-Qualcomm_sda_1234567890-0:5'))
    parser.add_argument('--disk-guid', help='optional expected GPT disk GUID; otherwise report the discovered GUID')
    parser.add_argument('--output', type=Path, default=ROOT/'build/usb-storage/control.json')
    parser.add_argument('--recovery-seconds', type=float, default=120,
                        help='bounded passive firmware wait after reboot or with wait')
    args = parser.parse_args()
    if not 0 < args.recovery_seconds <= 600:
        parser.error('--recovery-seconds must be greater than 0 and at most 600 seconds')
    if args.no_uart and args.command == 'reboot':
        parser.error('USB exporter has no USB reboot command; use flush --no-uart, then the power button')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.no_uart or args.command == 'flush':
        usb_only(args)
        return
    fd = open_uart(args.uart)
    uart = bytearray()
    report = {'command':args.command, 'uart':str(args.uart), 'firmware_serial':args.serial,
              'device':str(args.device), 'host_flushed':False,
              'device_flushed':False, 'firmware_recovered':False}
    try:
        if args.command == 'wait':
            report['passive_uart_only'] = True
            report['firmware_recovered'] = wait_firmware(fd, args.serial, args.recovery_seconds, uart)
            if not report['firmware_recovered']:
                raise TimeoutError('exact EDK2 Fastboot device has not reappeared')
        elif args.command == 'start':
            result = subprocess.run(['fastboot','-s',args.serial,'getvar','product'], capture_output=True, text=True, check=True, timeout=10)
            if 'product: sagit' not in result.stdout+result.stderr:
                raise ValueError('unexpected Fastboot product')
            subprocess.run(['fastboot','-s',args.serial,'stage',str(args.payload)], check=True, timeout=40)
            subprocess.run(['fastboot','-s',args.serial,'oem','run-efi'], check=True, timeout=10)
            uart += exchange(fd, '', '[mi6-storage] ready;', timeout=60)
            report['exporter_ready'] = True
            end = time.monotonic()+20
            while True:
                try:
                    with args.device.open('rb') as device:
                        sector, size = storage.geometry(device.fileno())
                        primary, _, _, _ = storage.read_gpt(device.fileno(), sector, size)
                        guid = str(uuid.UUID(bytes_le=primary['raw'][56:72])).upper()
                        if args.disk_guid and guid != args.disk_guid.upper():
                            raise ValueError('unexpected exported GPT identity')
                    report['exported_disk_guid'] = guid
                    break
                except OSError:
                    if time.monotonic() >= end:
                        raise TimeoutError('serial controller is ready but expected UFS LUN did not enumerate')
                    time.sleep(0.1)
        else:
            # Do not send a reboot word to a different serial console.
            uart += exchange(fd, 'status', '[mi6-storage] status: exporting UFS; serial-control=v1')
            report['exporter_verified'] = True
            if args.command == 'reboot':
                flush_host(args.device)
                report['host_flushed'] = True
                uart += exchange(fd, 'reboot', '[mi6-storage] flushed; rebooting to firmware', timeout=30)
                report['device_flushed'] = True
                # Firmware battery/profile initialization can exceed 45s after
                # repeated warm boots; keep capturing without sending commands.
                report['firmware_recovered'] = wait_firmware(fd, args.serial, args.recovery_seconds, uart)
                if not report['firmware_recovered']:
                    raise TimeoutError('storage was flushed but EDK2 Fastboot has not reappeared')
    finally:
        os.close(fd)
        args.output.with_suffix('.uart.txt').write_text(uart.decode('utf8','replace'))
        args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(uart.decode('utf8','replace'))
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()
