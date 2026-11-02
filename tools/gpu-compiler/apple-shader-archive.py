#!/usr/bin/env python3
"""Inspect Apple GPU archives and rebuild exact specializations from matching AIR.

This never executes or translates Apple GPU instructions. It authenticates AIR
by the archive's base HASH, restores SBUF function constants and invokes the
same offline AIR -> NIR -> A540 compiler used by the driver.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

MAX_BYTES = 64 * 1024 * 1024

class FormatError(ValueError):
    pass

class Reader:
    def __init__(self, data: bytes):
        self.data = data
    def span(self, at: int, size: int) -> bytes:
        if at < 0 or size < 0 or at > len(self.data) or size > len(self.data) - at:
            raise FormatError('Archive range exceeds its container')
        return self.data[at:at + size]
    def uint(self, at: int, size: int = 4) -> int:
        return int.from_bytes(self.span(at, size), 'little')
    def string(self, at: int, size: int) -> str:
        value = self.span(at, size)
        if not value or value[-1] or b'\0' in value[:-1] or size > 4096:
            raise FormatError('Invalid archive symbol')
        return value[:-1].decode('utf-8')


def members(data: bytes) -> list[bytes]:
    r = Reader(data)
    if len(data) > MAX_BYTES or r.span(0, 4) != b'\xcf\xfa\xed\xfe' or r.uint(4) != 0x1000013 or r.uint(12) != 2:
        raise FormatError('Expected a bounded Apple GPU Mach-O executable')
    count, extent = r.uint(16), r.uint(20)
    r.span(32, extent)
    if not 1 <= count <= 4096:
        raise FormatError('Invalid load command count')
    at, end, section = 32, 32 + extent, None
    for _ in range(count):
        command, size = r.uint(at), r.uint(at + 4)
        if size < 8 or size % 8 or size > end - at:
            raise FormatError('Invalid load command')
        if command == 0x19:
            if size < 72 or size != 72 + 80 * r.uint(at + 64):
                raise FormatError('Invalid segment section table')
            start, length = r.uint(at + 40, 8), r.uint(at + 48, 8)
            r.span(start, length)
            for i in range(r.uint(at + 64)):
                pos = at + 72 + i * 80
                if r.span(pos, 16) != b'__metallib' + b'\0' * 6:
                    continue
                offset, length2 = r.uint(pos + 48), r.uint(pos + 40, 8)
                if section is not None or offset < max(start, end) or offset > start + length or length2 > start + length - offset:
                    raise FormatError('Invalid embedded metallib section')
                section = r.span(offset, length2)
        at += size
    if at != end or not section:
        raise FormatError('No embedded Metal metadata')
    r, at, output = Reader(section), 0, []
    while at < len(section):
        if r.span(at, 4) != b'MTLB':
            raise FormatError('Invalid aligned metallib member')
        size = r.uint(at + 16, 8)
        if size < 88:
            raise FormatError('Truncated metallib member')
        output.append(r.span(at, size))
        aligned = (at + size + 15) & ~15
        if any(r.span(at + size, aligned - at - size)):
            raise FormatError('Nonzero archive member padding')
        at = aligned
    return output


def tags(r: Reader, at: int, end: int) -> dict[str, bytes]:
    result = {}
    while at < end:
        tag = r.span(at, 4).decode('ascii')
        at += 4
        if tag == 'ENDT':
            if at != end:
                raise FormatError('Trailing tag data')
            return result
        size = r.uint(at, 2)
        at += 2
        if size > end - at or tag in result:
            raise FormatError('Invalid or duplicate metadata tag')
        result[tag] = r.span(at, size)
        at += size
    raise FormatError('Missing metadata terminator')


def specialization(r: Reader, start: int, size: int) -> tuple[str, str, list[list]]:
    # SBUF's FlatBuffer starts at an absolute 16-byte boundary. Padding and the
    # AIRC identifier are checked; no magic scanning in executable sections.
    base = (start + 15) & ~15
    if any(r.span(start, base - start)) or size < base - start + 8:
        raise FormatError('Invalid SBUF padding')
    f = Reader(r.span(base, size - (base - start)))
    if f.span(4, 4) != b'AIRC':
        raise FormatError('Unsupported SBUF identifier')
    def field(table: int, index: int, width: int = 4) -> int | None:
        vtable = table - int.from_bytes(f.span(table, 4), 'little', signed=True)
        length, object_size = f.uint(vtable, 2), f.uint(vtable + 2, 2)
        if length < 4 or length % 2 or object_size < 4:
            raise FormatError('Invalid AIRC vtable')
        f.span(vtable, length)
        f.span(table, object_size)
        offset = f.uint(vtable + 4 + index * 2, 2) if 4 + index * 2 < length else 0
        if offset and (offset < 4 or offset + width > object_size):
            raise FormatError('Invalid AIRC field offset')
        return table + offset if offset else None
    def pointer(at: int | None) -> int:
        if at is None:
            raise FormatError('Missing AIRC pointer')
        target = at + f.uint(at)
        f.span(target, 4)
        return target
    def string(at: int | None) -> str:
        target = pointer(at)
        return f.string(target + 4, f.uint(target) + 1)
    table = pointer(field(f.uint(0), 2))
    name, alias = string(field(table, 0)), string(field(table, 1))
    vector = pointer(field(table, 2))
    count = f.uint(vector)
    if count > 256:
        raise FormatError('Too many specialization constants')
    values, seen = [], set()
    for i in range(count):
        value = pointer(vector + 4 + i * 4)
        index_field, type_field, data_field = field(value, 0), field(value, 1, 1), field(value, 2)
        index = f.uint(index_field) if index_field is not None else 0
        kind = f.uint(type_field, 1) if type_field is not None else 0
        # Verified RenderBox AIRC ABI: uint4 union payload, a relative pointer
        # to a fixed 16-byte struct (no vector length prefix).
        # Other wire types need their own evidence; do not infer byte packing.
        if kind != 36 or data_field is None or index > 65535 or index in seen:
            raise FormatError('Unsupported or duplicate AIRC constant layout')
        seen.add(index)
        values.append([index, kind, f.span(pointer(data_field), 16).hex()])
    return name, alias, sorted(values)


def references(data: bytes) -> list[dict]:
    output = []
    for member_index, data in enumerate(members(data)):
        r = Reader(data)
        if r.uint(6, 2) != 2 or r.uint(16, 8) != len(data):
            raise FormatError('Invalid MTLB v2 metadata')
        sections = [(r.uint(24 + 16 * i, 8), r.uint(32 + 16 * i, 8)) for i in range(4)]
        for at, size in sections:
            if at < 88:
                raise FormatError('MTLB section overlaps header')
            r.span(at, size)
        start, size = sections[0]
        count, at, end = r.uint(start), start + 4, start + size + 4
        if not 1 <= count <= 8192 or end > sections[1][0]:
            raise FormatError('Invalid MTLB function list')
        functions = {}
        for _ in range(count):
            length = r.uint(at)
            if length < 8 or length > end - at:
                raise FormatError('Invalid function group')
            record = tags(r, at + 4, at + length)
            at += length
            if 'NAME' not in record:
                continue
            name = Reader(record['NAME']).string(0, len(record['NAME']))
            if name in functions or len(record.get('HASH', b'')) != 32:
                raise FormatError('Duplicate name or invalid function hash')
            functions[name] = record
        if at != end:
            raise FormatError('Extra MTLB function records')
        extra = tags(r, end, sections[1][0])
        if 'SLST' not in extra:
            continue
        if len(extra['SLST']) != 16:
            raise FormatError('Invalid SLST location')
        start, size = struct.unpack('<QQ', extra['SLST'])
        r.span(start, size)
        count, at, end = r.uint(start), start + 4, start + size
        if count > 8192:
            raise FormatError('Too many archived specializations')
        for _ in range(count):
            length = r.uint(at)
            if length < 16 or length > end - at or r.span(at + 4, 4) != b'SBUF':
                raise FormatError('Unsupported specialization group')
            payload_size = r.uint(at + 8)
            if payload_size != length - 16 or r.span(at + length - 4, 4) != b'ENDT':
                raise FormatError('Invalid SBUF extent')
            name, alias, values = specialization(r, at + 12, payload_size)
            if name not in functions or alias not in functions:
                raise FormatError('SBUF name is absent from function list')
            base_hash = functions[name]['HASH']
            link = functions[alias].get('SPCI', b'')
            if len(link) != 41 or link[0] or link[1:33] != base_hash or int.from_bytes(link[33:], 'little') != at - start:
                raise FormatError('SPCI does not authenticate the specialization source')
            output.append({'member': member_index, 'name': name, 'alias': alias,
                           'air_sha256': base_hash.hex(), 'constants': values})
            at += length
        if at != end:
            raise FormatError('Trailing SLST data')
    return output


def runtime_modules(data: bytes) -> list[tuple[str, bytes]]:
    """AGX runtime symbol table: count, NUL name/u32 offset, u32 size/AIR records."""
    r, at, entries = Reader(data), 4, []
    count = r.uint(0)
    if len(data) > MAX_BYTES or not 1 <= count <= 8192:
        raise FormatError('Invalid runtime symbol count')
    for _ in range(count):
        end = data.find(b'\0', at, min(len(data), at + 4096))
        if end < at:
            raise FormatError('Unterminated runtime symbol')
        name = r.string(at, end - at + 1)
        entries.append((name, r.uint(end + 1)))
        at = end + 5
    output, names = [], set()
    for name, offset in entries:
        if offset != at or name in names:
            raise FormatError('Invalid runtime module sequence')
        names.add(name)
        size = r.uint(offset)
        payload = r.span(offset + 4, size)
        wrapper = Reader(payload)
        if wrapper.span(0, 4) != b'\xde\xc0\x17\x0b' or wrapper.uint(4) or wrapper.uint(8) != 20:
            raise FormatError('Unsupported runtime AIR wrapper')
        if wrapper.span(20, 4) != b'BC\xc0\xde' or not 4 <= wrapper.uint(12) <= size - 20:
            raise FormatError('Invalid runtime bitcode extent')
        output.append((name, payload))
        at = offset + 4 + size
    if at != len(data):
        raise FormatError('Trailing runtime data')
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--air-library', action='append', type=Path, default=[])
    parser.add_argument('--helper', type=Path, default=Path('build/gpu-system-shaders/system-shader-tool'))
    parser.add_argument('--compile', action='store_true', help='Rebuild references after exact SHA256 source matching')
    parser.add_argument('--runtime', action='store_true', help='Extract AGX runtime helper AIR without treating it as stage entry code')
    args = parser.parse_args()
    if args.runtime and args.compile:
        parser.error('--runtime helpers are not independently dispatchable GPU entries')
    if args.archive.stat().st_size > MAX_BYTES:
        parser.error('archive exceeds 64 MiB')
    data = args.archive.read_bytes()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'archive': str(args.archive), 'sha256': hashlib.sha256(data).hexdigest(), 'hardware_tested': False}
    if args.runtime:
        rows = []
        for name, payload in runtime_modules(data):
            digest = hashlib.sha256(payload).hexdigest()
            (args.output / (digest + '.air')).write_bytes(payload)
            rows.append({'name': name, 'air_sha256': digest, 'bytes': len(payload)})
        report['runtime_helpers'] = rows
    else:
        rows = references(data)
        if not rows:
            raise FormatError('No recoverable precompiled specializations')
        report['specializations'] = rows
        sources = {}
        for path in args.air_library:
            if path.stat().st_size > MAX_BYTES:
                raise FormatError('AIR source library exceeds 64 MiB')
            payload = path.read_bytes()
            indexed = subprocess.run([str(args.helper.resolve()), 'index', str(path)], check=True, capture_output=True, text=True, timeout=30)
            for line in indexed.stdout.splitlines():
                digest, name, kind, offset, size = line.split('\t')
                air = Reader(payload).span(int(offset), int(size))
                if hashlib.sha256(air).hexdigest() != digest:
                    raise FormatError('AIR source SHA256 mismatch')
                sources[digest, bytes.fromhex(name).decode()] = air
        for row in rows:
            air = sources.get((row['air_sha256'], row['name']))
            row['source_matched'] = air is not None
            if not args.compile:
                continue
            if air is None:
                row['error'] = 'No hash-matching source AIR supplied'
                continue
            source = args.output / (row['air_sha256'] + '.air')
            source.write_bytes(air)
            constants = args.output / (str(row['member']) + '.constants')
            constants.write_text(''.join(f'{i} {t} {h}\n' for i, t, h in row['constants']))
            image = args.output / (str(row['member']) + '.a540')
            image.unlink(missing_ok=True)
            try:
                result = subprocess.run([str(args.helper.resolve()), 'compile', str(source), row['name'], str(constants), str(image)], capture_output=True, text=True, timeout=60)
                row['compiled'] = result.returncode == 0
                row['diagnostic'] = result.stderr.strip()
                if result.returncode:
                    row['error'] = result.stderr.strip() or f'compiler exited {result.returncode}'
                else:
                    row['image_sha256'] = hashlib.sha256(image.read_bytes()).hexdigest()
            except subprocess.TimeoutExpired:
                row['error'] = 'Offline compiler timeout'
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: len(report[key]) for key in ('specializations', 'runtime_helpers') if key in report}))
    return int(args.compile and any(not row.get('compiled') for row in rows))

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (FormatError, UnicodeError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from error
