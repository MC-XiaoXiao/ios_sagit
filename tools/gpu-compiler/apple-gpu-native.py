#!/usr/bin/env python3
"""Extract bounded native Apple GPU code, symbols and relocations for ISA lifting.

This tool does not translate unknown GPU instructions or treat recovered AIR as
machine-code translation. Native code always retains its architecture tag.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

_SPEC = importlib.util.spec_from_file_location('apple_shader_archive_reader', Path(__file__).with_name('apple-shader-archive.py'))
_ARCHIVE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_ARCHIVE)
Reader, FormatError, MAX_BYTES = _ARCHIVE.Reader, _ARCHIVE.FormatError, _ARCHIVE.MAX_BYTES


def macho(data: bytes) -> dict:
    r = Reader(data)
    if len(data) > MAX_BYTES or r.span(0, 4) != b'\xcf\xfa\xed\xfe' or r.uint(4) != 0x1000013 or r.uint(12) not in (1, 2):
        raise FormatError('Expected a bounded Apple GPU Mach-O object or executable')
    count, command_bytes = r.uint(16), r.uint(20)
    if not 1 <= count <= 4096 or command_bytes < 8 * count:
        raise FormatError('Invalid native load-command count')
    r.span(32, command_bytes)
    at, command_end = 32, 32 + command_bytes
    extent, sections, tables, symtab = command_end, [], [], None
    for _ in range(count):
        command, size = r.uint(at), r.uint(at + 4)
        if size < 8 or size % 8 or size > command_end - at:
            raise FormatError('Invalid native load command')
        if command == 0x19:
            if size < 72 or size != 72 + 80 * r.uint(at + 64):
                raise FormatError('Invalid native segment section table')
            offset, length = r.uint(at + 40, 8), r.uint(at + 48, 8)
            r.span(offset, length)
            extent = max(extent, offset + length)
            for i in range(r.uint(at + 64)):
                pos = at + 72 + 80 * i
                name = r.span(pos, 16).split(b'\0')[0].decode('ascii')
                segment = r.span(pos + 16, 16).split(b'\0')[0].decode('ascii')
                address, length = r.uint(pos + 32, 8), r.uint(pos + 40, 8)
                offset, flags = r.uint(pos + 48), r.uint(pos + 64)
                reloc, reloc_count = r.uint(pos + 56), r.uint(pos + 60)
                if r.uint(pos + 52) > 31:
                    raise FormatError('Invalid native section alignment')
                if length and offset < command_end:
                    raise FormatError('Native section overlaps commands')
                if reloc_count > 131072:
                    raise FormatError('Too many native relocations')
                # AGX1 objects can have metadata beyond segment.filesize. The
                # bounded section and relocation records define object extent.
                r.span(offset, length)
                r.span(reloc, 8 * reloc_count)
                extent = max(extent, offset + length, reloc + 8 * reloc_count)
                sections.append({'name': name, 'segment': segment, 'offset': offset, 'bytes': length,
                                 'address': address, 'flags': flags, 'relocation_offset': reloc,
                                 'relocation_count': reloc_count})
        elif command == 2:
            if size != 24 or symtab is not None:
                raise FormatError('Invalid native symbol table')
            offset, n, strings, length = (r.uint(at + i) for i in (8, 12, 16, 20))
            if n > 131072:
                raise FormatError('Too many native symbols')
            r.span(offset, n * 16)
            r.span(strings, length)
            extent = max(extent, offset + n * 16, strings + length)
            symtab = offset, n, strings, length
        elif command == 0xb:
            if size != 80:
                raise FormatError('Invalid native dynamic symbol table')
            for field, stride in ((32, 8), (40, 56), (48, 4), (56, 4), (64, 8), (72, 8)):
                offset, n = r.uint(at + field), r.uint(at + field + 4)
                if n > 131072:
                    raise FormatError('Too many dynamic symbol entries')
                r.span(offset, n * stride)
                extent = max(extent, offset + n * stride)
        elif command == 0x31:
            if size != 40:
                raise FormatError('Invalid native note command')
            offset, length = r.uint(at + 24, 8), r.uint(at + 32, 8)
            r.span(offset, length)
            extent = max(extent, offset + length)
        elif command not in (0x1b, 0x24, 0x25, 0x32):
            raise FormatError(f'Unsupported native load command {command:#x}')
        at += size
    if at != command_end:
        raise FormatError('Native load-command extent mismatch')
    ranges = []
    for section in sections:
        if not section['bytes']:
            continue
        start, stop = section['offset'], section['offset'] + section['bytes']
        if any(start < end and begin < stop for begin, end in ranges):
            raise FormatError('Overlapping native sections')
        ranges.append((start, stop))
    symbols = []
    if symtab:
        offset, n, strings, length = symtab
        names = r.span(strings, length)
        for i in range(n):
            name, kind, section, description, value = struct.unpack('<IBBHQ', r.span(offset + 16 * i, 16))
            if name >= length:
                raise FormatError('Native symbol string offset')
            end = names.find(b'\0', name)
            if end < name or end - name > 4096:
                raise FormatError('Native symbol string extent')
            if (kind & 14) == 14 and not 1 <= section <= len(sections):
                raise FormatError('Native symbol section index')
            symbols.append({'name': names[name:end].decode('utf-8'), 'kind': kind,
                            'section': section, 'description': description, 'value': value})
    return {'cpu_type': r.uint(4), 'cpu_subtype': r.uint(8), 'file_type': r.uint(12),
            'bytes': extent, 'sections': sections, 'symbols': symbols}


def native_objects(data: bytes) -> list[dict]:
    root = macho(data)
    if root['file_type'] == 1:
        return [dict(root, phase='object', offset=0)]
    output = []
    for section in root['sections']:
        if section['name'] not in ('__fragment', '__vertex', '__kernel', '__compute') or not section['bytes']:
            continue
        raw = Reader(data).span(section['offset'], section['bytes'])
        at = 0
        while at < len(raw):
            obj = macho(raw[at:])
            if obj['file_type'] != 1 or len(output) >= 8192:
                raise FormatError('Invalid native object sequence')
            output.append(dict(obj, phase=section['name'], offset=section['offset'] + at))
            end = at + obj['bytes']
            aligned = (end + 15) & ~15
            if aligned > len(raw) or any(raw[end:aligned]):
                raise FormatError('Invalid native object alignment padding')
            at = aligned
    if not output:
        raise FormatError('No native shader object sections')
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.archive.stat().st_size > MAX_BYTES:
        parser.error('archive exceeds 64 MiB')
    data = args.archive.read_bytes()
    rows = native_objects(data)
    args.output.mkdir(parents=True, exist_ok=True)
    for i, obj in enumerate(rows):
        payload = Reader(data).span(obj['offset'], obj['bytes'])
        obj['sha256'] = hashlib.sha256(payload).hexdigest()
        (args.output / f'{i}.gpu-object').write_bytes(payload)
        obj['code_sections'] = []
        for n, section in enumerate(obj['sections']):
            if not section['flags'] & 0x80000400 or not section['bytes']:
                continue
            code = Reader(payload).span(section['offset'], section['bytes'])
            name = f'{i}-{n}.gpu-code'
            (args.output / name).write_bytes(code)
            obj['code_sections'].append({'path': name, 'sha256': hashlib.sha256(code).hexdigest(), 'bytes': len(code)})
        obj['translation_status'] = 'requires verified opcode decoding and native shader ABI lowering'
    report = {'archive': str(args.archive), 'sha256': hashlib.sha256(data).hexdigest(),
              'hardware_tested': False, 'machine_code_translated': False, 'objects': rows}
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'native_objects': len(rows), 'code_bytes': sum(s['bytes'] for r in rows for s in r['code_sections']),
                      'machine_code_translated': False}))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
