/* Portable validation shared by the production drivers and offline tests. */
#ifndef _MI6_ARM_PLATFORM_CONFIG_H
#define _MI6_ARM_PLATFORM_CONFIG_H
#include <stdint.h>
#include <stddef.h>

namespace ARMPlatformConfig {

inline uint32_t readLE32(const void *bytes)
{
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
        uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

inline uint64_t readLE64(const void *bytes)
{
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    return uint64_t(readLE32(p)) | uint64_t(readLE32(p + 4)) << 32;
}

inline bool stringEquals(const void *bytes, size_t length, const char *expected)
{
    if (!bytes || !expected || !length) return false;
    const char *p = static_cast<const char *>(bytes);
    size_t i = 0;
    while (i < length && expected[i] && p[i] == expected[i]) ++i;
    return i < length && !expected[i] && !p[i] && i + 1 == length;
}

inline bool enabled(const void *bytes, size_t length)
{
    return !bytes || stringEquals(bytes, length, "okay") ||
        stringEquals(bytes, length, "ok");
}

// Apple arm64 DT reg tuples: little-endian uint64 address, uint64 length.
// All ranges must be nonempty, aligned and fit the target physical aperture.
inline bool validReg(const void *bytes, size_t length, uint64_t limit)
{
    if (!bytes || !length || length % 16) return false;
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0; i < length; i += 16) {
        uint64_t base = readLE64(p + i), size = readLE64(p + i + 8);
        if (!size || (base & 3) || (size & 3) || base >= limit || size > limit - base)
            return false;
    }
    return true;
}

inline bool validSDM835Affinity(uint32_t mpidr)
{
    return (mpidr & ~0x103U) == 0;
}

inline bool addSDM835CPU(const void *bytes, size_t length, uint32_t &seen)
{
    if (!bytes || length != 4) return false;
    uint32_t affinity = readLE32(bytes);
    if (!validSDM835Affinity(affinity)) return false;
    uint32_t bit = 1U << ((affinity >> 8) * 4 + (affinity & 3));
    if (seen & bit) return false;
    seen |= bit;
    return true;
}

inline bool validRanges(const void *bytes, size_t length, uint64_t limit)
{
    if (!length) return true; // Explicit empty ranges means identity mapping.
    if (!bytes || length % 24) return false;
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0; i < length; i += 24) {
        uint64_t child = readLE64(p + i), parent = readLE64(p + i + 8);
        uint64_t size = readLE64(p + i + 16);
        if (!size || (child & 3) || (parent & 3) || (size & 3) ||
            child >= limit || parent >= limit || size > limit - child || size > limit - parent)
            return false;
    }
    return true;
}

enum PSCIConduit { kNoPSCI, kSMC, kHVC };
inline PSCIConduit conduit(const void *bytes, size_t length)
{
    if (stringEquals(bytes, length, "smc")) return kSMC;
    if (stringEquals(bytes, length, "hvc")) return kHVC;
    return kNoPSCI;
}

} // namespace ARMPlatformConfig
#endif
