// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomSPMI.h>
#include "PMI8998WLEDHardware.h"

class PMI8998WLED : public IOService {
    OSDeclareDefaultStructors(PMI8998WLED);
    QcomSPMI *spmi = nullptr;
    IOWorkLoop *loop = nullptr;
    IOCommandGate *gate = nullptr;
    IOService *client = nullptr;
    PMI8998Backlight::Hardware hardware;
    uint32_t identifier = 0;
    uint32_t nativeBrightnessRequests = 0;
    bool added = false, retainedBus = false, ready = false;
    struct Bus {
        QcomSPMI *spmi;
        IOReturn error = kIOReturnSuccess;
        QcomPower::Status transfer(uint8_t, uint16_t, uint8_t *, unsigned, bool);
        bool irqChannel(uint8_t sid, uint8_t peripheral, uint16_t &apid) const {
            return spmi->irqChannel(sid, peripheral, apid);
        }
        void delay(unsigned);
    };
    static IOReturn action(OSObject *, void *, void *, void *, void *);
    static IOReturn brightnessAction(OSObject *, void *, void *, void *, void *);
    bool publishState();
    void cleanup();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
};
