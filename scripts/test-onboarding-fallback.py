#!/usr/bin/env python3
"""Execute original and patched OBDevice instructions in isolated ARM64 QEMU.

The MG/UIScreen answers and scalar Objective-C object operations are explicit
fixtures. Native initialization, width selection, height table, and fallback
instructions execute unchanged at their real unslid addresses. No device I/O.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

from apfs_sealed import SealedImage, digest, require

spec = importlib.util.spec_from_file_location('ui_patch', Path(__file__).with_name('prepare-system-ui-patches.py'))
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)

# screen class, iPad, bounds width/height, original template, patched template
CASES = [
    ('unknown -1, current geometry', -1, 0, 360., 640., 0, 8),
    ('missing class 0', 0, 0, 360., 640., 0, 8),
    ('unhandled class inside switch', 6, 0, 360., 640., 0, 8),
    ('unknown future class', 100, 0, 360., 640., 0, 8),
    ('unknown, landscape', -1, 0, 640., 360., 0, 8),
    ('unknown, narrow width', -1, 0, 320., 568., 0, 7),
    ('unknown, 414 width', -1, 0, 414., 736., 0, 9),
    ('unknown, 744 width', -1, 0, 744., 1133., 0, 6),
    ('native class 5', 5, 0, 320., 568., 5, 5),
    ('native class 8', 8, 0, 375., 667., 4, 4),
    ('native class 9', 9, 0, 414., 736., 4, 4),
    ('native class 10', 10, 0, 360., 640., 3, 3),
    ('native class 15', 15, 0, 375., 812., 2, 2),
    ('native class 18 narrow', 18, 0, 360., 640., 2, 2),
    ('native class 18 wide', 18, 0, 414., 896., 1, 1),
    ('iPad unknown', -1, 1, 360., 640., 6, 6),
    ('iPad missing class', 0, 1, 360., 640., 6, 6),
    ('unavailable bounds', -1, 0, 0., 0., 0, 0),
    ('one zero dimension', -1, 0, 360., 0., 0, 0),
    ('negative geometry', -1, 0, -360., 640., 0, 0),
    ('NaN width', -1, 0, float('nan'), 640., 0, 0),
    ('infinite height', -1, 0, 360., float('inf'), 0, 0),
    ('implausible geometry', -1, 0, 9000., 10000., 0, 0),
    ('native template with unavailable bounds', 8, 0, 0., 0., 4, 4),
]


def verify(source, prepared, output):
    output.mkdir(parents=True, exist_ok=False)
    image = SealedImage(source)
    try:
        original = image.read_file(patch.CACHE)
        require(digest(original).hex() == patch.CACHE_SHA256, 'unexpected source cache')
        native = lambda at, size: original[at-patch.CACHE_BASE:at-patch.CACHE_BASE+size]
        heights = struct.unpack('<10d', native(0x19b9757a8, 80))
        regions = [('device_init', 0x19b94f3ac, 0x178), ('icon_height', 0x19b956930, 0x54),
                   ('native_tables', 0x19b975000, 0x1000), ('native_width', 0x19b94f524, 0x4c)]
        records = []
        for mode in ('original', 'patched'):
            folder = output/mode
            folder.mkdir()
            sections = []
            for name, address, size in regions:
                raw = bytearray(native(address, size))
                if mode == 'patched' and name == 'device_init':
                    at = patch.HOOK-address
                    raw[at:at+4] = bytes.fromhex(json.loads((prepared/'manifest.json').read_text())['hook_after'])
                (folder/(name+'.bin')).write_bytes(raw)
                sections.append((name, address, f'.incbin "{name}.bin"'))
            if mode == 'patched':
                (folder/'fallback.bin').write_bytes((prepared/'fallback.bin').read_bytes())
                sections.append(('fallback', patch.CAVE, '.incbin "fallback.bin"'))
            fixtures = [
                ('string_answer', 0x1a1c5de8c, 'ldr x0, =object\nret'),
                ('string_compare', 0x1a1c5ddfc, 'ldr x0, [x25, #8]\ncmp x0, #0\ncset x0, eq\nret'),
                ('cf_release', 0x1a1c5ddf0, 'ret'),
                ('integer_answer', 0x1a1c5de80, 'ldr x16, =0x1e8ac8628\ncmp x0, x16\nb.ne 1f\nldr x0, [x25]\nret\n1: mov x0, #1\nret'),
                ('new_object', 0x1a1c5dfac, 'ldr x0, =object\nret'),
                ('retain_auto', 0x1a1c5dfd0, 'ret'),
                ('release19', 0x1a1c5e090, 'ret'),
                ('release20', 0x1a1c5e09c, 'ret'),
                ('release8', 0x1a1c5e108, 'ret'),
                ('current_device', 0x19b98c980, 'ldr x0, =object\nret'),
                ('main_screen', 0x19b98ed80, 'ldr x0, =object\nret'),
                ('bounds', 0x19b98bd60, 'fmov d0, xzr\nfmov d1, xzr\nldr d2, [x25, #16]\nldr d3, [x25, #24]\nret'),
                ('set_type', 0x19b992600, 'str x2, [x0, #16]\nret'),
                ('get_type', 0x19b9933c0, 'ldr x0, [x0, #16]\nret'),
                ('set_template', 0x19b9922e0, 'str x2, [x0, #24]\nret'),
                ('get_template', 0x19b992f40, 'ldr x0, [x0, #24]\nret'),
                ('set_home_button', 0x19b991260, 'strb w2, [x0, #8]\nret'),
            ]
            helpers = []
            for name, address, instructions in fixtures:
                sections.append((name, address, f'b env_{name}'))
                helpers.append(f'env_{name}:\n{instructions}')
            sections += [('native_globals', 0x1f2a77158, '.quad 0'),
                         ('helpers', 0x1a1c60000, '\n'.join(helpers))]
            asm = '''.section .start,"ax"
.global _start
_start:
    ldr x0, =0x40200000
    mov sp, x0
    mov x0, #3
    lsl x0, x0, #20
    msr cpacr_el1, x0
    isb
    ldr x25, =cases
    mov x26, #CASE_COUNT
next_case:
    ldr x0, =object
    stp xzr, xzr, [x0]
    stp xzr, xzr, [x0, #16]
    mov x19, #0x191
    mov x20, #0x202
    mov x21, #0x212
    mov x22, #0x222
    fmov d8, #8.0
    fmov d9, #9.0
    ldr x8, =0x19b94f3ac
    blr x8
    cmp x19, #0x191
    b.ne fail
    cmp x20, #0x202
    b.ne fail
    cmp x21, #0x212
    b.ne fail
    cmp x22, #0x222
    b.ne fail
    fmov d0, #8.0
    fcmp d8, d0
    b.ne fail
    fmov d0, #9.0
    fcmp d9, d0
    b.ne fail
    ldr x0, =object
    ldr x0, [x0, #24]
    ldr x1, [x25, #32]
    cmp x0, x1
    b.ne fail
    ldr x8, =0x19b956930
    blr x8
    ldr d1, [x25, #40]
    fcmp d0, d1
    b.ne fail
    add x25, x25, #48
    subs x26, x26, #1
    b.ne next_case
    mov x0, #4
    ldr x1, =success
    hlt #0xf000
    mov x0, #0x20
    ldr x1, =exit_ok
    hlt #0xf000
fail:
    mov x0, #4
    ldr x1, =failure
    hlt #0xf000
    mov x0, #0x20
    ldr x1, =exit_bad
    hlt #0xf000
.balign 8
exit_ok: .quad 0x20026,0
exit_bad: .quad 0x20026,1
success: .asciz "OBDEVICE_OK CASE_COUNT cases; register preservation OK\\n"
failure: .asciz "OBDEVICE_FAILED\\n"
.balign 16
object: .space 64
cases:
'''.replace('CASE_COUNT', str(len(CASES)))
            for _, screen_class, pad, width, height, old, new in CASES:
                template = new if mode == 'patched' else old
                icon_height = heights[template-1] if template else 0.
                values = (screen_class, pad, width, height, template, icon_height)
                for value in values:
                    integer = struct.unpack('<Q', struct.pack('<d', value))[0] if isinstance(value, float) else value
                    asm += f'.quad {integer}\n'
            for name, _, instructions in sections:
                asm += f'.section .{name},"ax"\n{instructions}\n'
            (folder/'native.S').write_text(asm)
            placements = [('start', 0x40080000, '')]+sections
            ld = 'ENTRY(_start)\nPHDRS {\n'
            ld += '\n'.join(f' p{i} PT_LOAD FLAGS(7);' for i in range(len(placements)))+'\n}\nSECTIONS {\n'
            for i, (name, address, _) in enumerate(placements):
                ld += f' .{name} {address:#x} : {{ *(.{name}) }} :p{i}\n'
            ld += ' /DISCARD/ : { *(.comment) *(.note*) }\n}\n'
            (folder/'native.ld').write_text(ld)
            subprocess.run(['clang', '--target=aarch64-none-elf', '-c', 'native.S', '-o', 'native.o'], cwd=folder, check=True)
            subprocess.run(['ld.lld', '-T', 'native.ld', 'native.o', '-o', 'native.elf'], cwd=folder, check=True)
            command = ['qemu-system-aarch64', '-M', 'virt', '-cpu', 'cortex-a57', '-m', '8G', '-nographic',
                       '-monitor', 'none', '-serial', 'none', '-nic', 'none', '-semihosting-config',
                       'enable=on,target=native', '-kernel', 'native.elf']
            result = subprocess.run(command, cwd=folder, timeout=20, capture_output=True, text=True)
            log = result.stdout+result.stderr
            (folder/'qemu.log').write_text(log)
            passed = result.returncode == 0 and f'OBDEVICE_OK {len(CASES)} cases' in log
            records.append({'mode': mode, 'passed': passed, 'returncode': result.returncode, 'log': log})
            require(passed, f'{mode} native execution failed: '+log)
        report = {'all_cases_passed': True, 'cases_per_version': len(CASES), 'runs': records,
                  'native_height_table': heights, 'cases': [dict(label=r[0], screen_class=r[1], is_pad=r[2],
                  bounds=[str(r[3]), str(r[4])], original_template=r[5], patched_template=r[6]) for r in CASES],
                  'fixture_boundary': 'MG/UIScreen answers and scalar ObjC operations only',
                  'hardware_or_optical_verification': False}
        (output/'verification.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(records, indent=2))
    finally:
        image.close()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--prepared', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    verify(args.source, args.prepared, args.output)
