// Pinned 22H374 OBDevice once-initializer continuation.
// The initializer already saved LR, x19-x22 and d8-d9. Its d9/d8 hold the
// real UIScreen bounds. Only unclassified phones get a native width template.
.section .text,"ax"
.balign 16
.global onboarding_template_fallback
onboarding_template_fallback:
    ldr x0, [x21, #0x158]
    bl ob_get_type
    cmp x0, #1
    b.ne resume
    ldr x0, [x21, #0x158]
    bl ob_get_template
    cbnz x0, resume

    // Reject unavailable/nonpositive/nonfinite geometry; never invent bounds.
    fcmp d9, #0.0
    b.le resume
    fcmp d8, #0.0
    b.le resume
    mov x8, #0x40c0000000000000 // 8192 logical points, conservative upper bound
    fmov d1, x8
    fcmp d9, d1
    b.hi resume
    fcmp d8, d1
    b.hi resume
    fmin d0, d9, d8
    bl ob_template_for_width
    mov x2, x0
    ldr x0, [x21, #0x158]
    bl ob_set_template
resume:
    // Displaced ADRP; the original next instruction supplies its page offset.
    adrp x0, ob_home_button_key
    b ob_initializer_resume
