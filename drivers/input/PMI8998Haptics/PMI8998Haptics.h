// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomSPMI.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include "PMI8998HapticsHardware.h"

class PMI8998Haptics : public IOService {
    OSDeclareDefaultStructors(PMI8998Haptics);
    QcomSPMI *spmi = nullptr;
    IOWorkLoop *loop = nullptr;
    IOCommandGate *gate = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOUserClient *owner = nullptr; // Borrowed, removed synchronously by client close/stop.
    PMI8998Haptic::Config config{};
    PMI8998Haptic::Hardware hardware;
    bool gateAdded = false, timerAdded = false, pm = false, running = false, awake = false;
    uint64_t generation = 0;
    IOReturn lastIO = kIOReturnSuccess;
    struct Bus {
        QcomSPMI *spmi;
        IOReturn error = kIOReturnSuccess;
        bool read(uint8_t, uint16_t, uint8_t *, unsigned);
        bool write(uint8_t, uint16_t, uint8_t *, unsigned);
    };
    struct Request {
        uint32_t selector;
        uint64_t *output;
        const uint64_t *input;
        IOUserClient *client;
        bool close = false;
    };
    static IOReturn action(OSObject *, void *, void *, void *, void *);
    static IOReturn powerAction(OSObject *, void *, void *, void *, void *);
    static void tick(OSObject *, IOTimerEventSource *);
    void arm(Bus &);
    void publish();
    void cleanup();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
    IOReturn request(uint32_t, IOUserClient *, const uint64_t *, uint64_t *);
    void closeClient(IOUserClient *);
};
