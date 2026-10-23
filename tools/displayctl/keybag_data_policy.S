/* Selected only by the explicit --development-no-sep build policy.
 * Generic ARM has no hardware keybag to unlock. These public state queries
 * must not initialize MobileGestalt or wait for AKS during service startup.
 * Same-cache branches remain valid under the shared-cache slide.
 */
.text
.p2align 2
.global keybag_device_lock_state
keybag_device_lock_state:
    mov w0, #3
    ret

.global keybag_device_unlocked_since_boot
keybag_device_unlocked_since_boot:
    mov w0, #1
    ret

.global keybag_user_unlocked_since_boot
keybag_user_unlocked_since_boot:
    mov w0, #1
    ret
