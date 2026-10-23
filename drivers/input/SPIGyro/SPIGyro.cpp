// SPDX-License-Identifier: GPL-2.0-only
#include "SPIGyro.h"
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#define super IOHIDDevice
OSDefineMetaClassAndStructors(SPIGyro, super);

bool SPIGyro::Transport::read(uint8_t reg, uint8_t *bytes, size_t length) {
    if (!bytes || !length || length > 6 || reg >= 0x80) return false;
    uint8_t tx[7]{uint8_t(reg | 0x80)}, rx[7]{};
    if (device->transfer(owner, tx, rx, length + 1) != kIOReturnSuccess) return false;
    memcpy(bytes, rx + 1, length);
    return true;
}
bool SPIGyro::Transport::write(uint8_t reg, uint8_t value) {
    if (reg >= 0x80) return false;
    uint8_t bytes[]{reg, value};
    return device->transfer(owner, bytes, nullptr, sizeof(bytes)) == kIOReturnSuccess;
}
void SPIGyro::Transport::delayMS(unsigned ms) { IOSleep(ms); }
bool SPIGyro::handleStart(IOService *provider) {
    if (!super::handleStart(provider)) return false;
    fDevice = OSDynamicCast(GenericSPIDevice, provider);
    // Chip identity is chosen by matching, then verified against WHO_AM_I.
    auto *model = OSDynamicCast(OSNumber, getProperty("GyroModel"));
    auto *mount = OSDynamicCast(OSData, provider->getProperty("gyro,mount"));
    if (!fDevice || !model || (model->unsigned32BitValue() != 1 && model->unsigned32BitValue() != 2) ||
        !mount || mount->getLength() != sizeof(fMount.axis)) return false;
    memcpy(fMount.axis, mount->getBytesNoCopy(), sizeof(fMount.axis));
    fModel = Gyro::Model(model->unsigned32BitValue());
    // Refuse to assume a phone orientation or share a live IMU client.
    if (!fMount.valid() || !fDevice->open(this)) return false;
    fOpen = true; fTransport.device = fDevice; fTransport.owner = this;
    fLoop = IOWorkLoop::workLoop();
    fGate = IOCommandGate::commandGate(this);
    fTimer = IOTimerEventSource::timerEventSource(this, timer);
    fBuffer = IOBufferMemoryDescriptor::withCapacity(GyroHID::InputBytes, kIODirectionOutIn);
    if (!fLoop || !fGate || !fTimer || !fBuffer ||
        fLoop->addEventSource(fGate) != kIOReturnSuccess) goto fail;
    fGateAdded = true;
    if (fLoop->addEventSource(fTimer) != kIOReturnSuccess) goto fail;
    fTimerAdded = true;
    {
        const auto result = fSensor.initialize(fModel);
        setProperty("GyroLastError", uint64_t(result), 32);
        if (result != Gyro::Result::Success) goto fail;
    }
    setProperty("Built-In", true);
    setProperty("HIDDefaultBehavior", "");
    setProperty("GyroFactoryCalibrationApplied", false);
    setProperty("GyroSampleUnits", "rad/s");
    setProperty("GyroSampleMode", "polling");
    setProperty("GyroHardwareRateHz", fModel == Gyro::Model::LSM6DSM ? 208 : 200, 32);
    setProperty("ReportInterval", fIntervalUS, 32);
    setProperty("HIDInputOffline", true);
    return true;
fail:
    cleanup(); return false;
}
bool SPIGyro::start(IOService *provider) {
    if (!super::start(provider)) { cleanup(); return false; }
    fHIDReady = true;
    IOPMPowerState states[2]{};
    states[0].version = states[1].version = 1;
    // Retained resources are required in both states. Off puts the gyro into
    // standby; it does not remove shared supplies or power-collapse the SSC.
    states[0].inputPowerRequirement = states[0].outputPowerCharacter = kIOPMPowerOn;
    states[1].inputPowerRequirement = states[1].outputPowerCharacter = kIOPMPowerOn;
    states[1].capabilityFlags = kIOPMDeviceUsable;
    PMinit(); fPM = true; provider->joinPMtree(this);
    if (registerPowerDriver(this, states, 2) != kIOReturnSuccess ||
        fGate->runAction(power, reinterpret_cast<void *>(1)) != kIOReturnSuccess) {
        super::stop(provider); return false;
    }
    changePowerStateTo(1);
    registerService();
    return true;
}
void SPIGyro::offline(Gyro::Result error) {
    fRunning = false; fLastValid = false;
    fTimer->cancelTimeout();
    setProperty("GyroLastError", uint64_t(error), 32);
    setProperty("HIDInputOffline", true);
    if (!fSensor.sleep()) setProperty("GyroStandbyFailed", true);
}
void SPIGyro::sample() {
    if (!fRunning || fStopping || !fHIDReady) return;
    Gyro::Sample raw, mounted;
    auto result = fSensor.sample(raw);
    if (result != Gyro::Result::Success) {
        fLastValid = false;
        setProperty("GyroLastError", uint64_t(result), 32);
        // No-data is not a zero angular velocity. Bound a stuck data-ready line.
        if (result == Gyro::Result::NoData) {
            if (++fEmpty >= 50) { offline(result); return; }
        } else if (result == Gyro::Result::Configuration || ++fErrors >= 3) {
            offline(result); return;
        }
    } else if (!fMount.apply(raw, mounted)) {
        offline(Gyro::Result::Configuration); return;
    } else {
        fErrors = fEmpty = 0;
        uint8_t bytes[GyroHID::InputBytes];
        GyroHID::report(mounted, bytes);
        fLastValid = false;
        if (fBuffer->writeBytes(0, bytes, sizeof(bytes)) != sizeof(bytes) ||
            handleReport(fBuffer, kIOHIDReportTypeInput, 0) != kIOReturnSuccess) {
            // Do not mark a rejected report as successfully delivered.
            setProperty("GyroHIDDeliveryFailed", true);
        } else {
            memcpy(fLast, bytes, sizeof(bytes));
            clock_get_uptime(&fLastTime);
            fLastValid = true; ++fSamples;
            setProperty("GyroSamplesDelivered", fSamples, 64);
            setProperty("GyroLastError", uint64_t(0), 32);
            setProperty("GyroHIDDeliveryFailed", false);
            setProperty("HIDInputOffline", false);
        }
    }
    fTimer->setTimeoutUS(fIntervalUS);
}
void SPIGyro::timer(OSObject *owner, IOTimerEventSource *) { static_cast<SPIGyro *>(owner)->sample(); }
IOReturn SPIGyro::power(OSObject *owner, void *value, void *, void *, void *) {
    auto *self = static_cast<SPIGyro *>(owner);
    if (value == reinterpret_cast<void *>(1)) {
        if (self->fStopping) return kIOReturnNotReady;
        if (self->fRunning) return kIOReturnSuccess;
        if (!self->fSensor.ready()) {
            auto result = self->fSensor.initialize(self->fModel);
            if (result != Gyro::Result::Success) {
                self->offline(result); return kIOReturnIOError;
            }
        }
        self->fLastValid = false;
        self->fErrors = self->fEmpty = 0;
        self->fRunning = true;
        self->setProperty("GyroStandbyFailed", false);
        self->fTimer->setTimeoutUS(self->fIntervalUS);
        return kIOReturnSuccess;
    }
    self->fRunning = false; self->fLastValid = false;
    if (self->fTimer) self->fTimer->cancelTimeout();
    self->setProperty("HIDInputOffline", true);
    if (value == reinterpret_cast<void *>(2)) {
        self->fStopping = true; self->fHIDReady = false;
        if (!self->fSensor.restore()) {
            self->setProperty("GyroRestoreFailed", true);
            return kIOReturnIOError;
        }
    } else if (!self->fSensor.sleep()) {
        self->setProperty("GyroStandbyFailed", true);
        return kIOReturnIOError;
    }
    return kIOReturnSuccess;
}
IOReturn SPIGyro::setPowerState(unsigned long state, IOService *) {
    if (state > 1) return kIOPMNoSuchState;
    if (fGateAdded) fGate->runAction(power, reinterpret_cast<void *>(state));
    return IOPMAckImplied;
}
struct GyroReportRequest {
    IOMemoryDescriptor *buffer;
    IOHIDReportType type;
    uint32_t id;
    bool write;
};
IOReturn SPIGyro::reportAction(OSObject *owner, void *argument, void *, void *, void *) {
    auto *self = static_cast<SPIGyro *>(owner);
    auto &r = *static_cast<GyroReportRequest *>(argument);
    if (self->fStopping) return kIOReturnNotReady;
    if (r.type == kIOHIDReportTypeFeature && r.id == GyroHID::IntervalID) {
        uint8_t bytes[GyroHID::FeatureBytes];
        if (r.write) {
            uint32_t interval = 0;
            if (r.buffer->getLength() != sizeof(bytes) ||
                r.buffer->readBytes(0, bytes, sizeof(bytes)) != sizeof(bytes) ||
                !GyroHID::interval(bytes, sizeof(bytes), interval)) return kIOReturnBadArgument;
            self->fIntervalUS = interval;
            self->fLastValid = false;
            self->setProperty("ReportInterval", interval, 32);
            if (self->fRunning) {
                self->fTimer->cancelTimeout();
                self->fTimer->setTimeoutUS(interval);
            }
            return kIOReturnSuccess;
        }
        GyroHID::feature(self->fIntervalUS, bytes);
        return r.buffer->getLength() >= sizeof(bytes) &&
               r.buffer->writeBytes(0, bytes, sizeof(bytes)) == sizeof(bytes)
               ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    if (r.write || r.type != kIOHIDReportTypeInput || r.id != GyroHID::InputID) return kIOReturnUnsupported;
    if (!self->fRunning || !self->fLastValid) return kIOReturnNotReady;
    uint64_t now, ns; clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now - self->fLastTime, &ns);
    if (ns > uint64_t(self->fIntervalUS) * 2000) return kIOReturnNotReady;
    return r.buffer->getLength() >= sizeof(self->fLast) &&
           r.buffer->writeBytes(0, self->fLast, sizeof(self->fLast)) == sizeof(self->fLast)
           ? kIOReturnSuccess : kIOReturnBadArgument;
}
IOReturn SPIGyro::getReport(IOMemoryDescriptor *out, IOHIDReportType type, IOOptionBits options) {
    if (!out) return kIOReturnBadArgument;
    if (!fGateAdded) return kIOReturnNotReady;
    GyroReportRequest r{out, type, options & 255, false};
    return fGate->runAction(reportAction, &r);
}
IOReturn SPIGyro::setReport(IOMemoryDescriptor *in, IOHIDReportType type, IOOptionBits options) {
    if (!in) return kIOReturnBadArgument;
    if (!fGateAdded) return kIOReturnNotReady;
    GyroReportRequest r{in, type, options & 255, true};
    return fGate->runAction(reportAction, &r);
}
IOReturn SPIGyro::newReportDescriptor(IOMemoryDescriptor **out) const {
    if (!out) return kIOReturnBadArgument;
    *out = IOBufferMemoryDescriptor::withBytes(GyroHID::Descriptor, sizeof(GyroHID::Descriptor), kIODirectionOutIn);
    return *out ? kIOReturnSuccess : kIOReturnNoMemory;
}
IOWorkLoop *SPIGyro::getWorkLoop() const { return fLoop; }
OSString *SPIGyro::newTransportString() const { return OSString::withCString("SPI"); }
OSString *SPIGyro::newManufacturerString() const {
    return OSString::withCString(fModel == Gyro::Model::LSM6DSM ? "STMicroelectronics" : "TDK InvenSense");
}
OSString *SPIGyro::newProductString() const { return OSString::withCString("SPI Gyroscope"); }
OSNumber *SPIGyro::newPrimaryUsageNumber() const { return OSNumber::withNumber(0x76, 32); }
OSNumber *SPIGyro::newPrimaryUsagePageNumber() const { return OSNumber::withNumber(0x20, 32); }
void SPIGyro::stop(IOService *provider) {
    if (fGateAdded) fGate->runAction(power, reinterpret_cast<void *>(2));
    super::stop(provider);
}
void SPIGyro::handleStop(IOService *provider) { cleanup(); super::handleStop(provider); }
void SPIGyro::cleanup() {
    if (fPM) { PMstop(); fPM = false; }
    if (fGateAdded) fGate->runAction(power, reinterpret_cast<void *>(2));
    else { fStopping = true; fHIDReady = false; }
    if (fTimer) fTimer->cancelTimeout();
    if (fTimerAdded) { fLoop->removeEventSource(fTimer); fTimerAdded = false; }
    if (fGateAdded) { fLoop->removeEventSource(fGate); fGateAdded = false; }
    if (fOpen) {
        if (!fSensor.restore()) IOLog("SPIGyro: register restore failed\n");
        fDevice->close(this); fOpen = false;
    }
    OSSafeReleaseNULL(fTimer); OSSafeReleaseNULL(fGate);
    OSSafeReleaseNULL(fLoop); OSSafeReleaseNULL(fBuffer);
}
void SPIGyro::free() { cleanup(); super::free(); }
