// SPDX-License-Identifier: GPL-2.0-only
#include "QcomBattery.h"
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <kern/clock.h>

OSDefineMetaClassAndStructors(QcomBatteryPowerSource, IOPMPowerSource);
OSDefineMetaClassAndStructors(QcomBattery, IOService);

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
                         "ChargerState", "ChargerFaults", "PMICPowerPath"};
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
    setProperty("BatteryObserverOnly", true);
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
    IOLog("QcomBattery: read-only FG observer ready; interval=%u ms\n", config.pollMS);
    return true;
fail:
    cleanup(); IOService::stop(provider); return false;
}
void QcomBattery::poll() {
    if (!running || !awake) return;
    Bus bus{spmi};
    PMICBattery::Sample sample;
    auto result = PMICBattery::readSample(bus, config, sample);
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
            source->setProperty("BatteryObserverOnly", true);
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
    timer->setTimeoutMS(config.pollMS);
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
