/* Explicit --development-no-sep policy for the unprotected device bag.
 * SpringBoardFoundation queries AKS directly, including its packed 66-byte
 * iOS 18 record. An unavailable endpoint otherwise leaves an all-zero record,
 * which means that a passcode exists. Other bags and invalid outputs retain
 * the original wrappers and backend errors. No key operation is implemented.
 */
.text
.p2align 2
.global keybag_aks_device_state
keybag_aks_device_state:
    cbnz w0, 1f
    cbz x1, 1f
    b keybag_aks_unprotected_state
1:
    mov x2, x1
    mov w1, #17
    b keybag_aks_native_state

.global keybag_aks_extended_device_state
keybag_aks_extended_device_state:
    cbnz w0, 1f
    cbz x1, 1f
    b keybag_aks_unprotected_state
1:
    mov x2, x1
    mov w1, #35
    b keybag_aks_native_state

keybag_aks_unprotected_state:
    stp xzr, xzr, [x1]
    stp xzr, xzr, [x1, #16]
    stp xzr, xzr, [x1, #32]
    stp xzr, xzr, [x1, #48]
    strh wzr, [x1, #64]
    mov w8, #2
    str w8, [x1]
    mov w8, #3
    str w8, [x1, #4]
    mov w8, #4
    str w8, [x1, #20]
    mov w0, #0
    ret
