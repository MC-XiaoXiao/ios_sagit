"""Capture logs, run commands or open an interactive Mi 6 USB CDC console."""
import argparse
import errno
import fcntl
import math
import os
import json
from pathlib import Path
import select
import signal
import re
import secrets
import shlex
import struct
import sys
import termios
import time
import tty


def ports(serial=None, sysfs=Path('/sys/class/tty')):
    """Select an ACM interface alongside Apple's native DeviceMux interface."""
    found = []
    for tty in sorted(sysfs.glob('ttyACM*')):
        for device in (tty / 'device').resolve().parents:
            try:
                if (device / 'idVendor').read_text().strip() != '05ac':
                    continue
                identity = (device / 'serial').read_text().strip()
                if serial and identity != serial:
                    break
                mux = any((i / 'bInterfaceClass').read_text().strip() == 'ff' and
                          (i / 'bInterfaceSubClass').read_text().strip() == 'fe' and
                          (i / 'bInterfaceProtocol').read_text().strip() == '02'
                          for i in device.glob(device.name + ':*'))
                if mux:
                    found.append((Path('/dev') / tty.name, identity))
                break
            except OSError:
                continue
    return found


def open_port(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        attrs = termios.tcgetattr(fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.HUPCL
        attrs[4] = attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack('I', termios.TIOCM_DTR | termios.TIOCM_RTS))
        return fd
    except BaseException:
        os.close(fd)
        raise


def run_command(fd, command, receive, timeout=30.0, byte_delay=0.001):
    """Wait for an unpredictable result line, never the terminal's command echo."""
    token = 'MI6_USB_' + secrets.token_hex(16)
    payload = ("eval " + shlex.quote(command) + "; __mi6_usb_status=$?; printf '\\n" +
               token + ":%d\\n' \"$__mi6_usb_status\"\r").encode()
    deadline = time.monotonic() + timeout
    received = bytearray()
    pattern = re.compile(rb'(?:^|[\r\n])' + token.encode() + rb':([0-9]+)[\r\n]')
    position = 0
    while time.monotonic() < deadline:
        if position < len(payload):
            if select.select([], [fd], [], 0)[1]:
                try:
                    position += os.write(fd, payload[position:position + 1])
                except BlockingIOError:
                    pass
            received.extend(receive(fd, byte_delay))
        else:
            received.extend(receive(fd, min(0.2, max(0, deadline - time.monotonic()))))
        match = pattern.search(received)
        if match:
            return int(match[1])
    raise TimeoutError('USB command completion not received; execution state is unknown and the command was not retried')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=Path, help='explicit CDC tty; otherwise discover DeviceMux + ACM')
    parser.add_argument('--serial', help='native USB serial number for automatic discovery')
    parser.add_argument('--output', type=Path, required=True, help='append raw console bytes to this file')
    parser.add_argument('--seconds', type=float, help='stop after this many seconds, including reconnect waits')
    parser.add_argument('--quiet', action='store_true', help='save logs without mirroring to stdout')
    parser.add_argument('--interactive', action='store_true', help='forward this terminal to USB; Ctrl-] exits')
    parser.add_argument('--report', type=Path, help='write connection and command results as JSON')
    parser.add_argument('--require-bash', action='store_true', help='require a root Bash response before running commands')
    parser.add_argument('--wait', type=float, default=30, help='deadline for each command in seconds')
    parser.add_argument('--byte-delay', type=float, default=0.001, help='seconds between command bytes')
    parser.add_argument('commands', nargs='*', help='shell commands; nonzero results stop the batch')
    args = parser.parse_args(argv)
    if args.seconds is None and (args.commands or args.require_bash):
        args.seconds = 120
    if args.seconds is not None and (not math.isfinite(args.seconds) or args.seconds <= 0):
        parser.error('--seconds must be finite and positive')
    if args.port and args.serial:
        parser.error('--serial applies to automatic discovery; use it without --port')
    if args.interactive and (args.commands or args.quiet or not sys.stdin.isatty()):
        parser.error('--interactive requires a terminal and cannot be combined with commands or --quiet')
    if not math.isfinite(args.wait) or args.wait <= 0 or not math.isfinite(args.byte_delay) or args.byte_delay <= 0:
        parser.error('--wait and --byte-delay must be finite and positive')
    stopped = False
    def stop(signum, frame):
        nonlocal stopped
        stopped = True
    previous = {s: signal.signal(s, stop) for s in (signal.SIGINT, signal.SIGTERM)}
    fd = None
    permission_since = None
    started = time.monotonic()
    stdin_state = None
    batch_started = False
    report = {'transport': 'usb-cdc', 'uart_used': False, 'connections': [],
              'commands': [], 'bytes': 0, 'success': False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with args.output.open('ab', buffering=0) as log:
            def receive(port_fd, timeout):
                if stopped:
                    raise InterruptedError('USB console interrupted')
                if not select.select([port_fd], [], [], timeout)[0]:
                    return b''
                try:
                    data = os.read(port_fd, 65536)
                except BlockingIOError:
                    return b''
                if not data:
                    raise OSError(errno.ENODEV, 'CDC disconnected')
                log.write(data)
                report['bytes'] += len(data)
                if not args.quiet:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
                return data
            print('Waiting for Mi 6 USB console; output: ' + str(args.output), file=sys.stderr)
            if args.interactive:
                stdin_state = termios.tcgetattr(sys.stdin.fileno())
                tty.setraw(sys.stdin.fileno())
            while not stopped and (args.seconds is None or time.monotonic() - started < args.seconds):
                if fd is None:
                    candidates = [(args.port, '')] if args.port else ports(args.serial)
                    if len(candidates) > 1:
                        parser.error('multiple composite consoles; specify --serial or --port')
                    if not candidates:
                        permission_since = None
                        time.sleep(0.1)
                        continue
                    port, identity = candidates[0]
                    try:
                        fd = open_port(port)
                    except OSError as error:
                        # The tty appears before udev assigns its final group.
                        if error.errno == errno.EACCES:
                            if permission_since is None:
                                permission_since = time.monotonic()
                            if time.monotonic() - permission_since >= 5:
                                raise
                        elif error.errno not in (errno.ENOENT, errno.ENODEV, errno.ENXIO, errno.EIO):
                            raise
                        time.sleep(0.1)
                        continue
                    print(f'Connected: {port} {identity}', file=sys.stderr)
                    report['connections'].append({'port': str(port), 'serial': identity})
                    permission_since = None
                try:
                    if args.commands or args.require_bash:
                        batch_started = True
                        check = 'test -n "$BASH_VERSION" && test "$EUID" = 0' if args.require_bash else ':'
                        def command_timeout():
                            remaining = args.wait if args.seconds is None else min(args.wait, args.seconds - (time.monotonic() - started))
                            if remaining <= 0:
                                raise TimeoutError('USB command batch deadline expired')
                            return remaining
                        if run_command(fd, check, receive, command_timeout(), args.byte_delay):
                            raise RuntimeError('USB root Bash verification failed')
                        report['root_bash_verified'] = bool(args.require_bash)
                        for command in args.commands:
                            status = run_command(fd, command, receive, command_timeout(), args.byte_delay)
                            report['commands'].append({'command': command, 'status': status})
                            if status:
                                raise RuntimeError(f'USB command exited with status {status}: {command}')
                        report['success'] = True
                        return 0
                    receive(fd, 0.05 if args.interactive else 0.2)
                    if args.interactive and select.select([sys.stdin.fileno()], [], [], 0)[0]:
                        pending = os.read(sys.stdin.fileno(), 4096)
                        if not pending or b'\x1d' in pending:
                            report['success'] = True
                            return 0
                        while pending and not stopped:
                            if select.select([], [fd], [], 0.2)[1]:
                                pending = pending[os.write(fd, pending):]
                except OSError as error:
                    if batch_started:
                        raise
                    if error.errno in (errno.EAGAIN, errno.EINTR):
                        continue
                    if error.errno not in (errno.ENODEV, errno.EIO, errno.EBADF):
                        raise
                    os.close(fd)
                    fd = None
                    print('Disconnected; waiting for re-enumeration', file=sys.stderr)
            if (args.commands or args.require_bash) and not batch_started:
                raise TimeoutError('USB console did not enumerate before the deadline')
            report['success'] = not bool(args.commands or args.require_bash)
    except BaseException as error:
        report['error'] = str(error)
        raise
    finally:
        if stdin_state is not None:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, stdin_state)
        if fd is not None:
            os.close(fd)
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            report['elapsed_seconds'] = round(time.monotonic() - started, 3)
            args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    main()
