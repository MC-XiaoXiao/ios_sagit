// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandRegisters.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>

namespace Baseband {
RegisterIO::~RegisterIO() { unbind(); }
void RegisterIO::unbind() {
    OSSafeReleaseNULL(maps[1]); OSSafeReleaseNULL(maps[0]);
}
bool RegisterIO::bind(IOService *provider) {
    if (maps[0] || maps[1] || !provider ||
        (provider->getDeviceMemoryCount() != 2 && provider->getDeviceMemoryCount() != 4))
        return false;
    const uint64_t addresses[] = {Q6::Base, Q6::RMBBase};
    for (unsigned i = 0; i < 2; ++i) {
        auto *memory = provider->getDeviceMemoryWithIndex(i);
        if (!memory || memory->getPhysicalAddress() != addresses[i] ||
            memory->getLength() != 4096) return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        maps[i] = provider->mapDeviceMemoryWithIndex(i, kIOMapInhibitCache);
        if (!maps[i] || maps[i]->getLength() < 4096 || !maps[i]->getVirtualAddress()) {
            unbind(); return false;
        }
    }
    return true;
}
bool RegisterIO::read(unsigned space, uint32_t offset, uint32_t &value) {
    if (space >= 2 || !maps[space] || (offset & 3) || offset > 4092) return false;
    value = *reinterpret_cast<volatile uint32_t *>(maps[space]->getVirtualAddress() + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return true;
}
bool RegisterIO::write(unsigned space, uint32_t offset, uint32_t value) {
    if (space >= 2 || !maps[space] || (offset & 3) || offset > 4092) return false;
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(maps[space]->getVirtualAddress() + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
    return true;
}
uint64_t RegisterIO::nowUS() {
    uint64_t absolute = 0, nanos = 0;
    clock_get_uptime(&absolute); absolutetime_to_nanoseconds(absolute, &nanos);
    return nanos / 1000;
}
void RegisterIO::delayUS(unsigned delay) { IODelay(delay); }
template class Q6::Core<RegisterIO>;
} // namespace Baseband
