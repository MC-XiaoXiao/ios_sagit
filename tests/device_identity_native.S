/* Offline native MobileGestalt classifier harness. Firmware bytes are extracted
 * from the local Cryptex by scripts/analyze-device-identity.py. Only external
 * libc/logging dependencies are modeled; no mobile device is accessed. */
.section .text.start,"ax"
.global _start
_start:
    ldr x0, =0x40100000
    mov sp, x0
    ldr x0, =guard_value
    ldr x1, =0x1e5e8c7d0
    str x0, [x1]
    mov x19, #0
    ldr x20, =cases
next_case:
    ldr x0, [x20], #8
    cbz x0, done
    ldr x1, =0x1f29372f0
    str x0, [x1]
    ldr x1, =0x1f29372e8
    mov x2, #-1
    str x2, [x1]
    ldr x1, =0x1f2937260
    str w2, [x1]
    ldr x0, =0x19cef0394
    blr x0
    ldr x1, =0x1f2937260
    ldr w0, [x1]
    ldr x2, [x20], #8
    cmp w0, w2
    b.ne fail
    add x19, x19, #1
    b next_case
done:
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
guard_value: .quad 0x1122334455667788
cases:
 .quad old_product,2, new_product,1, stock_phone,1, stock_ipad,3, stock_ipod,2, unknown,2, wrong_case,2, 0,0
old_product: .asciz "Xiaomi Mi 6 (sagit)"
new_product: .incbin "product.bin"
stock_phone: .asciz "iPhone9,1"
stock_ipad: .asciz "iPad7,11"
stock_ipod: .asciz "iPod9,1"
unknown: .asciz "Unrecognized"
wrong_case: .asciz "iphoneMi6,1"
success: .asciz "NATIVE_CLASSIFICATION_OK: 7 cases\n"
failure: .asciz "NATIVE_CLASSIFICATION_FAILED\n"
.section .native,"ax"
.incbin "classifier.bin"
.section .native_strings,"a"
.incbin "class-strings.bin"
.section .strncmp,"ax"
    cbz x2, equal
loop:
    ldrb w3,[x0],#1
    ldrb w4,[x1],#1
    subs w5,w3,w4
    b.ne compare_return
    cbz w3, equal
    subs x2,x2,#1
    b.ne loop
equal: mov w5,#0
compare_return: mov w0,w5
    ret
.section .logger,"ax"
    ret
.section .strrchr,"ax"
    mov x0,#0
    ret
.section .oslog,"ax"
    mov x0,#0
    ret
.section .guard,"aw",@nobits
    .space 8
.section .globals,"aw",@nobits
    .space 0x100
