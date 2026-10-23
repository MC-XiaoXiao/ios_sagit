#!/usr/bin/env python3
"""Summarize a native full-system .ips report without another device boot."""
import argparse
import bisect
from collections import Counter
import json
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--cache-inventory', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    decoder = json.JSONDecoder()
    text = args.report.read_text()
    header, end = decoder.raw_decode(text)
    report, _ = decoder.raw_decode(text[end:].lstrip())
    snapshot = report.get('stackshot', report)
    if 'processByPid' not in snapshot:
        parser.error('this report has no full-system stackshot')
    kernel = args.kernel.read_bytes()
    pos = 32
    kernel_uuid = None
    for _ in range(struct.unpack_from('<I', kernel, 16)[0]):
        command, length = struct.unpack_from('<II', kernel, pos)
        if command == 0x1b:
            kernel_uuid = kernel[pos + 8:pos + 24].hex()
        pos += length
    inventory = json.loads(args.cache_inventory.read_text())
    images = snapshot['binaryImages']
    for uid, base, kind in images:
        if kind == 'K' and uid.replace('-', '').lower() != kernel_uuid:
            parser.error('kernel UUID differs from the report; symbols would be misleading')
        if kind == 'S' and uid.replace('-', '').lower() != inventory['cache_uuid']:
            parser.error('shared-cache UUID differs from the report')
    symbols = []
    for line in subprocess.check_output(['llvm-nm', '-n', str(args.kernel)], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols.append((int(parts[0], 16), parts[2]))
    addresses = [a for a, _ in symbols]
    ranges = sorted((r['start'], r['end'], i['path'])
                    for i in inventory['images'] for r in i['text_ranges'])
    starts = [a for a, _, _ in ranges]

    def frame(pair):
        index, offset = pair
        uid, base, kind = images[index]
        address = base + offset
        value = {'address': hex(address), 'type': kind}
        if kind == 'K':
            at = bisect.bisect_right(addresses, address) - 1
            if at >= 0 and address - addresses[at] < 0x10000:
                value.update(symbol=symbols[at][1], offset=hex(address - addresses[at]))
        elif kind == 'S':
            at = bisect.bisect_right(starts, address) - 1
            if at >= 0 and address < ranges[at][1]:
                value['image'] = ranges[at][2]
        return value

    counts, tasks, throttled = Counter(), {}, []
    first_unlock = []
    for pid, task in snapshot['processByPid'].items():
        entry = {k: v for k, v in task.items() if k != 'threadById'}
        entry['threads'] = {}
        for tid, thread in task['threadById'].items():
            th = {k: v for k, v in thread.items() if k not in ('userFrames', 'kernelFrames')}
            th['user_frames'] = [frame(f) for f in thread.get('userFrames', [])]
            th['kernel_frames'] = [frame(f) for f in thread.get('kernelFrames', [])]
            names = {f['symbol'] for f in th['kernel_frames'] if 'symbol' in f}
            counts.update(names)
            identity = {'pid': int(pid), 'process': task['procname'], 'tid': int(tid)}
            if '_throttle_lowpri_io' in names:
                throttled.append(identity)
            if any(f['address'] == '0x19b109a28' for f in th['user_frames']):
                first_unlock.append(identity)
            entry['threads'][tid] = th
        tasks[pid] = entry
    summary = {'source': str(args.report), 'uptime': report.get('uptime'),
               'boot_uuid': report.get('bootSessionUUID'), 'tasks': len(tasks),
               'kernel_frame_counts': dict(counts.most_common()),
               'throttled_threads': throttled, 'first_unlock_waits': first_unlock}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'summary': summary, 'processes': tasks}, indent=2) + '\n')
    print(json.dumps({'source': str(args.report), 'uptime': summary['uptime'],
                      'tasks': len(tasks), 'throttled_threads': len(throttled),
                      'first_unlock_waits': first_unlock}, indent=2))


if __name__ == '__main__':
    main()
