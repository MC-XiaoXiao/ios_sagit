// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <GenericSPI.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include "GyroHID.h"

class SPIGyro : public IOHIDDevice {
    OSDeclareDefaultStructors(SPIGyro);
    GenericSPIDevice *fDevice = nullptr;
    struct Transport {
        GenericSPIDevice *device = nullptr;
        IOService *owner = nullptr;
        bool read(uint8_t, uint8_t *, size_t);
        bool write(uint8_t, uint8_t);
        void delayMS(unsigned);
    } fTransport;
    Gyro::Sensor<Transport> fSensor{fTransport};
    Gyro::Model fModel = Gyro::Model::LSM6DSM;
    Gyro::Mount fMount;
    IOWorkLoop *fLoop = nullptr;
    IOCommandGate *fGate = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    IOBufferMemoryDescriptor *fBuffer = nullptr;
    bool fOpen = false, fGateAdded = false, fTimerAdded = false, fPM = false;
    bool fRunning = false, fStopping = false, fHIDReady = false, fLastValid = false;
    uint32_t fIntervalUS = GyroHID::DefaultIntervalUS;
    unsigned fErrors = 0, fEmpty = 0;
    uint8_t fLast[GyroHID::InputBytes]{};
    uint64_t fLastTime = 0, fSamples = 0;
    static void timer(OSObject *, IOTimerEventSource *);
    static IOReturn power(OSObject *, void *, void *, void *, void *);
    static IOReturn reportAction(OSObject *, void *, void *, void *, void *);
    void sample();
    void cleanup();
    void offline(Gyro::Result);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool handleStart(IOService *) APPLE_KEXT_OVERRIDE;
    void handleStop(IOService *) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    OSString *newTransportString() const APPLE_KEXT_OVERRIDE;
    OSString *newManufacturerString() const APPLE_KEXT_OVERRIDE;
    OSString *newProductString() const APPLE_KEXT_OVERRIDE;
    OSNumber *newPrimaryUsageNumber() const APPLE_KEXT_OVERRIDE;
    OSNumber *newPrimaryUsagePageNumber() const APPLE_KEXT_OVERRIDE;
    IOReturn newReportDescriptor(IOMemoryDescriptor **) const APPLE_KEXT_OVERRIDE;
    IOReturn getReport(IOMemoryDescriptor *, IOHIDReportType, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn setReport(IOMemoryDescriptor *, IOHIDReportType, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
};
