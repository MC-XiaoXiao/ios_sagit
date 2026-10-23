// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOWatchdog.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>

class QualcommWatchdogTimer : public IOWatchdog {
    OSDeclareDefaultStructors(QualcommWatchdogTimer);
    IOMemoryMap *fMap = nullptr;
    IOWorkLoop *fLoop = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    volatile UInt32 *fRegisters = nullptr;
    bool fRunning = false, fPMStarted = false, fFrameworkStarted = false, fSelfTest = false;
    UInt64 fFeeds = 0;
    UInt64 fCaptureDeadline = 0;
    bool fCaptureActive = false;
    static void tick(OSObject *, IOTimerEventSource *);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    void systemWillShutdown(IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
    void enableWatchdog() APPLE_KEXT_OVERRIDE;
    void disableWatchdog() APPLE_KEXT_OVERRIDE;
    void resetWatchdog() APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE { return fLoop; }
    // Register adapter shared with the deterministic hardware tests.
    void write(uint32_t offset, uint32_t value);
};
