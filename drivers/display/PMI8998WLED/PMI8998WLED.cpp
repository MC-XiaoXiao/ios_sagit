// SPDX-License-Identifier: GPL-2.0-only
#include "PMI8998WLED.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <kern/task.h>
#include <machine/machine_routines.h>
#define super IOService
OSDefineMetaClassAndStructors(PMI8998WLED, super);

namespace {
IOReturn convert(QcomPower::Status s) {
    using namespace QcomPower;
    switch (s) {
        case OK: return kIOReturnSuccess;
        case Invalid: return kIOReturnBadArgument;
        case Unsupported: return kIOReturnUnsupported;
        case NotReady: return kIOReturnNotReady;
        case Busy: return kIOReturnBusy;
        case TimedOut: return kIOReturnTimeout;
        case Denied: return kIOReturnNotPermitted;
        case Stale: return kIOReturnOffline;
        default: return kIOReturnIOError;
    }
}
}
QcomPower::Status PMI8998WLED::Bus::transfer(uint8_t sid, uint16_t address, uint8_t *p, unsigned n, bool write) {
    error = spmi->transfer(sid, address, p, n, write);
    return error == kIOReturnSuccess ? QcomPower::OK : QcomPower::Failed;
}
void PMI8998WLED::Bus::delay(unsigned us) { IOSleep((us + 999) / 1000); }

bool PMI8998WLED::start(IOService *provider) {
    spmi = OSDynamicCast(QcomSPMI, provider);
    auto *data = spmi ? OSDynamicCast(OSData, spmi->getProvider()->getProperty("qcom,pmi8998-wled-handoff")) : nullptr;
    if (!data || data->getLength() != 32) return false;
    const auto *p = static_cast<const uint8_t *>(data->getBytesNoCopy());
    PMI8998Backlight::Config config{QcomPower::le32(p), QcomPower::le32(p+4), QcomPower::le32(p+8),
        QcomPower::le32(p+12), QcomPower::le32(p+16), QcomPower::le32(p+20), QcomPower::le32(p+24), QcomPower::le32(p+28)};
    if (!config.valid() || !super::start(provider)) return false;
    identifier = config.identifier;
    loop = spmi->getWorkLoop(); if (loop) loop->retain();
    gate = IOCommandGate::commandGate(this, action);
    if (!loop || !gate || loop->addEventSource(gate) != kIOReturnSuccess) goto fail;
    added = true;
    if (spmi->retainBus(this, true) != kIOReturnSuccess) goto fail;
    retainedBus = true;
    {
        // Share the SPMI workloop for the entire sequence, including delays;
        // regulator/RTC/PMIC traffic cannot interleave a brightness latch.
        auto result = gate->runAction([](OSObject *o, void *arg, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<PMI8998WLED *>(o);
            Bus bus{self->spmi};
            const auto &c = *static_cast<PMI8998Backlight::Config *>(arg);
            auto s = self->hardware.initialize(bus, c);
            if (s == QcomPower::NotReady && bus.error == kIOReturnSuccess) {
                // Capture the firmware state that failed adoption. These are
                // ordinary reads on the same serialized bus, never recovery
                // writes to an unaccepted backlight configuration.
                const uint16_t registers[] = {uint16_t(c.control + 0x08), uint16_t(c.control + 0x10),
                    uint16_t(c.control + 0x46), uint16_t(c.sink + 0x46), uint16_t(c.sink + 0x47),
                    uint16_t(c.sink + 0x4a), uint16_t(c.sink + 0x50), uint16_t(c.sink + 0x60)};
                for (auto address : registers) {
                    uint8_t value = 0;
                    auto status = self->spmi->transfer(c.sid, address, &value, 1, false);
                    IOLog("PMI8998WLED: adoption SID=%u register=%04x value=%02x result=%x\n",
                        c.sid, address, value, status);
                }
            }
            return bus.error != kIOReturnSuccess ? bus.error : convert(s);
        }, &config);
        if (result != kIOReturnSuccess) {
            IOLog("PMI8998WLED: retained WLED4 handoff rejected (%x)\n", result);
            goto fail;
        }
    }
    ready = true;
    if (!setProperty("DisplayBacklightIdentifier", identifier, 32) ||
        !setProperty("DisplayBacklightProtocolVersion", 1ULL, 32) ||
        !setProperty("DisplayBacklightMaximum", 4095ULL, 32) ||
        !setProperty("BacklightController", "PMI8998 WLED4") ||
        !publishState() || !setProperty("backlight-control", true)) goto fail;
    registerService();
    IOLog("PMI8998WLED: firmware brightness adopted, strings=%x; native brightness properties ready\n", config.strings);
    return true;
fail:
    cleanup(); super::stop(provider); return false;
}
IOReturn PMI8998WLED::action(OSObject *owner, void *arg, void *callerArg, void *, void *) {
    auto *self = static_cast<PMI8998WLED *>(owner);
    auto *caller = static_cast<IOService *>(callerArg);
    auto &r = *static_cast<DisplayBacklight::Request *>(arg);
    using namespace DisplayBacklight;
    if (r.identifier != self->identifier) return kIOReturnBadArgument;
    if (r.operation == Release) {
        if (self->client != caller) return kIOReturnNotOpen;
        auto *previous = self->client; self->client = nullptr;
        previous->release(); return kIOReturnSuccess;
    }
    if (!self->ready) return kIOReturnNotReady;
    if (r.operation == Claim) {
        if (self->client && self->client != caller) return kIOReturnExclusiveAccess;
        if (!self->hardware.available()) return kIOReturnOffline;
        if (!self->client) { caller->retain(); self->client = caller; }
    } else if (self->client != caller) return kIOReturnNotOpen;
    Bus bus{self->spmi};
    auto s = QcomPower::OK;
    if (r.operation == SetPower) s = self->hardware.setPower(bus, r.value);
    else if (r.operation == SetBrightness) s = self->hardware.setBrightness(bus, r.value);
    self->hardware.state(r);
    self->publishState();
    return bus.error != kIOReturnSuccess ? bus.error : convert(s);
}
bool PMI8998WLED::publishState() {
    DisplayBacklight::Request r;
    hardware.state(r);
    setProperty("BacklightStateValid", bool(r.valid));
    setProperty("BacklightPower", bool(r.power));
    setProperty("BacklightBrightness", r.brightness, 32);
    setProperty("BacklightAppliedBrightness", r.appliedBrightness, 32);
    // Original iOS 18 BCAppleBacklightBrtControl discovers backlight-control
    // and reads this dictionary's brightness min/max before normalizing UI
    // values. Advertise DAC units only; nits/current require panel calibration.
    auto *parameters = OSDictionary::withCapacity(1);
    auto *brightness = OSDictionary::withCapacity(3);
    auto *minimum = OSNumber::withNumber(0ULL, 32);
    auto *maximum = OSNumber::withNumber(4095ULL, 32);
    auto *value = OSNumber::withNumber(r.brightness, 32);
    bool ok = parameters && brightness && minimum && maximum && value &&
        brightness->setObject("min", minimum) && brightness->setObject("max", maximum) &&
        brightness->setObject("value", value) && parameters->setObject("brightness", brightness) &&
        setProperty("IODisplayParameters", parameters);
    OSSafeReleaseNULL(value); OSSafeReleaseNULL(maximum); OSSafeReleaseNULL(minimum);
    OSSafeReleaseNULL(brightness); OSSafeReleaseNULL(parameters);
    return ok;
}
IOReturn PMI8998WLED::brightnessAction(OSObject *owner, void *arg, void *, void *, void *) {
    auto *self = static_cast<PMI8998WLED *>(owner);
    if (!self->ready) return kIOReturnNotReady;
    Bus bus{self->spmi};
    auto s = self->hardware.setBrightness(bus, uint32_t(reinterpret_cast<uintptr_t>(arg)));
    auto result = bus.error != kIOReturnSuccess ? bus.error : convert(s);
    if (!self->publishState() && result == kIOReturnSuccess) result = kIOReturnNoMemory;
    ++self->nativeBrightnessRequests;
    self->setProperty("NativeBrightnessRequestCount", self->nativeBrightnessRequests, 32);
    self->setProperty("BacklightLastError", uint32_t(result), 32);
    if (self->nativeBrightnessRequests <= 8)
        IOLog("PMI8998WLED: native brightness=%u result=%x\n", uint32_t(reinterpret_cast<uintptr_t>(arg)), result);
    return result;
}
IOReturn PMI8998WLED::setProperties(OSObject *object) {
    auto *properties = OSDynamicCast(OSDictionary, object);
    if (!properties || !properties->getObject("brightness")) return super::setProperties(object);
    auto *value = OSDynamicCast(OSNumber, properties->getObject("brightness"));
    if (properties->getCount() != 1 || !value || value->unsigned64BitValue() > 4095)
        return kIOReturnBadArgument;
    // Match the original AppleARMBacklight write entitlement, with the usual
    // administrator path for the existing development diagnostic client.
    auto *entitlement = IOUserClient::copyClientEntitlement(current_task(), "com.apple.private.dfr.brightness-access");
    bool permitted = entitlement == kOSBooleanTrue ||
        IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    OSSafeReleaseNULL(entitlement);
    if (!permitted) return kIOReturnNotPrivileged;
    return added ? gate->runAction(brightnessAction,
        reinterpret_cast<void *>(uintptr_t(value->unsigned32BitValue()))) : kIOReturnNotReady;
}
IOReturn PMI8998WLED::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) {
    if (!name || !name->isEqualTo(DisplayBacklight::Function))
        return super::callPlatformFunction(name, wait, p1, p2, p3, p4);
    auto *r = static_cast<DisplayBacklight::Request *>(p1);
    if (!r || !p2 || p3 || p4 || r->version != 1 || r->size != sizeof(*r) || r->operation > DisplayBacklight::SetPower ||
        (r->operation == DisplayBacklight::SetPower && r->value > 1) ||
        (r->operation == DisplayBacklight::SetBrightness && r->value > 4095)) return kIOReturnBadArgument;
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    return added ? gate->runAction(action, r, p2) : kIOReturnNotReady;
}
IOWorkLoop *PMI8998WLED::getWorkLoop() const { return loop; }
bool PMI8998WLED::requestTerminate(IOService *, IOOptionBits) { return false; }
void PMI8998WLED::cleanup() {
    if (added) {
        gate->runAction([](OSObject *o, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<PMI8998WLED *>(o);
            self->ready = false;
            auto *previous = self->client; self->client = nullptr;
            OSSafeReleaseNULL(previous); return kIOReturnSuccess;
        });
        loop->removeEventSource(gate); added = false;
    } else ready = false;
    if (retainedBus) { spmi->retainBus(this, false); retainedBus = false; }
    OSSafeReleaseNULL(gate); OSSafeReleaseNULL(loop);
}
void PMI8998WLED::stop(IOService *p) { cleanup(); super::stop(p); }
void PMI8998WLED::free() { cleanup(); super::free(); }
