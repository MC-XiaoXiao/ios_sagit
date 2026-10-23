// SPDX-License-Identifier: BSD-2-Clause
#include "QcomPowerService.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#define super GenericClockController
OSDefineMetaClassAndAbstractStructors(QcomPowerService, super);
bool QcomPowerService::mapResources(IOService *provider, const UInt64 *addresses, const UInt64 *sizes, unsigned count)
{
    if (!count || count > 5 || provider->getDeviceMemoryWithIndex(count)) return false;
    for (unsigned i = 0; i < count; ++i) {
        auto *memory = provider->getDeviceMemoryWithIndex(i);
        if (!memory || memory->getPhysicalAddress() != addresses[i] || memory->getLength() != sizes[i]) {
            IOLog("QcomPowerService: %s resource %u mismatch: address=0x%llx size=0x%llx\n",
                provider->getName(), i, memory ? (unsigned long long)memory->getPhysicalAddress() : 0,
                memory ? (unsigned long long)memory->getLength() : 0);
            return false;
        }
        IOLog("QcomPowerService: mapping %s resource %u\n", provider->getName(), i);
        fMaps[i] = provider->mapDeviceMemoryWithIndex(i);
        if (!fMaps[i] || !fMaps[i]->getVirtualAddress() || fMaps[i]->getLength() < sizes[i]) return false;
    }
    IOLog("QcomPowerService: mapped %s resources\n", provider->getName());
    return true;
}
uint32_t QcomPowerService::read(void *context, unsigned bank, uint32_t offset)
{
    auto *self = static_cast<QcomPowerService *>(context);
    if (bank >= 5 || !self->fMaps[bank] || (offset & 3) || offset > self->fMaps[bank]->getLength()-4) return UINT32_MAX;
    auto *registers = reinterpret_cast<volatile UInt32 *>(self->fMaps[bank]->getVirtualAddress());
    UInt32 value = registers[offset/4];
    __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld" : "+r"(value) :: "memory"); return value;
}
void QcomPowerService::write(void *context, unsigned bank, uint32_t offset, uint32_t value)
{
    auto *self = static_cast<QcomPowerService *>(context);
    if (bank >= 5 || !self->fMaps[bank] || (offset & 3) || offset > self->fMaps[bank]->getLength()-4) return;
    auto *registers = reinterpret_cast<volatile UInt32 *>(self->fMaps[bank]->getVirtualAddress());
    __asm__ volatile("dmb oshst" ::: "memory"); registers[offset/4] = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
void QcomPowerService::wait(void *, unsigned us)
{
    if (us >= 1000) { IOSleep((us+999)/1000); return; }
    // XNU IODelay can thread_block when CPU idle latency is unknown. A PMIC
    // observer poll during boot must not depend on a timer-driven wakeup or
    // Apple's WFE timeout behavior. Bound short polling by the ARM timebase.
    UInt64 deadline, now;
    clock_interval_to_deadline(us, 1000, &deadline);
    do { clock_get_uptime(&now); __asm__ volatile("yield"); } while (now < deadline);
}
void QcomPowerService::kick(void *context) { static_cast<QcomPowerService *>(context)->signal(); }
IOReturn QcomPowerService::resourceRequest(ClockController::TargetedRequest &r)
{
    IOService *nub = getProvider(), *bus = nub ? nub->getProvider() : nullptr;
    if (!bus) return kIOReturnNotAttached;
    const OSSymbol *function = OSSymbol::withCStringNoCopy(ClockController::Function);
    if (!function) return kIOReturnNoMemory;
    IOReturn status = bus->callPlatformFunction(function, false, &r.request, nullptr, nullptr, nullptr);
    function->release(); return status;
}
IOReturn QcomPowerService::convert(QcomPower::Status s)
{
    using namespace QcomPower;
    switch (s) {
    case OK: return kIOReturnSuccess;
    case Invalid: return kIOReturnBadArgument;
    case Unsupported: return kIOReturnUnsupported;
    case NotReady: return kIOReturnNotReady;
    case Busy: return kIOReturnBusy;
    case TimedOut: return kIOReturnTimeout;
    case Denied: return kIOReturnNotPermitted;
    case Failed: case Stale: return kIOReturnIOError;
    }
    return kIOReturnError;
}
const UInt8 *QcomPowerService::cells(IOService *provider, const char *key, unsigned stride, unsigned &count)
{
    count = 0;
    if (!provider->getProperty(key)) return nullptr;
    auto *data = OSDynamicCast(OSData, provider->getProperty(key));
    if (!data || !data->getLength() || data->getLength() % stride) { count = UINT32_MAX; return nullptr; }
    count = data->getLength()/stride;
    return static_cast<const UInt8 *>(data->getBytesNoCopy());
}
void QcomPowerService::free()
{
    for (auto &map : fMaps) OSSafeReleaseNULL(map);
    super::free();
}
