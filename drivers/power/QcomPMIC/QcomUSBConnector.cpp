// SPDX-License-Identifier: GPL-2.0-only
#include "QcomUSBConnector.h"
#include "QcomPMIC.h"
#include "../../usb/QcomUSB/USBConnectorProtocol.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>

OSDefineMetaClassAndStructors(QcomUSBConnector, IOService);
namespace {
USB::Result result(IOReturn r) {
    if (r == kIOReturnSuccess) return USB::Result::Success;
    if (r == kIOReturnTimeout) return USB::Result::Timeout;
    if (r == kIOReturnBusy) return USB::Result::Busy;
    if (r == kIOReturnNotReady || r == kIOReturnOffline) return USB::Result::NotReady;
    if (r == kIOReturnUnsupported) return USB::Result::Unsupported;
    return USB::Result::HardwareError;
}
IOReturn nativeResult(USB::Result r) {
    switch (r) {
    case USB::Result::Success: return kIOReturnSuccess;
    case USB::Result::Invalid: return kIOReturnBadArgument;
    case USB::Result::Unsupported: return kIOReturnUnsupported;
    case USB::Result::Busy: return kIOReturnBusy;
    case USB::Result::Timeout: return kIOReturnTimeout;
    case USB::Result::NotReady: return kIOReturnNotReady;
    case USB::Result::Aborted: return kIOReturnAborted;
    case USB::Result::Overflow: return kIOReturnOverrun;
    default: return kIOReturnIOError;
    }
}
}

struct QcomUSBConnector::State {
    QcomUSBConnector *owner;
    QcomPMIC *pmic;
    PMICUSB::Config config{};
    IOLock *lock = nullptr;
    IOSimpleLock *noticeLock = nullptr;
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    thread_call_t notifier = nullptr;
    IOService *client = nullptr, *leaseOwner = nullptr, *noticeOwner = nullptr;
    UInt64 serial = 0, lease = 0, edge = 0, fault = 0, noticeLease = 0;
    unsigned registered = 0;
    bool claimed = false, timerAdded = false, stopping = false, releasing = false, ready = false;
    bool sink = false, notifying = false, ignoreCC = false;
    bool pm = false, suspended = true;
    USB::ConnectorState last{};
    PMICUSB::Hardware<State> hardware{*this};
    State(QcomUSBConnector *service, QcomPMIC *provider) : owner(service), pmic(provider) {
        pmic->retain(); lock = IOLockAlloc(); noticeLock = IOSimpleLockAlloc();
    }
    USB::Result read(UInt16 address, UInt8 *bytes, unsigned count) {
        return result(pmic->usbTransfer(owner, address, bytes, count, false));
    }
    USB::Result write(UInt16 address, const UInt8 *bytes, unsigned count) {
        if (count > 8) return USB::Result::Invalid;
        UInt8 copy[8]{}; bcopy(bytes, copy, count);
        return result(pmic->usbTransfer(owner, address, copy, count, true));
    }
    void delay(unsigned ms) { IOSleep(ms); }
    UInt64 nowMS() {
        UInt64 ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;
    }
    UInt64 faults() {
        const auto irq = IOSimpleLockLockDisableInterrupt(noticeLock);
        const auto value = fault;
        IOSimpleLockUnlockEnableInterrupt(noticeLock, irq); return value;
    }
    void suppressCC(bool value) {
        const auto irq = IOSimpleLockLockDisableInterrupt(noticeLock);
        if (value && !ignoreCC) { if (edge != UINT64_MAX) ++edge; invalidate(); }
        ignoreCC = value;
        IOSimpleLockUnlockEnableInterrupt(noticeLock, irq);
    }
    UInt64 edges() {
        const auto irq = IOSimpleLockLockDisableInterrupt(noticeLock);
        const auto value = edge;
        IOSimpleLockUnlockEnableInterrupt(noticeLock, irq); return value;
    }
    void sampled(const USB::ConnectorState &s) {
        last = s;
        owner->setProperty("USBOverCurrent", s.overCurrent);
        owner->setProperty("USBSourceAuthorized", hardware.sourceAuthorized());
        owner->setProperty("USBVCONNEnabled", hardware.vconnSourcing());
        const auto irq = IOSimpleLockLockDisableInterrupt(noticeLock);
        sink = s.power == USB::PowerRole::Sink;
        IOSimpleLockUnlockEnableInterrupt(noticeLock, irq);
    }
    void invalidate() { if (notifier) thread_call_enter(notifier); }
};

bool QcomUSBConnector::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fStarted = true;
    auto *pmic = OSDynamicCast(QcomPMIC, provider);
    if (!pmic) goto fail;
    fState = new State(this, pmic);
    if (!fState || !fState->lock || !fState->noticeLock || !pmic->usbConfiguration(fState->config)) goto fail;
    {
        auto &s = *fState;
        s.loop = IOWorkLoop::workLoop();
        s.timer = IOTimerEventSource::timerEventSource(this, poll);
        s.notifier = thread_call_allocate_with_options(notify, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
        if (!s.loop || !s.timer || !s.notifier || s.loop->addEventSource(s.timer) != kIOReturnSuccess) goto fail;
        s.timerAdded = true;
        if (pmic->claimUSB(this, true) != kIOReturnSuccess) goto fail;
        s.claimed = true;
        setProperty("USBConnectorKind", "PMI8998-TypeC");
        setProperty("USBSourceAuthorized", false);
        setProperty("USBSourceCurrentMicroamps", s.config.sourceMicroamps, 32);
        setProperty("USBVCONNSupported", s.config.vconn!=0);
        setProperty("SystemSuspendSupported", false);
        IOPMPowerState states[2]{};
        states[0].version = states[1].version = 1;
        states[0].inputPowerRequirement = kIOPMPowerOn;
        states[1].capabilityFlags = kIOPMDeviceUsable;
        states[1].outputPowerCharacter = states[1].inputPowerRequirement = kIOPMPowerOn;
        PMinit(); s.pm = true; provider->joinPMtree(this);
        if (registerPowerDriver(this, states, 2) != kIOReturnSuccess) goto fail;
        changePowerStateTo(1);
    }
    registerService(); publishResource(USBConnector::Resource, this); fPublished = true;
    return true;
fail:
    stop(provider); return false;
}

void QcomUSBConnector::interrupt(OSObject *target, void *, IOService *, int source) {
    auto *self = static_cast<QcomUSBConnector *>(target);
    auto &s = *self->fState;
    // Called on the parent's SPMI loop. Never take the connector's sleep lock
    // here: RPC uses the opposite order (connector lock -> SPMI gate).
    const auto irq = IOSimpleLockLockDisableInterrupt(s.noticeLock);
    const bool meaningful = (source != 0 || !s.ignoreCC) && (source != 1 || s.sink);
    if (meaningful && s.edge != UINT64_MAX) ++s.edge;
    if (source >= 2 && s.fault != UINT64_MAX) ++s.fault;
    if (meaningful && s.notifier) thread_call_enter(s.notifier);
    IOSimpleLockUnlockEnableInterrupt(s.noticeLock, irq);
}
void QcomUSBConnector::notify(void *argument, void *) {
    auto *self = static_cast<QcomUSBConnector *>(argument);
    auto &s = *self->fState;
    const auto irq = IOSimpleLockLockDisableInterrupt(s.noticeLock);
    auto *client = s.noticeOwner;
    const auto lease = s.noticeLease;
    if (client) s.notifying = true;
    IOSimpleLockUnlockEnableInterrupt(s.noticeLock, irq);
    if (!client) return;
    USBConnector::Notice notice{}; notice.lease = lease; // untrusted until Sample
    client->message(iokit_vendor_specific_msg(USBConnector::Changed), self, &notice);
    const auto done = IOSimpleLockLockDisableInterrupt(s.noticeLock);
    s.notifying = false;
    IOSimpleLockUnlockEnableInterrupt(s.noticeLock, done);
}
void QcomUSBConnector::poll(OSObject *owner, IOTimerEventSource *timer) {
    auto *self = static_cast<QcomUSBConnector *>(owner);
    auto &s = *self->fState; IOLockLock(s.lock);
    if (s.lease && s.ready && !s.releasing && !s.stopping && !s.suspended && !self->fPinned) {
        USB::ConnectorState now{};
        auto r = s.hardware.sample(now);
        if (s.hardware.failed()) self->pin();
        self->setProperty("USBSourceAuthorized", !self->fPinned && s.hardware.sourceAuthorized());
        const bool changed = r != USB::Result::Success || !now.sameRoute(s.last) ||
            now.vbus != s.last.vbus || now.overCurrent != s.last.overCurrent;
        if (r == USB::Result::Success) s.sampled(now);
        if (changed) {
            if (s.hardware.sourcing() && (r != USB::Result::Success || now.desired() == USB::Role::None))
                if (s.hardware.stop() != USB::Result::Success) self->pin();
            s.invalidate();
        }
        timer->setTimeoutMS(20);
    }
    IOLockUnlock(s.lock);
}
void QcomUSBConnector::pin() {
    if (!fPinned) { retain(); fPinned = true; }
    setProperty("USBConnectorFault", true);
    setProperty("USBSourceAuthorized", false);
    if (fState) fState->invalidate();
}
IOWorkLoop *QcomUSBConnector::getWorkLoop() const { return fState ? fState->loop : nullptr; }
IOReturn QcomUSBConnector::setPowerState(unsigned long state, IOService *) {
    if (state > 1) return kIOPMNoSuchState;
    if (!fState || !fState->lock) return IOPMAckImplied;
    auto &s = *fState; IOLockLock(s.lock);
    if (s.stopping) { IOLockUnlock(s.lock); return IOPMAckImplied; }
    // PM down runs before the PMIC removes its child's power domain. Retain
    // the resource lease across sleep, but revoke admission and source power.
    if (!state) {
        s.suspended = true;
        s.timer->cancelTimeout();
        setProperty("USBSourceAuthorized", false);
        s.invalidate();
        if (s.lease && s.hardware.suspend() != USB::Result::Success) pin();
    } else {
        const auto r = s.lease ? s.hardware.resume() : USB::Result::Success;
        s.suspended = r != USB::Result::Success;
        if (r != USB::Result::Success) pin();
        else {
            s.invalidate();
            if (s.lease && s.ready && !fPinned) s.timer->setTimeoutMS(20);
        }
    }
    setProperty("USBConnectorPowerReady", !s.suspended && !fPinned);
    IOLockUnlock(s.lock); return IOPMAckImplied;
}
bool QcomUSBConnector::handleOpen(IOService *client, IOOptionBits, void *) {
    if (!fState || !fState->lock || !client) return false;
    auto &s = *fState; IOLockLock(s.lock);
    const bool ok = !s.stopping && !fPinned && (!s.client || s.client == client);
    if (ok) s.client = client;
    IOLockUnlock(s.lock); return ok;
}
void QcomUSBConnector::handleClose(IOService *client, IOOptionBits) {
    if (!fState || !fState->lock) return;
    auto &s = *fState; IOLockLock(s.lock);
    if (s.client == client && !s.lease) s.client = nullptr;
    IOLockUnlock(s.lock);
}
bool QcomUSBConnector::handleIsOpen(const IOService *client) const {
    if (!fState || !fState->lock) return false;
    auto &s = *fState; IOLockLock(s.lock);
    const bool open = s.client && (!client || s.client == client);
    IOLockUnlock(s.lock); return open;
}
IOReturn QcomUSBConnector::callPlatformFunction(const OSSymbol *name, bool wait, void *a, void *b, void *c, void *d) {
    if (!name || !name->isEqualTo(USBConnector::Function)) return IOService::callPlatformFunction(name, wait, a, b, c, d);
    auto *request = static_cast<USBConnector::Request *>(a); auto *client = static_cast<IOService *>(b);
    if (!request || !client || c || d || !USBConnector::valid(*request)) return kIOReturnBadArgument;
    if (ml_at_interrupt_context() || !fState) return kIOReturnNotReady;
    auto &s = *fState; IOLockLock(s.lock);
    IOReturn status = kIOReturnNotReady;
    IOService *releaseOwner = nullptr; bool unpin = false;
    if (s.client != client || s.stopping || s.releasing) goto out;
    if (request->operation == USBConnector::Operation::Acquire) {
        if (s.suspended) goto out;
        if (s.lease || fPinned) { status = kIOReturnBusy; goto out; }
        if (s.serial == UINT64_MAX) { status = kIOReturnNoResources; goto out; }
        s.lease = ++s.serial; s.leaseOwner = client; client->retain();
        request->lease = s.lease;
        {
            const auto irq = IOSimpleLockLockDisableInterrupt(s.noticeLock);
            s.noticeOwner = client; s.noticeLease = s.lease;
            IOSimpleLockUnlockEnableInterrupt(s.noticeLock, irq);
        }
        setProperty("USBConnectorAcquireStage","Configure");
        {
            const UInt16 addresses[]={PMICUSB::Reg::Type,PMICUSB::Reg::Subtype,PMICUSB::Reg::TypeCConfig,
                                     PMICUSB::Reg::OTGCommand,PMICUSB::Reg::OTGStatus};
            UInt8 values[5]{};unsigned count=0;
            USB::Result probe=USB::Result::Success;
            for (;count<5;++count) {
                probe=s.read(addresses[count],values+count,1);
                if (probe!=USB::Result::Success) break;
            }
            setProperty("USBConnectorProbeStatus",static_cast<UInt32>(probe),32);
            setProperty("USBConnectorInitialRegisters",values,count);
        }
        status = nativeResult(s.hardware.configure(s.config));
        setProperty("USBConnectorAcquireStatus",static_cast<UInt32>(status),32);
        while (status == kIOReturnSuccess && s.registered < 6) {
            setProperty("USBConnectorAcquireStage","RegisterInterrupt");
            setProperty("USBConnectorInterruptIndex",s.registered,32);
            status = registerInterrupt(s.registered, this, interrupt);
            setProperty("USBConnectorAcquireStatus",static_cast<UInt32>(status),32);
            if (status == kIOReturnSuccess) ++s.registered;
        }
        for (unsigned i = 0; status == kIOReturnSuccess && i < s.registered; ++i) {
            setProperty("USBConnectorAcquireStage","EnableInterrupt");
            setProperty("USBConnectorInterruptIndex",i,32);
            status = enableInterrupt(i);
            setProperty("USBConnectorAcquireStatus",static_cast<UInt32>(status),32);
        }
        if (status == kIOReturnSuccess) {
            setProperty("USBConnectorAcquireStage","Sample");
            status = nativeResult(s.hardware.sample(request->connector));
            if (status == kIOReturnSuccess) s.sampled(request->connector);
            // An unsupported/debouncing partner does not lose the lease. A
            // subsequent Sample can observe unplug/replug after a good acquire.
            if (status == kIOReturnUnsupported || status == kIOReturnNotReady || status == kIOReturnBusy)
                status = kIOReturnSuccess;
        }
        s.ready = status == kIOReturnSuccess;
        setProperty("USBConnectorAcquireStatus",static_cast<UInt32>(status),32);
        if (s.ready) setProperty("USBConnectorAcquireStage","Ready");
        if(s.ready)request->sourceMicroamps=s.config.sourceMicroamps;
        setProperty("USBSourceAuthorized", s.ready && s.hardware.sourceAuthorized());
        if (s.ready) s.timer->setTimeoutMS(20);
        else if (s.hardware.holds()) pin();
        goto out;
    }
    if (s.leaseOwner != client || request->lease != s.lease) { status = kIOReturnBadArgument; goto out; }
    if (fPinned && request->operation != USBConnector::Operation::Release &&
        !(request->operation == USBConnector::Operation::Source && !request->source) &&
        !(request->operation == USBConnector::Operation::SafePower && request->next == USB::Role::None)) goto out;
    if (request->operation == USBConnector::Operation::Release) {
        {
            const auto irq = IOSimpleLockLockDisableInterrupt(s.noticeLock);
            const bool active = s.notifying;
            if (!active) { s.noticeOwner = nullptr; s.noticeLease = 0; s.releasing = true; }
            IOSimpleLockUnlockEnableInterrupt(s.noticeLock, irq);
            if (active) { status = kIOReturnBusy; goto out; }
        }
        s.timer->cancelTimeout();
        status = nativeResult(s.hardware.release());
        if (status == kIOReturnSuccess) for (unsigned i = 0; i < s.registered; ++i) {
            const auto disabled = disableInterrupt(i);
            if (disabled != kIOReturnSuccess) status = disabled;
        }
        IOLockUnlock(s.lock);
        // No workloop or sleep lock is held across draining the message task.
        thread_call_cancel_wait(s.notifier);
        IOLockLock(s.lock); s.releasing = false;
        if (status == kIOReturnSuccess) {
            releaseOwner = s.leaseOwner; s.leaseOwner = nullptr; s.lease = 0; s.ready = false;
            setProperty("USBSourceAuthorized", false);
            if (fPinned) { fPinned = false; unpin = true; setProperty("USBConnectorFault", false); }
        } else {
            const auto irq = IOSimpleLockLockDisableInterrupt(s.noticeLock);
            s.noticeOwner = s.leaseOwner; s.noticeLease = s.lease;
            IOSimpleLockUnlockEnableInterrupt(s.noticeLock, irq);
            pin();
        }
    } else if ((request->operation == USBConnector::Operation::Source && !request->source) ||
               (request->operation == USBConnector::Operation::SafePower && request->next == USB::Role::None)) {
        status = nativeResult(s.hardware.stop());
        if (status != kIOReturnSuccess) pin();
    } else if (s.ready && !s.suspended) {
        if (request->operation == USBConnector::Operation::Sample) {
            status = nativeResult(s.hardware.sample(request->connector));
            if (status == kIOReturnSuccess) s.sampled(request->connector);
            else s.invalidate();
            if (s.hardware.failed()) pin();
            setProperty("USBSourceAuthorized", !fPinned && s.hardware.sourceAuthorized());
        } else if (request->operation == USBConnector::Operation::SafePower) {
            status = nativeResult(s.hardware.safePower(request->next, request->connector));
        } else {
            status = nativeResult(request->source ? s.hardware.source(request->connector) : s.hardware.stop());
            if (s.hardware.failed() || (status != kIOReturnSuccess && s.hardware.sourcing())) pin();
            setProperty("USBSourceAuthorized", !fPinned && s.hardware.sourceAuthorized());
        }
    }
out:
    IOLockUnlock(s.lock);
    if (releaseOwner) releaseOwner->release();
    if (unpin) release();
    return status;
}
bool QcomUSBConnector::requestTerminate(IOService *, IOOptionBits) {
    if (!fState || !fState->lock) return !fPinned;
    auto &s = *fState; IOLockLock(s.lock);
    const bool ok = !s.client && !s.lease && !s.releasing && !fPinned;
    IOLockUnlock(s.lock); return ok;
}
void QcomUSBConnector::stop(IOService *provider) {
    if (fState && fState->lock) {
        auto &s = *fState; IOLockLock(s.lock);
        s.stopping = true;
        if (s.client || s.lease || s.releasing) { pin(); IOLockUnlock(s.lock); return; }
        IOLockUnlock(s.lock);
        if (s.pm) { PMstop(); s.pm = false; }
        if (fPublished) {
            auto *resources = getResourceService();
            if (resources->getProperty(USBConnector::Resource) == this) resources->removeProperty(USBConnector::Resource);
            fPublished = false;
        }
        if (s.timer) s.timer->cancelTimeout();
        while (s.registered) {
            const auto index = s.registered - 1;
            disableInterrupt(index);
            if (unregisterInterrupt(index) != kIOReturnSuccess) { pin(); return; }
            --s.registered;
        }
        if (s.notifier) thread_call_cancel_wait(s.notifier);
        if (s.timerAdded) { s.loop->removeEventSource(s.timer); s.timerAdded = false; }
        if (s.claimed) {
            if (s.pmic->claimUSB(this, false) != kIOReturnSuccess) { pin(); return; }
            s.claimed = false;
        }
    }
    if (fStarted) { IOService::stop(provider); fStarted = false; }
}
void QcomUSBConnector::free() {
    if (fState) {
        stop(fState->pmic); if (fPinned) return;
        auto *s = fState; fState = nullptr;
        if (s->notifier) thread_call_free(s->notifier);
        OSSafeReleaseNULL(s->timer); OSSafeReleaseNULL(s->loop); OSSafeReleaseNULL(s->pmic);
        if (s->noticeLock) IOSimpleLockFree(s->noticeLock);
        if (s->lock) IOLockFree(s->lock);
        delete s;
    }
    IOService::free();
}
