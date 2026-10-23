// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <QcomSPMI.h>
#include "PMI8998Battery.h"

// Created explicitly with IOPMPowerSource::init(void), which initializes the
// native settings dictionary (IOService's personality initializer does not).
class QcomBatteryPowerSource : public IOPMPowerSource {
    OSDeclareDefaultStructors(QcomBatteryPowerSource);
    void number(const char *, uint32_t);
    void boolean(const char *, bool);
    void forget(const char *);
    void clearMeasurements();
public:
    void publish(const PMICBattery::Sample &);
    void invalidate(PMICBattery::Result);
};

class QcomBattery : public IOService {
    OSDeclareDefaultStructors(QcomBattery);
    QcomSPMI *spmi = nullptr;
    IOWorkLoop *loop = nullptr;
    IOCommandGate *gate = nullptr;
    IOTimerEventSource *timer = nullptr;
    QcomBatteryPowerSource *source = nullptr;
    PMICBattery::Config config{};
    bool gateAdded = false, timerAdded = false, pm = false;
    bool running = false, awake = true, published = false;
    UInt64 successes = 0, failures = 0;
    struct Bus {
        QcomSPMI *spmi;
        IOReturn error = kIOReturnSuccess;
        bool read(uint8_t, uint16_t, uint8_t *, unsigned);
    };
    static void tick(OSObject *, IOTimerEventSource *);
    static IOReturn powerAction(OSObject *, void *, void *, void *, void *);
    void poll();
    void cleanup();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
};
