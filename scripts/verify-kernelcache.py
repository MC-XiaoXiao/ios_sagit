#!/usr/bin/env python3
"""Fail-closed structural validation for generated arm64 prelinked caches.

This validates serialized structure and selected relocation invariants. It does
not certify driver behavior, crypto POST, or complete private ABI compatibility.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys

spec = importlib.util.spec_from_file_location("inventory", Path(__file__).with_name("analyze-kernelcache.py"))
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)

isa_spec = importlib.util.spec_from_file_location("armv8_compat", Path(__file__).with_name("armv8_compat.py"))
isa = importlib.util.module_from_spec(isa_spec)
isa_spec.loader.exec_module(isa)

integrity_spec = importlib.util.spec_from_file_location(
    "kernelcache_integrity", Path(__file__).with_name("kernelcache_integrity.py"))
integrity = importlib.util.module_from_spec(integrity_spec)
integrity_spec.loader.exec_module(integrity)


def need(condition, message):
    if not condition:
        raise ValueError(message)


def within(start, size, base, length):
    return 0 <= size and base <= start <= base + length and size <= base + length - start


def disjoint(ranges, context):
    ranges = sorted((start, start + size, name) for start, size, name in ranges if size)
    for a, b in zip(ranges, ranges[1:]):
        need(a[1] <= b[0], f"{context}: overlap {a[2]} and {b[2]}")


class Image:
    def __init__(self, data, base=0, label="kernel", outer=None):
        self.data, self.base, self.label, self.outer = data, base, label, outer
        self.segments, self.sections, self.commands = [], [], []
        need(within(base, 32, 0, len(data)), f"{label}: truncated header")
        h = struct.unpack_from("<8I", data, base)
        self.header = h
        need(h[0] == 0xfeedfacf and h[1] == 0x100000c and h[2] == 0,
             f"{label}: expected thin arm64/all Mach-O")
        need(h[3] == (11 if outer else 2), f"{label}: unexpected filetype {h[3]}")
        need(h[7] == 0, f"{label}: nonzero reserved header word")
        need(h[4] > 0 and within(base + 32, h[5], 0, len(data)), f"{label}: invalid command range")
        pos, end = base + 32, base + 32 + h[5]
        for i in range(h[4]):
            need(pos + 8 <= end, f"{label}: command {i} outside sizeofcmds")
            cmd, size = struct.unpack_from("<II", data, pos)
            need(size >= 8 and size % 8 == 0 and pos + size <= end,
                 f"{label}: invalid command {i} cmd={cmd:#x} size={size}")
            self.commands.append((cmd, pos, size))
            if cmd == 0x19:
                need(size >= 72, f"{label}: truncated segment command")
                s = struct.unpack_from("<II16sQQQQiiII", data, pos)
                name = s[2].rstrip(b"\0").decode()
                seg = dict(name=name, vm=s[3], vmsize=s[4], off=s[5], size=s[6], maxprot=s[7], prot=s[8])
                need(size == 72 + s[9] * 80, f"{label}/{name}: section count disagrees with command size")
                need(not any(x["name"] == name for x in self.segments), f"{label}: duplicate segment {name}")
                need(s[3] + s[4] <= 1 << 64 and s[6] <= s[4], f"{label}/{name}: invalid VM size")
                need(not s[6] or within(base + s[5], s[6], 0, len(data)), f"{label}/{name}: file range outside cache")
                need(s[8] & ~s[7] == 0 and s[7] & ~7 == 0, f"{label}/{name}: invalid protection")
                need(s[8] & 6 != 6, f"{label}/{name}: writable executable segment")
                if outer and s[4]:
                    owner = outer.owner(s[3], s[4])
                    need(owner is not None, f"{label}/{name}: VM range outside outer segments")
                    need(s[8] & ~owner["prot"] == 0, f"{label}/{name}: protection exceeds outer segment")
                    if s[6]:
                        need(outer.offset(s[3], s[6]) == base + s[5], f"{label}/{name}: file and VM mappings disagree")
                elif not outer and s[4]:
                    need(s[3] % 4096 == 0 and s[5] % 4096 == 0, f"{label}/{name}: outer segment not page aligned")
                self.segments.append(seg)
                for n in range(s[9]):
                    x = struct.unpack_from("<16s16sQQIIIIIIII", data, pos + 72 + 80 * n)
                    section = dict(name=x[0].rstrip(b"\0").decode(), segment=name, vm=x[2], size=x[3],
                                   off=x[4], align=x[5], reloff=x[6], nreloc=x[7], flags=x[8], r1=x[9], r2=x[10])
                    context = f"{label}/{name}/{section['name']}"
                    need(x[1].rstrip(b"\0").decode() == name, f"{context}: wrong segment name")
                    need(within(x[2], x[3], s[3], s[4]), f"{context}: VM outside segment")
                    need(x[5] <= 31 and x[2] % (1 << x[5]) == 0, f"{context}: invalid alignment")
                    if x[8] & 0xff in (1, 0xc, 0x12):
                        need(x[4] == 0, f"{context}: zerofill has a file offset")
                    elif x[3]:
                        need(within(x[4], x[3], s[5], s[6]) and x[4] - s[5] == x[2] - s[3],
                             f"{context}: file and VM section ranges disagree")
                    if x[7]:
                        need(within(base + x[6], x[7] * 8, 0, len(data)), f"{context}: relocation table outside file")
                        need(not outer, f"{context}: generated Kext retains section relocations")
                    self.sections.append(section)
            pos += size
        need(pos == end, f"{label}: ncmds does not consume sizeofcmds")
        disjoint([(s["vm"], s["vmsize"], s["name"]) for s in self.segments], f"{label} VM")
        disjoint([(s["off"], s["size"], s["name"]) for s in self.segments], f"{label} file")
        for seg in self.segments:
            disjoint([(s["vm"], s["size"], s["name"]) for s in self.sections if s["segment"] == seg["name"]],
                     f"{label}/{seg['name']} sections")
        self.check_tables()

    def owner(self, address, size=1):
        return next((s for s in self.segments if within(address, size, s["vm"], s["vmsize"])), None)

    def offset(self, address, size=1):
        seg = next((s for s in self.segments if within(address, size, s["vm"], s["size"])), None)
        need(seg is not None, f"{self.label}: {address:#x}+{size:#x} is not file backed")
        return self.base + seg["off"] + address - seg["vm"]

    def executable(self, address):
        seg = self.owner(address, 4)
        return address % 4 == 0 and seg is not None and bool(seg["prot"] & 4)

    def check_tables(self):
        symcount = None
        for cmd, pos, size in self.commands:
            context = f"{self.label}/command {cmd:#x}"
            if cmd == 2:
                need(size == 24, f"{context}: invalid symtab command")
                _, _, sym, count, strings, length = struct.unpack_from("<6I", self.data, pos)
                symcount = count
                need(within(self.base + sym, count * 16, 0, len(self.data))
                     and within(self.base + strings, length, 0, len(self.data)), f"{context}: symbol table outside file")
                if self.outer:
                    need((sym, count, strings, length) == (0, 0, 0, 0), f"{context}: stale Kext symtab")
                for i in range(count):
                    ix, ty, sec, desc, val = struct.unpack_from("<IBBHQ", self.data, self.base + sym + i * 16)
                    need(ix < length and self.data.find(b"\0", self.base + strings + ix, self.base + strings + length) != -1,
                         f"{context}: bad symbol string {i}")
                    if not ty & 0xe0:
                        need(ty & 0xe != 0, f"{context}: undefined/common symbol {i}")
                        if ty & 0xe == 0xe:
                            need(1 <= sec <= len(self.sections), f"{context}: invalid symbol section {i}")
            elif cmd in (0x1d, 0x1e, 0x26, 0x29, 0x2b, 0x2e, 0x80000033, 0x80000034):
                need(size == 16, f"{context}: invalid linkedit command")
                offset, length = struct.unpack_from("<II", self.data, pos + 8)
                need(not length or within(self.base + offset, length, 0, len(self.data)), f"{context}: payload outside file")
                if self.outer:
                    need(offset == 0 and length == 0, f"{context}: stale/unsupported Kext LINKEDIT payload")
            elif cmd == 0xb:
                need(size == 80, f"{context}: invalid dysymtab command")
                d = struct.unpack_from("<20I", self.data, pos)
                if self.outer:
                    need(not any(d[2:]), f"{context}: unconsumed Kext dynamic symbols/relocations")
                else:
                    need(symcount is not None, f"{context}: dysymtab without preceding symtab")
                    for ix, count in [(d[2], d[3]), (d[4], d[5]), (d[6], d[7])]:
                        need(ix + count <= symcount, f"{context}: invalid symbol index range")
                    need(d[7] == 0, f"{context}: undefined dynamic symbols")
                    for offset, count, unit in [(d[8], d[9], 8), (d[10], d[11], 56), (d[12], d[13], 4),
                                                (d[14], d[15], 4), (d[16], d[17], 8), (d[18], d[19], 8)]:
                        need(not count or within(offset, count * unit, 0, len(self.data)), f"{context}: table outside file")
                    text = next((s for s in self.segments if s["name"] == "__TEXT"), None)
                    need(text is not None, f"{context}: missing kernel TEXT")
                    for offset, count, external in [(d[16], d[17], True), (d[18], d[19], False)]:
                        for n in range(count):
                            address, bits = struct.unpack_from("<iI", self.data, offset + n * 8)
                            need(address >= 0, f"{context}: unsupported scattered/negative relocation {n}")
                            index, is_external = bits & 0xffffff, bool(bits & (1 << 27))
                            need(is_external == external, f"{context}: wrong relocation table kind {n}")
                            need(index < symcount if external else index <= len(self.sections),
                                 f"{context}: invalid relocation symbol/section {n}")
                            self.offset(text["vm"] + address, 1 << ((bits >> 25) & 3))
        for section in self.sections:
            if section["flags"] & 0xff in (9, 10):
                need(section["size"] % 8 == 0, f"{self.label}: partial structor pointer")
                offset = self.offset(section["vm"], section["size"])
                for n in range(0, section["size"], 8):
                    pointer = struct.unpack_from("<Q", self.data, offset + n)[0]
                    need((self.outer or self).executable(pointer), f"{self.label}: invalid structor {pointer:#x}")
            if self.outer and section["name"] == "__got":
                need(section["size"] % 8 == 0, f"{self.label}: partial GOT pointer")
                offset = self.offset(section["vm"], section["size"])
                for n in range(0, section["size"], 8):
                    pointer = struct.unpack_from("<Q", self.data, offset + n)[0]
                    need(pointer and self.outer.owner(pointer) is not None,
                         f"{self.label}: unresolved/unmapped GOT entry {n // 8}: {pointer:#x}")
            if self.outer and section["name"] == "__stubs":
                need(section["r2"] == 12 and section["size"] % 12 == 0,
                     f"{self.label}: unsupported/partial ARM64 import stub")
                offset = self.offset(section["vm"], section["size"])
                got = next((s for s in self.sections if s["name"] == "__got"), None)
                need(got is not None, f"{self.label}: import stubs without GOT")
                for n in range(0, section["size"], 12):
                    adrp, ldr, branch = struct.unpack_from("<3I", self.data, offset + n)
                    reg = adrp & 31
                    need(adrp & 0x9f000000 == 0x90000000 and ldr & 0xffc00000 == 0xf9400000
                         and ldr & 31 == reg and (ldr >> 5) & 31 == reg
                         and branch & 0xfffffc1f == 0xd61f0000 and (branch >> 5) & 31 == reg,
                         f"{self.label}: unsupported import stub at {section['vm'] + n:#x}")
                    immediate = ((adrp >> 5) & 0x7ffff) * 4 + ((adrp >> 29) & 3)
                    if immediate & (1 << 20):
                        immediate -= 1 << 21
                    slot = ((section["vm"] + n) & ~4095) + immediate * 4096 + ((ldr >> 10) & 4095) * 8
                    need(within(slot, 8, got["vm"], got["size"]), f"{self.label}: stub references wrong GOT slot {slot:#x}")
                    function = struct.unpack_from("<Q", self.data, self.offset(slot, 8))[0]
                    need(self.outer.executable(function), f"{self.label}: stub target is not executable: {function:#x}")


def check_kcgen_walk(data, kernel, images):
    """Mirror XNU kalloc_type_view_parse's KCGEN header walk, including padding."""
    section = next((s for s in kernel.sections if s['segment'] == '__PRELINK_TEXT' and s['name'] == '__text'), None)
    need(section is not None, 'KCGEN: missing PRELINK_TEXT/__text section')
    # Resource-only Mach-Os have no mapped segment; invert the outer mapping.
    by_address = {next(s['vm'] + image.base - s['off'] for s in kernel.segments
                       if within(image.base, 32, s['off'], s['size'])): image for image in images.values()}
    cursor, end, seen = section['vm'], section['vm']+section['size'], set()
    while cursor < end:
        off = kernel.offset(cursor, 4)
        magic = struct.unpack_from('<I', data, off)[0]
        if magic == 0:
            need(not any(data[off:kernel.offset(end-1)+1]),
                 f'KCGEN: interior zero gap at {cursor:#x}; XNU treats it as end of Kexts')
            break
        need(magic == 0xfeedfacf and cursor in by_address, f'KCGEN: unlisted/invalid Kext header at {cursor:#x}')
        image = by_address[cursor]; seen.add(cursor)
        text = next((s for s in image.segments if s['name'] == '__TEXT'), None)
        size = text['size'] if text else image.header[5]
        if text:
            need(any(cmd == 0x1e for cmd, pos, length in image.commands),
                 f'{image.label}: KCGEN requires split-segment marker')
        stride = (size+63) & ~63
        need(stride >= 32+image.header[5] and stride <= end-cursor,
             f'{image.label}: invalid KCGEN header stride')
        cursor += stride
    need(seen == set(by_address), 'KCGEN: header walk does not visit every serialized Kext')



def check_vm_protection_boundaries(kernel, page_size=16384):
    """Different protections cannot be represented inside one software VM page."""
    segments = sorted((s for s in kernel.segments if s['vmsize']), key=lambda s: s['vm'])
    for left, right in zip(segments, segments[1:]):
        last_page = (left['vm'] + left['vmsize'] - 1) // page_size
        first_page = right['vm'] // page_size
        need(last_page < first_page or left['prot'] == right['prot'],
             f"{left['name']}/{right['name']}: different protections share a VM page")

def check_local_constants(images, inputs):
    """Compare local constant data except declared pointer relocation sites."""
    checked, seen = {}, set()
    for name, path in inputs:
        need(name in images and name not in seen, f"{name}: missing/duplicate local Kext input")
        seen.add(name)
        original = Path(path).read_bytes()
        need(len(original) >= 32, f"{name}: truncated local Kext")
        h = struct.unpack_from('<8I', original)
        need(h[:4] == (0xfeedfacf, 0x100000c, 0, 11), f"{name}: invalid local Kext Mach-O")
        need(within(32, h[5], 0, len(original)), f"{name}: invalid local load commands")
        sections, relocations, text_base, pos = [], [], None, 32
        for _ in range(h[4]):
            need(pos + 8 <= 32 + h[5], f"{name}: truncated local command")
            cmd, size = struct.unpack_from('<II', original, pos)
            need(size >= 8 and pos + size <= 32 + h[5], f"{name}: invalid local command")
            if cmd == 0x19:
                need(size >= 72, f"{name}: short local segment")
                segment = original[pos+8:pos+24].rstrip(b'\0').decode()
                count = struct.unpack_from('<I', original, pos+64)[0]
                need(size == 72 + count*80, f"{name}: invalid local section count")
                if segment == '__TEXT':
                    text_base = struct.unpack_from('<Q', original, pos+24)[0]
                for i in range(count):
                    s = struct.unpack_from('<16s16sQQIIIIIIII', original, pos+72+80*i)
                    key = (segment, s[0].rstrip(b'\0').decode())
                    if segment == '__DATA_CONST' or key == ('__TEXT', '__const'):
                        need(within(s[4], s[3], 0, len(original)), f"{name}: local constants outside file")
                        sections.append((key, s[2], s[3], s[4]))
            elif cmd == 0xb:
                need(size == 80, f"{name}: invalid local relocation command")
                d = struct.unpack_from('<20I', original, pos)
                for offset, count in ((d[16], d[17]), (d[18], d[19])):
                    need(within(offset, count*8, 0, len(original)), f"{name}: local relocations outside file")
                    for i in range(count):
                        address, bits = struct.unpack_from('<iI', original, offset+8*i)
                        need(address >= 0 and bits >> 28 == 0 and not bits & (1 << 24)
                             and (bits >> 25) & 3 == 3, f"{name}: unsupported local pointer relocation")
                        if bits & 0xffffff or bits & (1 << 27):
                            relocations.append(address)
            pos += size
        need(pos == 32 + h[5] and text_base is not None, f"{name}: incomplete local commands")
        pointer_bytes = {text_base + address + i for address in relocations for i in range(8)}
        image, byte_count = images[name], 0
        for key, address, size, offset in sections:
            target = next((s for s in image.sections if (s['segment'], s['name']) == key), None)
            need(target is not None and target['size'] == size, f"{name}/{key}: local constants changed size")
            final = image.offset(target['vm'], size)
            for i in range(size):
                if address + i not in pointer_bytes:
                    need(original[offset+i] == image.data[final+i],
                         f"{name}/{key}: scalar constant changed at {address+i:#x}")
                    byte_count += 1
        checked[name] = {'constant_bytes_preserved': byte_count, 'source_sha256': hashlib.sha256(original).hexdigest()}
    return checked


def validate(path, expected_ids=(), source=None, local=None, local_kexts=()):
    data = path.read_bytes()
    kernel = Image(data)
    check_vm_protection_boundaries(kernel)
    _, kexts = inventory.inspect(path)
    by_id = {k["CFBundleIdentifier"]: k for k in kexts}
    need(len(by_id) == len(kexts) and len(kexts) > 0, "duplicate or empty Kext inventory")
    if expected_ids:
        need(set(by_id) == set(expected_ids), "selected Bundle ID set differs from serialized set")
    loaded, ranges, images = set(), [], {}
    for name, kext in by_id.items():
        libraries = inventory.architecture_libraries(kext)
        for dependency in libraries:
            need(dependency in by_id, f"{name}: missing dependency {dependency}")
            requested = libraries[dependency]
            supplied = by_id[dependency].get("CFBundleVersion", "")
            def version(text):
                match = re.fullmatch(r"(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:(d|a|b|fc)(\d+))?", text)
                need(match is not None, f"{name}: unsupported/empty dependency version {text!r}")
                a, b, c, stage, number = match.groups()
                return (int(a), int(b or 0), int(c or 0), {"d": 0, "a": 1, "b": 2, "fc": 3, None: 4}[stage], int(number or 0))
            need(version(requested) <= version(supplied), f"{name}: dependency {dependency} version too old")
            compatible = inventory.architecture_property(by_id[dependency], "OSBundleCompatibleVersion")
            if compatible:
                need(version(compatible) <= version(requested), f"{name}: dependency {dependency} incompatible version floor")
        address = kext.get("_PrelinkExecutableLoadAddr", 0)
        if not address:
            need(not kext.get("CFBundleExecutable"), f"{name}: executable without load address")
            continue
        need(address not in loaded, f"{name}: duplicate load address")
        loaded.add(address)
        need(address == kext.get("_PrelinkExecutableSourceAddr"), f"{name}: source/load address mismatch")
        size = kext.get("_PrelinkExecutableSize", 0)
        need(size >= 32, f"{name}: missing executable size")
        owner = kernel.owner(address, size)
        need(owner is not None and owner["name"] == "__PRELINK_TEXT", f"{name}: header range outside PRELINK_TEXT")
        image = Image(data, kernel.offset(address, size), name, kernel)
        need(32 + image.header[5] <= size, f"{name}: load commands exceed executable size")
        ranges.append((address, size, name + " header"))
        images[name] = image
        text = next((s for s in image.segments if s["name"] == "__TEXT"), None)
        if text:
            need(text["vm"] == address and text["off"] == 0 and text["size"] == size,
                 f"{name}: executable size/header segment mismatch")
        else:
            need(kext.get("OSKernelResource") is True, f"{name}: non-resource without TEXT")
            need(not any(s["vmsize"] for s in image.segments), f"{name}: resource retains a mapped payload")
        kmod = kext.get("_PrelinkKmodInfo", 0)
        if kmod:
            off = kernel.offset(kmod, 196)
            try:
                kname = data[off + 16:off + 80].split(b"\0", 1)[0].decode()
            except UnicodeError as error:
                raise ValueError(f"{name}: corrupted kmod identity at {kmod:#x}: {error}") from error
            need(kname == name, f"{name}: kmod name mismatch")
            version = data[off + 80:off + 144].split(b"\0", 1)[0].decode()
            need(version == kext.get("CFBundleVersion"), f"{name}: kmod version mismatch")
            need(struct.unpack_from("<i", data, off + 8)[0] == 1, f"{name}: wrong kmod info version")
            need(struct.unpack_from("<Q", data, off + 156)[0] == address, f"{name}: kmod address mismatch")
            for delta in (180, 188):
                function = struct.unpack_from("<Q", data, off + delta)[0]
                need(function == 0 or image.executable(function), f"{name}: invalid kmod start/stop {function:#x}")
        else:
            need(kext.get("OSKernelResource") is True, f"{name}: executable without kmod info")
    disjoint(ranges, "Kext header ranges")
    for segment_name in ("__TEXT", "__TEXT_EXEC", "__DATA", "__DATA_CONST"):
        disjoint([(s["vm"], s["vmsize"], name) for name, image in images.items()
                  for s in image.segments if s["name"] == segment_name], f"Kext {segment_name}")
    entry = None
    for cmd, pos, size in kernel.commands:
        if cmd == 5:
            need(size == 288, "unexpected ARM64 thread command layout")
            entry = struct.unpack_from("<Q", data, pos + 272)[0]
    need(entry is not None and kernel.executable(entry), "kernel entry is missing or not executable")
    checks = ["Mach-O commands", "segments and sections", "VM protection boundaries", "file/VM mappings and overlap",
              "symbol/relocation ranges, indices and relocation sites", "Kext metadata, versions and dependency closure",
              "Kext segment ownership", "structor/GOT pointers and ARM64 import stubs", "kmod records", "kernel entry"]
    local_constants = check_local_constants(images, local_kexts)
    if local_constants:
        checks.append("local Kext scalar constants preserved outside explicit pointer relocation sites")
    check_kcgen_walk(data, kernel, images)
    checks.append("XNU KCGEN Kext header walk and terminal zero padding")
    isa_report = isa.audit(data, kernel, images)
    checks.append("Kext ARMv8-A+CRC+Crypto ISA and regenerated static helpers")
    integrity_report = integrity.audit(data, images, source)
    checks.append("corecrypto final executable-section HMAC")
    if local:
        original = local.read_bytes()
        template = Image(original)
        if isa_report['patched']:
            need(b'MI6_ARMV8_AOT_V1\0' in original and b'__ARMV8_EXEC\0' in original,
                 "kernel: native template lacks ARMv8 helper mapping support")
        packed = {"__PRELINK_TEXT", "__PLK_TEXT_EXEC", "__PRELINK_DATA", "__PLK_DATA_CONST",
                  "__PRELINK_INFO", "__PLK_LLVM_COV", "__PLK_LINKEDIT"}
        added = any(s["name"] == isa.SEGMENT for s in kernel.segments)
        growth = 232 if added else 0
        need(kernel.header[4] == template.header[4] + int(added)
             and kernel.header[5] == template.header[5] + growth,
             "kernel: unexpected native header growth")
        need(kernel.header[:4] == template.header[:4] and kernel.header[6:] == template.header[6:],
             "kernel: native header fields changed")
        if added:
            end = 32 + template.header[5]
            need(kernel.commands[-1] == (0x19, end, 232)
                 and data[end + 8:end + 24].rstrip(b'\0') == isa.SEGMENT.encode()
                 and not any(original[end:end + growth]), "kernel: helper command did not use zeroed header padding")
            need(end + growth <= min(s['off'] for s in template.sections if s['off'] and s['size']),
                 "kernel: helper command overwrote native section data")
        for segment in template.segments:
            if segment["name"] in packed:
                continue
            actual = next((s for s in kernel.segments if s["name"] == segment["name"]), None)
            need(actual == segment, f"kernel: native segment metadata changed: {segment['name']}")
            skip = 32 + kernel.header[5] if segment["off"] == 0 else 0
            start, end = segment["off"] + skip, segment["off"] + segment["size"]
            if data[start:end] != original[start:end]:
                first = next(i for i in range(start, end) if data[i] != original[i])
                raise ValueError(f"kernel: native bytes changed: {segment['name']} at file {first:#x}; "
                                 f"expected={original[first:first + 16].hex()} actual={data[first:first + 16].hex()}")
        original_commands = [(c, p, n) for c, p, n in template.commands if c != 0x19]
        output_commands = [(c, p, n) for c, p, n in kernel.commands if c != 0x19]
        need(len(original_commands) == len(output_commands), "kernel: native load commands changed")
        for (c, p, n), (c2, p2, n2) in zip(original_commands, output_commands):
            need(c == c2 and original[p:p + n] == data[p2:p2 + n2], "kernel: native load command changed")
        checks.append("native kernel segment contents and non-prelink commands preserved")
    if source:
        # Independent preservation check against the source's complete standard
        # tables. Offsets are relative to each Kext TEXT_EXEC, not to its name map.
        original = source.read_bytes()
        meta, source_kexts = inventory.inspect(source)
        isa_report.update(isa.audit_source(data, kernel, images, original, meta, source_kexts))
        for k in source_kexts:
            name = k["CFBundleIdentifier"]
            if name in by_id:
                fields = {"CFBundleVersion", "OSBundleLibraries", "OSBundleCompatibleVersion", "IOKitPersonalities", "OSKernelResource"}
                # The source's fallback and every per-architecture dictionary
                # must survive, even those not selected by this arm64 build.
                fields.update(field for field in set(k) | set(by_id[name])
                              if field.startswith(("OSBundleLibraries_", "OSBundleCompatibleVersion_")))
                for field in sorted(fields):
                    need(k.get(field) == by_id[name].get(field), f"{name}: source {field} changed during serialization")
        def source_offset(address, length):
            for s in meta["segments"]:
                if within(address, length, int(s["vmaddr"], 16), s["filesize"]):
                    return s["fileoff"] + address - int(s["vmaddr"], 16)
            raise ValueError("source Kext header outside source cache")
        # Obtain source Kext segments without enforcing generated-image rules.
        constants = Path(__file__).parents[1] / "kernel/osfmk/corecrypto/ccsha256_K.c"
        words = re.findall(r"0x[0-9a-fA-F]{8}", constants.read_text())
        need(len(words) == 64, "cannot obtain complete independent SHA256 constants")
        table = struct.pack("<64I", *(int(x, 16) for x in words))
        count = 0
        for k in source_kexts:
            name = k["CFBundleIdentifier"]
            if name not in images or k.get("OSKernelResource"):
                continue
            start = source_offset(k["_PrelinkExecutableLoadAddr"], 32)
            h = struct.unpack_from("<8I", original, start)
            pos = start + 32
            for _ in range(h[4]):
                cmd, size = struct.unpack_from("<II", original, pos)
                if cmd == 0x19:
                    s = struct.unpack_from("<II16sQQQQiiII", original, pos)
                    if s[2].rstrip(b"\0") == b"__TEXT_EXEC":
                        off = source_offset(s[3], s[6])
                        blob = original[off:off + s[6]]
                        relative = blob.find(table)
                        target = next(x for x in images[name].segments if x["name"] == "__TEXT_EXEC")
                        while relative != -1:
                            dest = images[name].offset(target["vm"] + relative, len(table))
                            need(data[dest:dest + len(table)] == table, f"{name}: SHA256 table corrupted at {relative:#x}")
                            count += 1
                            relative = blob.find(table, relative + len(table))
                pos += size
        checks.append(f"source SHA256 constant tables preserved: {count}")
    return {"status": "passed", "errors": [], "warnings": [], "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
            "kext_count": len(kexts), "bundle_ids": sorted(by_id), "entry": hex(entry), "checks": checks,
            "isa": isa_report,
            "integrity": integrity_report,
            "local_constants": local_constants,
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest() if source else None,
            "local_kernel_sha256": hashlib.sha256(local.read_bytes()).hexdigest() if local else None,
            "scope": "offline structure and listed invariants; runtime/private ABI/crypto POST are not certified"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernelcache", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--expected-id", action="append", default=[])
    parser.add_argument("--source-kernelcache", type=Path)
    parser.add_argument("--local-kernel", type=Path)
    parser.add_argument("--local-kext", action="append", nargs=2, default=[], metavar=("BUNDLE_ID", "EXECUTABLE"))
    args = parser.parse_args()
    try:
        result = validate(args.kernelcache, args.expected_id, args.source_kernelcache, args.local_kernel, args.local_kext)
    except (ValueError, KeyError, struct.error, OSError, UnicodeError, ETError) as error:
        result = {"status": "failed", "errors": [str(error)]}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result["status"] == "passed" else 1


ETError = inventory.ET.ParseError
if __name__ == "__main__":
    sys.exit(main())
