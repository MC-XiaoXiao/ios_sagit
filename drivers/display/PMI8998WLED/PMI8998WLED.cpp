// SPDX-License-Identifier: GPL-2.0-only
#include "PMI8998WLED.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <kern/task.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#define super AppleARMBacklight
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
    ++transfers;
    error = spmi->transfer(sid, address, p, n, write);
    return error == kIOReturnSuccess ? QcomPower::OK : QcomPower::Failed;
}
void PMI8998WLED::Bus::delay(unsigned us) { IOSleep((us + 999) / 1000); }

bool PMI8998WLED::start(IOService *provider) {
    int nativeDiscovery = 1;
    int nominalNits = 0;
    // CoreBrightness discovers this native superclass independently of the
    // backlight-control property. Never publish an incomplete native model.
    if (!PE_parse_boot_argn("wled-nominal-max-nits", &nominalNits, sizeof(nominalNits)) ||
            nominalNits <= 0 || nominalNits > 1000) {
        IOLog("PMI8998WLED: valid nominal luminance model required\n");
        return false;
    }
    nativeBrightness.maximumMilliNits = uint32_t(nominalNits) * 1000;
    spmi = OSDynamicCast(QcomSPMI, provider);
    auto *data = spmi ? OSDynamicCast(OSData, spmi->getProvider()->getProperty("qcom,pmi8998-wled-handoff")) : nullptr;
    if (!data || data->getLength() != 32) return false;
    const auto *p = static_cast<const uint8_t *>(data->getBytesNoCopy());
    PMI8998Backlight::Config config{QcomPower::le32(p), QcomPower::le32(p+4), QcomPower::le32(p+8),
        QcomPower::le32(p+12), QcomPower::le32(p+16), QcomPower::le32(p+20), QcomPower::le32(p+24), QcomPower::le32(p+28)};
    if (!config.valid() || !IOService::start(provider)) return false;
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
    PE_parse_boot_argn("wled-native-discovery", &nativeDiscovery, sizeof(nativeDiscovery));
    {
        uint8_t enabled = 1;
        uint32_t potentialHeadroom = 1u << 16;
        // 22H374 reads eight bytes for the reference value, then converts
        // its low signed 16.16 word. Keep the complete native payload.
        uint64_t referenceHeadroom = 1u << 16;
        // Native BCAppleBacklightBrtControl explicitly permits the property
        // protocol with this byte; no DFR/DCP hardware identity is claimed.
        if (!setProperty("force-brightness-control", &enabled, sizeof(enabled)) ||
                !setProperty("EDRPotentialHeadroom", &potentialHeadroom, sizeof(potentialHeadroom)) ||
                !setProperty("EDRReferenceHeadroom", &referenceHeadroom, sizeof(referenceHeadroom)) ||
                !setProperty("BacklightLuminanceModel", "nominal-linear-dac") ||
                !publishNativeModel(config)) goto fail;
    }
    if (!setProperty("DisplayBacklightIdentifier", identifier, 32) ||
        !setProperty("DisplayBacklightProtocolVersion", 1ULL, 32) ||
        !setProperty("DisplayBacklightMaximum", 4095ULL, 32) ||
        !setProperty("BacklightController", "PMI8998 WLED4") ||
        !publishState() || !setProperty("backlight-control", nativeDiscovery != 0 && nativeBrightness.valid())) goto fail;
    registerService();
    IOLog("PMI8998WLED: firmware brightness adopted, strings=%x; native brightness properties ready\n", config.strings);
    return true;
fail:
    cleanup(); super::stop(provider); return false;
}
bool PMI8998WLED::publishNativeModel(const PMI8998Backlight::Config &config) {
    // Observe firmware full-scale current without changing the sink or boost.
    auto sampled = gate->runAction([](OSObject *owner, void *argument, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<PMI8998WLED *>(owner);
        const auto &c = *static_cast<const PMI8998Backlight::Config *>(argument);
        uint32_t total = 0;
        for (unsigned i = 0; i < 4; ++i) if (c.strings & (1u << i)) {
            uint8_t current = 0;
            auto status = self->spmi->transfer(c.sid, uint16_t(c.sink + 0x52 + 16 * i), &current, 1, false);
            if (status != kIOReturnSuccess) return status;
            // Linux v6.12 qcom-wled.c: WLED4 FS_CURR[3:0] selects
            // 0..30 mA in 2.5 mA steps. Zero cannot support this model.
            current &= 15;
            if (!current || current > 12) return kIOReturnUnsupported;
            total += uint32_t(current) * 2500;
        }
        self->maximumMicroAmps = total;
        return total ? kIOReturnSuccess : kIOReturnNotReady;
    }, const_cast<PMI8998Backlight::Config *>(&config));
    if (sampled != kIOReturnSuccess) return false;
    auto *parameters = OSDictionary::withCapacity(10);
    if (!parameters) return false;
    auto number = [parameters](const char *key, uint32_t value) {
        auto *n = OSNumber::withNumber(value, 32);
        bool ok = n && parameters->setObject(key, n);
        OSSafeReleaseNULL(n); return ok;
    };
    const uint32_t maximumNits = uint32_t((uint64_t(nativeBrightness.maximumMilliNits) << 16) / 1000);
    const uint32_t minimumNits = uint32_t((uint64_t(nativeBrightness.milliNits(1)) << 16) / 1000);
    const uint32_t middleNits = uint32_t((uint64_t(nativeBrightness.milliNits(2048)) << 16) / 1000);
    const uint32_t maximumCurrent = uint32_t((uint64_t(maximumMicroAmps) << 16) / 1000);
    const uint32_t middleCurrent = uint32_t((uint64_t(maximumMicroAmps) * 2048 * 65536) / (4095 * 1000ULL));
    bool ok = number("current-for-max-backlight", maximumCurrent) &&
        number("current-for-mid-backlight", middleCurrent) &&
        number("hardware-max-current-limit", maximumCurrent) &&
        number("lMaxProduct", maximumNits) && number("lMidProduct", middleNits) && number("lMinProduct", minimumNits) &&
        number("lMaxPanel", maximumNits) && number("lMidPanel", middleNits) && number("lMinPanel", minimumNits) &&
        number("milliAmps2NitsScaleFactor", uint32_t((uint64_t(nativeBrightness.maximumMilliNits) << 16) / maximumMicroAmps));
    uint32_t table[] = {minimumNits, middleNits, maximumNits};
    uint8_t enabled = 1;
    // 22H374 requires the new-architecture flag, fixed-point calibration and
    // marketing table together. The legacy fallback uses its table too early.
    ok = ok && setProperty("backlight-calibration-parameters", parameters) &&
        setProperty("backlight-marketing-table", table, sizeof(table)) &&
        setProperty("backlight-marketing-table-force", &enabled, sizeof(enabled)) &&
        setProperty("new-backlight-architecture", true) &&
        setProperty("BacklightConfiguredMaximumMicroAmps", maximumMicroAmps, 32);
    parameters->release();
    return ok;
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
    if (r.operation == Query) ++self->queryRequests;
    if (r.operation == SetPower) { ++self->powerRequests; s = self->hardware.setPower(bus, r.value); }
    else if (r.operation == SetBrightness) s = self->hardware.setBrightness(bus, r.value);
    self->hardware.state(r);
    self->hardwareTransfers += bus.transfers;
    auto result = bus.error != kIOReturnSuccess ? bus.error : convert(s);
    if (r.operation == SetPower || r.operation == SetBrightness) self->lastError = result;
    self->publishState();
    return result;
}
bool PMI8998WLED::publishState() {
    DisplayBacklight::Request r;
    hardware.state(r);
    setProperty("BacklightStateValid", bool(r.valid));
    setProperty("BacklightPower", bool(r.power));
    setProperty("BacklightBrightness", r.brightness, 32);
    setProperty("BacklightAppliedBrightness", r.appliedBrightness, 32);
    setProperty("BacklightLastError", uint32_t(lastError), 32);
    setProperty("BacklightPowerRequestCount", powerRequests, 64);
    setProperty("BacklightQueryCount", queryRequests, 64);
    setProperty("BacklightHardwareTransferCount", hardwareTransfers, 64);
    // Original iOS 18 BCAppleBacklightBrtControl discovers backlight-control
    // and reads this dictionary's brightness min/max before normalizing UI
    // values. Nits are published only with an explicit nominal board model;
    // Full-scale current is read from the PMIC; the optical model is nominal.
    auto *parameters = OSDictionary::withCapacity(2);
    auto *brightness = OSDictionary::withCapacity(3);
    auto *minimum = OSNumber::withNumber(0ULL, 32);
    auto *maximum = OSNumber::withNumber(4095ULL, 32);
    auto *value = OSNumber::withNumber(r.brightness, 32);
    DisplayBacklight::State snapshot;
    snapshot.brightness = r.brightness; snapshot.appliedBrightness = r.appliedBrightness;
    snapshot.power = r.power; snapshot.valid = r.valid; snapshot.lastError = uint32_t(lastError);
    auto *state = OSData::withBytes(&snapshot, sizeof(snapshot));
    bool ok = state && setProperty(DisplayBacklight::StateProperty, state) &&
        parameters && brightness && minimum && maximum && value &&
        brightness->setObject("min", minimum) && brightness->setObject("max", maximum) &&
        brightness->setObject("value", value) && parameters->setObject("brightness", brightness);
    if (ok && nativeBrightness.valid()) {
        auto *nits = OSDictionary::withCapacity(3);
        auto *nitsMin = OSNumber::withNumber(0ULL, 32);
        auto *nitsMax = OSNumber::withNumber(nativeBrightness.maximumMilliNits, 32);
        auto *nitsValue = OSNumber::withNumber(nativeBrightness.milliNits(r.brightness), 32);
        ok = nits && nitsMin && nitsMax && nitsValue &&
            nits->setObject("min", nitsMin) && nits->setObject("max", nitsMax) &&
            nits->setObject("value", nitsValue) && parameters->setObject("BrightnessMilliNits", nits);
        OSSafeReleaseNULL(nitsValue); OSSafeReleaseNULL(nitsMax); OSSafeReleaseNULL(nitsMin); OSSafeReleaseNULL(nits);
    }
    ok = ok && setProperty("IODisplayParameters", parameters);
    OSSafeReleaseNULL(value); OSSafeReleaseNULL(maximum); OSSafeReleaseNULL(minimum);
    OSSafeReleaseNULL(state);
    OSSafeReleaseNULL(brightness); OSSafeReleaseNULL(parameters);
    return ok;
}
IOReturn PMI8998WLED::brightnessAction(OSObject *owner, void *arg, void *, void *, void *) {
    auto *self = static_cast<PMI8998WLED *>(owner);
    if (!self->ready) return kIOReturnNotReady;
    Bus bus{self->spmi};
    auto s = self->hardware.setBrightness(bus, uint32_t(reinterpret_cast<uintptr_t>(arg)));
    self->hardwareTransfers += bus.transfers;
    auto result = bus.error != kIOReturnSuccess ? bus.error : convert(s);
    self->lastError = result;
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
    if (!properties) return IOService::setProperties(object);
    auto *raw = properties->getObject("brightness");
    auto *nits = properties->getObject("brightness-nits");
    if (!raw && !nits) return IOService::setProperties(object);
    auto *value = OSDynamicCast(OSNumber, raw ? raw : nits);
    if (properties->getCount() != 1 || !value) return kIOReturnBadArgument;
    uint32_t target = 0;
    if (raw) {
        if (value->unsigned64BitValue() > 4095) return kIOReturnBadArgument;
        target = value->unsigned32BitValue();
    } else if (!nativeBrightness.dac(value->unsigned64BitValue(), target)) return kIOReturnBadArgument;
    // Match the original AppleARMBacklight write entitlement, with the usual
    // administrator path for the existing development diagnostic client.
    auto *entitlement = IOUserClient::copyClientEntitlement(current_task(), "com.apple.private.dfr.brightness-access");
    bool permitted = entitlement == kOSBooleanTrue ||
        IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    OSSafeReleaseNULL(entitlement);
    if (!permitted) return kIOReturnNotPrivileged;
    return added ? gate->runAction(brightnessAction,
        reinterpret_cast<void *>(uintptr_t(target))) : kIOReturnNotReady;
}
IOReturn PMI8998WLED::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) {
    if (!name || !name->isEqualTo(DisplayBacklight::Function))
        return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
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
void PMI8998WLED::free() { cleanup(); IOService::free(); }

SInt32 PMI8998WLED::nativeSlot168(SInt32 luminance, SInt32 scale) {
    if (!ready || !nativeBrightness.valid() || luminance < 0 || scale < 0 || ml_at_interrupt_context()) return 0;
    uint64_t fixed = (uint64_t(luminance) * uint32_t(scale)) >> 16;
    const uint64_t maximum = (uint64_t(nativeBrightness.maximumMilliNits) << 16) / 1000;
    if (fixed > maximum) fixed = maximum;
    uint32_t target = 0;
    if (!nativeBrightness.dac(fixed, target) || !added ||
            gate->runAction(brightnessAction, reinterpret_cast<void *>(uintptr_t(target))) != kIOReturnSuccess) return 0;
    return SInt32((uint64_t(nativeBrightness.milliNits(target)) << 16) / 1000);
}
