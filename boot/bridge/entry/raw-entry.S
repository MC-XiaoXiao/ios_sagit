// SPDX-License-Identifier: BSD-2-Clause
.section .text.boot,"ax"
.p2align 12
.globl raw_image_start
raw_image_start:
    b raw_start
    .long 0
    .quad 0x80000
    .quad raw_image_end - raw_image_start
    .quad 0xa // little endian, 4 KiB pages, relocatable physical placement
    .quad 0,0,0
    .long 0x644d5241 // ARM64 Image magic
    .long 0
.globl raw_start
raw_start:
    msr daifset, #15
    mov x20, x0 // Firmware FDT from Linux/Android arm64 boot protocol.
    adr x19, raw_image_start
    adrp x1, raw_rela_start
    add x1, x1, :lo12:raw_rela_start
    adrp x2, raw_rela_end
    add x2, x2, :lo12:raw_rela_end
1:
    cmp x1, x2
    b.hs 2f
    ldp x3, x4, [x1], #16
    ldr x5, [x1], #8
    mov x6, #1027 // R_AARCH64_RELATIVE
    cmp x4, x6
    b.ne 9f
    add x3, x3, x19
    add x5, x5, x19
    str x5, [x3]
    b 1b
2:
    adrp x1, raw_stack_top
    add x1, x1, :lo12:raw_stack_top
    mov sp, x1
    mov x0, x20
    bl raw_main
9:
    wfe
    b 9b
.section .data.stack,"aw"
.p2align 4
    .space 32768
raw_stack_top:
