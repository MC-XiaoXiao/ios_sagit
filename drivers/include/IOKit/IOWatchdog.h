// SPDX-License-Identifier: BSD-2-Clause
// Subclass ABI of the pinned iOS 18 arm64 AppleARMWatchdogTimer provider.
// The original framework supplies monitoring policy and its entitled user client.
#pragma once
#include <IOKit/IOService.h>

class IOWatchdog : public IOService {
    OSDeclareAbstractStructors(IOWatchdog);
    UInt8 nativeState[112];
public:
    // Audited nonvirtual framework entry points; see apple-watchdog-abi.json.
    bool start(IOService *, UInt32, UInt32);
    void checkForTimeout();
    bool socWatchdogEnabled();
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
    // Native slots 168..171, following IOService's 168 slots.
    virtual void enableWatchdog() = 0;
    virtual void disableWatchdog() = 0;
    virtual void resetWatchdog() = 0;
    virtual void publishWatchdogProperties();
};
static_assert(sizeof(IOService) == 136, "native IOService ABI");
static_assert(sizeof(IOWatchdog) == 248, "native IOWatchdog ABI");
