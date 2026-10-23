/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _ARM_DISPLAY_PROPERTIES_H
#define _ARM_DISPLAY_PROPERTIES_H
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <stdint.h>

// Apple DT cells are little endian after loader conversion. Do not accept
// Linux big endian phandle tuples as Apple platform function identifiers.
inline bool displayProperty32(IORegistryEntry *s, const char *key, uint32_t &v) {
    auto d = OSDynamicCast(OSData, s->getProperty(key));
    if (!d || d->getLength() != 4) return false;
    const auto b = static_cast<const uint8_t *>(d->getBytesNoCopy());
    v = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    return true;
}
inline bool displayProperty64(IORegistryEntry *s, const char *key, uint64_t &v) {
    auto d = OSDynamicCast(OSData, s->getProperty(key));
    if (!d || d->getLength() != 8) return false;
    const auto b = static_cast<const uint8_t *>(d->getBytesNoCopy());
    v = 0;
    for (unsigned i = 0; i != 8; ++i) v |= uint64_t(b[i]) << (8 * i);
    return true;
}
#endif
