// SPDX-License-Identifier: BSD-2-Clause
#include "QcomUSBCDC.h"
#include "QcomUSB.h"
#include "USBCDCACM.h"
#include "USBConsoleBridge.h"
#include "USBIODMA.h"
#include "DWC3Control.h"
#include "DWC3DeviceReset.h"
#include "USBDeviceIsochronous.h"
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSNumber.h>
#include <kern/clock.h>

OSDefineMetaClassAndStructors(QcomUSBCDC, IOService);

namespace {
using Result = USB::Result;
using ACM = USB::CDCACM;
constexpr unsigned endpoints[] = {ACM::Out, ACM::In, ACM::Notify};
// The kernel console is global even on SoCs with multiple USB controllers.
// A role incarnation owns it until all DMA and callbacks have stopped.
QcomUSBCDC *consoleOwner = nullptr;
bool accepted(Result r) { return r == Result::Success || r == Result::Pending; }
uint64_t timeUS() { uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000; }
}

struct QcomUSBCDC::State {
    static constexpr unsigned TRBBytes = 4 * sizeof(DWC3::TRB), EventOffset = 6 * TRBBytes;
    static constexpr unsigned EventBytes = 4096, SetupOffset = EventOffset + EventBytes;
    QcomUSBCDC *owner;
    QcomUSBController *nub;
    USB::MMIO &io;
    DWC3::Core<USB::MMIO> &core;
    IOMapper *mapper;
    IOWorkLoop *loop;
    IOCommandGate *gate = nullptr;
    IOInterruptEventSource *irq = nullptr;
    IOTimerEventSource *timer = nullptr;
    USB::IOMemoryAPI memory;
    USB::DMARegion<USB::IOMemoryAPI> descriptors, controlData, bulkOut, bulkIn, notification;
    DWC3::Control<DWC3::Core<USB::MMIO>, USB::MMIO> control;
    using Transfer = DWC3::Transfer<DWC3::Core<USB::MMIO>, USB::MMIO>;
    Transfer out, in, notify;
    DWC3::DeviceReset<DWC3::Core<USB::MMIO>, decltype(control), Transfer> busReset;
    ACM acm;
    uint64_t epoch, serial = 0, token = 0, controlDeadline = 0, stopDeadline = 0;
    USB::Speed speed = USB::Speed::Unknown;
    USB::SetupPacket setup{};
    ACM::Response response{};
    DWC3::ControlNotification notices[32]{};
    unsigned noticeCount = 0, rxBytes = 0, rxOffset = 0;
    bool gateAdded = false, irqAdded = false, timerAdded = false, opened = true;
    bool running = false, stopping = false, released = false, fault = false, reset = false, disconnected = false;
    bool suspended = false, pendingAction = false, actionAwaitStatus = false;
    bool rxReady = false, configured = false;
    bool pinned = false, permanentPin = false;
    IOSimpleLock *pinLock = IOSimpleLockAlloc();
    State(QcomUSBCDC *device, QcomUSBController *provider, USB::MMIO &registers,
          DWC3::Core<USB::MMIO> &controller, IOMapper *deviceMapper, unsigned vid, unsigned pid) :
        owner(device), nub(provider), io(registers), core(controller), mapper(deviceMapper),
        loop(provider->getWorkLoop()), memory(mapper, provider->resourceContract()),
        descriptors(memory), controlData(memory), bulkOut(memory), bulkIn(memory), notification(memory),
        control(core, io), out(core, io), in(core, io), notify(core, io), busReset(core, control, out, in, notify),
        acm(uint16_t(vid), uint16_t(pid)),
        epoch(provider->epoch()) { nub->retain(); loop->retain(); }
    template<class F> IOReturn gated(F function) {
        return gate->runAction([](OSObject *, void *arg, void *, void *, void *) -> IOReturn {
            return (*static_cast<F *>(arg))();
        }, &function);
    }
    bool isPinned(bool permanent = false) const {
        if (!pinLock) return false;
        const auto flags = IOSimpleLockLockDisableInterrupt(pinLock);
        const bool value = permanent ? permanentPin : pinned;
        IOSimpleLockUnlockEnableInterrupt(pinLock, flags); return value;
    }
    void pin(bool permanent = false) {
        if (!pinLock) return;
        const auto flags = IOSimpleLockLockDisableInterrupt(pinLock);
        if (!pinned) { owner->retain(); pinned = true; }
        permanentPin |= permanent;
        IOSimpleLockUnlockEnableInterrupt(pinLock, flags);
        nub->reportFault(owner, epoch); owner->setProperty("USBConsoleFault", true);
    }
    void unpin() {
        if (!pinLock) return;
        const auto flags = IOSimpleLockLockDisableInterrupt(pinLock);
        const bool drop = pinned && !permanentPin;
        if (drop) pinned = false;
        IOSimpleLockUnlockEnableInterrupt(pinLock, flags);
        if (drop) owner->release();
    }
    Transfer &transfer(unsigned ep) { return ep == ACM::Out ? out : ep == ACM::In ? in : notify; }
    USB::DMARegion<USB::IOMemoryAPI> &buffer(unsigned ep) {
        return ep == ACM::Out ? bulkOut : ep == ACM::In ? bulkIn : notification;
    }
    unsigned packet(unsigned ep) const { return ep == ACM::Notify ? 16 : speed == USB::Speed::High ? 512 : 64; }
    bool admitted() const { return !stopping && !fault && nub->accepts(owner, epoch); }
    void note(const DWC3::ControlNotification &message) {
        if (message.kind == DWC3::ControlNotificationKind::None) return;
        if (noticeCount == 32) { fault = true; return; }
        notices[noticeCount++] = message;
    }
    // Retain every mapping and the complete resource lease if halt cannot be
    // proved. Parent isolation is the only alternate proof; no bare timeout
    // permits reclaiming these buffers or touching reset-held MMIO.
    bool offline() {
        running = false; ml_usb_console_set_enabled(false);
        if (irq) irq->disable();
        if (timer) timer->cancelTimeout();
        if (!opened) return true;
        if (!nub->dmaIsolated() && core.halt() != Result::Success) {
            descriptors.quarantine(); controlData.quarantine(); bulkOut.quarantine();
            bulkIn.quarantine(); notification.quarantine(); pin(); return false;
        }
        if (control.currentState() != DWC3::ControlState::Uninitialized) {
            DWC3::ControlNotification ignored;
            if (control.resetAfterHalt(ignored) != Result::Success) { pin(true); return false; }
        }
        Transfer *transfers[] = {&out, &in, &notify};
        for (auto *td : transfers) if (td->ownsBuffers()) {
            DWC3::TransferCompletion ignored;
            if (td->reclaimAfterHalt(ignored) != Result::Success) { pin(true); return false; }
        }
        descriptors.retireAfterIsolation(); controlData.retireAfterIsolation(); bulkOut.retireAfterIsolation();
        bulkIn.retireAfterIsolation(); notification.retireAfterIsolation();
        acm.reset(); configured = pendingAction = actionAwaitStatus = suspended = rxReady = false;
        busReset.isolated();
        token = controlDeadline = stopDeadline = 0; noticeCount = rxBytes = rxOffset = 0;
        return true;
    }
    bool online() {
        if (!admitted() || !core.stopped() || core.select(USB::Role::Device) != Result::Success ||
            core.beginEndpointConfiguration() != Result::Success) return false;
        speed = USB::Speed::Unknown;
        for (unsigned ep = 0; ep < 2; ++ep) {
            const DWC3::EndpointConfiguration cfg{ep, DWC3::EndpointType::Control, USB::Speed::High, 64, 0, 0};
            if (core.configureEndpoint(cfg) != Result::Success) return false;
        }
        if (control.negotiate(USB::Speed::High) != Result::Success ||
            core.installEvents(descriptors.busAddress() + EventOffset, EventBytes) != Result::Success ||
            descriptors.publish(epoch) != Result::Success ||
            control.start() != Result::Success || core.runDevice(USB::Speed::High) != Result::Success) return false;
        running = true; irq->enable(); timer->setTimeoutMS(5); return true;
    }
    void beginBusReset(bool removed) {
        uint32_t stalled = 0;
        for (unsigned ep : endpoints) if (acm.stalled(ep)) stalled |= 1u << ep;
        disconnected = removed;
        ml_usb_console_set_enabled(false);
        acm.reset(); configured = pendingAction = actionAwaitStatus = suspended = rxReady = false;
        token = controlDeadline = stopDeadline = 0; noticeCount = rxBytes = rxOffset = 0;
        response = {};
        if (!accepted(busReset.begin(timeUS(), stalled))) fault = true;
    }
    void event(DWC3::Event e) {
        if (fault || reset) return;
        if (e.device()) {
            if (!e.deviceSpecific()) { fault = true; return; }
            switch (e.type()) {
            case 0: beginBusReset(true); break;
            case 1: beginBusReset(false); break;
            case 2:
                disconnected = suspended = false;
                if (USB::deviceConnectionSpeed(USB::Speed::High, io.read(DWC3::DSTS), speed) != Result::Success ||
                    (!busReset.active() && control.negotiate(speed) != Result::Success)) fault = true;
                break;
            case 3: suspended = (e.parameters() & 15) != 0; break;
            case 4: suspended = false; break;
            case 6: suspended = true; break;
            case 9: case 11: fault = true; break;
            default: break;
            }
            return;
        }
        const unsigned ep = e.endpointNumber();
        if (ep < 2) {
            // A late XferNotReady from the reset-displaced control request
            // is harmless while bulk END completions are still draining.
            if (busReset.stoppingControl() && control.currentState() == DWC3::ControlState::Stopped) return;
            DWC3::ControlNotification message;
            if (!accepted(control.handle(e, epoch, message))) fault = true;
            // Reset-displaced DATA/STATUS never reaches the fresh policy,
            // but a preserved SETUP TD can deliver the first new request.
            if (!busReset.stoppingControl()) note(message);
        } else if (ep == ACM::Out || ep == ACM::In || ep == ACM::Notify) {
            DWC3::TransferCompletion done;
            const auto result = transfer(ep).handle(e, epoch, done);
            if (result == Result::Success) {
                if (buffer(ep).retire(epoch) != Result::Success) fault = true;
                if (done.result == Result::Success && ep == ACM::Out) { rxReady = true; rxBytes = done.bytes; rxOffset = 0; }
                else if (done.result != Result::Success && done.result != Result::Aborted) fault = true;
            } else if (result != Result::Pending && result != Result::NotReady) fault = true;
        }
    }
    bool submit(unsigned ep, unsigned bytes) {
        if (serial == UINT64_MAX || buffer(ep).publish(epoch) != Result::Success) return false;
        DWC3::DMASegment segment{buffer(ep).busAddress(), bytes};
        return transfer(ep).submit(&segment, 1, bytes, packet(ep), DWC3::TRBType::Normal, ++serial,
            ep == ACM::In && bytes && bytes % packet(ep) == 0) == Result::Success;
    }
    void reject() {
        pendingAction = false; controlDeadline = 0;
        DWC3::ControlNotification message;
        if (!accepted(control.reject(token, message))) fault = true;
        note(message);
    }
    // Called after acknowledging the event-ring snapshot. Endpoint stops are
    // asynchronous; STATUS waits for matching END completions, with a bound.
    void action() {
        if (!pendingAction || fault) return;
        const bool all = response.action == ACM::Action::Configuration;
        uint32_t mask = all ? ((1u << ACM::Out) | (1u << ACM::In) | (1u << ACM::Notify)) :
            response.action == ACM::Action::Interface ?
                (response.value ? ((1u << ACM::Out) | (1u << ACM::In)) : (1u << ACM::Notify)) :
                (1u << response.endpoint);
        bool waiting = false;
        for (unsigned ep : endpoints) if (mask & (1u << ep)) {
            auto &td = transfer(ep);
            if (td.ownsBuffers()) {
                waiting = true;
                if (td.currentState() != DWC3::TransferState::Stopping && !accepted(td.cancel())) fault = true;
            }
        }
        if (waiting) {
            if (!stopDeadline) stopDeadline = timeUS() + 10000;
            else if (timeUS() >= stopDeadline) fault = true;
            return;
        }
        stopDeadline = 0;
        if (all) {
            configured = false; ml_usb_console_set_enabled(false); rxReady = false;
            if (core.beginEndpointConfiguration(2) != Result::Success) { fault = true; return; }
            if (response.value) for (unsigned ep : endpoints) {
                DWC3::EndpointConfiguration cfg{ep, ep == ACM::Notify ? DWC3::EndpointType::Interrupt :
                    DWC3::EndpointType::Bulk, speed, packet(ep), ep == ACM::Notify ? (speed == USB::Speed::High ? 9u : 16u) : 0u, 0};
                if (core.configureEndpoint(cfg) != Result::Success) { fault = true; return; }
            }
        } else {
            if (mask & (1u << ACM::Out)) rxReady = false;
            if (mask & (1u << ACM::Notify)) acm.requestNotification();
            for (unsigned ep : endpoints) if (mask & (1u << ep)) {
                uint32_t reply = 0;
                const auto cmd = response.action == ACM::Action::Halt && response.value ?
                    DWC3::Command::SetStall : DWC3::Command::ClearStall;
                if (core.command(ep, cmd, 0, 0, 0, 0, reply) != Result::Success) { fault = true; return; }
            }
        }
        pendingAction = false; actionAwaitStatus = true;
        if (!accepted(control.allowStatus(token))) fault = true;
    }
    void policy(const DWC3::ControlNotification &message) {
        if (message.kind == DWC3::ControlNotificationKind::Setup) {
            if (message.transaction != control.currentTransaction()) return;
            token = message.transaction; setup = message.setup;
            acm.abortControl();
            controlDeadline = timeUS() + 5000000;
            response = acm.setup(setup, speed, static_cast<uint8_t *>(controlData.bytes()));
            switch (response.action) {
            case ACM::Action::Reply: case ACM::Action::ReceiveLineCoding: {
                if (!setup.length) { if (!accepted(control.allowStatus(token))) fault = true; break; }
                const unsigned capacity = setup.in() ? response.length : 64;
                DWC3::DMASegment segment{controlData.busAddress(), capacity};
                if (controlData.publish(epoch) != Result::Success ||
                    control.supplyData(token, capacity ? &segment : nullptr, capacity ? 1 : 0, response.length) != Result::Success)
                    fault = true;
                break;
            }
            case ACM::Action::Address:
                // DWC3 applies the requested address before sending STATUS.
                if (core.setAddress(response.value) != Result::Success || !accepted(control.allowStatus(token))) fault = true;
                break;
            case ACM::Action::Status: if (!accepted(control.allowStatus(token))) fault = true; break;
            case ACM::Action::Configuration: case ACM::Action::Halt: case ACM::Action::Interface:
                pendingAction = true; break;
            default: reject(); break;
            }
        } else if (message.transaction == token) {
            if (message.kind == DWC3::ControlNotificationKind::Data) {
                if (controlData.retire(epoch) != Result::Success) { fault = true; return; }
                if (!setup.in() && !acm.stageLineCoding(static_cast<const uint8_t *>(controlData.bytes()), message.bytes)) reject();
                else if (!accepted(control.allowStatus(token))) fault = true;
            } else if (message.kind == DWC3::ControlNotificationKind::Status) {
                acm.commit(setup); controlDeadline = 0; actionAwaitStatus = false;
                if (response.action == ACM::Action::Configuration) {
                    configured = acm.configured(); ml_usb_console_set_enabled(configured);
                }
            } else if (message.kind == DWC3::ControlNotificationKind::Aborted) {
                acm.abortControl();
                if (controlData.currentState() == USB::DMAState::Owned && controlData.retire(epoch) != Result::Success) fault = true;
                // Hardware configuration/halt may already have changed.
                // Restart a fresh bus incarnation instead of exposing it
                // under the previous descriptor state.
                if (response.action == ACM::Action::Configuration || response.action == ACM::Action::Halt ||
                    response.action == ACM::Action::Interface || response.action == ACM::Action::Address) reset = true;
                pendingAction = actionAwaitStatus = false; controlDeadline = 0;
            }
        }
    }
    void service() {
        for (unsigned i = 0; i < noticeCount && !fault; ++i) policy(notices[i]);
        noticeCount = 0;
        if (fault || reset) return;
        action();
        if (!configured || suspended || pendingAction || actionAwaitStatus || fault) return;
        if (rxReady) {
            if (rxBytes > ACM::BufferBytes) { fault = true; return; }
            // Keep the unaccepted suffix in this DMA buffer. No next OUT is
            // queued until the console has room, so the host sees USB NAK
            // backpressure instead of losing input on ring/trylock contention.
            const auto result = ACM::drainReceive(static_cast<const uint8_t *>(bulkOut.bytes()), rxBytes,
                rxOffset, [](const uint8_t *bytes, unsigned count) { return ml_usb_console_write_rx(bytes, count); });
            if (!accepted(result)) { fault = true; return; }
            if (result == Result::Success) rxReady = false;
        }
        if (!rxReady && !acm.stalled(ACM::Out) && !out.ownsBuffers() && !submit(ACM::Out, ACM::BufferBytes)) fault = true;
        if (acm.terminalOpen() && !acm.stalled(ACM::In) && !in.ownsBuffers()) {
            const unsigned bytes = ml_usb_console_read_tx(bulkIn.bytes(), ACM::BufferBytes);
            if (bytes && !submit(ACM::In, bytes)) fault = true;
        }
        if (acm.needsNotification() && !acm.stalled(ACM::Notify) && !notify.ownsBuffers()) {
            acm.serialState(static_cast<uint8_t *>(notification.bytes()));
            if (!submit(ACM::Notify, 10)) fault = true;
            else acm.notified();
        }
    }
    void poll() {
        if (!running) return;
        if (!admitted()) { offline(); return; }
        auto handler = [this](DWC3::Event e) { event(e); };
        if (core.drainEvents(reinterpret_cast<const volatile uint32_t *>(
                static_cast<uint8_t *>(descriptors.bytes()) + EventOffset), handler) != Result::Success) fault = true;
        if (!fault && busReset.active()) {
            const auto result = busReset.finish(timeUS(), speed == USB::Speed::Unknown ? USB::Speed::High : speed);
            if (!accepted(result)) fault = true;
            else if (result == Result::Success && controlData.currentState() == USB::DMAState::Owned &&
                controlData.retire(epoch) != Result::Success) fault = true;
        }
        if (!fault && !reset && !disconnected && !busReset.active()) service();
        if (controlDeadline && timeUS() >= controlDeadline) fault = true;
        if (fault || reset) {
            const bool restart = reset && !fault;
            reset = false;
            if (!offline()) return;
            if (fault) { pin(); return; }
            if (restart && !online()) { fault = true; offline(); pin(); }
        }
    }
};

bool QcomUSBCDC::start(IOService *provider) {
    if (fState || fStarted) return false;
    auto *nub = OSDynamicCast(QcomUSBController, provider);
    auto *vid = nub ? OSDynamicCast(OSNumber, nub->getProperty("USBConsoleVendorID")) : nullptr;
    auto *pid = nub ? OSDynamicCast(OSNumber, nub->getProperty("USBConsoleProductID")) : nullptr;
    if (!nub || nub->role() != USB::Role::Device || !vid || !pid ||
        !ACM::validIdentity(vid->unsigned32BitValue(), pid->unsigned32BitValue())) {
        IOLog("QcomUSBCDC: qcom.usb-cdc requires valid qcom.usb-cdc-vid and qcom.usb-cdc-pid\n"); return false;
    }
    QcomUSBCDC *expected = nullptr;
    if (!__atomic_compare_exchange_n(&consoleOwner, &expected, this, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    fConsoleClaimed = true;
    if (!IOService::start(provider)) {
        __atomic_store_n(&consoleOwner, static_cast<QcomUSBCDC *>(nullptr), __ATOMIC_RELEASE);
        fConsoleClaimed = false; return false;
    }
    fStarted = true;
    if (!nub->getWorkLoop() || !nub->open(this)) goto fail;
    {
        USB::MMIO *io = nullptr; DWC3::Core<USB::MMIO> *core = nullptr;
        auto *gate = IOCommandGate::commandGate(this);
        if (!gate || nub->getWorkLoop()->addEventSource(gate) != kIOReturnSuccess) {
            OSSafeReleaseNULL(gate); nub->close(this); goto fail;
        }
        gate->runAction([](OSObject *owner, void *a, void *b, void *c, void *) -> IOReturn {
            auto *n = static_cast<QcomUSBController *>(a);
            *static_cast<USB::MMIO **>(b) = n->registers(static_cast<IOService *>(owner), n->epoch());
            *static_cast<DWC3::Core<USB::MMIO> **>(c) = n->deviceCore(static_cast<IOService *>(owner), n->epoch());
            return kIOReturnSuccess;
        }, nub, &io, &core);
        nub->getWorkLoop()->removeEventSource(gate); gate->release();
        if (!io || !core || core->endpointCount() <= ACM::Notify) { nub->close(this); goto fail; }
        auto *mapper = nub->copyDeviceMapper();
        fState = new State(this, nub, *io, *core, mapper, vid->unsigned32BitValue(), pid->unsigned32BitValue());
        if (!fState) { OSSafeReleaseNULL(mapper); nub->close(this); goto fail; }
    }
    {
        auto &s = *fState;
        s.gate = IOCommandGate::commandGate(this);
        s.irq = IOInterruptEventSource::interruptEventSource(this, interrupt, nub, 0);
        s.timer = IOTimerEventSource::timerEventSource(this, timer);
        if (!s.pinLock || !s.gate || !s.irq || !s.timer) goto fail;
        if (s.loop->addEventSource(s.gate) != kIOReturnSuccess) goto fail;
        s.gateAdded = true;
        if (s.loop->addEventSource(s.irq) != kIOReturnSuccess) goto fail;
        s.irqAdded = true; s.irq->disable();
        if (s.loop->addEventSource(s.timer) != kIOReturnSuccess) goto fail;
        s.timerAdded = true;
        // All allocation/mapping is outside the shared gate and IRQ action.
        if (s.descriptors.allocate(State::SetupOffset + 64, 64) != Result::Success ||
            s.controlData.allocate(ACM::BufferBytes, 64) != Result::Success ||
            s.bulkOut.allocate(ACM::BufferBytes, 64) != Result::Success ||
            s.bulkIn.allocate(ACM::BufferBytes, 64) != Result::Success ||
            s.notification.allocate(64, 64) != Result::Success) goto fail;
        auto *bytes = static_cast<uint8_t *>(s.descriptors.bytes());
        DWC3::ControlMemory controlMemory{reinterpret_cast<DWC3::TRB *>(bytes),
            reinterpret_cast<DWC3::TRB *>(bytes + State::TRBBytes), s.descriptors.busAddress(),
            s.descriptors.busAddress() + State::TRBBytes, 4, bytes + State::SetupOffset,
            s.descriptors.busAddress() + State::SetupOffset};
        if (s.control.initialize(controlMemory, 64, s.epoch) != Result::Success) goto fail;
        for (unsigned ep : endpoints)
            if (s.transfer(ep).initialize(ep, reinterpret_cast<DWC3::TRB *>(bytes + ep * State::TRBBytes),
                    s.descriptors.busAddress() + ep * State::TRBBytes, 4, s.epoch) != Result::Success) goto fail;
        if (s.gated([&] { return s.online() ? kIOReturnSuccess : kIOReturnIOError; }) != kIOReturnSuccess) goto fail;
        setProperty("USBConsoleProtocol", "CDC ACM"); setProperty("USBConsoleMaximumSpeed", "High");
        registerService(); return true;
    }
fail:
    stop(provider); return false;
}

void QcomUSBCDC::stop(IOService *provider) {
    if (fState && !fState->released) {
        auto &s = *fState;
        if (s.isPinned(true)) return;
        bool idle = true;
        if (s.gateAdded) s.gated([&] { s.stopping = true; idle = s.offline(); return kIOReturnSuccess; });
        if (!idle) return;
        if (s.timerAdded) { s.loop->removeEventSource(s.timer); s.timerAdded = false; }
        if (s.irqAdded) { s.loop->removeEventSource(s.irq); s.irqAdded = false; }
        if (s.gateAdded) { s.loop->removeEventSource(s.gate); s.gateAdded = false; }
        // A failed cleanup retains the object and mapper for parent isolation.
        USB::DMARegion<USB::IOMemoryAPI> *regions[] = {&s.descriptors, &s.controlData, &s.bulkOut, &s.bulkIn, &s.notification};
        for (auto *region : regions) {
            if (region->release() != Result::Success) { s.pin(!region->cleanupRetryable()); return; }
        }
        if (s.opened) { s.nub->close(this); s.opened = false; }
        s.released = true; s.unpin();
    }
    if (fStarted) { IOService::stop(provider); fStarted = false; }
    if (fConsoleClaimed) {
        __atomic_store_n(&consoleOwner, static_cast<QcomUSBCDC *>(nullptr), __ATOMIC_RELEASE);
        fConsoleClaimed = false;
    }
}

void QcomUSBCDC::free() {
    if (fState) {
        stop(fState->nub);
        if (fState->isPinned()) return;
        auto *s = fState; fState = nullptr;
        OSSafeReleaseNULL(s->timer); OSSafeReleaseNULL(s->irq); OSSafeReleaseNULL(s->gate);
        OSSafeReleaseNULL(s->mapper); OSSafeReleaseNULL(s->nub); OSSafeReleaseNULL(s->loop);
        if (s->pinLock) IOSimpleLockFree(s->pinLock);
        delete s;
    }
    IOService::free();
}
bool QcomUSBCDC::requestTerminate(IOService *, IOOptionBits) {
    return !fState || !fState->isPinned() || (!fState->isPinned(true) && fState->nub->dmaRecoveryReady());
}
IOWorkLoop *QcomUSBCDC::getWorkLoop() const { return fState ? fState->loop : nullptr; }
void QcomUSBCDC::interrupt(OSObject *owner, IOInterruptEventSource *, int) {
    auto *self = static_cast<QcomUSBCDC *>(owner); if (self->fState) self->fState->poll();
}
void QcomUSBCDC::timer(OSObject *owner, IOTimerEventSource *) {
    auto *self = static_cast<QcomUSBCDC *>(owner); auto *s = self->fState;
    if (!s || s->stopping || !s->running) return;
    s->poll(); if (s->running) s->timer->setTimeoutMS(5);
}
