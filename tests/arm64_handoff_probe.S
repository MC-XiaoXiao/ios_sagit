// SPDX-License-Identifier: BSD-2-Clause
// This is a loader ABI probe, not XNU. It validates the raw bridge on QEMU.
.text
.globl probe_entry
probe_entry:
    mrs x1, CurrentEL
    cmp x1, #4
    b.ne 9f
    ldrh w1, [x0]
    cmp w1, #2
    b.ne 9f
    ldrh w1, [x0, #2]
    cmp w1, #2
    b.ne 9f
    ldr x1, [x0, #16]
    tst x1, #0x1fffff
    b.ne 9f
    ldr x1, [x0, #24]
    mov x2, #0x10000000
    cmp x1, x2
    b.ne 9f
    adr x1, message
    mov x2, #0x09000000
1:
    ldrb w3, [x1], #1
    cbz w3, 9f
2:
    ldr w4, [x2, #0x18]
    tbnz w4, #5, 2b
    str w3, [x2]
    b 1b
9:
    wfe
    b 9b
message:
    .asciz "[ARM64 probe] Mach-O entry / boot_args OK\r\n"
