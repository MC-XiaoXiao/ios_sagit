#!/usr/bin/env python3
"""Ahead-of-time Kext ISA lowering for SDM835 (ARMv8-A + CRC + AES/SHA2).

Helpers are ordinary mapped kernel code reached with B, never an exception
handler. Unsupported/undecodable executable words fail closed. Embedded records
allow the offline verifier to regenerate and check every replacement.
"""
import argparse
import collections
import ctypes as C
import ctypes.util
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct

PROFILE = 'armv8-a+crc+crypto'
SEGMENT = '__ARMV8_EXEC'
VERSION = 1


def require(ok, message):
    if not ok:
        raise ValueError(message)


class Decoder:
    def __init__(self, extended=False):
        name = os.environ.get('KERNELCACHE_PATCHER_LLVM_LIBRARY') or next(
            (found for candidate in ('LLVM', 'LLVM-23', 'LLVM-22', 'LLVM-21', 'LLVM-20', 'LLVM-19', 'LLVM-18')
             if (found := ctypes.util.find_library(candidate))), None)
        require(name is not None, 'ISA gate requires the LLVM AArch64 disassembler (libLLVM)')
        self.lib = C.CDLL(name)
        for suffix in ('TargetInfo', 'TargetMC', 'Disassembler'):
            getattr(self.lib, 'LLVMInitializeAArch64' + suffix)()
        create = self.lib.LLVMCreateDisasmCPUFeatures
        create.restype = C.c_void_p
        create.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_void_p, C.c_int, C.c_void_p, C.c_void_p]
        features = '+v8a,+crc,+aes,+sha2'
        if extended:
            features += ',+v9.6a,+fullfp16,+sha3,+sm4,+pauth,+sve2,+sme2,+mte,+lse,+rcpc'
        self.ctx = create(b'aarch64', b'generic', features.encode(), None, 0, None, None)
        require(self.ctx, 'cannot initialize LLVM AArch64 decoder')
        self.lib.LLVMDisasmInstruction.argtypes = [C.c_void_p, C.c_void_p, C.c_uint64, C.c_uint64, C.c_void_p, C.c_size_t]
        self.lib.LLVMDisasmInstruction.restype = C.c_size_t
        self.lib.LLVMDisasmDispose.argtypes = [C.c_void_p]
        self.out = C.create_string_buffer(256)
        if not extended:
            require(self.decode(0x4e284a00) is not None and self.decode(0xce608000) is None
                    and self.decode(0xf8210062) is None,
                    'LLVM decoder does not enforce the requested ARMv8-A profile')

    def decode(self, word):
        buf = C.c_uint32(word)
        size = self.lib.LLVMDisasmInstruction(self.ctx, C.byref(buf), 4, 0, self.out, len(self.out))
        return self.out.value.decode().strip() if size == 4 else None

    def __del__(self):
        if getattr(self, 'ctx', None):
            self.lib.LLVMDisasmDispose(self.ctx)


class Assembler:
    def __init__(self):
        name = ctypes.util.find_library('keystone')
        require(name is not None, 'ISA lowering requires libkeystone')
        self.lib = C.CDLL(name)
        self.handle = C.c_void_p()
        self.lib.ks_open.argtypes = [C.c_int, C.c_int, C.POINTER(C.c_void_p)]
        require(self.lib.ks_open(2, 0, C.byref(self.handle)) == 0, 'cannot initialize ARM64 assembler')
        self.lib.ks_asm.argtypes = [C.c_void_p, C.c_char_p, C.c_uint64, C.POINTER(C.c_void_p), C.POINTER(C.c_size_t), C.POINTER(C.c_size_t)]
        self.lib.ks_free.argtypes = [C.c_void_p]
        self.lib.ks_close.argtypes = [C.c_void_p]

    def assemble(self, lines):
        encoded, size, count = C.c_void_p(), C.c_size_t(), C.c_size_t()
        source = '\n'.join(lines).encode()
        result = self.lib.ks_asm(self.handle, source, 0, C.byref(encoded), C.byref(size), C.byref(count))
        require(result == 0, 'cannot assemble ARMv8 helper: ' + source.decode())
        try:
            return C.string_at(encoded, size.value)
        finally:
            self.lib.ks_free(encoded)

    def __del__(self):
        if getattr(self, 'handle', None):
            self.lib.ks_close(self.handle)


def branch(site, target):
    delta = target - site
    require(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27),
            f'ARMv8 helper branch out of range: {site:#x} -> {target:#x}')
    return struct.pack('<I', 0x14000000 | ((delta // 4) & 0x3ffffff))


def crypto_kind(word):
    for mask, value, name in (
        (0xffe0fc00, 0xce608000, 'sha512h'),
        (0xffe0fc00, 0xce608400, 'sha512h2'),
        (0xfffffc00, 0xcec08000, 'sha512su0'),
        (0xffe0fc00, 0xce608800, 'sha512su1'),
        (0xffe08000, 0xce000000, 'eor3'),
        (0xffe08000, 0xce200000, 'bcax'),
        (0xffe0fc00, 0xce608c00, 'rax1'),
        (0xffe00000, 0xce800000, 'xar'),
    ):
        if word & mask == value:
            return name
    return None


def instruction_kind(word):
    crypto = crypto_kind(word)
    if crypto:
        return crypto
    if word & 0x3fa07c00 == 0x08a07c00:
        return 'cas'
    # Regular LSE RMW encoding; reject reserved opcodes and non-LSE loads.
    if word & 0x3f200c00 == 0x38200000:
        operation = (word >> 12) & 15
        if operation <= 8:
            return ('ldadd', 'ldclr', 'ldeor', 'ldset', 'ldsmax', 'ldsmin', 'ldumax', 'ldumin', 'swp')[operation]
    if word & 0x3ffffc00 == 0x38bfc000:
        return 'ldapr'
    return None


def atomic_assembly(word, kind):
    width = 8 << (word >> 30)
    rs, rn, rt = (word >> 16) & 31, (word >> 5) & 31, word & 31
    register = 'x' if width == 64 else 'w'
    suffix = {8: 'b', 16: 'h', 32: '', 64: ''}[width]
    if kind == 'ldapr':
        return [f'ldar{suffix} {register}{rt if rt != 31 else "zr"}, [{"sp" if rn == 31 else "x"+str(rn)}]']
    lines = ['sub sp, sp, #256']
    lines += [f'stp x{i}, x{i+1}, [sp, #{i*8}]' for i in range(0, 30, 2)]
    lines += ['str x30, [sp, #240]', 'mrs x15, nzcv', 'str x15, [sp, #248]']
    lines += ['add x0, sp, #256' if rn == 31 else f'ldr x0, [sp, #{rn*8}]']
    lines += ['mov x1, xzr' if rs == 31 else f'ldr x1, [sp, #{rs*8}]']
    if width < 32:
        lines += [f'and w1, w1, #{(1 << width)-1}']
    if kind == 'cas':
        acquire, release = bool(word & (1<<22)), bool(word & (1<<15))
        lines += ['mov x2, xzr' if rt == 31 else f'ldr x2, [sp, #{rt*8}]']
    else:
        acquire, release = bool(word & (1<<23)) and rt != 31, bool(word & (1<<22))
    load = ('ldaxr' if acquire else 'ldxr') + suffix
    store = ('stlxr' if release else 'stxr') + suffix
    lines += ['retry:', f'{load} {register}3, [x0]']
    if kind == 'cas':
        lines += [f'cmp {register}3, {register}1', 'b.ne mismatch',
                  f'{store} w5, {register}2, [x0]', 'cbnz w5, retry', 'b done', 'mismatch:', 'clrex', 'done:']
        output = rs
    else:
        if kind in ('ldadd', 'ldclr', 'ldeor', 'ldset'):
            op = dict(ldadd='add', ldclr='bic', ldeor='eor', ldset='orr')[kind]
            lines += [f'{op} {register}4, {register}3, {register}1']
        elif kind == 'swp':
            lines += [f'mov {register}4, {register}1']
        else:
            if kind in ('ldsmax', 'ldsmin') and width < 32:
                ext = 'sxtb' if width == 8 else 'sxth'
                lines += [f'{ext} w6, w3', f'{ext} w7, w1', 'cmp w6, w7']
            else:
                lines += [f'cmp {register}3, {register}1']
            condition = dict(ldsmax='gt', ldsmin='lt', ldumax='hi', ldumin='lo')[kind]
            lines += [f'csel {register}4, {register}3, {register}1, {condition}']
        lines += [f'{store} w5, {register}4, [x0]', 'cbnz w5, retry']
        output = rt
    if output != 31:
        lines += [f'str x3, [sp, #{output*8}]']
    lines += ['ldr x15, [sp, #248]', 'msr nzcv, x15']
    lines += [f'ldp x{i}, x{i+1}, [sp, #{i*8}]' for i in range(0, 30, 2)]
    lines += ['ldr x30, [sp, #240]', 'add sp, sp, #256']
    return lines


def helper_assembly(word):
    """One instruction's semantics; original sources are snapshotted for aliases."""
    kind = crypto_kind(word)
    if kind is None:
        kind = instruction_kind(word)
        require(kind is not None, f'no safe ARMv8 lowering for {word:#010x}')
        return atomic_assembly(word, kind)
    d, n, m, a = word & 31, (word >> 5) & 31, (word >> 16) & 31, (word >> 10) & 31
    lines = ['sub sp, sp, #144']
    lines += [f'stp x{i}, x{i+1}, [sp, #{i*8}]' for i in range(0, 16, 2)]
    lines += ['mrs x15, nzcv', 'str x15, [sp, #128]']
    for vector, reg in ((d, 0), (n, 2), (m, 4), (a, 6)):
        lines += [f'umov x{reg}, v{vector}.d[0]', f'umov x{reg+1}, v{vector}.d[1]']

    def sigma(src, rotations, logical=None):
        lines.extend([f'ror x8, {src}, #{rotations[0]}', f'ror x9, {src}, #{rotations[1]}', 'eor x8, x8, x9'])
        lines.append(f'lsr x9, {src}, #{logical}' if logical else f'ror x9, {src}, #{rotations[2]}')
        lines.append('eor x8, x8, x9')

    def choice(x, y, z):
        lines.extend([f'eor x10, {y}, {z}', f'and x10, x10, {x}', f'eor x10, x10, {z}'])

    def majority(x, y, z):
        lines.extend([f'and x10, {x}, {y}', f'orr x11, {x}, {y}', f'and x11, x11, {z}', 'orr x10, x10, x11'])

    if kind == 'sha512h':
        sigma('x5', (14, 18, 41)); choice('x5', 'x2', 'x3')
        lines += ['add x1, x1, x8', 'add x1, x1, x10', 'add x12, x1, x4']
        sigma('x12', (14, 18, 41)); choice('x12', 'x5', 'x2')
        lines += ['add x0, x0, x8', 'add x0, x0, x10']
    elif kind == 'sha512h2':
        sigma('x4', (28, 34, 39)); majority('x2', 'x5', 'x4')
        lines += ['add x1, x1, x8', 'add x1, x1, x10']
        sigma('x1', (28, 34, 39)); majority('x1', 'x4', 'x5')
        lines += ['add x0, x0, x8', 'add x0, x0, x10']
    elif kind == 'sha512su0':
        sigma('x1', (1, 8), 7)
        lines += ['add x0, x0, x8']
        sigma('x2', (1, 8), 7)
        lines += ['add x1, x1, x8']
    elif kind == 'sha512su1':
        sigma('x2', (19, 61), 6)
        lines += ['add x0, x0, x8', 'add x0, x0, x4']
        sigma('x3', (19, 61), 6)
        lines += ['add x1, x1, x8', 'add x1, x1, x5']
    else:
        for lane in (0, 1):
            dest, nn, mm, aa = f'x{lane}', f'x{2+lane}', f'x{4+lane}', f'x{6+lane}'
            if kind == 'eor3':
                lines += [f'eor {dest}, {nn}, {mm}', f'eor {dest}, {dest}, {aa}']
            elif kind == 'bcax':
                lines += [f'bic x8, {mm}, {aa}', f'eor {dest}, {nn}, x8']
            elif kind == 'rax1':
                lines += [f'ror x8, {mm}, #63', f'eor {dest}, {nn}, x8']
            elif kind == 'xar':
                lines += [f'eor {dest}, {nn}, {mm}', f'ror {dest}, {dest}, #{(word >> 10) & 63}']
    lines += [f'ins v{d}.d[0], x0', f'ins v{d}.d[1], x1', 'ldr x15, [sp, #128]', 'msr nzcv, x15']
    lines += [f'ldp x{i}, x{i+1}, [sp, #{i*8}]' for i in range(0, 16, 2)]
    lines += ['add sp, sp, #144']
    return lines


def helper(word, address, return_to, assembler):
    body = assembler.assemble(helper_assembly(word))
    return body + branch(address + len(body), return_to)


def generic_code_data(blob):
    """Complete algorithm tables and byte-bounded signed16 jump dispatches."""
    masks = b''.join(((1 << n) - 1).to_bytes(8, 'little') for n in range(65))
    shuffle = b''.join(bytes(lane % n if n else 0 for lane in range(32)) for n in range(16))
    shuffle += bytes(32 if not n else 16 if n in (4, 8) else (32 // n) * n for n in range(16))
    adler = bytes(range(32, 0, -1)) + struct.pack('<II', 65521, ((1 << 47) + 65520) // 65521)
    result = []
    for label, pattern in [('low-bit-masks', masks), ('LZ4-decode-literals', shuffle), ('Adler32-literals', adler)]:
        start = blob.find(pattern)
        while start >= 0:
            if start % 4 == 0: result.append((start, len(pattern), label))
            start = blob.find(pattern, start + len(pattern))
    def word(off): return struct.unpack_from('<I', blob, off)[0]
    for i in range(0, len(blob) - 15, 4):
        bound, load, add, branch_word = struct.unpack_from('<4I', blob, i)
        if bound & 0xfffffc00 not in (0x92401c00, 0x12001c00): continue
        index, base = bound & 31, (load >> 5) & 31
        if (index == 31 or base == 31 or index == base or load & 0xffe0fc00 != 0x78a07800
            or load & 31 != index or (load >> 16) & 31 != index
            or add & 0xffe0fc00 != 0x8b000000 or add & 31 != index
            or (add >> 5) & 31 != base or (add >> 16) & 31 != index
            or branch_word != 0xd61f0000 | (index << 5)): continue
        for back in range(i - 4, max(-1, i - 256), -4):
            adr = word(back)
            if adr & 31 != base: continue
            if adr & 0x9f000000 != 0x10000000: break
            immediate = ((adr >> 29) & 3) | (((adr >> 5) & 0x7ffff) << 2)
            if immediate & (1 << 20): immediate -= 1 << 21
            table = back + immediate
            if table < 0 or table % 4 or table + 512 > len(blob): break
            targets = [table + n[0] for n in struct.iter_unpack('<h', blob[table:table+512])]
            if all(0 <= t < table and t % 4 == 0 and word(t) in (0xd503249f, 0xd50324df) for t in targets):
                result.append((table, 512, 'byte-opcode-signed16-dispatch'))
            break
    return sorted(set(result))


def known_data(blob, bundle):
    """Exact complete constant vectors, never arbitrary undecodable-word skips."""
    generic = generic_code_data(blob)
    if bundle == 'com.apple.kec.Libm':
        definitions = Path(__file__).parents[1] / 'tools/kernelcache_patcher/include/AppleMathTables.inc'
        tables = re.findall(r'static constexpr uint8_t (kMath\w+)\[\] = \{(.*?)\};', definitions.read_text(), re.S)
        require(len(tables) == 13, 'cannot load complete audited Libm literal pools')
        result = []
        for label, table in tables:
            pattern = bytes(int(w, 16) for w in re.findall(r'0x([0-9a-f]{2})\b', table))
            require(len(pattern) >= 12 and len(pattern) % 4 == 0, 'invalid audited Libm pool')
            start = blob.find(pattern)
            while start >= 0:
                if start % 4 == 0:
                    result.append((start, len(pattern), label))
                start = blob.find(pattern, start + len(pattern))
        return sorted(generic + result)
    if bundle != 'com.apple.kec.corecrypto':
        return generic
    constants = Path(__file__).parents[1] / 'kernel/osfmk/corecrypto/ccsha256_K.c'
    words = re.findall(r'0x[0-9a-fA-F]{8}', constants.read_text())
    require(len(words) == 64, 'cannot load SHA256 standard table')
    reverse = bytes(range(15, -1, -1))
    patterns = {
        'SHA256-K': struct.pack('<64I', *(int(w, 16) for w in words)),
        'SHA1-K-vectors': b''.join(struct.pack('<I', w)*4 for w in (0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6)),
        'AES-key-schedule-vectors': bytes.fromhex('01020408102040801b36000000000000') + b'\x52'*16 + bytes.fromhex('0d0a070004010e0b0805020f0c090603'),
        'byte-reversal-vector': reverse,
        'partial-block-masks': b''.join(((1 << (8*n))-1).to_bytes(16, 'little') for n in range(2, 9)),
        'GHASH-polynomial': struct.pack('<4Q', 0xc200000000000000, 1, 0, 0)[:16],
    }
    result = []
    for label, pattern in patterns.items():
        start = blob.find(pattern)
        while start >= 0:
            if start % 4 == 0:
                result.append((start, len(pattern), label))
            start = blob.find(pattern, start + len(pattern))
    return sorted(generic + result)


def scan(data, images):
    baseline, extended = Decoder(), Decoder(True)
    changes, exclusions = [], []
    count = 0
    for bundle, image in sorted(images.items()):
        for sec in image.sections:
            if not sec['flags'] & 0x80000400:
                continue
            require(sec['size'] % 4 == 0, f'{bundle}: partial executable word')
            start = image.offset(sec['vm'], sec['size'])
            blob = data[start:start+sec['size']]
            spans = known_data(blob, bundle)
            for off, size, label in spans:
                exclusions.append(dict(bundle=bundle, address=sec['vm']+off, size=size, kind=label,
                                       sha256=hashlib.sha256(blob[off:off+size]).hexdigest()))
            for off in range(0, len(blob), 4):
                if any(a <= off < a+n for a, n, _ in spans):
                    continue
                count += 1
                word = struct.unpack_from('<I', blob, off)[0]
                if baseline.decode(word):
                    continue
                asm = extended.decode(word)
                require(asm is not None, f'{bundle}: undecodable executable word {word:#010x} at {sec["vm"]+off:#x}')
                require(instruction_kind(word) is not None,
                        f'{bundle}: unsupported ISA lowering at {sec["vm"]+off:#x}: {asm}')
                changes.append(dict(bundle=bundle, address=sec['vm']+off, offset=start+off,
                                    word=word, mnemonic=instruction_kind(word)))
    return changes, exclusions, count


def context(path):
    spec = importlib.util.spec_from_file_location('armv8_verifier', Path(__file__).with_name('verify-kernelcache.py'))
    v = importlib.util.module_from_spec(spec); spec.loader.exec_module(v)
    data = path.read_bytes(); kernel = v.Image(data)
    _, kexts = v.inventory.inspect(path)
    images = {k['CFBundleIdentifier']: v.Image(data, kernel.offset(k['_PrelinkExecutableLoadAddr']), k['CFBundleIdentifier'], kernel)
              for k in kexts if k.get('_PrelinkExecutableLoadAddr')}
    v.check_kcgen_walk(data, kernel, images)
    return data, kernel, images


def align(n, a=16384):
    return (n+a-1) & -a


def lower(path):
    data, kernel, images = context(path)
    require(not any(s['name'] == SEGMENT for s in kernel.segments), 'cache already has ARMv8 helpers; verify instead of rewriting')
    changes, exclusions, scanned = scan(data, images)
    if not changes:
        return dict(profile=PROFILE, patched=0, scanned_words=scanned)
    require(b'MI6_ARMV8_AOT_V1\0' in data,
            'native kernel lacks ARMv8 AOT segment mapping support; rebuild SDM835 kernel first')
    assembler = Assembler(); baseline = Decoder()
    fileoff = align(len(data)); vm = align(max(s['vm']+s['vmsize'] for s in kernel.segments))
    code = bytearray()
    for record in changes:
        record['helper'] = vm + len(code)
        body = helper(record['word'], record['helper'], record['address']+4, assembler)
        for off in range(0, len(body), 4):
            require(baseline.decode(struct.unpack_from('<I', body, off)[0]), 'helper contains nonbaseline instruction')
        record['size'] = len(body); record['sha256'] = hashlib.sha256(body).hexdigest()
        code += body
    manifest = dict(version=VERSION, profile=PROFILE, patches=changes, exclusions=exclusions, scanned_words=scanned)
    metadata = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
    metaoff = align(len(code), 16); size = align(metaoff + len(metadata))
    command_end = 32 + kernel.header[5]; command_size = 232
    first_section = min(s['off'] for s in kernel.sections if s['size'] and s['off'])
    require(command_end + command_size <= first_section and not any(data[command_end:command_end+command_size]),
            'not enough zeroed native header padding for ARMv8 helper segment')
    result = bytearray(data); result.extend(bytes(fileoff-len(result)+size))
    struct.pack_into('<II16sQQQQiiII', result, command_end, 0x19, command_size, SEGMENT.encode(), vm, size, fileoff, size, 5, 5, 2, 0)
    for idx, (name, relative, length, flags) in enumerate((('__armv8', 0, len(code), 0x80000400), ('__patches', metaoff, len(metadata), 0))):
        struct.pack_into('<16s16sQQIIIIIIII', result, command_end+72+idx*80,
                         name.encode(), SEGMENT.encode(), vm+relative, length, fileoff+relative, 2 if idx == 0 else 4, 0, 0, flags, 0, 0, 0)
    struct.pack_into('<II', result, 16, kernel.header[4]+1, kernel.header[5]+command_size)
    result[fileoff:fileoff+len(code)] = code
    result[fileoff+metaoff:fileoff+metaoff+len(metadata)] = metadata
    for r in changes:
        result[r['offset']:r['offset']+4] = branch(r['address'], r['helper'])
    path.write_bytes(result)
    return dict(profile=PROFILE, patched=len(changes), scanned_words=scanned,
                helpers_bytes=len(code), counts=dict(collections.Counter(r['mnemonic'] for r in changes)))


def audit(data, kernel, images):
    changes, exclusions, count = scan(data, images)
    require(not changes, f'ISA gate: {len(changes)} unlowered ARMv8.1+ instructions')
    segment = next((s for s in kernel.segments if s['name'] == SEGMENT), None)
    if segment is None:
        return dict(profile=PROFILE, patched=0, scanned_words=count, exclusions=exclusions)
    require(b'MI6_ARMV8_AOT_V1\0' in data, 'native kernel lacks ARMv8 AOT mapping support')
    sections = [s for s in kernel.sections if s['segment'] == SEGMENT]
    require([s['name'] for s in sections] == ['__armv8', '__patches'], 'invalid ARMv8 helper sections')
    code, meta = sections
    require(segment['prot'] == segment['maxprot'] == 5 and code['flags'] == 0x80000400 and meta['flags'] == 0,
            'invalid ARMv8 helper permissions/flags')
    manifest = json.loads(data[kernel.offset(meta['vm']):kernel.offset(meta['vm'])+meta['size']])
    require(manifest['version'] == VERSION and manifest['profile'] == PROFILE, 'unsupported ARMv8 patch manifest')
    require(manifest['exclusions'] == exclusions and manifest['scanned_words'] == count, 'ARMv8 ISA scan/constant exclusions changed')
    assembler = Assembler(); baseline = Decoder(); cursor = code['vm']; seen = set()
    restored = bytearray(data)
    for r in manifest['patches']:
        require(r['bundle'] in images and r['address'] not in seen, 'invalid/duplicate ARMv8 patch site')
        image = images[r['bundle']]; seen.add(r['address'])
        require(any(s['flags'] & 0x80000400 and s['vm'] <= r['address'] <= s['vm']+s['size']-4 for s in image.sections), 'ARMv8 patch site outside Kext code')
        require(image.offset(r['address'], 4) == r['offset'] and r['helper'] == cursor,
                'ARMv8 helper mapping differs from manifest')
        require(instruction_kind(r['word']) == r['mnemonic'], 'ARMv8 original instruction differs from manifest')
        body = helper(r['word'], cursor, r['address']+4, assembler)
        require(len(body) == r['size'] and cursor+len(body) <= code['vm']+code['size'], 'invalid ARMv8 helper size')
        offset = kernel.offset(cursor, len(body))
        require(data[offset:offset+len(body)] == body and hashlib.sha256(body).hexdigest() == r['sha256'], 'ARMv8 helper bytes differ from regenerated semantics')
        require(data[r['offset']:r['offset']+4] == branch(r['address'], cursor), 'ARMv8 patch branch differs from manifest')
        for off in range(0, len(body), 4):
            require(baseline.decode(struct.unpack_from('<I', body, off)[0]), 'ARMv8 helper contains unsupported instruction')
        struct.pack_into('<I', restored, r['offset'], r['word'])
        cursor += len(body)
    require(cursor == code['vm']+code['size'] and seen, 'unaccounted/empty ARMv8 helper code')
    # Re-scan the restored image so deleting a record cannot hide an instruction.
    originals, original_exclusions, original_count = scan(restored, images)
    require(originals == [{k: r[k] for k in ('bundle', 'address', 'offset', 'word', 'mnemonic')} for r in manifest['patches']],
            'ARMv8 manifest does not cover the original instruction set')
    return dict(profile=PROFILE, patched=len(seen), scanned_words=count, helpers_bytes=code['size'],
                counts=dict(collections.Counter(r['mnemonic'] for r in manifest['patches'])), exclusions=exclusions)


def audit_source(data, kernel, images, original, source_meta, source_kexts):
    """Bind embedded patch records and constant exclusions to the stock firmware.

    A branch by itself proves no provenance. Reconstruct each source executable
    section's offsets independently, then require a matching record for every
    instruction that the baseline CPU cannot execute.
    """
    baseline = Decoder()
    meta_section = next((s for s in kernel.sections if s['segment'] == SEGMENT and s['name'] == '__patches'), None)
    records = []
    if meta_section:
        off = kernel.offset(meta_section['vm'], meta_section['size'])
        records = json.loads(data[off:off+meta_section['size']])['patches']
    records_by_site = {(r['bundle'], r['address']): r for r in records}
    checked, constants = set(), 0

    def source_offset(address, size):
        for s in source_meta['segments']:
            vm = int(s['vmaddr'], 16)
            if vm <= address and size <= s['filesize'] - (address-vm):
                return s['fileoff'] + address-vm
        raise ValueError(f'ISA source range outside cache: {address:#x}+{size:#x}')

    stock_ids = set()
    for kext in source_kexts:
        bundle = kext['CFBundleIdentifier']
        if bundle not in images or kext.get('OSKernelResource'):
            continue
        stock_ids.add(bundle)
        start = source_offset(kext['_PrelinkExecutableLoadAddr'], 32)
        header = struct.unpack_from('<8I', original, start)
        pos = start+32
        for _ in range(header[4]):
            cmd, size = struct.unpack_from('<II', original, pos)
            if cmd == 0x19:
                segment = struct.unpack_from('<II16sQQQQiiII', original, pos)
                segment_name = segment[2].rstrip(b'\0').decode()
                for i in range(segment[9]):
                    sec = struct.unpack_from('<16s16sQQIIIIIIII', original, pos+72+i*80)
                    if not sec[8] & 0x80000400:
                        continue
                    name = sec[0].rstrip(b'\0').decode()
                    target = next(s for s in images[bundle].sections if s['segment'] == segment_name and s['name'] == name)
                    require(target['size'] == sec[3], f'{bundle}: executable section size changed')
                    off = source_offset(sec[2], sec[3]); blob = original[off:off+sec[3]]
                    spans = known_data(blob, bundle)
                    for relative, length, label in spans:
                        dest = images[bundle].offset(target['vm']+relative, length)
                        require(data[dest:dest+length] == blob[relative:relative+length],
                                f'{bundle}: ISA-excluded source constant {label} was modified')
                        constants += 1
                    for relative in range(0, len(blob), 4):
                        if any(a <= relative < a+n for a, n, _ in spans):
                            continue
                        word = struct.unpack_from('<I', blob, relative)[0]
                        if baseline.decode(word):
                            continue
                        site = (bundle, target['vm']+relative)
                        r = records_by_site.get(site)
                        require(r is not None and r['word'] == word,
                                f'{bundle}: source ISA extension lacks matching static patch at {site[1]:#x}')
                        checked.add(site)
            pos += size
    require(all((r['bundle'], r['address']) in checked for r in records if r['bundle'] in stock_ids),
            'ARMv8 manifest contains a stock patch absent from source firmware')
    return dict(stock_patches_verified=len(checked), source_constant_spans_verified=constants)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('scan', 'lower', 'verify'))
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    if args.mode == 'lower':
        result = lower(args.kernelcache)
    else:
        data, kernel, images = context(args.kernelcache)
        if args.mode == 'verify':
            result = audit(data, kernel, images)
        else:
            changes, exclusions, count = scan(data, images)
            result = dict(profile=PROFILE, scanned_words=count, patches=changes, exclusions=exclusions,
                          counts=dict(collections.Counter(r['mnemonic'] for r in changes)))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
