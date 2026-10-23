// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandSignalIO.h"
#include <SMEMProtocol.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

namespace Baseband {
namespace {
Result converted(IOReturn r) {
    switch (r) {
    case kIOReturnSuccess: return Result::OK;
    case kIOReturnNotResponding: case kIOReturnOffline: return Result::Quarantined;
    case kIOReturnNoMemory: return Result::NoMemory;
    case kIOReturnUnsupported: return Result::Unsupported;
    case kIOReturnBusy: return Result::Busy;
    case kIOReturnBadArgument: return Result::Invalid;
    case kIOReturnNotReady: case kIOReturnNotAttached: return Result::NotReady;
    default: return Result::IO;
    }
}
}
struct NativeSignals::Context {
    IOService *owner = nullptr, *smem = nullptr;
    IOWorkLoop *loop = nullptr;
    IOMemoryMap *mailbox = nullptr;
    IOInterruptEventSource *events[3]{};
    bool added[3]{}, opened = false, ringsBound = false, negotiated = false;
    uint32_t remoteFeatures = 0;
    uint64_t epoch;
    SMP2P::Monitor monitor;
    GLINK::ColdPeerGate peer;
    struct Item {
        Context &context;
        volatile uint8_t *data = nullptr;
        uint64_t offset = 0, bytes = 0;
        explicit Item(Context &c) : context(c) {}
        bool read32(uint32_t at, uint32_t &value) {
            if (!data || (at & 3) || !range(at, 4, bytes)) return false;
            value = *reinterpret_cast<volatile uint32_t *>(data + at); return true;
        }
        bool store32(uint32_t at, uint32_t value) {
            if (!data || (at & 3) || !range(at, 4, bytes)) return false;
            *reinterpret_cast<volatile uint32_t *>(data + at) = value; return true;
        }
        void acquire() { __asm__ volatile("dmb oshld" ::: "memory"); }
        void release() { __asm__ volatile("dmb oshst" ::: "memory"); }
        bool notify() { return context.notify(14); }
    } outbound{*this}, inbound{*this}, descriptor{*this}, tx{*this}, rx{*this};
    explicit Context(uint64_t generation) : epoch(generation) {}
    ~Context() {
        for (unsigned i = 0; i < 3; ++i) {
            if (events[i]) events[i]->disable();
            if (added[i]) loop->removeEventSource(events[i]);
            OSSafeReleaseNULL(events[i]);
        }
        if (opened) smem->close(owner);
        OSSafeReleaseNULL(smem); OSSafeReleaseNULL(mailbox); OSSafeReleaseNULL(loop); OSSafeReleaseNULL(owner);
    }
    bool notify(unsigned bit) {
        if (!mailbox || (bit != 14 && bit != 15)) return false;
        // APCS IPC is write-one-to-set. Never RMW a shared RPM/ADSP mailbox.
        __asm__ volatile("dsb oshst" ::: "memory");
        *reinterpret_cast<volatile uint32_t *>(mailbox->getVirtualAddress() + 8) = 1u << bit;
        __asm__ volatile("dsb oshst" ::: "memory"); return true;
    }
    Result item(uint32_t host, uint32_t id, uint32_t allocate, Item &out) {
        QcomSMEMProtocol::Request request;
        request.operation = allocate ? QcomSMEMProtocol::Allocate : QcomSMEMProtocol::Lookup;
        request.host = host; request.item = id; request.bytes = allocate;
        const auto r = smem->callPlatformFunction(QcomSMEMProtocol::Function, true, &request, owner, nullptr, nullptr);
        if (r != kIOReturnSuccess) return converted(r);
        if ((request.flags & QcomSMEMProtocol::Cached) || !request.mapping || !request.length ||
            (request.offset & 7) || !range(request.offset, request.length, QcomSMEMProtocol::Bytes)) return Result::Invalid;
        out.offset = request.offset; out.bytes = request.length;
        out.data = static_cast<volatile uint8_t *>(request.mapping); return Result::OK;
    }
    static bool kick(void *c) { return static_cast<Context *>(c)->notify(15); }
};
NativeSignals::~NativeSignals() { close(); }
Result NativeSignals::bind(IOService *owner, IOWorkLoop *loop, uint64_t generation,
                          IOInterruptEventSource::Action smp, IOInterruptEventSource::Action glink,
                          IOInterruptEventSource::Action watchdog, IOFilterInterruptEventSource::Filter watchdogFilter) {
    if (context || !owner || !loop || !generation || generation <= lastEpoch || !smp || !glink || !watchdog)
        return Result::Invalid;
    auto *provider = owner->getProvider();
    if (!provider || provider->getDeviceMemoryCount() != 4) return Result::Invalid;
    auto *range = provider->getDeviceMemoryWithIndex(2);
    if (!range || range->getPhysicalAddress() != 0x17911000 || range->getLength() != 4096) return Result::Invalid;
    context = new Context(generation);
    if (!context) return Result::NoMemory;
    auto &c = *context; c.owner = owner; owner->retain(); c.loop = loop; loop->retain();
    auto *matching = IOService::serviceMatching("QcomSMEM");
    if (!matching) { close(); return Result::NoMemory; }
    c.smem = IOService::waitForMatchingService(matching, 5000000000ULL); matching->release();
    if (!c.smem) { close(); return Result::NotReady; }
    if (!c.smem->open(owner)) { close(); return Result::Busy; }
    c.opened = true;
    c.mailbox = provider->mapDeviceMemoryWithIndex(2, kIOMapAnywhere | kIOMapInhibitCache);
    if (!c.mailbox || c.mailbox->getLength() != 4096 || !c.mailbox->getVirtualAddress()) { close(); return Result::IO; }
    auto r = c.item(QcomSMEMProtocol::GlobalHost, SMP2P::OutboundItem, SMP2P::Bytes, c.outbound);
    if (r == Result::OK) r = c.item(1, 478, 32, c.descriptor);
    if (r == Result::OK) r = c.item(1, 479, GLINK::ModemFIFOBytes, c.tx);
    if (r != Result::OK) { close(); return r; }
    if ((c.outbound.bytes != 340 && c.outbound.bytes != 344) || c.descriptor.bytes != 32 ||
        c.tx.bytes != GLINK::ModemFIFOBytes) { close(); return Result::Invalid; }
    uint32_t txTail = 0, rxHead = 0;
    if (!c.descriptor.read32(0, txTail) || !c.descriptor.read32(12, rxHead)) { close(); return Result::IO; }
    c.descriptor.acquire();
    r = c.peer.observe(txTail, rxHead);
    if (r != Result::OK && r != Result::NotReady) { close(); return r; }
    // Disable IRQs during setup; the Board enables them before releasing Q6
    // reset, after installing its state/dispatch handlers.
    const IOInterruptEventSource::Action actions[] = {watchdog, smp, glink};
    for (unsigned i = 0; i < 3; ++i) {
        c.events[i] = i == 0 && watchdogFilter ?
            IOFilterInterruptEventSource::filterInterruptEventSource(owner, actions[i], watchdogFilter, provider, i) :
            IOInterruptEventSource::interruptEventSource(owner, actions[i], provider, i);
        if (c.events[i]) c.events[i]->disable();
        if (!c.events[i] || loop->addEventSource(c.events[i]) != kIOReturnSuccess) { close(); return Result::IO; }
        c.added[i] = true; c.events[i]->disable();
    }
    lastEpoch = generation;
    r = SMP2P::publish(c.outbound);
    if (r != Result::OK) return r; // Board retains this object until reset/drain cleanup.
    if (!c.descriptor.store32(4, 0) || !c.descriptor.store32(8, 0)) return Result::Quarantined;
    c.descriptor.release();
    return Result::OK;
}
Result NativeSignals::sample(uint64_t generation, SMP2P::Observation &observation, bool negotiate) {
    observation = {};
    if (!context || generation != context->epoch) return Result::Stale;
    auto &c = *context;
    if (!c.inbound.data) {
        const auto r = c.item(QcomSMEMProtocol::GlobalHost, SMP2P::InboundItem, 0, c.inbound);
        if (r != Result::OK) return r;
        if (c.inbound.bytes != 340 && c.inbound.bytes != 344) return Result::Invalid;
    }
    uint8_t image[SMP2P::Bytes]{};
    auto r = SMP2P::snapshot(c.inbound, image, sizeof(image));
    if (r == Result::NotReady && negotiate) { c.negotiated = false; c.remoteFeatures = 0; c.monitor.reset(); }
    if (r != Result::OK) return r;
    if (read32(image) != SMP2P::Magic || image[4] != 1 || read16(image + 8) != SMP2P::Modem ||
        read16(image + 10) != SMP2P::AP || read16(image + 12) != SMP2P::MaxEntries ||
        read16(image + 14) > SMP2P::MaxEntries) return Result::Invalid;
    const uint32_t features = read32(image + 4) >> 8;
    if (!negotiate) return c.monitor.sample(image, sizeof(image), observation);
    if (c.negotiated && features != c.remoteFeatures) return Result::Stale;
    if (!c.negotiated) {
        // Both peers must advertise SSR before the AP acknowledges restarts.
        if (!c.outbound.store32(4, 1 | ((features & SMP2P::SSRFeature) << 8))) return Result::Quarantined;
        c.outbound.release(); if (!c.outbound.notify()) return Result::Quarantined;
        c.remoteFeatures = features; c.negotiated = true;
    }
    r = c.monitor.sample(image, sizeof(image), observation);
    if (r == Result::OK) r = SMP2P::acknowledge(c.outbound, observation.outboundFlags);
    return r;
}
Result NativeSignals::stopRequest(uint64_t generation, bool requested) {
    return !context || generation != context->epoch ? Result::Stale : SMP2P::stop(context->outbound, requested);
}
Result NativeSignals::bindRings(uint64_t generation, GLINK::MemoryRing &transmit, GLINK::MemoryRing &receive) {
    if (!context || generation != context->epoch) return Result::Stale;
    auto &c = *context;
    if (c.ringsBound) return Result::Busy;
    uint32_t txTail = 0, rxHead = 0;
    if (!c.descriptor.read32(0, txTail) || !c.descriptor.read32(12, rxHead)) return Result::IO;
    c.descriptor.acquire();
    auto fresh = c.peer.observe(txTail, rxHead);
    if (fresh != Result::OK) return fresh;
    const auto r = c.item(1, 480, 0, c.rx); if (r != Result::OK) return r;
    if (c.rx.bytes != GLINK::ModemFIFOBytes) return Result::Invalid;
    const Context::Item *items[] = {&c.outbound, &c.inbound, &c.descriptor, &c.tx, &c.rx};
    for (unsigned i = 0; i < 5; ++i) for (unsigned j = 0; j < i; ++j)
        if (overlaps(items[i]->offset, items[i]->bytes, items[j]->offset, items[j]->bytes)) return Result::Invalid;
    auto *p = reinterpret_cast<volatile uint32_t *>(c.descriptor.data);
    if (!transmit.bind(c.tx.data, GLINK::ModemFIFOBytes, p + 1, p, &c, Context::kick) ||
        !receive.bind(c.rx.data, GLINK::ModemFIFOBytes, p + 3, p + 2, &c, Context::kick)) return Result::Invalid;
    c.ringsBound = true; return Result::OK;
}
bool NativeSignals::notifyGLINK() { return context && context->notify(15); }
void NativeSignals::enable() { if (context) for (auto *event : context->events) if (event) event->enable(); }
void NativeSignals::disable() { if (context) for (auto *event : context->events) if (event) event->disable(); }
void NativeSignals::close() { disable(); delete context; context = nullptr; }
} // namespace Baseband
