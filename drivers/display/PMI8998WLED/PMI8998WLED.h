// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomSPMI.h>
#include <AppleARMPlatform/AppleARMBacklight.h>
#include "PMI8998WLEDHardware.h"
#include "NativeBrightness.h"

class PMI8998WLED : public AppleARMBacklight {
    OSDeclareDefaultStructors(PMI8998WLED);
    QcomSPMI *spmi = nullptr;
    IOWorkLoop *loop = nullptr;
    IOCommandGate *gate = nullptr;
    IOService *client = nullptr;
    PMI8998Backlight::Hardware hardware;
    PMI8998Backlight::NativeBrightness nativeBrightness;
    uint32_t identifier = 0;
    uint32_t nativeBrightnessRequests = 0;
    uint32_t maximumMicroAmps = 0;
    uint64_t powerRequests = 0, queryRequests = 0, hardwareTransfers = 0;
    IOReturn lastError = kIOReturnSuccess;
    bool added = false, retainedBus = false, ready = false;
    struct Bus {
        QcomSPMI *spmi;
        uint64_t transfers = 0;
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
    bool publishNativeModel(const PMI8998Backlight::Config &);
    void cleanup();
public:
    // The retained Qualcomm controller owns hardware and policy. The native
    // Apple constructor/metadata supply the backlight type expected by iOS;
    // Apple SoC initialization and its private workloop are not entered.
    bool init(OSDictionary *properties) APPLE_KEXT_OVERRIDE { return IOService::init(properties); }
    IOReturn configureReport(IOReportChannelList *channels, IOReportConfigureAction action, void *result, void *destination) APPLE_KEXT_OVERRIDE {
        return IOService::configureReport(channels, action, result, destination);
    }
    IOReturn updateReport(IOReportChannelList *channels, IOReportUpdateAction action, void *result, void *destination) APPLE_KEXT_OVERRIDE {
        return IOService::updateReport(channels, action, result, destination);
    }
    IOReturn message(UInt32 type, IOService *provider, void *argument) APPLE_KEXT_OVERRIDE {
        return IOService::message(type, provider, argument);
    }
    IOReturn setPowerState(unsigned long state, IOService *provider) APPLE_KEXT_OVERRIDE {
        return IOService::setPowerState(state, provider);
    }
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
protected:
    SInt32 nativeSlot168(SInt32, SInt32) APPLE_KEXT_OVERRIDE;
};
