#!/usr/bin/env python3
"""Differential execution: QEMU max native instructions versus Cortex-A53 helpers."""
import argparse
import json
import random
from pathlib import Path
import struct
import subprocess
import armv8_compat as compat

ROOT = Path(__file__).resolve().parents[1]


def cases():
    rng = random.Random(0x83518)
    bases = [0xce608000, 0xce608400, 0xcec08000, 0xce608800, 0xce000000, 0xce200000, 0xce608c00, 0xce800000]
    result = []
    for base in bases:
        for i in range(40):
            d, n, m, a = [rng.randrange(32) for _ in range(4)]
            if i < 8:
                # All aliases, individual input aliases and high vector numbers.
                d, n, m, a = [(0,0,0,0), (31,31,31,31), (3,3,4,5), (3,4,3,5),
                              (3,4,5,3), (3,4,4,5), (3,4,5,4), (3,4,5,5)][i]
            word = base | d | (n << 5)
            kind = compat.crypto_kind(base)
            if kind != 'sha512su0': word |= m << 16
            if kind in ('bcax', 'eor3'): word |= a << 10
            if kind == 'xar': word |= (i % 64 if i < 8 else rng.randrange(64)) << 10
            vectors = [rng.getrandbits(128) for _ in range(32)]
            if i == 0: vectors = [0]*32
            if i == 1: vectors = [(1<<128)-1]*32
            result.append((word, [rng.getrandbits(64) for _ in range(31)], vectors, (i % 16) << 28, 0, 0x43001000))
    # LSE success/mismatch, all widths/orders, zero registers, aliases, SP base.
    aliases = [(1,3,2), (1,3,1), (3,3,2), (1,3,3), (3,3,3), (31,3,2), (1,3,31), (1,31,2)]
    for operation in list(range(9)) + ['cas', 'ldapr']:
        for size in range(4):
            width = 8 << size
            for order in range(4) if operation != 'ldapr' else [0]:
                for i, (rs, rn, rt) in enumerate(aliases):
                    for match in (False, True):
                        if operation == 'cas':
                            word = (size<<30) | 0x08a07c00 | ((order&1)<<22) | ((order>>1)<<15) | (rs<<16) | (rn<<5) | rt
                        elif operation == 'ldapr':
                            word = (size<<30) | 0x38bfc000 | (rn<<5) | rt
                        else:
                            word = (size<<30) | 0x38200000 | ((order&1)<<23) | ((order>>1)<<22) | (rs<<16) | (operation<<12) | (rn<<5) | rt
                        regs = [rng.getrandbits(64) for _ in range(31)]
                        address = 0x44000000 if rn == 31 else 0x43001000
                        if rn != 31: regs[rn] = address
                        memory = rng.getrandbits(128)
                        if operation == 'cas' and match:
                            memory = (memory & ~((1<<width)-1)) | ((regs[rs] if rs != 31 else 0) & ((1<<width)-1))
                        vectors = [rng.getrandbits(128) for _ in range(32)]
                        result.append((word, regs, vectors, rng.randrange(16)<<28, memory, address))
    return result


def source(items, lowered, destination):
    lines = ['.text', '.global _start', '_start:', 'mov x0, #0x44000000', 'mov sp, x0',
             'mrs x0, cpacr_el1', 'orr x0, x0, #0x300000', 'msr cpacr_el1, x0', 'isb',
             'adrp x1, open_args', 'add x1, x1, :lo12:open_args', 'mov x0, #1', 'hlt #0xf000', 'adrp x1, file_handle', 'add x1, x1, :lo12:file_handle', 'str x0, [x1]']
    for i, (word, regs, vecs, nzcv, memory, address) in enumerate(items):
        lines += [f'adrp x16, seed_{i}', f'add x16, x16, :lo12:seed_{i}']
        lines += ['add x2, x16, #768', 'ldp x0, x1, [x2]', f'mov x17, #{address & 0xffff}', f'movk x17, #{address >> 16}, lsl #16', 'stp x0, x1, [x17]']
        lines += [f'ldp q{j}, q{j+1}, [x16, #{j*16}]' for j in range(0,32,2)]
        lines += [f'mov x17, #{nzcv}', 'msr nzcv, x17']
        lines += ['add x16, x16, #512']
        lines += [f'ldp x{j}, x{j+1}, [x16, #{j*8}]' for j in range(0,30,2) if j != 16]
        lines += ['ldr x30, [x16, #240]', 'ldp x16, x17, [x16, #128]']
        lines += [f'b helper_{i}' if lowered else f'.inst {word:#x}', f'return_{i}:', 'sub sp, sp, #800']
        lines += [f'stp x{j}, x{j+1}, [sp, #{j*8}]' for j in range(0,30,2)]
        lines += ['str x30, [sp, #240]', 'mrs x0, nzcv', 'str x0, [sp, #248]', 'add x0, sp, #800',
                  'str x0, [sp, #256]', 'str xzr, [sp, #264]']
        lines += [f'stp q{j}, q{j+1}, [sp, #{272+j*16}]' for j in range(0,32,2)]
        lines += [f'mov x0, #{address & 0xffff}', f'movk x0, #{address >> 16}, lsl #16', 'ldp x1, x2, [x0]', 'str x1, [sp, #784]', 'str x2, [sp, #792]', 'mov x2, sp', 'sub sp, sp, #32',
                  'adrp x1, file_handle', 'add x1, x1, :lo12:file_handle', 'ldr x0, [x1]', 'str x0, [sp]', 'str x2, [sp, #8]',
                  'mov x0, #800', 'str x0, [sp, #16]', 'mov x1, sp', 'mov x0, #5', 'hlt #0xf000',
                  'add sp, sp, #832', f'b next_{i}', '.ltorg', f'next_{i}:']
    lines += ['mov x0, #0x20', 'adrp x1, exit_args', 'add x1, x1, :lo12:exit_args', 'hlt #0xf000', 'b .', '.ltorg', '.balign 8',
              'exit_args: .quad 0x20026, 0', 'file_handle: .quad 0', f'filename: .asciz "{destination}"', '.balign 8',
              f'open_args: .quad filename, 5, {len(str(destination))}']
    assembler = compat.Assembler()
    for i, (word, *_) in enumerate(items):
        encoded = assembler.assemble(compat.helper_assembly(word))
        body = [f'.inst {w:#x}' for w, in struct.iter_unpack('<I', encoded)]
        # Branches inside the encoded helper are position independent.
        lines += [f'helper_{i}:'] + body + [f'b return_{i}']
    lines += ['.data', '.balign 16']
    for i, (word, regs, vecs, nzcv, memory, address) in enumerate(items):
        values = [(v & ((1<<64)-1), v >> 64) for v in vecs]
        lines += [f'seed_{i}:'] + [f'.quad {lo:#x}, {hi:#x}' for lo,hi in values] + [f'.quad {r:#x}' for r in regs] + ['.quad 0', f'.quad {memory & ((1<<64)-1):#x}, {memory >> 64:#x}']
    return '\n'.join(lines)+'\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/armv8-compat/differential')
    args = parser.parse_args(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    items = cases()
    for lowered, cpu, name in ((False, 'max', 'native'), (True, 'cortex-a53', 'lowered')):
        asm = out/(name+'.s'); binary = out/(name+'.elf'); output = out/(name+'.bin')
        asm.write_text(source(items, lowered, output))
        subprocess.run(['clang', '--target=aarch64-none-elf', '-nostdlib', '-fuse-ld=lld', '-Wl,-Ttext=0x40080000', str(asm), '-o', str(binary)], check=True, capture_output=True)
        output.unlink(missing_ok=True)
        subprocess.run(['qemu-system-aarch64', '-M', 'virt', '-cpu', cpu, '-m', '128M', '-nographic', '-monitor', 'none', '-serial', 'none', '-semihosting-config', 'enable=on,target=native', '-kernel', str(binary)], check=True, timeout=45, capture_output=True)
        assert output.stat().st_size == len(items)*800, (name, output.stat().st_size)
    native = (out/'native.bin').read_bytes(); lowered = (out/'lowered.bin').read_bytes()
    for i, (word, *_) in enumerate(items):
        a, b = native[i*800:(i+1)*800], lowered[i*800:(i+1)*800]
        if a != b:
            offsets = [off for off in range(0,800,8) if a[off:off+8] != b[off:off+8]]
            raise AssertionError(f'case {i} {compat.instruction_kind(word)} {word:#x}: differences at {offsets}; native={a.hex()} lowered={b.hex()}')
    report = dict(status='passed', cases=len(items), original_cpu='max', lowered_cpu='cortex-a53',
                  compared='all 31 GPRs, SP, NZCV, all 32 SIMD registers, atomic memory', kinds=sorted({compat.instruction_kind(w) for w,*_ in items}))
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n'); print(json.dumps(report,indent=2))


if __name__ == '__main__': main()
