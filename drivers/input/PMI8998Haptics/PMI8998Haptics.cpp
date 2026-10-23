// SPDX-License-Identifier: GPL-2.0-only
#include "PMI8998Haptics.h"
#include "PMI8998HapticsUserClient.h"
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>

OSDefineMetaClassAndStructors(PMI8998Haptics, IOService);
namespace {
UInt64 nowNS() {
    UInt64 ticks, ns;
    clock_get_uptime(&ticks); absolutetime_to_nanoseconds(ticks, &ns);
    return ns;
}
IOReturn convert(Haptics::Result r) {
    using Haptics::Result;
    switch (r) {
    case Result::Success: return kIOReturnSuccess;
    case Result::Invalid: return kIOReturnBadArgument;
    case Result::NotReady: return kIOReturnNotReady;
    case Result::Busy: return kIOReturnBusy;
    case Result::WrongChip: return kIOReturnUnsupported;
    default: return kIOReturnIOError;
    }
}
}
bool PMI8998Haptics::Bus::read(uint8_t sid, uint16_t address, uint8_t *p, unsigned n) {
    const auto r = spmi->transfer(sid, address, p, n, false);
    if (r != kIOReturnSuccess && error == kIOReturnSuccess) error = r;
    return r == kIOReturnSuccess;
}
bool PMI8998Haptics::Bus::write(uint8_t sid, uint16_t address, uint8_t *p, unsigned n) {
    const auto r = spmi->transfer(sid, address, p, n, true);
    if (r != kIOReturnSuccess && error == kIOReturnSuccess) error = r;
    return r == kIOReturnSuccess;
}
bool PMI8998Haptics::start(IOService *provider) {
    spmi = OSDynamicCast(QcomSPMI, provider);
    auto *dt = spmi ? spmi->getProvider() : nullptr;
    auto *data = dt ? OSDynamicCast(OSData, dt->getProperty("qcom,pmi8998-haptics")) : nullptr;
    if (!data || data->getLength() != 8 * sizeof(UInt32)) return false;
    const auto *p = static_cast<const uint8_t *>(data->getBytesNoCopy());
    config = {QcomPower::le32(p), QcomPower::le32(p+4), QcomPower::le32(p+8),
              QcomPower::le32(p+12), QcomPower::le32(p+16), QcomPower::le32(p+20),
              QcomPower::le32(p+24), QcomPower::le32(p+28)};
    if (!config.owned() || !IOService::start(provider)) return false;
    // No private loop / nested bus lock. Secure access pairs and configuration
    // sequences share exactly the same workloop as all SPMI transactions.
    loop = spmi->getWorkLoop();
    if (loop) loop->retain();
    gate = IOCommandGate::commandGate(this);
    timer = IOTimerEventSource::timerEventSource(this, tick);
    if (!loop || !gate || !timer) goto fail;
    if (loop->addEventSource(gate) != kIOReturnSuccess) goto fail;
    gateAdded = true;
    if (loop->addEventSource(timer) != kIOReturnSuccess) goto fail;
    timerAdded = true;
    {
        IOPMPowerState states[2]{};
        states[0].version = states[1].version = 1;
        states[1].capabilityFlags = kIOPMDeviceUsable;
        states[1].inputPowerRequirement = states[1].outputPowerCharacter = kIOPMPowerOn;
        gate->runAction([](OSObject *o, void *, void *, void *, void *) -> IOReturn {
            static_cast<PMI8998Haptics *>(o)->running = true;
            return kIOReturnSuccess;
        });
        PMinit(); pm = true; provider->joinPMtree(this);
        if (registerPowerDriver(this, states, 2) != kIOReturnSuccess) goto fail;
        changePowerStateTo(1);
    }
    setProperty("HapticsTransport", "PMI8998-SPMI-direct-LRA");
    setProperty("HapticsProtocolVersion", Haptics::Version, 32);
    setProperty("HapticsNativeAOPBridge", false);
    gate->runAction([](OSObject *o, void *, void *, void *, void *) -> IOReturn {
        static_cast<PMI8998Haptics *>(o)->publish(); return kIOReturnSuccess;
    });
    registerService();
    return true;
fail:
    cleanup(); IOService::stop(provider); return false;
}
void PMI8998Haptics::publish() {
    setProperty("HapticsState", uint32_t(hardware.currentState()), 32);
    setProperty("HapticsInitialized", hardware.isInitialized());
    setProperty("HapticsOutputKnownOff", hardware.knownOff());
    setProperty("HapticsAwake", awake);
    setProperty("HapticsLastResult", uint32_t(hardware.lastResult()), 32);
    setProperty("HapticsLastIOError", uint32_t(lastIO), 32);
    setProperty("HapticsAppliedMillivolts", hardware.appliedMV(), 32);
    setProperty("HapticsGeneration", generation, 64);
}
void PMI8998Haptics::arm(Bus &bus) {
    if (!hardware.pending()) {
        timer->cancelTimeout();
        // A transport fault can leave output uncertain. Retain the owner so
        // its close still attempts an independent cut after bus recovery.
        if (hardware.knownOff()) owner = nullptr;
        return;
    }
    const auto r = timer->setTimeoutMS(5);
    if (r != kIOReturnSuccess) {
        hardware.timerFailure(bus);
        if (hardware.knownOff()) owner = nullptr;
        lastIO = r;
    }
}
void PMI8998Haptics::tick(OSObject *object, IOTimerEventSource *) {
    auto *self = static_cast<PMI8998Haptics *>(object);
    if (!self->running || !self->awake) return;
    Bus bus{self->spmi};
    self->hardware.advance(bus, nowNS());
    self->lastIO = bus.error;
    self->arm(bus);
    if (bus.error != kIOReturnSuccess) self->lastIO = bus.error;
    self->publish();
}
IOReturn PMI8998Haptics::action(OSObject *object, void *arg, void *, void *, void *) {
    auto *self = static_cast<PMI8998Haptics *>(object);
    auto &r = *static_cast<Request *>(arg);
    auto *client = OSDynamicCast(PMI8998HapticsUserClient, r.client);
    if (!client) return kIOReturnBadArgument;
    if (r.close) client->closed = true;
    else if (client->closed || client->isInactive()) return kIOReturnNotAttached;
    if (!self->running) return kIOReturnNotReady;
    Bus bus{self->spmi};
    if (r.close) {
        if (self->owner == r.client) {
            self->timer->cancelTimeout();
            self->hardware.quiesce(bus, !self->awake);
            self->lastIO = bus.error;
            self->owner = nullptr;
            self->publish();
        }
        return kIOReturnSuccess;
    }
    const auto now = nowNS();
    if (r.selector == Haptics::GetInfo) {
        const uint64_t info[Haptics::InfoScalars] = {Haptics::Version, self->config.periodUS,
            self->config.maximumMV, self->config.currentMA, self->config.maximumMS, 5, 1000, 0};
        for (unsigned i = 0; i < Haptics::InfoScalars; ++i) r.output[i] = info[i];
        return kIOReturnSuccess;
    }
    if (r.selector == Haptics::GetStatus) {
        const auto &h = self->hardware;
        const uint64_t status[Haptics::StatusScalars] = {Haptics::Version, uint32_t(h.currentState()),
            h.isInitialized(), h.knownOff(), h.requestedStrength(), h.appliedMV(), h.remainingMS(now),
            uint32_t(h.lastResult()), uint32_t(self->lastIO), self->generation, self->awake, bool(self->owner)};
        for (unsigned i = 0; i < Haptics::StatusScalars; ++i) r.output[i] = status[i];
        return kIOReturnSuccess;
    }
    if (!self->awake) return kIOReturnOffline;
    if (self->owner && self->owner != r.client) return kIOReturnExclusiveAccess;
    Haptics::Result result;
    if (r.selector == Haptics::Play) {
        result = self->hardware.play(bus, now, uint32_t(r.input[0]), uint32_t(r.input[1]));
        if (result == Haptics::Result::Success) { self->owner = r.client; ++self->generation; }
    } else if (r.selector == Haptics::Stop) result = self->hardware.stop(bus, now);
    else return kIOReturnUnsupported;
    self->lastIO = bus.error;
    self->arm(bus);
    if (bus.error != kIOReturnSuccess) self->lastIO = bus.error;
    self->publish();
    if (self->hardware.currentState() == Haptics::State::Fault && result == Haptics::Result::Success)
        result = self->hardware.lastResult();
    return self->lastIO != kIOReturnSuccess ? self->lastIO : convert(result);
}
IOReturn PMI8998Haptics::request(uint32_t selector, IOUserClient *client,
                                const uint64_t *input, uint64_t *output) {
    if (!client || selector >= Haptics::SelectorCount ||
        ((selector == Haptics::GetInfo || selector == Haptics::GetStatus) && !output) ||
        (selector == Haptics::Play && !Haptics::validCall(selector, 2, 0, input)))
        return kIOReturnBadArgument;
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    Request r{selector, output, input, client};
    return gateAdded ? gate->runAction(action, &r) : kIOReturnNotReady;
}
void PMI8998Haptics::closeClient(IOUserClient *client) {
    Request r{Haptics::Stop, nullptr, nullptr, client, true};
    if (gateAdded) gate->runAction(action, &r);
}
IOReturn PMI8998Haptics::powerAction(OSObject *object, void *arg, void *, void *, void *) {
    auto *self = static_cast<PMI8998Haptics *>(object);
    if (!self->running) return kIOReturnNotReady;
    const bool awake = bool(reinterpret_cast<uintptr_t>(arg));
    if (self->awake == awake) return kIOReturnSuccess;
    self->timer->cancelTimeout(); self->owner = nullptr;
    self->awake = awake;
    Bus bus{self->spmi};
    const auto r = self->awake ? self->hardware.initialize(bus, self->config, nowNS()) :
                               self->hardware.quiesce(bus, true);
    self->lastIO = bus.error;
    self->publish();
    return bus.error != kIOReturnSuccess ? bus.error : convert(r);
}
IOReturn PMI8998Haptics::setPowerState(unsigned long state, IOService *) {
    if (state > 1) return kIOPMNoSuchState;
    if (gateAdded) gate->runAction(powerAction, reinterpret_cast<void *>(uintptr_t(state)));
    return IOPMAckImplied;
}
IOReturn PMI8998Haptics::newUserClient(task_t task, void *token, UInt32 type,
                                     OSDictionary *properties, IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    if (type != Haptics::DiagnosticClient) return kIOReturnUnsupported;
    if (IOUserClient::clientHasPrivilege(task, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    if (isInactive()) return kIOReturnNotAttached;
    auto *client = new PMI8998HapticsUserClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, token, type, properties)) { client->release(); return kIOReturnNotPrivileged; }
    if (!client->attach(this)) { client->release(); return kIOReturnNotAttached; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnNotReady; }
    *handler = client; return kIOReturnSuccess;
}
IOWorkLoop *PMI8998Haptics::getWorkLoop() const { return loop; }
bool PMI8998Haptics::requestTerminate(IOService *, IOOptionBits) { return false; }
void PMI8998Haptics::cleanup() {
    if (gateAdded) gate->runAction([](OSObject *o, void *, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<PMI8998Haptics *>(o);
        if (self->timer) self->timer->cancelTimeout();
        Bus bus{self->spmi};
        self->hardware.quiesce(bus, true);
        self->lastIO = bus.error;
        self->running = self->awake = false; self->owner = nullptr;
        self->publish(); return kIOReturnSuccess;
    });
    if (pm) { PMstop(); pm = false; }
    if (timerAdded) { loop->removeEventSource(timer); timerAdded = false; }
    if (gateAdded) { loop->removeEventSource(gate); gateAdded = false; }
    OSSafeReleaseNULL(timer); OSSafeReleaseNULL(gate); OSSafeReleaseNULL(loop);
}
void PMI8998Haptics::stop(IOService *provider) { cleanup(); IOService::stop(provider); }
void PMI8998Haptics::free() { cleanup(); IOService::free(); }
