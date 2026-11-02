// SPDX-License-Identifier: BSD-2-Clause
.text
.p2align 4
.globl bridge_enter
bridge_enter:
    msr daifset, #15
    // EFI on this platform uses identity mappings. No memory access below.
    mrs x9, CurrentEL
    cmp x9, #8
    b.eq 2f
    mrs x9, sctlr_el1
    bic x9, x9, #1
    bic x9, x9, #4
    bic x9, x9, #0x1000
    dsb sy
    msr sctlr_el1, x9
    isb
    tlbi vmalle1
    dsb sy
    isb
    b 3f
2:
    mrs x9, sctlr_el2
    bic x9, x9, #1
    bic x9, x9, #4
    bic x9, x9, #0x1000
    dsb sy
    msr sctlr_el2, x9
    isb
    mov x9, #0x80000000
    msr hcr_el2, x9
    mov x9, #3
    msr cnthctl_el2, x9
    msr cntvoff_el2, xzr
    mov x9, #0x0800
    movk x9, #0x30d0, lsl #16
    msr sctlr_el1, x9
    adr x9, 3f
    msr elr_el2, x9
    mov x9, #0x3c5
    msr spsr_el2, x9
    eret
3:
    mov x9, #0x300000
    msr cpacr_el1, x9
    msr cntv_ctl_el0, xzr
    msr cntp_ctl_el0, xzr
    isb
    br x1
