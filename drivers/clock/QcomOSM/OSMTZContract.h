// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

// Offline-verified contract of stock TZ.BF.4.0.6-00178 on MSM8998 v2.
// See docs/power/osm-tz-{scm,initialization}-audit.json for the binary hash,
// table selection and monitor access checks. These observable boot parameters
// identify a compatible initial state, not a firmware measurement/attestation.
// They precede a fallible protected post-init routine whose caller ignores its
// return value. They cannot prove that stage completed. Do not bypass SCM to
// read +0x1050 (a later marker) without separate nonsecure-access evidence.
namespace QcomOSMTZ {
constexpr uint32_t Profile = 1;
constexpr uint32_t CompleteChecks = 0xff;
constexpr uint32_t base(unsigned cluster) { return 0x179c0000U + cluster * 0x2000U; }
constexpr uint32_t seq(unsigned number) { return 0x300U + number * 4U; }
struct Parameter { unsigned number; uint32_t initial[2]; };
constexpr Parameter BootParameters[] = {
    {15, {0, 0}}, {31, {0, 0}}, {32, {0, 0}},
    {57, {0x10, 0x0b}}, {58, {0x11, 0x0c}},
    {72, {0x16, 0x19}}, {76, {0x4b9, 0x5b9}}, {88, {0, 0}}
};
static_assert(sizeof(BootParameters) / sizeof(BootParameters[0]) == 8, "TZ check mask");

// Deliberately use only the unconditional OSM subset of the monitor allowlist.
// TZ silently returns zero/drops writes outside it, while reporting success.
// In v2 SEQ55/56 hold TZ-owned 0x2c5ffd/0x2f5ffd, matching ACD encodings.
// They are not HLOS-writable MEM-ACC corner thresholds.
inline bool permitted(uint32_t physical) {
    if (physical < base(0) || physical >= base(0) + 0x4000U) return false;
    const uint32_t offset = (physical - base(0)) % 0x2000U;
    for (const auto &p : BootParameters) if (offset == seq(p.number)) return true;
    return false;
}
}
