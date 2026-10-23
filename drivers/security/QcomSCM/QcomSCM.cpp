// SPDX-License-Identifier: GPL-2.0-only
#include "QcomSCM.h"
#include "SCMPAS.h"
#include "SMCCC.h"
#include <SecureFirmwareProtocol.h>
#include <SecureMonitorAction.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMapper.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>

OSDefineMetaClassAndStructors(QcomSCM, IOService);
static_assert(uint32_t(QcomSecure::Phase::Warm) == SecureFirmware::Warm &&
    uint32_t(QcomSecure::Phase::Faulted) == SecureFirmware::Faulted, "PAS phase ABI");
namespace {
using QcomSecure::Status;
IOReturn native(Status s) {
    switch (s) {
    case Status::OK: return kIOReturnSuccess;
    case Status::Invalid: return kIOReturnBadArgument;
    case Status::Unsupported: return kIOReturnUnsupported;
    case Status::NoMemory: return kIOReturnNoMemory;
    case Status::Busy: return kIOReturnBusy;
    case Status::Denied: return kIOReturnNotPermitted;
    default: return kIOReturnIOError;
    }
}
struct Monitor {
    void smc(QcomSecure::Registers &r) {
        QcomSecure::smc(r);
    }
    void sleep(unsigned ms) { IOSleep(ms); }
};
} // namespace

struct QcomSCM::State {
    IOService *provider, *bus, *owner = nullptr;
    IOLock *lock = nullptr, *monitorLock = nullptr;
    Monitor monitor;
    QcomSecure::Transport<Monitor> transport{monitor};
    QcomSecure::PAS<QcomSecure::Transport<Monitor>> pas{transport};
    IOBufferMemoryDescriptor *metadata = nullptr;
    IOMemoryDescriptor *region = nullptr;
    uint64_t serial = 0, token = 0, capabilities = 0;
    unsigned activeActions = 0;
    bool stopping = false, metadataPrepared = false, regionPrepared = false;
    explicit State(IOService *nub) : provider(nub), bus(nub->getProvider()) {
        provider->retain(); if (bus) bus->retain();
        lock = IOLockAlloc(); monitorLock = IOLockAlloc();
    }
    bool scoped(IOService *client) const {
        auto *nub = client ? client->getProvider() : nullptr;
        return client && client->metaCast("QcomGPU") && nub && nub->getProvider() == bus;
    }
    bool address(IOMemoryDescriptor *memory, uint64_t &pa) const {
        const uint64_t bytes = memory->getLength();
        IOByteCount length = 0;
        pa = memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
        const uint64_t limit = transport.is64() ? 1ULL << 40 : 1ULL << 32;
        return bytes && !(bytes & 4095) && bytes <= 16 * 1024 * 1024 && pa && !(pa & 4095) &&
            pa < limit && bytes <= limit - pa && length >= bytes;
    }
    bool dispose(IOMemoryDescriptor *&memory, bool &prepared) {
        if (prepared && memory->complete(kIODirectionInOut) != kIOReturnSuccess) { pas.fault(); return false; }
        prepared = false; OSSafeReleaseNULL(memory); return true;
    }
    bool disposeMetadata() {
        IOMemoryDescriptor *memory = metadata;
        if (!dispose(memory, metadataPrepared)) return false;
        metadata = nullptr; return true;
    }
    bool available(uint32_t service, uint32_t command) {
        const auto r = transport.available(service, command);
        return r.status == Status::OK && r.value == 1;
    }
    Status init(OSData *data) {
        if (pas.phase() != QcomSecure::Phase::Empty || metadata || region) return Status::Invalid;
        const auto bytes = data->getLength();
        if (bytes < 52 || bytes > 4 * 1024 * 1024) return Status::Invalid;
        const auto *image = static_cast<const uint8_t *>(data->getBytesNoCopy());
        if (!image || memcmp(image, "\177ELF", 4) || image[4] != 1 || image[5] != 1) return Status::Invalid;
        const uint64_t capacity = (uint64_t(bytes) + 4095) & ~4095ULL;
        const uint64_t mask = ((1ULL << (transport.is64() ? 40 : 32)) - 1) & ~4095ULL;
        if (reinterpret_cast<uintptr_t>(IOMapper::gSystem) & 3) return Status::Busy;
        // MapperNone overlaps the buffer cache-mode field. Request host PA
        // contiguity explicitly and keep the uncached allocation mode intact.
        metadata = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryHostPhysicallyContiguous | kIOMapInhibitCache,
            capacity, mask);
        if (!metadata) return Status::NoMemory;
        if (!metadata->getBytesNoCopy() || metadata->prepare(kIODirectionInOut) != kIOReturnSuccess) {
            OSSafeReleaseNULL(metadata); return Status::NoMemory;
        }
        metadataPrepared = true;
        uint64_t pa = 0;
        if (!address(metadata, pa)) { disposeMetadata(); return Status::Invalid; }
        bzero(metadata->getBytesNoCopy(), capacity);
        memcpy(metadata->getBytesNoCopy(), image, bytes);
        const auto status = pas.init(pa);
        // A service-level rejection may leave metadata protected. Keep it
        // until reset if the firmware did not confirm a clean transaction.
        if (status == Status::Denied) pas.fault();
        if (pas.phase() == QcomSecure::Phase::Empty) disposeMetadata();
        return status;
    }
    Status setup(IOMemoryDescriptor *memory) {
        if (pas.phase() != QcomSecure::Phase::Metadata || region || !metadata) return Status::Invalid;
        const auto bytes = memory->getLength();
        if (memory == metadata || !bytes || (bytes & 4095) || bytes > 16 * 1024 * 1024) return Status::Invalid;
        memory->retain(); region = memory;
        if (memory->prepare(kIODirectionInOut) != kIOReturnSuccess) {
            OSSafeReleaseNULL(region); return Status::NoMemory;
        }
        regionPrepared = true;
        uint64_t pa = 0;
        if (!address(memory, pa)) { dispose(region, regionPrepared); return Status::Invalid; }
        uint64_t metadataPA = 0;
        if (!address(metadata, metadataPA) || (pa < metadataPA + metadata->getLength() &&
            metadataPA < pa + memory->getLength())) { dispose(region, regionPrepared); return Status::Invalid; }
        const auto status = pas.setup(pa, memory->getLength());
        if (status == Status::Denied) pas.fault();
        if (pas.phase() == QcomSecure::Phase::Metadata) dispose(region, regionPrepared);
        return status;
    }
    Status authenticate() {
        if (pas.phase() != QcomSecure::Phase::Region || !region || !regionPrepared) return Status::Invalid;
        // The loader writes the payload after MEM_SETUP. PAS consumes host
        // physical memory, so make those writes visible before authentication.
        if (region->performOperation(kIOMemoryIncoherentIOStore, 0, region->getLength()) != kIOReturnSuccess)
            return Status::Unsupported;
        return pas.authenticate();
    }
};

bool QcomSCM::quarantined() const {
    return __atomic_load_n(&fPinned, __ATOMIC_ACQUIRE);
}
void QcomSCM::latchQuarantine() {
    if (!__atomic_exchange_n(&fPinned, true, __ATOMIC_ACQ_REL)) retain();
}
void QcomSCM::pin() {
    latchQuarantine();
    if (fState) fState->pas.fault();
    setProperty("SecureFirmwareQuarantined", true);
}
bool QcomSCM::start(IOService *provider) {
    auto *method = OSDynamicCast(OSData, provider->getProperty("method"));
    if (!method || method->getLength() != 4 || memcmp(method->getBytesNoCopy(), "smc", 4) ||
        provider->getProperty("reg") || !IOService::start(provider)) return false;
    fStarted = true;
    if (!provider->open(this)) { stop(provider); return false; }
    fProviderOpen = true;
    fState = new State(provider);
    if (!fState || !fState->lock || !fState->monitorLock || !fState->bus || fState->transport.probe() != Status::OK) {
        stop(provider); return false;
    }
    auto &s = *fState;
    if (s.available(1, 10)) s.capabilities |= SecureFirmware::RemoteState;
    bool pas = true;
    const uint32_t commands[] = {1, 2, 5, 6, 7};
    for (const auto command : commands) pas = s.available(2, command) && pas;
    if (pas) {
        const auto supported = s.transport.supported();
        if (supported.status == Status::OK && supported.value) s.capabilities |= SecureFirmware::PeripheralAuthentication;
    }
    if (!setProperty("SecureFirmwareCapabilities", s.capabilities, 64) ||
        !setProperty("SecureFirmwareConvention", s.transport.is64() ? "smc64" : "smc32") ||
        !setProperty("SecureFirmwareQuarantined", false)) { stop(provider); return false; }
    registerService(kIOServiceSynchronous);
    publishResource("SCMControllerReady", this);
    return true;
}
IOReturn QcomSCM::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) {
    if (name && name->isEqualTo(SecureMonitorAction::Function)) {
        if (ml_at_interrupt_context() || !fState || !fState->lock) return kIOReturnNotReady;
        auto *r=static_cast<SecureMonitorAction::Request *>(p1);
        auto *client=OSDynamicCast(IOService,static_cast<OSObject *>(p2));
        if (!r || !SecureMonitorAction::valid(*r) || !client || p3 || p4) return kIOReturnBadArgument;
        auto &s=*fState;
        auto *nub=client->getProvider();
        // Resident clients on this SoC bus retain their own command buffers.
        // The modem needs MP_ASSIGN, whose seven arguments cannot use PAS13.
        if ((!client->metaCast("FingerprintHID") && !client->metaCast("QcomBaseband")) ||
            !nub || nub->getProvider()!=s.bus) return kIOReturnNotPermitted;
        IOLockLock(s.lock);
        if (s.stopping || quarantined() || s.activeActions==UINT32_MAX) {
            IOLockUnlock(s.lock); return kIOReturnOffline;
        }
        ++s.activeActions;
        IOLockUnlock(s.lock);
        // PAS validation only reads the lease and cached phase. A slow QSEE
        // transaction must not hold that metadata lock across secure calls.
        // PAS takes lock -> monitorLock; this path releases monitorLock before
        // reacquiring lock, so neither client can reverse that order.
        IOLockLock(s.monitorLock);
        IOReturn result=(__atomic_load_n(&s.stopping, __ATOMIC_ACQUIRE) || quarantined()) ?
            kIOReturnOffline : r->action(r->context);
        // An unresolved secure-world continuation must block new SCM traffic,
        // including PAS. Latch before releasing monitorLock; publishing the
        // faulted phase below cannot leave a window for another transaction.
        if (result==kIOReturnNotResponding) latchQuarantine();
        IOLockUnlock(s.monitorLock);
        IOLockLock(s.lock);
        --s.activeActions;
        if (quarantined()) pin();
        IOLockUnlock(s.lock);
        return result;
    }
    if (!name || !name->isEqualTo(SecureFirmware::Function))
        return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
    if (ml_at_interrupt_context() || !fState || !fState->lock) return kIOReturnNotReady;
    auto *r = static_cast<SecureFirmware::Request *>(p1);
    auto *client = OSDynamicCast(IOService, static_cast<OSObject *>(p2));
    if (!r || !SecureFirmware::valid(*r) || !client || p3 || p4 ||
        (r->metadata && !OSDynamicCast(OSData, r->metadata)) ||
        (r->memory && !OSDynamicCast(IOMemoryDescriptor, r->memory))) return kIOReturnBadArgument;
    auto &s = *fState;
    IOLockLock(s.lock);
    Status status = Status::Invalid;
    bool unpin = false;
    if (s.stopping || quarantined()) status = Status::Ambiguous;
    else if (client != s.owner || !s.scoped(client)) status = Status::Denied;
    else if (r->action == SecureFirmware::Action::Acquire) {
        if (s.token) status = Status::Busy;
        else if (!s.capabilities || s.serial == UINT64_MAX) status = Status::Unsupported;
        else { s.token = ++s.serial; r->token = s.token; retain(); status = Status::OK; }
    } else if (!s.token || r->token != s.token) status = Status::Denied;
    else {
        const bool secure = r->action != SecureFirmware::Action::Validate && r->action != SecureFirmware::Action::Release;
        if (secure) IOLockLock(s.monitorLock);
        // A QSEE action can quarantine the monitor while PAS waits for it.
        if (quarantined()) status = Status::Ambiguous;
        else switch (r->action) {
        case SecureFirmware::Action::Resume:
            status = s.capabilities & SecureFirmware::RemoteState ? s.pas.resume() : Status::Unsupported; break;
        case SecureFirmware::Action::Validate:
            status = s.pas.phase() == QcomSecure::Phase::Faulted ? Status::Ambiguous : Status::OK; break;
        case SecureFirmware::Action::Release:
            if (!s.metadata && !s.region && s.pas.release()) { s.token = 0; r->token = 0; unpin = true; status = Status::OK; }
            else status = Status::Busy;
            break;
        case SecureFirmware::Action::InitImage:
            status = s.capabilities & SecureFirmware::PeripheralAuthentication ? s.init(r->metadata) : Status::Unsupported; break;
        case SecureFirmware::Action::MemSetup:
            status = s.setup(r->memory); break;
        case SecureFirmware::Action::Authenticate:
            status = s.authenticate(); break;
        case SecureFirmware::Action::Shutdown:
            status = s.pas.shutdown();
            if (status == Status::OK && (!s.dispose(s.region, s.regionPrepared) || !s.disposeMetadata())) status = Status::Ambiguous;
            break;
        default: break;
        }
        if (s.pas.phase() == QcomSecure::Phase::Faulted) latchQuarantine();
        if (secure) IOLockUnlock(s.monitorLock);
    }
    if (quarantined() || s.pas.phase() == QcomSecure::Phase::Faulted) pin();
    r->capabilities = s.capabilities; r->state = uint32_t(s.pas.phase());
    r->flags = quarantined() ? SecureFirmware::Quarantined : 0;
    IOLockUnlock(s.lock);
    if (unpin) release();
    return native(status);
}
bool QcomSCM::handleOpen(IOService *client, IOOptionBits, void *) {
    if (!fState || !fState->lock) return false;
    auto &s = *fState; IOLockLock(s.lock);
    const bool allowed = !s.stopping && !quarantined() && s.scoped(client) && (!s.owner || s.owner == client);
    if (allowed && !s.owner) { s.owner = client; client->retain(); }
    IOLockUnlock(s.lock); return allowed;
}
void QcomSCM::handleClose(IOService *client, IOOptionBits) {
    if (!fState || !fState->lock) return;
    auto &s = *fState; IOLockLock(s.lock); IOService *closed = nullptr;
    if (client == s.owner) {
        if (s.token || s.metadata || s.region) pin();
        else { closed = s.owner; s.owner = nullptr; }
    }
    IOLockUnlock(s.lock); if (closed) closed->release();
}
bool QcomSCM::handleIsOpen(const IOService *client) const {
    if (!fState || !fState->lock) return false;
    auto &s = *fState; IOLockLock(s.lock);
    const bool open = s.owner && (!client || client == s.owner);
    IOLockUnlock(s.lock); return open;
}
bool QcomSCM::requestTerminate(IOService *, IOOptionBits) {
    if (!fState || !fState->lock) return !quarantined();
    auto &s = *fState; IOLockLock(s.lock);
    const bool allowed = !s.owner && !s.token && !s.metadata && !s.region && !s.activeActions && !quarantined();
    IOLockUnlock(s.lock); return allowed;
}
void QcomSCM::stop(IOService *provider) {
    if (fState && fState->lock) {
        auto &s = *fState; IOLockLock(s.lock); __atomic_store_n(&s.stopping, true, __ATOMIC_RELEASE);
        if (s.owner || s.token || s.metadata || s.region || s.activeActions) pin();
        IOLockUnlock(s.lock);
    }
    if (quarantined()) return;
    if (fProviderOpen) { provider->close(this); fProviderOpen = false; }
    if (fStarted) { IOService::stop(provider); fStarted = false; }
}
void QcomSCM::free() {
    if (quarantined()) return;
    if (fState) {
        OSSafeReleaseNULL(fState->provider); OSSafeReleaseNULL(fState->bus);
        if (fState->lock) IOLockFree(fState->lock);
        if (fState->monitorLock) IOLockFree(fState->monitorLock);
        delete fState; fState = nullptr;
    }
    IOService::free();
}
