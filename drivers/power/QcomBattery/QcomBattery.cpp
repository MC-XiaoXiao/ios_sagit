// SPDX-License-Identifier: GPL-2.0-only
#include "QcomBattery.h"
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <kern/clock.h>
#include <IOKit/IOUserClient.h>

OSDefineMetaClassAndStructors(QcomBatteryPowerSource, IOPMPowerSource);
OSDefineMetaClassAndStructors(QcomBattery, IOService);

IOReturn QcomBatteryPowerSource::setProperties(OSObject *object) {
    auto *owner = OSDynamicCast(QcomBattery, getProvider());
    return owner ? owner->setProperties(object) : kIOReturnNotAttached;
}
void QcomBatteryPowerSource::publishControl(const PMICBattery::ChargeControl &c, bool allowed) {
    boolean("ChargeControlSupported", allowed);
    boolean("ChargeControlActive", c.active);
    boolean("ChargeControlFault", c.fault);
    boolean("ChargeControlReadbackValid", c.active && c.commandKnown);
    if (c.active && c.commandKnown) boolean("ChargeControlCommandEnabled", c.commandEnabled);
    else forget("ChargeControlCommandEnabled");
    number("ChargeControlError", uint32_t(c.result));
    if (c.active) {
        number("ChargeLimit", c.policy.limit);
        number("ChargeInhibitReasons", c.inhibit);
        boolean("ChargingEnabled", c.policy.manualEnabled);
        boolean("PredictiveChargingInhibit", c.policy.predictive);
    }
    updateStatus();
}
void QcomBattery::publishControl() {
    setProperty("ChargeControlSupported", controlAllowed);
    setProperty("ChargeControlActive", control.active);
    setProperty("ChargeControlFault", control.fault);
    setProperty("ChargeControlReadbackValid", control.active && control.commandKnown);
    if (control.active && control.commandKnown) setProperty("ChargeControlCommandEnabled", control.commandEnabled);
    else removeProperty("ChargeControlCommandEnabled");
    setProperty("ChargeControlError", uint32_t(control.result), 32);
    setProperty("ChargeInhibitReasons", control.inhibit, 32);
    if (source) source->publishControl(control, controlAllowed);
}
static IOReturn controlReturn(PMICBattery::ControlResult result) {
    using R = PMICBattery::ControlResult;
    switch (result) {
        case R::Success: return kIOReturnSuccess;
        case R::NotReady: return kIOReturnNotReady;
        case R::Unsupported: return kIOReturnUnsupported;
        default: return kIOReturnIOError;
    }
}
IOReturn QcomBattery::setProperties(OSObject *object) {
    if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    auto *dict = OSDynamicCast(OSDictionary, object);
    if (!dict || !dict->getCount()) return kIOReturnBadArgument;
    // Validate the complete request before changing a vote or hardware state.
    const char *names[] = {"ChargingEnabled", "IsCharging", "PredictiveChargingInhibit", "ChargeLimit"};
    unsigned count = 0;
    for (unsigned i = 0; i < 4; ++i) if (auto *value = dict->getObject(names[i])) {
        ++count;
        if (i < 3) { if (!OSDynamicCast(OSBoolean, value)) return kIOReturnBadArgument; }
        else {
            auto *n = OSDynamicCast(OSNumber, value);
            if (!n || n->unsigned64BitValue() < 50 || n->unsigned64BitValue() > 100)
                return kIOReturnBadArgument;
        }
    }
    if (count != dict->getCount()) return kIOReturnUnsupported;
    if (!gateAdded) return kIOReturnNotReady;
    return gate->runAction([](OSObject *o, void *argument, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<QcomBattery *>(o);
        auto *d = static_cast<OSDictionary *>(argument);
        if (!self->running || !self->awake) return kIOReturnNotReady;
        if (!self->controlAllowed) return kIOReturnUnsupported;
        Bus bus{self->spmi};
        // A manual request can recover a latched transport fault only after a
        // successful, verified disable. Native policy updates cannot clear it.
        if (self->control.fault && d->getObject("ChargingEnabled")) {
            const auto r = self->control.recover(bus, self->config);
            if (r != PMICBattery::ControlResult::Success) {
                self->publishControl(); return controlReturn(r);
            }
        }
        auto &p = self->control.policy;
        if (auto *v = OSDynamicCast(OSBoolean, d->getObject("ChargingEnabled"))) p.manualEnabled = v->isTrue();
        if (auto *v = OSDynamicCast(OSBoolean, d->getObject("IsCharging"))) p.systemEnabled = v->isTrue();
        if (auto *v = OSDynamicCast(OSBoolean, d->getObject("PredictiveChargingInhibit"))) p.predictive = v->isTrue();
        if (auto *v = OSDynamicCast(OSNumber, d->getObject("ChargeLimit"))) p.setLimit(v->unsigned32BitValue());
        self->control.active = true;
        self->poll(); // Fresh telemetry and readback before acknowledging the request.
        return controlReturn(self->control.result);
    }, dict);
}

void QcomBatteryPowerSource::number(const char *name, uint32_t value) {
    auto *key = OSSymbol::withCString(name);
    auto *n = OSNumber::withNumber(value, 32);
    if (key && n) setPSProperty(key, n);
    OSSafeReleaseNULL(n); OSSafeReleaseNULL(key);
}
void QcomBatteryPowerSource::boolean(const char *name, bool value) {
    auto *key = OSSymbol::withCString(name);
    if (key) setPSProperty(key, value ? kOSBooleanTrue : kOSBooleanFalse);
    OSSafeReleaseNULL(key);
}
void QcomBatteryPowerSource::forget(const char *name) {
    if (properties->getObject(name)) {
        properties->removeObject(name);
        removeProperty(name);
        settingsChangedSinceUpdate = true;
    }
}
void QcomBatteryPowerSource::clearMeasurements() {
    const char *keys[] = {"CurrentCapacity", "MaxCapacity", "Voltage", "Amperage",
                         "InstantAmperage", "Temperature", "FullyCharged",
                         "AtWarnLevel", "AtCriticalLevel", "RawSOC"};
    for (const char *key : keys) forget(key);
}
void QcomBatteryPowerSource::publish(const PMICBattery::Sample &s) {
    forget("ErrorCondition");
    setBatteryInstalled(s.installed);
    setExternalConnected(s.external);
    // Capability/budget needs adapter negotiation; do not infer it from VBUS.
    setIsCharging(s.charging);
    setTimeRemaining(-1);
    boolean("BatteryTelemetryValid", true);
    number("ChargerState", s.chargeState);
    number("ChargerFaults", s.chargerFaults);
    number("PMICPowerPath", s.powerPath);
    boolean("ChargeCommandEnabled", s.chargeRequested);
    boolean("ChargeEnablePinControlled", s.pinControlled);
    boolean("InputCurrentLimited", s.inputLimited);
    boolean("ThermalChargingLimited", s.thermalLimited);
    boolean("BatteryNetCharging", s.installed && s.external && s.milliamps > 0);
    if (s.installed) {
        // iOS-style relative capacity. No guessed mAh or health is published.
        setProperty("BatteryCapacityUnits", "Percent");
        setCurrentCapacity(s.percent);
        setMaxCapacity(100);
        setVoltage(s.millivolts);
        setAmperage(s.milliamps);
        number("InstantAmperage", uint32_t(s.milliamps));
        number("RawSOC", s.rawSOC);
        boolean("FullyCharged", s.full);
        // Low-battery shutdown policy belongs to the system, not this observer.
        if (s.temperatureValid) number("Temperature", uint32_t(s.centiCelsius));
        else forget("Temperature");
    } else clearMeasurements();
    updateStatus();
}
void QcomBatteryPowerSource::invalidate(PMICBattery::Result result) {
    clearMeasurements();
    const char *keys[] = {"BatteryInstalled", "ExternalConnected", "IsCharging",
                         "ChargerState", "ChargerFaults", "PMICPowerPath", "ChargeCommandEnabled", "ChargeEnablePinControlled",
                         "InputCurrentLimited", "ThermalChargingLimited", "BatteryNetCharging"};
    for (const char *key : keys) forget(key);
    setTimeRemaining(-1);
    boolean("BatteryTelemetryValid", false);
    auto *error = OSSymbol::withCString(result == PMICBattery::Result::Suspended ?
                                       "BatteryTelemetrySuspended" : "BatteryTelemetryUnavailable");
    if (error) { setErrorCondition(const_cast<OSSymbol *>(error)); error->release(); }
    updateStatus();
}

bool QcomBattery::Bus::read(uint8_t sid, uint16_t address, uint8_t *p, unsigned n) {
    error = spmi->transfer(sid, address, p, n, false);
    return error == kIOReturnSuccess;
}
bool QcomBattery::Bus::write(uint8_t sid, uint16_t address, const uint8_t *p, unsigned n) {
    // No generic register-write interface is exposed to userspace.
    if ((address != 0x1042 && address != 0x1051) || n != 1) { error = kIOReturnBadArgument; return false; }
    uint8_t value = *p;
    error = spmi->transfer(sid, address, &value, n, true);
    return error == kIOReturnSuccess;
}
static UInt64 uptimeNS() {
    UInt64 ticks, ns;
    clock_get_uptime(&ticks); absolutetime_to_nanoseconds(ticks, &ns);
    return ns;
}
bool QcomBattery::start(IOService *provider) {
    spmi = OSDynamicCast(QcomSPMI, provider);
    auto *dt = spmi ? spmi->getProvider() : nullptr;
    auto *data = dt ? OSDynamicCast(OSData, dt->getProperty("qcom,battery-observer")) : nullptr;
    if (!data || data->getLength() != 7 * sizeof(UInt32)) return false;
    const auto *p = static_cast<const uint8_t *>(data->getBytesNoCopy());
    config = {QcomPower::le32(p), QcomPower::le32(p+4), QcomPower::le32(p+8),
              QcomPower::le32(p+12), QcomPower::le32(p+16), QcomPower::le32(p+20),
              QcomPower::le32(p+24)};
    if (!config.valid() || !IOService::start(provider)) return false;
    if (auto *allow = OSDynamicCast(OSData, dt->getProperty("qcom,battery-charge-control")))
        controlAllowed = allow->getLength() == 4 &&
            QcomPower::le32(static_cast<const uint8_t *>(allow->getBytesNoCopy())) == 1;
    loop = IOWorkLoop::workLoop();
    gate = IOCommandGate::commandGate(this);
    timer = IOTimerEventSource::timerEventSource(this, tick);
    if (!loop || !gate || !timer) goto fail;
    if (loop->addEventSource(gate) != kIOReturnSuccess) goto fail;
    gateAdded = true;
    if (loop->addEventSource(timer) != kIOReturnSuccess) goto fail;
    timerAdded = true;
    setProperty("BatteryDataSource", "PMI8998-FG-Gen3/SPMI");
    setProperty("BatteryTelemetryValid", false);
    setProperty("BatteryObserverOnly", !controlAllowed);
    publishControl();
    setProperty("BatteryProfileVerified", false);
    setProperty("BatteryPollIntervalMS", config.pollMS, 32);
    {
        IOPMPowerState states[2]{};
        states[0].version = states[1].version = 1;
        states[1].capabilityFlags = kIOPMDeviceUsable;
        states[1].inputPowerRequirement = states[1].outputPowerCharacter = kIOPMPowerOn;
        PMinit(); pm = true; provider->joinPMtree(this);
        if (registerPowerDriver(this, states, 2) != kIOReturnSuccess) goto fail;
        gate->runAction([](OSObject *o, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<QcomBattery *>(o);
            self->running = true;
            if (self->awake) self->poll();
            return kIOReturnSuccess;
        });
        changePowerStateTo(1);
    }
    registerService();
    IOLog("QcomBattery: FG telemetry ready; charge-control=%u interval=%u ms\n", controlAllowed, config.pollMS);
    return true;
fail:
    cleanup(); IOService::stop(provider); return false;
}
void QcomBattery::poll() {
    if (!running || !awake) return;
    Bus bus{spmi};
    PMICBattery::Sample sample;
    auto result = PMICBattery::readSample(bus, config, sample);
    if (controlAllowed && control.active) {
        const bool valid = result == PMICBattery::Result::Success;
        control.synchronize(bus, config, valid ? &sample : nullptr);
        // Charging status predating a command write must never be published as
        // the current state. Take another coherent hardware sample.
        if (valid) result = PMICBattery::readSample(bus, config, sample);
        if (result != PMICBattery::Result::Success) control.synchronize(bus, config, nullptr);
    }
    const auto now = uptimeNS();
    setProperty("BatteryLastAttemptUptimeNS", now, 64);
    if (result == PMICBattery::Result::Success && !source) {
        source = new QcomBatteryPowerSource;
        if (!source || !source->init()) {
            OSSafeReleaseNULL(source);
        } else if (!source->attach(this)) {
            OSSafeReleaseNULL(source);
        } else if (!source->start(this)) {
            source->detach(this); OSSafeReleaseNULL(source);
        }
        if (!source) { result = PMICBattery::Result::IOFailure; bus.error = kIOReturnNoMemory; }
        else {
            source->setProperty("BatteryDataSource", "PMI8998-FG-Gen3/SPMI");
            source->setProperty("BatteryObserverOnly", !controlAllowed);
            source->setProperty("BatteryProfileVerified", false);
            source->setProperty("Transport Type", "Internal");
        }
    }
    if (result == PMICBattery::Result::Success) {
        source->setProperty("BatteryLastSuccessUptimeNS", now, 64);
        source->publish(sample);
        setProperty("BatteryLastSuccessUptimeNS", now, 64);
        setProperty("PMICDigitalMajor", sample.revision, 32);
        ++successes;
        if (!published) { source->registerService(); published = true; }
    } else {
        ++failures;
        if (source) source->invalidate(result);
        if (failures == 1) IOLog("QcomBattery: sample unavailable (%u, IOReturn=%x); will retry\n",
                                unsigned(result), bus.error);
    }
    setProperty("BatteryTelemetryValid", result == PMICBattery::Result::Success);
    setProperty("BatteryReadError", uint32_t(result), 32);
    setProperty("BatteryLastIOError", uint32_t(bus.error), 32);
    setProperty("BatterySuccessfulSamples", successes, 64);
    setProperty("BatteryFailedSamples", failures, 64);
    publishControl();
    const auto interval = control.active ? 1000 : config.pollMS;
    setProperty("BatteryPollIntervalMS", interval, 32);
    timer->setTimeoutMS(interval);
}
void QcomBattery::tick(OSObject *owner, IOTimerEventSource *) {
    static_cast<QcomBattery *>(owner)->poll(); // same workloop gate as PM/stop
}
IOReturn QcomBattery::powerAction(OSObject *o, void *state, void *, void *, void *) {
    auto *self = static_cast<QcomBattery *>(o);
    self->awake = state != nullptr;
    if (!self->running) return kIOReturnSuccess;
    self->timer->cancelTimeout();
    if (self->awake) self->poll();
    else {
        if (self->control.active) {
            Bus bus{self->spmi}; self->control.synchronize(bus, self->config, nullptr);
            self->publishControl();
        }
        self->setProperty("BatteryTelemetryValid", false);
        self->setProperty("BatteryReadError", uint32_t(PMICBattery::Result::Suspended), 32);
        if (self->source) self->source->invalidate(PMICBattery::Result::Suspended);
    }
    return kIOReturnSuccess;
}
IOReturn QcomBattery::setPowerState(unsigned long state, IOService *) {
    if (state > 1) return kIOPMNoSuchState;
    if (gateAdded) gate->runAction(powerAction, reinterpret_cast<void *>(uintptr_t(state)));
    return IOPMAckImplied;
}
IOWorkLoop *QcomBattery::getWorkLoop() const { return loop; }
bool QcomBattery::requestTerminate(IOService *, IOOptionBits) { return false; }
void QcomBattery::cleanup() {
    if (gateAdded) gate->runAction([](OSObject *o, void *, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<QcomBattery *>(o);
        self->running = false;
        if (self->control.active) {
            Bus bus{self->spmi}; self->control.synchronize(bus, self->config, nullptr);
            self->publishControl();
        }
        if (self->timer) self->timer->cancelTimeout();
        if (self->source) self->source->invalidate(PMICBattery::Result::Suspended);
        return kIOReturnSuccess;
    });
    if (pm) { PMstop(); pm = false; }
    if (timerAdded) { loop->removeEventSource(timer); timerAdded = false; }
    if (gateAdded) { loop->removeEventSource(gate); gateAdded = false; }
    if (source) { source->stop(this); source->detach(this); OSSafeReleaseNULL(source); }
    published = false;
    OSSafeReleaseNULL(timer); OSSafeReleaseNULL(gate); OSSafeReleaseNULL(loop);
}
void QcomBattery::stop(IOService *provider) { cleanup(); IOService::stop(provider); }
void QcomBattery::free() { cleanup(); IOService::free(); }
