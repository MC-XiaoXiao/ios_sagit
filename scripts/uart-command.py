#!/usr/bin/env python3
"""Send paced commands to an existing UART Shell and capture its response."""
import argparse
import fcntl
import os
from pathlib import Path
import re
import select
import termios
import time

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='/dev/ttyUSB0')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--wait', type=float, default=4)
parser.add_argument('--live', action='store_true', help='flush raw UART capture continuously for live diagnostics')
parser.add_argument('--require-bash', action='store_true', help='verify a live root Bash reply before sending the command batch')
parser.add_argument('--byte-delay', type=float, default=0.025,
                    help='seconds between bytes; increase when kernel logging delays RX polling')
parser.add_argument('commands', nargs='+')
args = parser.parse_args()
if args.byte_delay <= 0:
    parser.error('--byte-delay must be positive')
fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    os.close(fd)
    parser.error('UART is in use by another command or bridge test')
attrs = termios.tcgetattr(fd)
attrs[0] = attrs[1] = attrs[3] = 0
attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
attrs[4] = attrs[5] = termios.B115200
attrs[6][termios.VMIN] = attrs[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, attrs)
data = bytearray()
args.output.parent.mkdir(parents=True, exist_ok=True)
live = args.output.with_suffix('.bin').open('wb') if args.live else None
def receive(timeout):
    if select.select([fd], [], [], timeout)[0]:
        chunk = os.read(fd, 65536)
        data.extend(chunk)
        if live:
            live.write(chunk)
            live.flush()
def transmit(command):
    next_tx = time.monotonic()
    for byte in (command + '\r').encode():
        while time.monotonic() < next_tx:
            receive(max(0, min(0.025, next_tx - time.monotonic())))
        os.write(fd, bytes([byte]))
        next_tx = time.monotonic() + args.byte_delay
    termios.tcdrain(fd)
try:
    if args.require_bash:
        transmit('\recho UART_BASH_READY version=$BASH_VERSION uid=$EUID')
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            receive(0.1)
            if re.search(rb'UART_BASH_READY version=\d+\.\d+[^\s]* uid=0(?:\r|\n)', data):break
        else:raise RuntimeError('Live root Bash not verified; command batch was not sent')
    for command in args.commands:
        print('UART:', command, flush=True)
        transmit(command)
        deadline = time.monotonic() + args.wait
        while time.monotonic() < deadline:
            receive(0.1)
finally:
    os.close(fd)
    if live:
        live.close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix('.bin').write_bytes(data)
    clean = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', bytes(data)).decode('utf8', 'replace')
    args.output.write_text(clean)
    print(clean[-6000:])
