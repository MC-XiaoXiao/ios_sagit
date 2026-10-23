// SPDX-License-Identifier: GPL-2.0-only
// One AP owner for SMEM's TCSR lock, shared by modem and ADSP clients.
#include "QcomSMEM.h"
#include "../../baseband/QcomBaseband/BasebandSMEMPool.h"
#include <SMEMProtocol.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>

OSDefineMetaClassAndStructors(QcomSMEM, IOService);
namespace {
using Baseband::Result;
void acquire() { __asm__ volatile("dmb oshld" ::: "memory"); }
void release() { __asm__ volatile("dmb oshst" ::: "memory"); }
uint64_t nowUS() {
    uint64_t t = 0, ns = 0; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns / 1000;
}
bool smemReserved() {
    auto *node = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (!node) return false;
    auto *data = OSDynamicCast(OSData, node->getProperty("QcomSMEM"));
    uint64_t region[2]{};
    bool match = data && data->getLength() == sizeof(region);
    if (match) {
        memcpy(region, data->getBytesNoCopy(), sizeof(region));
        match = region[0] == QcomSMEMProtocol::Base && region[1] == QcomSMEMProtocol::Bytes;
    }
    node->release(); return match;
}
IOReturn native(Result r) {
    switch (r) {
    case Result::OK: return kIOReturnSuccess;
    case Result::Invalid: return kIOReturnBadArgument;
    case Result::Unsupported: return kIOReturnUnsupported;
    case Result::NotReady: return kIOReturnNotReady;
    case Result::Busy: return kIOReturnBusy;
    case Result::Timeout: return kIOReturnTimeout;
    case Result::NoMemory: return kIOReturnNoMemory;
    case Result::Quarantined: return kIOReturnNotResponding;
    default: return kIOReturnIOError;
    }
}
}
struct QcomSMEM::State {
    IOMemoryMap *maps[2]{};
    IOLock *apLock = nullptr;
    volatile uint8_t *memory = nullptr;
    volatile uint32_t *mutex = nullptr;
    bool held = false;
    Baseband::SMEMPool<State> pool{*this, QcomSMEMProtocol::Bytes};
    ~State() { OSSafeReleaseNULL(maps[1]); OSSafeReleaseNULL(maps[0]); if (apLock) IOLockFree(apLock); }
    bool bind(IOService *provider) {
        const uint64_t bases[] = {QcomSMEMProtocol::Base, 0x01f40000};
        const uint64_t sizes[] = {QcomSMEMProtocol::Bytes, 0x20000};
        if (!provider || provider->getDeviceMemoryCount() != 2 || !::smemReserved()) return false;
        apLock = IOLockAlloc(); if (!apLock) return false;
        for (unsigned i = 0; i < 2; ++i) {
            auto *range = provider->getDeviceMemoryWithIndex(i);
            if (!range || range->getPhysicalAddress() != bases[i] || range->getLength() != sizes[i]) return false;
            maps[i] = provider->mapDeviceMemoryWithIndex(i, kIOMapAnywhere |
                (i ? kIOMapInhibitCache : kIOMapWriteCombineCache));
            if (!maps[i] || !maps[i]->getVirtualAddress() || maps[i]->getLength() != sizes[i]) return false;
        }
        memory = reinterpret_cast<volatile uint8_t *>(maps[0]->getVirtualAddress());
        mutex = reinterpret_cast<volatile uint32_t *>(maps[1]->getVirtualAddress() + 0x3000);
        return true;
    }
    Result lock() {
        if (!apLock || !mutex) return Result::NotReady;
        IOLockLock(apLock);
        const auto start = nowUS();
        for (unsigned poll = 0; poll < 10000; ++poll) {
            *mutex = 1; __asm__ volatile("dsb oshst" ::: "memory");
            if (*mutex == 1) { ::acquire(); held = true; return Result::OK; }
            if (nowUS() - start >= 10000) break;
            IODelay(1);
        }
        IOLockUnlock(apLock); return Result::Busy;
    }
    bool unlock() {
        if (!held) return false;
        ::acquire(); const bool ours = *mutex == 1;
        if (ours) { ::release(); *mutex = 0; __asm__ volatile("dsb oshst" ::: "memory"); }
        // Never force-unlock or bust a different remote owner's hardware lock.
        held = false; IOLockUnlock(apLock); return ours;
    }
    void acquire() { ::acquire(); }
    void release() { ::release(); }
    bool read(uint32_t offset, void *buffer, size_t bytes) {
        if (!held || !buffer || !Baseband::range(offset, bytes, QcomSMEMProtocol::Bytes)) return false;
        auto *out = static_cast<uint8_t *>(buffer);
        for (size_t i = 0; i < bytes; ++i) out[i] = memory[offset + i];
        return true;
    }
    bool write(uint32_t offset, const void *buffer, size_t bytes) {
        if (!held || !buffer || !Baseband::range(offset, bytes, QcomSMEMProtocol::Bytes)) return false;
        const auto *in = static_cast<const uint8_t *>(buffer);
        for (size_t i = 0; i < bytes; ++i) memory[offset + i] = in[i];
        return true;
    }
    bool store32(uint32_t offset, uint32_t value) {
        if (!held || (offset & 3) || !Baseband::range(offset, 4, QcomSMEMProtocol::Bytes)) return false;
        *reinterpret_cast<volatile uint32_t *>(memory + offset) = value; return true;
    }
};
bool QcomSMEM::scoped(const IOService *client) const {
    auto *nub = client ? client->getProvider() : nullptr;
    auto *ours = getProvider();
    return client && (client->metaCast("QcomBaseband") || client->metaCast("QcomADSP")) &&
        ours && nub && nub->getProvider() == ours->getProvider();
}
bool QcomSMEM::handleOpen(IOService *client, IOOptionBits options, void *argument) {
    if (options || argument || !__atomic_load_n(&fActive, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&fPinned, __ATOMIC_ACQUIRE) || !scoped(client) || !fClients) return false;
    auto *&slot = client->metaCast("QcomBaseband") ? fModem : fADSP;
    if (slot || fClients->containsObject(client) || !fClients->setObject(client)) return false;
    slot = client; return true;
}
void QcomSMEM::handleClose(IOService *client, IOOptionBits) {
    if (fModem == client) fModem = nullptr;
    if (fADSP == client) fADSP = nullptr;
    if (fClients) fClients->removeObject(client);
}
bool QcomSMEM::handleIsOpen(const IOService *client) const {
    return fClients && (client ? fClients->containsObject(client) : fClients->getCount() != 0);
}
void QcomSMEM::pin() {
    if (!__atomic_exchange_n(&fPinned, true, __ATOMIC_ACQ_REL)) retain();
    setProperty("SMEMQuarantined", true);
}
IOReturn QcomSMEM::action(OSObject *object, void *request, void *caller, void *, void *) {
    auto *self = static_cast<QcomSMEM *>(object);
    auto *client = static_cast<IOService *>(caller);
    auto *r = static_cast<QcomSMEMProtocol::Request *>(request);
    if (!__atomic_load_n(&self->fActive, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&self->fPinned, __ATOMIC_ACQUIRE) || !self->fState) return kIOReturnNotReady;
    if (!self->scoped(client) || !self->isOpen(client)) return kIOReturnNotPermitted;
    if (!r || !QcomSMEMProtocol::permitted(*r, client->metaCast("QcomBaseband"))) return kIOReturnBadArgument;
    auto &s = *self->fState;
    Result result;
    if (r->operation == QcomSMEMProtocol::Snapshot) {
        result = s.lock();
        if (result != Result::OK) return native(result);
        const bool copied = s.read(0, r->buffer, r->bytes);
        if (!s.unlock()) { self->pin(); return kIOReturnNotResponding; }
        return copied ? kIOReturnSuccess : kIOReturnIOError;
    }
    Baseband::SMEMItem item{}; bool created = false;
    result = s.pool.item(uint16_t(r->host), uint16_t(r->item),
        r->operation == QcomSMEMProtocol::Allocate ? r->bytes : 0, item, created);
    if (result == Result::Quarantined) self->pin();
    if (result != Result::OK) return native(result);
    r->offset = item.offset; r->length = item.bytes;
    r->flags = (item.cached ? QcomSMEMProtocol::Cached : 0) | (created ? QcomSMEMProtocol::Created : 0);
    r->mapping = const_cast<uint8_t *>(s.memory + item.offset);
    return kIOReturnSuccess;
}
IOReturn QcomSMEM::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) {
    if (!name || !name->isEqualTo(QcomSMEMProtocol::Function))
        return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
    if (ml_at_interrupt_context() || !fGate) return kIOReturnNotReady;
    if (p3 || p4 || !p1 || !p2) return kIOReturnBadArgument;
    auto *client = OSDynamicCast(IOService, static_cast<OSObject *>(p2));
    if (!client) return kIOReturnBadArgument;
    return fGate->runAction(action, p1, client);
}
bool QcomSMEM::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fState = new State; fClients = OSSet::withCapacity(2);
    fLoop = IOWorkLoop::workLoop(); fGate = IOCommandGate::commandGate(this);
    if (!fState || !fClients || !fLoop || !fGate || fLoop->addEventSource(fGate) != kIOReturnSuccess ||
        !fState->bind(provider)) { stop(provider); return false; }
    setProperty("SMEMAddress", QcomSMEMProtocol::Base, 64);
    setProperty("SMEMBytes", QcomSMEMProtocol::Bytes, 64);
    setProperty("SMEMQuarantined", false);
    setProperty("SMEMHardwareLock", 3, 32);
    __atomic_store_n(&fActive, true, __ATOMIC_RELEASE);
    registerService(kIOServiceSynchronous); return true;
}
IOReturn QcomSMEM::deactivate(OSObject *object, void *, void *, void *, void *) {
    __atomic_store_n(&static_cast<QcomSMEM *>(object)->fActive, false, __ATOMIC_RELEASE);
    return kIOReturnSuccess;
}
void QcomSMEM::stop(IOService *provider) {
    if (fGate) fGate->runAction(deactivate);
    else __atomic_store_n(&fActive, false, __ATOMIC_RELEASE);
    IOService::stop(provider);
}
void QcomSMEM::free() {
    if (fLoop && fGate) fLoop->removeEventSource(fGate);
    OSSafeReleaseNULL(fGate); OSSafeReleaseNULL(fLoop); OSSafeReleaseNULL(fClients);
    delete fState; fState = nullptr; IOService::free();
}
bool QcomSMEM::requestTerminate(IOService *provider, IOOptionBits options) {
    return !__atomic_load_n(&fPinned, __ATOMIC_ACQUIRE) && !isOpen() && IOService::requestTerminate(provider, options);
}
IOWorkLoop *QcomSMEM::getWorkLoop() const { return fLoop; }
