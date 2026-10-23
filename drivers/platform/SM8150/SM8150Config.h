/* Apple DT validation; shared by the Kext and the portable host test. */
#ifndef _MI6_SM8150_CONFIG_H
#define _MI6_SM8150_CONFIG_H
#include <ARMPlatformConfig.h>

namespace SM8150Config {

inline bool validAffinity(uint32_t affinity)
{
    // Kryo 485 uses Aff1=0..7 and Aff0=0, unlike Kryo 280's 4+4 encoding.
    return (affinity & ~0x700U) == 0;
}

inline bool addCPU(const void *bytes, size_t length, uint32_t &seen)
{
    if (!bytes || length != 4) return false;
    uint32_t affinity = ARMPlatformConfig::readLE32(bytes);
    if (!validAffinity(affinity)) return false;
    uint32_t bit = 1U << (affinity >> 8);
    if (seen & bit) return false;
    seen |= bit;
    return true;
}

inline uint32_t schedulerCluster(uint32_t affinity)
{
    uint32_t core = affinity >> 8;
    return core < 4 ? 0 : (core < 7 ? 1 : 2);
}

inline uint32_t schedulerCore(uint32_t affinity)
{
    uint32_t core = affinity >> 8;
    return core < 4 ? core : (core < 7 ? core - 4 : 0);
}

inline bool identityRanges(const void *bytes, size_t length)
{
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    return p && length == 24 &&
        ARMPlatformConfig::readLE64(p) == 0 &&
        ARMPlatformConfig::readLE64(p + 8) == 0 &&
        ARMPlatformConfig::readLE64(p + 16) == 0x100000000ULL;
}

inline bool gicRegisters(const void *bytes, size_t length)
{
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    return p && length == 32 &&
        ARMPlatformConfig::readLE64(p) == 0x17a00000 &&
        ARMPlatformConfig::readLE64(p + 8) == 0x10000 &&
        ARMPlatformConfig::readLE64(p + 16) == 0x17a60000 &&
        ARMPlatformConfig::readLE64(p + 24) == 0x100000;
}
} // namespace SM8150Config
#endif
