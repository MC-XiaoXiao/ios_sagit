// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandSecureIO.h"
#include "BasebandSecure.h"
#include <SecureMonitorAction.h>
#include <IOBufferAllocation.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSString.h>
#include "../../security/QcomSCM/SMCCC.h"

namespace Baseband {
namespace {
// XNU's native page is 16 KiB. Retain/check the whole allocation, even though
// SCM's descriptor wire format consumes only its first 256 bytes.
constexpr uint64_t CommandBytes = 16384;
struct Monitor {
    uint8_t *data;
    uint64_t address;
    void smc(Secure::Registers &frame) {
        QcomSecure::Registers native{};
        for (unsigned i = 0; i < 8; ++i) native.x[i] = frame.x[i];
        QcomSecure::smc(native);
        for (unsigned i = 0; i < 8; ++i) frame.x[i] = native.x[i];
    }
    void delayMS(unsigned ms) { IOSleep(ms); }
    bool publish(const void *p, uint64_t pa, size_t bytes) {
        if (p != data || pa != address || bytes != Secure::ScratchBytes) return false;
        // The common allocator maps this as Normal Non-Cacheable memory.
        // Device memory would fault on unaligned ELF/SCM protocol fields.
        __asm__ volatile("dsb sy" ::: "memory"); return true;
    }
};
Result status(IOReturn r) {
    switch (r) {
    case kIOReturnSuccess: return Result::OK;
    case kIOReturnNotResponding: case kIOReturnOffline: return Result::Quarantined;
    case kIOReturnNoMemory: return Result::NoMemory;
    case kIOReturnUnsupported: return Result::Unsupported;
    case kIOReturnNotReady: case kIOReturnNotAttached: return Result::NotReady;
    default: return Result::IO;
    }
}
}
struct NativeAssignment::Context {
    IOService *owner, *scm;
    IOBufferMemoryDescriptor *memory;
    BootLayout layout;
    Monitor monitor;
    Secure::Assignment<Monitor> transport;
    bool pinned = false;
    bool probing = false;
    bool confirming = false;
    bool rmtfsScope = false;
    Region region{};
    Ownership from = Ownership::Unknown, to = Ownership::Unknown;
    Result result = Result::NotReady;
    Context(IOService *o, IOService *s, IOBufferMemoryDescriptor *m, uint64_t pa, const BootLayout &l)
        : owner(o), scm(s), memory(m), layout(l), monitor{static_cast<uint8_t *>(m->getBytesNoCopy()), pa},
          transport(monitor, monitor.data, pa) { owner->retain(); scm->retain(); }
    static int32_t run(void *pointer) {
        auto &c = *static_cast<Context *>(pointer);
        const auto permissions = c.region.address == 0x88f00000 && c.region.bytes == 0x200000 ? 6u : 7u;
        c.result = c.probing ? c.transport.probe() : c.transport.change(c.region, c.from, c.to, c.confirming, permissions);
        return c.result == Result::Quarantined ? kIOReturnNotResponding : kIOReturnSuccess;
    }
    Result call() {
        if (pinned) return Result::Quarantined;
        SecureMonitorAction::Request action;
        action.action = run; action.context = this;
        const auto r = scm->callPlatformFunction(SecureMonitorAction::Function, true, &action, owner, nullptr, nullptr);
        if (r != kIOReturnSuccess) {
            result = status(r);
            if (result == Result::Quarantined) pinned = true;
        }
        if (transport.quarantined()) pinned = true;
        if (pinned) {
            owner->setProperty("BasebandSCMQuarantined", true);
            return Result::Quarantined;
        }
        return result;
    }
};
NativeAssignment::~NativeAssignment() {
    // An unresolved monitor transaction can retain command physical memory.
    // Keep the entire context and its owner alive if cleanup is unproven.
    close();
}
bool NativeAssignment::quarantined() const { return context && context->pinned; }
Result NativeAssignment::bind(IOService *owner, IOService *scm, const BootLayout &admitted, bool rmtfs) {
    if (context || !owner || !scm || !owner->metaCast("QcomBaseband") ||
        !scm->metaCast("QcomSCM") || !admitted.valid()) return Result::Invalid;
    auto *onub = owner->getProvider(), *snub = scm->getProvider();
    if (!onub || !snub || onub->getProvider() != snub->getProvider()) return Result::Invalid;
    auto *convention = OSDynamicCast(OSString, scm->getProperty("SecureFirmwareConvention"));
    if (!convention || (!convention->isEqualTo("smc64") && !convention->isEqualTo("smc32")))
        return Result::Unsupported;
    auto *memory = IOKitMemory::allocateBuffer(IOKitMemory::bufferOptions(false, true), CommandBytes, CommandBytes, 0xffffc000);
    if (!memory) return Result::NoMemory;
    if (memory->prepare(kIODirectionInOut) != kIOReturnSuccess) { memory->release(); return Result::NoMemory; }
    IOByteCount length = 0;
    const uint64_t pa = memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    const Region regions[] = {admitted.modem, admitted.mba, admitted.metadata};
    bool valid = pa && !(pa & (CommandBytes - 1)) && range(pa, CommandBytes, uint64_t(1) << 32) && length >= CommandBytes;
    for (const auto &r : regions) valid &= !overlaps(r.address, r.bytes, pa, CommandBytes);
    if (rmtfs) valid &= !overlaps(0x88f00000, 0x200000, pa, CommandBytes);
    if (!valid) { memory->complete(); memory->release(); return Result::Invalid; }
    context = new Context(owner, scm, memory, pa, admitted);
    if (!context) { memory->complete(); memory->release(); return Result::NoMemory; }
    context->rmtfsScope = rmtfs;
    return Result::OK;
}
Result NativeAssignment::probe() {
    if (!context) return Result::NotReady;
    context->probing = true; return context->call();
}
Result NativeAssignment::assign(const Region &region, Ownership from, Ownership to) {
    if (!context) return Result::NotReady;
    const Region regions[] = {context->layout.modem, context->layout.mba, context->layout.metadata};
    bool admitted = false;
    for (const auto &r : regions) admitted |= region.address == r.address && region.bytes == r.bytes;
    admitted |= context->rmtfsScope && region.address == 0x88f00000 && region.bytes == 0x200000;
    if (!admitted) return Result::Invalid;
    context->probing = false; context->confirming = false;
    context->region = region; context->from = from; context->to = to;
    return context->call();
}
Result NativeAssignment::confirmHost(const Region &region) {
    if (!context) return Result::NotReady;
    const Region regions[] = {context->layout.modem, context->layout.mba, context->layout.metadata};
    bool admitted = false;
    for (const auto &r : regions) admitted |= region.address == r.address && region.bytes == r.bytes;
    admitted |= context->rmtfsScope && region.address == 0x88f00000 && region.bytes == 0x200000;
    if (!admitted) return Result::Invalid;
    context->probing = false; context->confirming = true; context->region = region;
    context->from = context->to = Ownership::Host;
    return context->call();
}
bool NativeAssignment::close() {
    if (!context) return true;
    if (context->pinned || context->memory->complete(kIODirectionInOut) != kIOReturnSuccess) {
        context->pinned = true; return false;
    }
    context->memory->release(); context->scm->release(); context->owner->release();
    delete context; context = nullptr; return true;
}
} // namespace Baseband
