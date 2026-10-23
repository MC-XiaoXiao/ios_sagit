// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandHaltIO.h"
#include <ClockControllerProtocol.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>

namespace Baseband {
HaltIO::~HaltIO() { unbind(); }
void HaltIO::unbind() { OSSafeReleaseNULL(map); OSSafeReleaseNULL(bus); }
bool HaltIO::bind(IOService *owner) {
    if (map || bus || !owner || !owner->metaCast("QcomBaseband")) return false;
    auto *provider = owner->getProvider();
    if (!provider || !provider->getProvider() || provider->getDeviceMemoryCount() != 4) return false;
    auto *range = provider->getDeviceMemoryWithIndex(3);
    if (!range || range->getPhysicalAddress() != AXI::Base || range->getLength() != AXI::Bytes) return false;
    map = provider->mapDeviceMemoryWithIndex(3, kIOMapAnywhere | kIOMapInhibitCache);
    if (!map || map->getLength() != AXI::Bytes || !map->getVirtualAddress()) { unbind(); return false; }
    bus = provider->getProvider(); bus->retain(); return true;
}
bool HaltIO::read(uint32_t offset, uint32_t &value) {
    if (!map || !AXI::registerOffset(offset, false)) return false;
    value = *reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress() + offset);
    __asm__ volatile("dmb oshld" ::: "memory"); return true;
}
bool HaltIO::write(uint32_t offset, uint32_t value) {
    if (!map || !AXI::registerOffset(offset, true) || value > 1) return false;
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress() + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory"); return true;
}
Result HaltIO::request(bool query, bool asserted, bool &state) {
    if (!bus) return Result::NotReady;
    using namespace ClockController;
    const auto op = query ? GetResetState : asserted ? AssertReset : DeassertReset;
    auto r = targeted(GCC, op, AXI::ResetID, 0, BasebandConsumer);
    const auto status = bus->callPlatformFunction(Function, false, &r.request, nullptr, nullptr, nullptr);
    if (!valid(r.request) || r.request.version != 2 || r.request.size != sizeof(r) || r.controller != GCC ||
        r.request.operation != op || r.request.id != AXI::ResetID || r.consumer != BasebandConsumer || r.flags)
        return Result::Quarantined;
    switch (status) {
    case kIOReturnSuccess: break;
    case kIOReturnBadArgument: return Result::Invalid;
    case kIOReturnUnsupported: return Result::Unsupported;
    case kIOReturnNotReady: case kIOReturnNotAttached: return Result::NotReady;
    case kIOReturnBusy: return Result::Busy;
    case kIOReturnNotPermitted: return Result::Unsupported;
    case kIOReturnTimeout: return Result::Timeout;
    default: return Result::Quarantined;
    }
    if (query) {
        if (r.request.value > 1) return Result::Quarantined;
        state = r.request.value != 0;
    }
    return Result::OK;
}
Result HaltIO::resetState(bool &state) { return request(true, false, state); }
Result HaltIO::reset(bool asserted) { bool state = false; return request(false, asserted, state); }
uint64_t HaltIO::nowUS() {
    uint64_t absolute = 0, ns = 0;
    clock_get_uptime(&absolute); absolutetime_to_nanoseconds(absolute, &ns); return ns / 1000;
}
void HaltIO::delayUS(unsigned us) { IODelay(us); }
template class AXI::Controller<HaltIO>;
} // namespace Baseband
