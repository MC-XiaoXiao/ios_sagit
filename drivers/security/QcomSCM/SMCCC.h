// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "SCMPAS.h"
namespace QcomSecure {
// Shared by the native IOKit service and the pre-XNU EFI firmware loader.
inline void smc(Registers &r) {
    register uint64_t x0 __asm__("x0") = r.x[0];
    register uint64_t x1 __asm__("x1") = r.x[1];
    register uint64_t x2 __asm__("x2") = r.x[2];
    register uint64_t x3 __asm__("x3") = r.x[3];
    register uint64_t x4 __asm__("x4") = r.x[4];
    register uint64_t x5 __asm__("x5") = r.x[5];
    register uint64_t x6 __asm__("x6") = r.x[6];
    register uint64_t x7 __asm__("x7") = r.x[7];
    __asm__ volatile("dsb sy\n\tsmc #0\n\tdsb sy\n\tisb"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
        : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
    r = {{x0, x1, x2, x3, x4, x5, x6, x7}};
}
} // namespace QcomSecure
