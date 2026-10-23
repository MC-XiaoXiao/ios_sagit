// SPDX-License-Identifier: GPL-2.0-only
#include "QcomADSP.h"
#include <SMEMProtocol.h>
#include "ADSPGLINK.h"
#include "ADSPOutput.h"
#include "../../baseband/QcomBaseband/BasebandFIFO.h"
#include "ADSPMemory.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>

OSDefineMetaClassAndStructors(QcomADSP, AudioOutputBackend);
namespace {
using ADSP::Result;
using ADSP::read16; using ADSP::read32;
enum Operation { Bootstrap, Configuration, Prepare, Begin, Halt, Position, Dispose, Poll, Power, Shutdown };
AudioOutput::Result audio(Result r) {
    switch (r) {
    case Result::OK: return AudioOutput::Result::OK;
    case Result::Busy: return AudioOutput::Result::Busy;
    case Result::Invalid: case Result::Unsupported: return AudioOutput::Result::Invalid;
    case Result::NotReady: return AudioOutput::Result::NotReady;
    case Result::Stale: return AudioOutput::Result::Stale;
    case Result::Timeout: return AudioOutput::Result::Timeout;
    case Result::Quarantined: return AudioOutput::Result::Retained;
    default: return AudioOutput::Result::IO;
    }
}
void acquire() { __asm__ volatile("dmb oshld" ::: "memory"); }
void release() { __asm__ volatile("dmb oshst" ::: "memory"); }
bool reservation(const char *key, uint64_t pa, uint64_t bytes) {
    // XNU's boot memory-map must exclude this allocation. A reg tuple on the
    // controller alone does not prove that pageable host RAM was reserved.
    auto *node = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (!node) return false;
    auto *data = OSDynamicCast(OSData, node->getProperty(key));
    uint64_t range[2]{};
    bool ok = data && data->getLength() == sizeof(range);
    if (ok) { memcpy(range, data->getBytesNoCopy(), sizeof(range)); ok = range[0] == pa && range[1] == bytes; }
    node->release(); return ok;
}
}

struct QcomADSP::State {
    QcomADSP &owner;
    ADSP::Handoff handoff{};
    IOMemoryMap *maps[2]{};
    volatile uint8_t *descriptor = nullptr, *smp = nullptr, *pcm = nullptr;
    volatile uint32_t *mailbox = nullptr;
    IOService *sharedMemory = nullptr;
    IOBufferMemoryDescriptor *source = nullptr;
    ADSP::RemoteState remoteState;
    uint32_t serial = 0;
    bool faulted = false, ready = false, outputReady = false, stopping = false, suspended = false;
    bool readNotification = false;
    struct Ring {
        State &s;
        bool tx;
        volatile uint8_t *mapping = nullptr;
        uint32_t bytes = 0;
        explicit Ring(State &state, bool transmit) : s(state), tx(transmit) {}
        bool head(uint32_t &v) { v = *reinterpret_cast<volatile uint32_t *>(s.descriptor + (tx ? 4 : 12)); return true; }
        bool tail(uint32_t &v) { v = *reinterpret_cast<volatile uint32_t *>(s.descriptor + (tx ? 0 : 8)); return true; }
        void acquire() { ::acquire(); }
        void release() { ::release(); }
        bool setHead(uint32_t v) {
            if (!tx) return false;
            *reinterpret_cast<volatile uint32_t *>(s.descriptor + 4) = v; return true;
        }
        bool setTail(uint32_t v) {
            if (tx) return false;
            *reinterpret_cast<volatile uint32_t *>(s.descriptor + 8) = v; return true;
        }
        bool read(uint32_t offset, void *out, size_t n) {
            if (tx || !Baseband::range(offset, n, bytes)) return false;
            auto *p = static_cast<uint8_t *>(out);
            for (size_t i = 0; i < n; ++i) p[i] = mapping[offset + i];
            return true;
        }
        bool write(uint32_t offset, const void *in, size_t n) {
            if (!tx || !Baseband::range(offset, n, bytes)) return false;
            auto *p = static_cast<const uint8_t *>(in);
            for (size_t i = 0; i < n; ++i) mapping[offset + i] = p[i];
            return true;
        }
        bool kick() { return s.kick() == Result::OK; }
    } tx{*this, true}, rx{*this, false};
    Baseband::FIFO<Ring> *txFIFO = nullptr, *rxFIFO = nullptr;
    ADSP::Link<State, State> link{*this, *this};
    ADSP::Transaction transaction;
    ADSP::Output output;
    uint8_t outgoing[256]{};
    explicit State(QcomADSP &service) : owner(service) {}
    ~State() {
        if (sharedMemory) { sharedMemory->close(&owner); sharedMemory->release(); }
        delete txFIFO; delete rxFIFO;
        OSSafeReleaseNULL(source);
        for (auto &map : maps) OSSafeReleaseNULL(map);
    }
    bool uncertain() const { return faulted || link.quarantined() || transaction.poisoned(); }
    uint64_t now() const { return mach_absolute_time(); }
    bool expired(uint64_t start, uint64_t timeout) const {
        uint64_t current = now(); return current < start || current - start >= timeout;
    }
    Result fail(Result r) {
        faulted = true; ready = outputReady = false;
        owner.setProperty("AudioOutputReady", false);
        owner.setProperty("ADSPTransportError", UInt64(uint32_t(r)), 32);
        if (output.retained()) owner.pin();
        return r;
    }
    Result kick() {
        ::release(); mailbox[8 / 4] = 1u << 9; __asm__ volatile("dsb oshst" ::: "memory");
        return Result::OK;
    }
    Result send(const void *p, size_t n) {
        if (!txFIFO) return Result::NotReady;
        Result r = txFIFO->push(p, n);
        if (r == Result::OK) readNotification = false;
        if (r == Result::Busy && !readNotification) {
            uint8_t notice[8]{};
            Baseband::GLINK::header(notice, Baseband::GLINK::ReadNotify, 0, 0);
            auto notified = txFIFO->push(notice, sizeof(notice), true);
            if (notified == Result::OK) readNotification = true;
            else if (notified != Result::Busy) return notified;
        }
        return r;
    }
    Result available(size_t &n) { return rxFIFO ? rxFIFO->available(n) : Result::NotReady; }
    Result peek(void *p, size_t n) { return rxFIFO->peek(0, p, n); }
    Result consume(size_t n) {
        Result r = rxFIFO->consume(n);
        // Publish tail before notifying a blocked producer (READ_NOTIF).
        return r == Result::OK ? kick() : r;
    }
    Result token(uint32_t &value) {
        value = 0;
        if (serial == UINT32_MAX) return Result::Quarantined;
        value = ++serial; return Result::OK;
    }
    Result live() {
        if (!smp || faulted) return Result::NotReady;
        uint8_t snapshot[340];
        uint32_t before = *reinterpret_cast<volatile uint32_t *>(smp + 16);
        acquire();
        for (unsigned i = 0; i < sizeof(snapshot); ++i) snapshot[i] = smp[i];
        acquire();
        uint32_t after = *reinterpret_cast<volatile uint32_t *>(smp + 16);
        // A concurrent SSR transition cannot produce an admissible mixed snapshot.
        if (before != after || read32(snapshot + 16) != after) return fail(Result::Stale);
        Result r = remoteState.sample(snapshot, sizeof(snapshot));
        return r == Result::OK ? r : fail(r);
    }
    Result pump() {
        Result r = live();
        if (r != Result::OK) return r;
        r = link.poll();
        if (r != Result::OK) return fail(r);
        return Result::OK;
    }
    Result receive(const void *p, size_t n) {
        ADSP::Packet packet{};
        Result r = ADSP::parseAPR(p, n, packet);
        if (r != Result::OK) return r;
        if (packet.opcode == ADSP::WriteDone ||
            (packet.opcode == ADSP::BasicResponse && packet.bytes == 8 && read32(packet.payload) == ADSP::Write))
            return output.completion(packet);
        r = transaction.receive(handoff.generation, packet);
        return r == Result::Stale ? Result::OK : r;
    }
    Result send(uint8_t svc, uint16_t port, uint32_t t, uint32_t opcode, const void *p, size_t n) {
        if (uncertain()) return Result::Quarantined;
        Result r = live(); if (r != Result::OK) return r;
        size_t bytes = ADSP::command(outgoing, sizeof(outgoing), svc, port, t, opcode, p, n);
        if (!bytes) return Result::Invalid;
        return link.send(outgoing, bytes);
    }
    Result request(uint8_t svc, uint16_t port, uint32_t opcode, uint32_t response,
                   const void *p, size_t n, uint8_t *reply, size_t &capacity) {
        if (uncertain()) return Result::Quarantined;
        uint32_t t = 0;
        Result r = token(t); if (r != Result::OK) return r;
        r = transaction.begin(handoff.generation, svc, port, t, opcode, response);
        if (r != Result::OK) return r;
        uint64_t timeout = 0;
        clock_interval_to_absolutetime_interval(500, kMillisecondScale, &timeout);
        const uint64_t start = now();
        bool published = false;
        for (;;) {
            if (!published) {
                r = send(svc, port, t, opcode, p, n);
                if (r == Result::OK) published = true;
                else if (r != Result::Busy) {
                    if (uncertain()) transaction.fault(); else transaction.unpublished();
                    return r;
                }
            }
            r = pump();
            if (r != Result::OK) { transaction.fault(); return r; }
            if (transaction.done()) {
                r = transaction.result();
                if (r != Result::OK) return r;
                if (transaction.bytes() > capacity || (transaction.bytes() && !reply)) return Result::Invalid;
                capacity = transaction.bytes();
                if (capacity) memcpy(reply, transaction.data(), capacity);
                return Result::OK;
            }
            if (expired(start, timeout)) {
                if (published) { transaction.timeout(); return fail(Result::Timeout); }
                transaction.unpublished(); return Result::Busy;
            }
            // Pump explicitly while the work-loop gate is held. There is no
            // dependency on an interrupt callback acquiring this same gate.
            IOSleep(1);
        }
    }
    Result copyPCM(uint64_t from, uint64_t to, size_t bytes) {
        if (!source || !pcm || !Baseband::range(from, bytes, source->getLength()) ||
            !Baseband::range(to, bytes, handoff.pcmBytes)) return Result::Invalid;
        auto *p = static_cast<const uint8_t *>(source->getBytesNoCopy());
        if (!p) return Result::Invalid;
        // The DSP sees this uncached retained window, never the user-writable
        // host ring. No user pointer/position enters an APR address or length.
        for (size_t i = 0; i < bytes; ++i) pcm[to + i] = p[from + i];
        ::release(); return Result::OK;
    }
    bool mapResources(IOService *provider) {
        const unsigned count = handoff.flags == ADSP::OutputFlags ? 2 : 1;
        if (provider->getDeviceMemoryCount() != count ||
            !reservation("ADSPFirmware", handoff.firmwarePA, handoff.firmwareBytes) ||
            !reservation("QcomSMEM", ADSP::SMEMBase, ADSP::SMEMBytes) ||
            (count == 2 && !reservation("ADSPPCM", handoff.pcmPA, handoff.pcmBytes))) return false;
        if (auto *irqProperty = provider->getProperty("interrupts")) {
            auto *data = OSDynamicCast(OSData, irqProperty);
            if (!data || data->getLength() != 8 || read32(data->getBytesNoCopy()) != 189 ||
                read32(static_cast<const uint8_t *>(data->getBytesNoCopy()) + 4) != 0) return false;
        }
        const uint64_t bases[] = {ADSP::MailboxBase, handoff.pcmPA};
        const uint64_t sizes[] = {ADSP::MailboxBytes, handoff.pcmBytes};
        for (unsigned i = 0; i < count; ++i) {
            auto *memory = provider->getDeviceMemoryWithIndex(i);
            if (!memory || memory->getPhysicalAddress() != bases[i] || memory->getLength() != sizes[i]) return false;
            maps[i] = provider->mapDeviceMemoryWithIndex(i, kIOMapAnywhere | kIOMapInhibitCache);
            if (!maps[i] || maps[i]->getLength() != sizes[i] || !maps[i]->getVirtualAddress()) return false;
        }
        mailbox = reinterpret_cast<volatile uint32_t *>(maps[0]->getVirtualAddress());
        if (count == 2) pcm = reinterpret_cast<volatile uint8_t *>(maps[1]->getVirtualAddress());
        return true;
    }
    Result discover() {
        // QcomSMEM serializes all AP clients before taking hardware lock 3.
        // Two independent AP lock owners would both observe owner ID 1 and
        // could race the modem allocator even though each used the HW lock.
        if (!sharedMemory) {
            auto *match = IOService::serviceMatching("QcomSMEM");
            if (!match) return Result::NoMemory;
            auto *service = IOService::waitForMatchingService(match, 5000000000ULL);
            match->release();
            if (!service) return Result::NotReady;
            if (!service->open(&owner)) { service->release(); return Result::Busy; }
            sharedMemory = service;
        }
        auto *snapshot = IONew(uint8_t, ADSP::SMEMBytes);
        if (!snapshot) return Result::NoMemory;
        QcomSMEMProtocol::Request request;
        request.operation = QcomSMEMProtocol::Snapshot;
        request.bytes = ADSP::SMEMBytes; request.buffer = snapshot;
        const auto copied = sharedMemory->callPlatformFunction(QcomSMEMProtocol::Function, true,
            &request, &owner, nullptr, nullptr);
        if (copied != kIOReturnSuccess) {
            IODelete(snapshot, uint8_t, ADSP::SMEMBytes);
            return copied == kIOReturnNotResponding ? Result::Quarantined :
                copied == kIOReturnBusy ? Result::Busy : Result::NotReady;
        }
        ADSP::MemoryLayout layout{};
        Result result = ADSP::discoverMemory(snapshot, ADSP::SMEMBytes, layout);
        if (result == Result::OK) {
            result = remoteState.bind(snapshot + layout.smp.offset, layout.smp.bytes);
        }
        IODelete(snapshot, uint8_t, ADSP::SMEMBytes);
        if (result != Result::OK) return result;
        // Borrow the sole owner's mappings. Revalidate each live allocation
        // against the locked snapshot; a changed offset/size fails admission.
        // Keeping QcomSMEM open retains these mappings through final CLOSE.
        const Baseband::SMEMItem items[] = {layout.descriptor, layout.tx, layout.rx, layout.smp};
        volatile uint8_t *mapping[4]{};
        const uint16_t ids[] = {478, 479, 480, 429};
        for (unsigned i = 0; i < 4; ++i) {
            QcomSMEMProtocol::Request item;
            item.host = i == 3 ? layout.smpHost : 2; item.item = ids[i];
            auto status = sharedMemory->callPlatformFunction(QcomSMEMProtocol::Function, true,
                &item, &owner, nullptr, nullptr);
            if (status != kIOReturnSuccess) return status == kIOReturnNotResponding ?
                Result::Quarantined : status == kIOReturnBusy ? Result::Busy : Result::NotReady;
            if (!ADSP::matchesMapping(item, items[i])) return Result::Stale;
            mapping[i] = static_cast<volatile uint8_t *>(item.mapping);
        }
        descriptor = mapping[0]; smp = mapping[3];
        tx.mapping = mapping[1]; tx.bytes = layout.tx.bytes;
        rx.mapping = mapping[2]; rx.bytes = layout.rx.bytes;
        uint32_t head = 0, tail = 0;
        tx.head(head); tx.tail(tail); acquire();
        if (head != tail) return Result::Busy; // prior AP endpoint has unretired work
        txFIFO = new Baseband::FIFO<Ring>(tx, tx.bytes);
        rxFIFO = new Baseband::FIFO<Ring>(rx, rx.bytes);
        return txFIFO && rxFIFO ? live() : Result::NoMemory;
    }
    Result bootstrap() {
        Result r = discover(); if (r != Result::OK) return r;
        r = link.begin(handoff.generation); if (r != Result::OK) return r;
        uint64_t timeout = 0; clock_interval_to_absolutetime_interval(1000, kMillisecondScale, &timeout);
        uint64_t start = now();
        while (!link.ready()) {
            r = pump(); if (r != Result::OK) return r;
            if (expired(start, timeout)) return fail(Result::Timeout);
            IOSleep(1);
        }
        uint8_t reply[64]{}; size_t bytes = sizeof(reply);
        r = request(ADSP::Core, 0, ADSP::GetState, ADSP::StateResponse, nullptr, 0, reply, bytes);
        if (r != Result::OK) return r;
        if (bytes != 4 || !read32(reply)) return Result::NotReady;
        ready = true; outputReady = handoff.flags == ADSP::OutputFlags;
        return Result::OK;
    }
    AudioOutput::Configuration configuration() const {
        return {1, 48000, 2, 16, 4096, 512, handoff.latencyFrames, 2048};
    }
    Result prepare(IOBufferMemoryDescriptor *memory, const AudioOutput::Configuration &config) {
        const auto expected = configuration();
        if (!ready || !outputReady || suspended || stopping || uncertain() || source ||
            !memory || memory->getLength() != 16384 || !memory->getBytesNoCopy() ||
            memcmp(&expected, &config, sizeof(config))) return Result::Invalid;
        Result r = live(); if (r != Result::OK) return r;
        if (output.retained()) return Result::Busy;
        for (unsigned i = 0; i < handoff.pcmBytes; ++i) pcm[i] = 0;
        ::release();
        source = memory; source->retain(); owner.pin();
        r = output.prepare(*this, handoff);
        if (r != Result::OK) {
            if (output.release(*this) == Result::OK) {
                OSSafeReleaseNULL(source); output = {}; owner.unpin();
            } else return Result::Quarantined;
        }
        return r;
    }
    Result dispose() {
        if (!source && !output.retained()) return Result::OK;
        Result r = output.release(*this);
        if (r != Result::OK) return r;
        OSSafeReleaseNULL(source); output = {}; owner.unpin();
        return Result::OK;
    }
    Result closeTransport() {
        if (!link.generation() || link.closed()) return Result::OK;
        if (uncertain()) return Result::Quarantined;
        Result r = link.close(); if (r != Result::OK) return r;
        uint64_t timeout = 0; clock_interval_to_absolutetime_interval(500, kMillisecondScale, &timeout);
        const uint64_t start = now();
        while (!link.closed()) {
            r = pump(); if (r != Result::OK) return r;
            if (expired(start, timeout)) return fail(Result::Timeout);
            IOSleep(1);
        }
        ready = outputReady = false;
        return Result::OK;
    }
};

void QcomADSP::pin() { if (!fPinned) { retain(); fPinned = true; } }
void QcomADSP::unpin() { if (fPinned) { fPinned = false; release(); } }
AudioOutput::Result QcomADSP::call(unsigned op, void *a, void *b) {
    if (ml_at_interrupt_context() || !fGateAdded) return AudioOutput::Result::NotReady;
    return static_cast<AudioOutput::Result>(fGate->runAction(action, reinterpret_cast<void *>(uintptr_t(op)), a, b));
}
IOReturn QcomADSP::action(OSObject *object, void *operation, void *a, void *b, void *) {
    auto *self = OSDynamicCast(QcomADSP, object);
    if (!self || !self->fState) return IOReturn(AudioOutput::Result::NotReady);
    auto &s = *self->fState;
    Result r = Result::NotReady;
    switch (uintptr_t(operation)) {
    case Bootstrap: r = s.bootstrap(); break;
    case Shutdown:
        s.stopping = true; s.outputReady = false;
        self->setProperty("AudioOutputReady", false);
        r = s.dispose();
        if (r == Result::OK) r = s.closeTransport();
        break;
    case Dispose: r = s.dispose(); break;
    case Halt: r = s.output.stop(s); break;
    case Power:
        s.suspended = uintptr_t(a) == 0;
        r = s.suspended ? s.output.stop(s) : s.live(); break;
    case Poll: r = s.stopping ? Result::NotReady : s.pump(); break;
    case Configuration:
        if (a && s.ready && s.outputReady && !s.stopping && !s.suspended && s.live() == Result::OK) {
            *static_cast<AudioOutput::Configuration *>(a) = s.configuration(); r = Result::OK;
        }
        break;
    case Prepare:
        if (b) r = s.prepare(OSDynamicCast(IOBufferMemoryDescriptor, static_cast<OSObject *>(a)),
                             *static_cast<const AudioOutput::Configuration *>(b));
        break;
    case Begin:
        if (a && !s.stopping && !s.suspended && s.outputReady && s.live() == Result::OK)
            r = s.output.start(s, *static_cast<uint64_t *>(a));
        break;
    case Position:
        if (a && s.pump() == Result::OK) r = s.output.position(s, *static_cast<AudioOutput::Position *>(a));
        break;
    default: r = Result::Invalid; break;
    }
    if (s.uncertain()) s.fail(Result::Quarantined);
    return IOReturn(audio(r));
}
bool QcomADSP::start(IOService *provider) {
    auto *data = OSDynamicCast(OSData, provider->getProperty("qcom,adsp-handoff"));
    ADSP::Handoff handoff{};
    if (!data || data->getLength() != sizeof(handoff)) return false;
    memcpy(&handoff, data->getBytesNoCopy(), sizeof(handoff));
    if (!ADSP::validHandoff(handoff) || !AudioOutputBackend::start(provider)) return false;
    fStarted = true;
    if (!provider->open(this)) { cleanup(); return false; }
    fProviderOpen = true;
    fState = new State(*this);
    if (!fState) { cleanup(); return false; }
    fState->handoff = handoff;
    if (!fState->mapResources(provider)) { cleanup(); return false; }
    fLoop = IOWorkLoop::workLoop(); fGate = IOCommandGate::commandGate(this);
    fTimer = IOTimerEventSource::timerEventSource(this, tick);
    if (!fLoop || !fGate || !fTimer || fLoop->addEventSource(fGate) != kIOReturnSuccess) { cleanup(); return false; }
    fGateAdded = true;
    if (fLoop->addEventSource(fTimer) != kIOReturnSuccess) { cleanup(); return false; }
    fTimerAdded = true;
    if (call(Bootstrap) != AudioOutput::Result::OK) { cleanup(); return false; }
    // Polling is sufficient for the synchronous APR pump. Optional real GLINK
    // IRQ 157 reduces receive latency; the 2 ms timer remains a lost-edge guard.
    if (provider->getProperty("interrupts")) {
        fIRQ = IOInterruptEventSource::interruptEventSource(this, interrupt, provider, 0);
        if (!fIRQ || fLoop->addEventSource(fIRQ) != kIOReturnSuccess) { cleanup(); return false; }
        fIRQAdded = true;
    }
    IOPMPowerState states[2]{};
    states[0].version = states[1].version = 1;
    states[0].inputPowerRequirement = states[1].inputPowerRequirement = kIOPMPowerOn;
    states[0].outputPowerCharacter = states[1].outputPowerCharacter = kIOPMPowerOn;
    states[1].capabilityFlags = kIOPMDeviceUsable;
    PMinit(); fPM = true; provider->joinPMtree(this);
    if (registerPowerDriver(this, states, 2) != kIOReturnSuccess) { cleanup(); return false; }
    if (!setProperty("ADSPTransport", "SMEM-GLINK-APR") ||
        !setProperty("ADSPRemotePID", UInt64(2), 32) ||
        !setProperty("ADSPTransportGeneration", handoff.generation, 64) ||
        !setProperty("ADSPColdBootSupported", false) ||
        !setProperty("SystemSuspendSupported", false) ||
        !setProperty("AudioTimestampHostClock", "APR response observation") ||
        !setProperty("AudioOutputReady", fState->outputReady)) { cleanup(); return false; }
    changePowerStateTo(1);
    if (fIRQ) fIRQ->enable();
    if (fTimer->setTimeoutMS(2) != kIOReturnSuccess) { cleanup(); return false; }
    registerService(); return true;
}
void QcomADSP::cleanup() {
    if (fTimer) fTimer->cancelTimeout();
    if (fIRQ) fIRQ->disable();
    if (fState && (fState->output.retained() ||
        (fState->link.generation() && !fState->link.closed() && call(Shutdown) != AudioOutput::Result::OK))) {
        setProperty("ADSPResourcesRetained", true); pin(); return;
    }
    if (fPM) { PMstop(); fPM = false; }
    if (fIRQAdded) { fLoop->removeEventSource(fIRQ); fIRQAdded = false; }
    if (fTimerAdded) { fLoop->removeEventSource(fTimer); fTimerAdded = false; }
    if (fGateAdded) { fLoop->removeEventSource(fGate); fGateAdded = false; }
    OSSafeReleaseNULL(fIRQ); OSSafeReleaseNULL(fTimer); OSSafeReleaseNULL(fGate); OSSafeReleaseNULL(fLoop);
    delete fState; fState = nullptr; fOwner = nullptr;
    if (fProviderOpen) { getProvider()->close(this); fProviderOpen = false; }
    if (fStarted) { AudioOutputBackend::stop(getProvider()); fStarted = false; }
    unpin();
}
void QcomADSP::stop(IOService *) { call(Shutdown); cleanup(); }
void QcomADSP::free() { cleanup(); AudioOutputBackend::free(); }
bool QcomADSP::requestTerminate(IOService *, IOOptionBits) { return false; }
IOWorkLoop *QcomADSP::getWorkLoop() const { return fLoop; }
bool QcomADSP::handleOpen(IOService *client, IOOptionBits, void *) {
    if (!fState || fState->stopping || fState->uncertain() || (fOwner && fOwner != client)) return false;
    fOwner = client; return true;
}
void QcomADSP::handleClose(IOService *client, IOOptionBits) {
    if (fOwner == client && fState && !fState->source) fOwner = nullptr;
}
bool QcomADSP::handleIsOpen(const IOService *client) const { return fOwner && (!client || client == fOwner); }
void QcomADSP::tick(OSObject *object, IOTimerEventSource *timer) {
    auto *self = OSDynamicCast(QcomADSP, object);
    if (!self || !self->fState || self->fState->stopping) return;
    auto r = self->call(Poll);
    if (r == AudioOutput::Result::OK && timer->setTimeoutMS(2) != kIOReturnSuccess)
        self->fState->fail(Result::IO);
}
void QcomADSP::interrupt(OSObject *object, IOInterruptEventSource *, int) {
    auto *self = OSDynamicCast(QcomADSP, object);
    if (self) self->call(Poll);
}
IOReturn QcomADSP::setPowerState(unsigned long state, IOService *) {
    if (state > 1) return kIOReturnBadArgument;
    auto r = call(Power, reinterpret_cast<void *>(state));
    if (r != AudioOutput::Result::OK) setProperty("ADSPPowerDrainError", UInt64(uint32_t(r)), 32);
    return IOPMAckImplied; // both states retain parent power; no cold collapse
}
AudioOutput::Result QcomADSP::outputConfiguration(AudioOutput::Configuration &c) { return call(Configuration, &c); }
AudioOutput::Result QcomADSP::prepareOutput(IOBufferMemoryDescriptor *memory, const AudioOutput::Configuration &c) {
    return call(Prepare, memory, const_cast<AudioOutput::Configuration *>(&c));
}
AudioOutput::Result QcomADSP::startOutput(uint64_t generation) { return call(Begin, &generation); }
AudioOutput::Result QcomADSP::stopOutput() { return call(Halt); }
AudioOutput::Result QcomADSP::readPosition(AudioOutput::Position &p) { return call(Position, &p); }
AudioOutput::Result QcomADSP::releaseOutput() { return call(Dispose); }
