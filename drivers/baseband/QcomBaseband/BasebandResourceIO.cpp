// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandResourceIO.h"
#include "BasebandHaltIO.h"
#include <IOKit/IOLib.h>

namespace Baseband {
struct NativeResources::Context {
    IOService *owner, *bus;
    HaltIO registers;
    AXI::Controller<HaltIO> axi{registers};
    ResourceLease<Context> lease;
    bool pinned = false;
    Context(IOService *o, IOService *b, const ResourcePolicy &p) : owner(o), bus(b), lease(*this, p) {
        owner->retain(); bus->retain();
    }
    ~Context() { bus->release(); owner->release(); }
    Result cold() { return axi.cold(); }
    Result request(ClockController::Controller kind, ClockController::Operation op,
                   uint32_t id, uint64_t &value, unsigned flags) {
        using namespace ClockController;
        auto r = targeted(kind, op, id, value, BasebandConsumer, flags);
        const auto status = bus->callPlatformFunction(Function, false, &r.request, nullptr, nullptr, nullptr);
        if (!valid(r.request) || r.request.version != 2 || r.request.size != sizeof(r) || r.controller != kind ||
            r.request.operation != op || r.request.id != id || r.consumer != BasebandConsumer || r.flags != flags)
            return Result::Quarantined;
        switch (status) {
        case kIOReturnSuccess: value = r.request.value; return Result::OK;
        case kIOReturnUnsupported: case kIOReturnNotPermitted: return Result::Unsupported;
        case kIOReturnBadArgument: return Result::Invalid;
        case kIOReturnNotReady: case kIOReturnNotAttached: return Result::NotReady;
        case kIOReturnBusy: return Result::Busy;
        case kIOReturnNoMemory: case kIOReturnNoResources: return Result::NoMemory;
        case kIOReturnTimeout: return Result::Timeout;
        default: return Result::Quarantined;
        }
    }
};
NativeResources::~NativeResources() { close(); }
bool NativeResources::quarantined() const {
    return context && (context->pinned || context->axi.quarantined() || context->lease.quarantined());
}
Result NativeResources::result(Result r) {
    if (context && (r == Result::Quarantined || quarantined())) {
        context->pinned = true;
        context->owner->setProperty("BasebandResourcesQuarantined", true);
        return Result::Quarantined;
    }
    return r;
}
Result NativeResources::bind(IOService *owner, const ResourcePolicy &policy) {
    if (context || !owner || !owner->metaCast("QcomBaseband") || !policy.valid()) return Result::Invalid;
    auto *provider = owner->getProvider();
    if (!provider || !provider->getProvider()) return Result::Invalid;
    context = new Context(owner, provider->getProvider(), policy);
    if (!context) return Result::NoMemory;
    if (!context->registers.bind(owner)) { close(); return Result::Invalid; }
    return Result::OK;
}
Result NativeResources::cold() { return !context ? Result::NotReady : result(context->axi.cold()); }
Result NativeResources::acquire() { return !context ? Result::NotReady : result(context->lease.acquire()); }
Result NativeResources::validate() { return !context ? Result::NotReady : result(context->lease.validate()); }
Result NativeResources::deassertReset() {
    if (!context || !context->lease.ready()) return Result::NotReady;
    auto r = validate(); if (r != Result::OK) return r;
    r = cold(); if (r != Result::OK) return r;
    r = context->registers.reset(false);
    if (r != Result::OK) return result(r == Result::Timeout || r == Result::IO ? Result::Quarantined : r);
    bool reset = true;
    r = context->registers.resetState(reset);
    return result(r == Result::OK && !reset ? Result::OK : Result::Quarantined);
}
Result NativeResources::quiesceAndReset() {
    if (!context || !context->lease.ready()) return Result::NotReady;
    const auto r = validate();
    return r == Result::OK ? result(context->axi.quiesceAndReset()) : r;
}
Result NativeResources::release() {
    return !context ? Result::NotReady : quarantined() ? Result::Quarantined : result(context->lease.release());
}
bool NativeResources::close() {
    if (!context) return true;
    // No implicit hardware shutdown from a destructor: only the Boot owner
    // can order reset/drain, memory reclamation, then vote/clock release.
    if (quarantined() || context->lease.held()) return false;
    delete context; context = nullptr; return true;
}
} // namespace Baseband
