// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandNative.h"
#include "BasebandResourceIO.h"
#include "BasebandSecureIO.h"
#include "BasebandRegisters.h"
#include "BasebandQRTR.h"
#include "BasebandAdmission.h"
#include "BasebandRMTFSIO.h"
#include "BasebandSysmon.h"
#include "BasebandShutdown.h"
#include "BasebandSSR.h"
#include "BasebandTFTP.h"
#include "BasebandTFTPIO.h"
#include <IOKit/IOLib.h>

namespace Baseband {
struct NativeController::Context {
    IOService *owner; IOWorkLoop *loop;
    MemoryHandoff handoff;
    NativeActions actions;
    OSData *mbaData, *modemData;
    FirmwareImage mba, modem;
    NativeResources resources;
    NativeAssignment secure;
    NativeMemory memory;
    NativeSignals signals;
    NativeRMTFSStorage storage;
    TFTP::Files files;
    QRTR::PortAllocator ports;
    Admission<Context> admission{*this};
    SMP2P::ReadyGate ready;
    uint32_t abort = 0;
    bool signalsBound = false, deasserted = false, pinned = false;
    bool linkStarted = false, stoppedByFault = false, gracefulStopping = false;
    uint64_t lastSignalPoll = 0, linkDeadline = 0;
    uint32_t signalState = 0;
    struct Registers {
        Context &c;
        RegisterIO raw;
        explicit Registers(Context &owner) : c(owner) {}
        bool check() {
            if (__atomic_load_n(&c.abort, __ATOMIC_ACQUIRE)) return false;
            if (c.deasserted && raw.nowUS() - c.lastSignalPoll >= 100) {
                c.lastSignalPoll = raw.nowUS();
                const auto r = c.sample();
                if (r != Result::OK && r != Result::NotReady && r != Result::Busy) {
                    __atomic_store_n(&c.abort, 1u, __ATOMIC_RELEASE); return false;
                }
            }
            return true;
        }
        bool read(unsigned s, uint32_t o, uint32_t &v) { return check() && raw.read(s, o, v); }
        bool write(unsigned s, uint32_t o, uint32_t v) { return check() && raw.write(s, o, v); }
        uint64_t nowUS() { return raw.nowUS(); }
        void delayUS(unsigned us) { raw.delayUS(us); }
    } registers{*this};
    Boot<Context, Registers> boot{*this, registers};
    GLINK::MemoryRing tx, rx;
    using Link = GLINK::Link<GLINK::MemoryRing>;
    Link *link = nullptr;
    QRTR::Router<Link> *router = nullptr;
    RMTFS::ServiceEndpoint<Link, NativeRMTFSStorage> *rmtfs = nullptr;
    Sysmon::Client<Link> *sysmon = nullptr;
    SSR::Client<Link> *ssr = nullptr;
    TFTP::Server<Link> *tftp = nullptr;
    unsigned serviceCursor = 0;
    Context(IOService *o, IOWorkLoop *l, const MemoryHandoff &h, OSData *b, OSData *m, const NativeActions &a)
        : owner(o), loop(l), handoff(h), actions(a), mbaData(b), modemData(m) {
        owner->retain(); loop->retain(); mbaData->retain(); modemData->retain();
    }
    ~Context() { delete tftp; delete ssr; delete sysmon; delete rmtfs; delete router; delete link; modemData->release(); mbaData->release(); loop->release(); owner->release(); }
    Result pin() {
        pinned = true; memory.quarantine(); storage.quarantine(); signals.disable();
        owner->setProperty("BasebandNativeQuarantined", true); return Result::Quarantined;
    }
    Result sample() {
        SMP2P::Observation observation;
        const auto r = signals.sample(handoff.epoch, observation);
        if (r == Result::NotReady) { ready.invalidated(); return r; }
        if (r != Result::OK) return r;
        signalState = observation.state;
        if (deasserted && observation.restart && boot.state() == State::Running) return Result::Stale;
        return ready.observe(observation);
    }
    Result admit(const BootLayout &layout) {
        MemoryHandoff current;
        const auto r = readMemoryHandoff(current);
        if (r != Result::OK) return r;
        uint8_t a[MemoryHandoff::MaxBytes], b[MemoryHandoff::MaxBytes];
        if (handoff.wireBytes() != current.wireBytes() || handoff.encode(a, handoff.wireBytes()) != Result::OK ||
            current.encode(b, current.wireBytes()) != Result::OK || memcmp(a, b, handoff.wireBytes()) ||
            layout.modem.address != handoff.layout.modem.address ||
            layout.modem.bytes != handoff.layout.modem.bytes || layout.mba.address != handoff.layout.mba.address ||
            layout.mba.bytes != handoff.layout.mba.bytes || layout.metadata.address != handoff.layout.metadata.address ||
            layout.metadata.bytes != handoff.layout.metadata.bytes) return Result::Invalid;
        return resources.cold();
    }
    Result acquire() {
        return admission.acquire(handoff.layout);
    }
    Result acquireResources() { return resources.acquire(); }
    Result probeSecure() { return secure.probe(); }
    Result confirmHost(const Region &r) { return secure.confirmHost(r); }
    Result mapConfirmedMemory() { return memory.bindConfirmed(handoff.layout); }
    bool hasAmbiguity() { return secure.quarantined() || resources.quarantined() || storage.quarantined(); }
    void closeSignals() { signals.close(); signalsBound = false; }
    bool closeMemory() {
        if (deasserted || (rmtfs && rmtfs->stopped() != Result::OK) || storage.retireShared(true) != Result::OK) return false;
        return memory.close();
    }
    Result releaseResources() { return resources.release(); }
    void disconnectRoutes() {
        if (router) router->disconnect();
        if (sysmon) sysmon->stopped();
        if (ssr) ssr->stopped();
        if (tftp) tftp->stopped();
    }
    Result initializeSignals() {
        auto r = signals.bind(owner, loop, handoff.epoch, actions.smp, actions.glink, actions.watchdog, actions.watchdogFilter);
        signalsBound = true; // Failed bind may already have published AP data.
        if (r == Result::OK) {
            SMP2P::Observation before;
            const auto sampled = signals.sample(handoff.epoch, before, false);
            if (sampled == Result::OK || sampled == Result::NotReady) ready.baseline(sampled == Result::OK, before);
            else r = sampled;
        }
        if (r == Result::OK) r = storage.acquireShared(secure);
        return r;
    }
    Result assign(const Region &region, Ownership from, Ownership to) {
        const auto r = secure.assign(region, from, to);
        if (r == Result::Quarantined) return pin();
        if (r != Result::OK) return r;
        if (memory.assigned(region, from, to) != Result::OK) return pin();
        return Result::OK;
    }
    bool copy(uint64_t a, const uint8_t *p, size_t n) { return !pinned && memory.copy(a, p, n); }
    bool zero(uint64_t a, size_t n) { return !pinned && memory.zero(a, n); }
    bool publish(uint64_t a, size_t n) { return !pinned && memory.publish(a, n); }
    Result stageMetadata(const FirmwareImage &image, const Region &r) { return memory.metadata(image, r); }
    Result deassertReset() {
        if (!signalsBound || pinned) return Result::NotReady;
        signals.enable();
        const auto r = resources.deassertReset();
        if (r == Result::OK) { deasserted = true; lastSignalPoll = 0; }
        return r;
    }
    Result waitReady(uint64_t epoch, unsigned timeout) {
        if (epoch != handoff.epoch) return Result::Stale;
        const auto start = registers.raw.nowUS();
        linkDeadline = start + timeout;
        for (unsigned polls = 0; polls <= timeout / 1000; ++polls) {
            if (__atomic_load_n(&abort, __ATOMIC_ACQUIRE)) return Result::IO;
            const auto r = sample();
            if (r != Result::OK && r != Result::NotReady && r != Result::Busy) return r;
            const auto transport = pumpTransport(registers.raw.nowUS());
            if (transport != Result::OK && transport != Result::NotReady && transport != Result::Busy) return transport;
            if (r == Result::OK && ready.ready()) return Result::OK;
            if (registers.raw.nowUS() - start >= timeout) break;
            IOSleep(1);
        }
        return Result::Timeout;
    }
    Result requestStop(uint64_t epoch, unsigned timeout) {
        if (epoch != handoff.epoch || !signalsBound) return Result::Stale;
        if (stoppedByFault || __atomic_load_n(&abort, __ATOMIC_ACQUIRE)) return Result::NotReady;
        gracefulStopping = true;
        ShutdownSequence<Context> sequence(*this); return sequence.run(epoch, timeout);
    }
    uint64_t shutdownNowUS() { return registers.raw.nowUS(); }
    bool shutdownAborted() const { return __atomic_load_n(&abort, __ATOMIC_ACQUIRE) != 0; }
    void shutdownDelayMS(unsigned ms) { IOSleep(ms); }
    Result beginGraceful(uint64_t epoch, uint64_t now, unsigned timeout) {
        return sysmon && linkStarted ? sysmon->shutdown(epoch, now, timeout) : Result::NotReady;
    }
    bool gracefulPending() const {
        return sysmon && (sysmon->state() == Sysmon::Phase::Selecting || sysmon->state() == Sysmon::Phase::Sending ||
                          sysmon->state() == Sysmon::Phase::Waiting);
    }
    Result gracefulOutcome() const { return sysmon ? sysmon->outcome() : Result::NotReady; }
    Result sampleStop(bool &ack) { const auto r = sample(); ack = r == Result::OK && (signalState & SMP2P::StopAck); return r; }
    Result stopBit(uint64_t epoch, bool requested) { return signals.stopRequest(epoch, requested); }
    Result pumpStop(uint64_t epoch, uint64_t now) { return epoch == handoff.epoch ? pumpTransport(now) : Result::Stale; }
    void recordGraceful(Result r) {
        owner->setProperty("BasebandSysmonShutdownResult", uint32_t(r), 32);
        owner->setProperty("BasebandSysmonTransport", uint32_t(sysmon ? sysmon->mode() : Sysmon::Mode::None), 32);
        owner->setProperty("BasebandSysmonQMIError", uint32_t(sysmon ? sysmon->qmiError() : 0), 32);
    }
    Result quiesceAndReset() {
        signals.disable();
        const auto r = resources.quiesceAndReset();
        if (r != Result::OK) return pin();
        deasserted = gracefulStopping = false; return Result::OK;
    }
    Result release() {
        return admission.release();
    }
    Result pumpTransport(uint64_t now) {
        Result r;
        if (!link) {
            r = signals.bindRings(handoff.epoch, tx, rx);
            if (r != Result::OK) return now >= linkDeadline ? Result::Timeout : r;
            link = new Link(tx, rx); if (!link) return Result::NoMemory;
            r = link->addChannel(Sysmon::ChannelName); if (r != Result::OK) return r;
            r = link->addChannel(SSR::ChannelName); if (r != Result::OK) return r;
        }
        if (!linkStarted) {
            r = link->start(handoff.epoch, 1, now, true);
            if (r != Result::OK) return now >= linkDeadline ? Result::Timeout : r;
            router = new QRTR::Router<Link>(*link, &ports); if (!router) return Result::NoMemory;
            r = router->start(handoff.epoch, 1, now); if (r != Result::OK) return r;
            rmtfs = new RMTFS::ServiceEndpoint<Link, NativeRMTFSStorage>(*link, storage);
            if (!rmtfs) return Result::NoMemory;
            r = rmtfs->start(handoff.epoch, 1, 0x3001, now); if (r != Result::OK) return r;
            tftp = new TFTP::Server<Link>(*link, files, &ports); if (!tftp) return Result::NoMemory;
            r = tftp->start(handoff.epoch, 1, 0x3003, now); if (r != Result::OK) return r;
            sysmon = new Sysmon::Client<Link>(*link, *router); if (!sysmon) return Result::NoMemory;
            r = sysmon->start(handoff.epoch, now); if (r != Result::OK) return r;
            ssr = new SSR::Client<Link>(*link); if (!ssr) return Result::NoMemory;
            r = ssr->start(handoff.epoch, now); if (r != Result::OK) return r;
            linkStarted = true;
        }
        for (unsigned batch = 0; batch < 4; ++batch) {
            if (__atomic_load_n(&abort, __ATOMIC_ACQUIRE)) return Result::IO;
            r = link->pump(handoff.epoch, now);
            if (r != Result::OK && r != Result::Busy && r != Result::NotReady) return r;
            if (!gracefulStopping && link->channelClosed("IPCRTR")) return Result::Stale;
            const uint8_t *data = nullptr; size_t bytes = 0;
            r = link->datagram(handoff.epoch, data, bytes);
            QRTR::Event event;
            if (r == Result::OK) {
                r = QRTR::feedServices(*router, *rmtfs, *tftp, handoff.epoch, data, bytes, now, event);
                if (r == Result::OK) sysmon->onEvent(handoff.epoch, event);
                // Unknown/stale QRTR destinations are not transmit
                // backpressure. Consume them so one unbound port cannot
                // block later nameservice discovery on the shared channel.
                if (r != Result::OK && r != Result::Busy && r != Result::Unsupported && r != Result::NotReady) return r;
                if (r != Result::Busy) { r = link->accept(handoff.epoch); if (r != Result::OK) return r; }
            } else if (r != Result::NotReady) return r;
            r = QRTR::progressServices(*router, *rmtfs, *tftp, serviceCursor, handoff.epoch, now, event);
            if (r != Result::OK && r != Result::Busy && r != Result::NotReady) return r;
            sysmon->onEvent(handoff.epoch, event);
            r = sysmon->progress(handoff.epoch, now);
            if (r != Result::OK && r != Result::Busy && r != Result::NotReady) return r;
            r = ssr->progress(handoff.epoch, now);
            if (r != Result::OK && r != Result::Busy && r != Result::NotReady) return r;
        }
        return Result::OK;
    }
    Result progress() {
        if (__atomic_load_n(&abort, __ATOMIC_ACQUIRE)) return Result::IO;
        const auto r = sample();
        if (r != Result::OK && r != Result::NotReady && r != Result::Busy) return r;
        if (!ready.ready()) return Result::Stale;
        return pumpTransport(registers.raw.nowUS());
    }
};
NativeController::~NativeController() { close(); }
Result NativeController::bind(IOService *owner, IOWorkLoop *loop, const MemoryHandoff &record,
                              OSData *mba, OSData *modem, const NativeActions &actions) {
    if (context || !owner || !owner->metaCast("QcomBaseband") || !loop || !record.valid() || !mba || !modem ||
        !actions.smp || !actions.glink || !actions.watchdog || !actions.watchdogFilter) return Result::Invalid;
    context = new Context(owner, loop, record, mba, modem, actions);
    if (!context) return Result::NoMemory;
    auto &c = *context;
    if (c.mba.parse(mba->getBytesNoCopy(), mba->getLength()) != Result::OK ||
        c.modem.parse(modem->getBytesNoCopy(), modem->getLength()) != Result::OK ||
        !c.registers.raw.bind(owner->getProvider())) { close(); return Result::Invalid; }
    auto r = c.resources.bind(owner, record.policy);
    RMTFS::StoragePolicy storagePolicy;
    if (r == Result::OK) r = readRMTFSPolicy(storagePolicy, record.epoch);
    if (r == Result::OK) r = c.storage.bind(owner, storagePolicy, &c.abort);
    if (r == Result::OK) r = readTFTPFiles(c.files, record);
    auto *matching = IOService::serviceMatching("QcomSCM");
    auto *scm = r == Result::OK && matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (r == Result::OK) r = scm ? c.secure.bind(owner, scm, record.layout, true) : Result::NotReady;
    OSSafeReleaseNULL(scm);
    if (r != Result::OK) close();
    return r;
}
Result NativeController::start() {
    if (!context) return Result::NotReady;
    auto &c = *context;
    const auto r = c.boot.start(c.handoff.epoch, c.mba, c.modem, c.handoff.layout);
    if (r == Result::OK) c.linkDeadline = c.registers.raw.nowUS() + 5000000;
    if (r == Result::Quarantined) c.pin();
    return r;
}
void NativeController::watchdog() { if (context) __atomic_store_n(&context->abort, 1u, __ATOMIC_RELEASE); }
Result NativeController::poll() {
    if (!context || context->boot.state() != State::Running || context->stoppedByFault) return Result::NotReady;
    const auto r = context->progress();
    if (r == Result::OK || r == Result::Busy || r == Result::NotReady) return r;
    context->stoppedByFault = true; context->owner->setProperty("BasebandRuntimeFailure", uint32_t(r), 32);
    const auto stopped = stop();
    return stopped == Result::OK ? r : Result::Quarantined;
}
Result NativeController::stop() {
    if (!context) return Result::OK;
    auto &c = *context; c.signals.disable();
    // Keep remote services available during graceful STOP_ACK: the peer may
    // submit its last NV writes before acknowledging. Admission disconnects
    // routes and closes handles only after mandatory AXI drain/reset.
    if (!c.boot.generation()) return Result::OK;
    const auto r = c.boot.stop(c.boot.generation());
    return r == Result::Quarantined ? c.pin() : r;
}
bool NativeController::quarantined() const {
    return context && (context->pinned || context->boot.quarantined() || context->resources.quarantined() ||
        context->secure.quarantined() || context->storage.quarantined() || (context->rmtfs && context->rmtfs->quarantined()));
}
bool NativeController::close() {
    if (!context) return true;
    if (quarantined() || context->admission.held() || context->boot.retainsResources()) return false;
    context->signals.close();
    if (!context->storage.close() || !context->memory.close() || !context->secure.close() || !context->resources.close()) return false;
    delete context; context = nullptr; return true;
}
State NativeController::state() const {
    if (!context) return State::Offline;
    return context->stoppedByFault && context->boot.state() == State::Offline ? State::Faulted : context->boot.state();
}
uint64_t NativeController::generation() const { return context ? context->boot.generation() : 0; }
uint32_t NativeController::missing() const {
    if (!context) return InitialRequirements;
    uint32_t result = InitialRequirements & ~Firmware;
    if (context->boot.state() == State::Running) {
        result &= ~(ReservedMemory | SecureOwnership | Clocks | Reset | SMP2PTransport);
        if (context->link && context->link->connected()) result &= ~GLINKTransport;
        if (context->storage.ready() && context->rmtfs && context->rmtfs->published() &&
            context->tftp && context->tftp->published()) result &= ~RemoteFilesystem;
    }
    return result;
}
} // namespace Baseband
