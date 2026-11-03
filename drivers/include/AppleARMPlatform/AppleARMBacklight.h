// SPDX-License-Identifier: BSD-2-Clause
// Subclass ABI of AppleARMPlatform in the pinned 22H374 provider image.
#pragma once
#include <IOKit/IOService.h>

class AppleARMBacklight : public IOService {
    OSDeclareAbstractStructors(AppleARMBacklight);
    UInt8 nativeState[880 - sizeof(IOService)];
public:
    bool init(OSDictionary *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
    IOReturn configureReport(IOReportChannelList *, IOReportConfigureAction, void *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn updateReport(IOReportChannelList *, IOReportUpdateAction, void *, void *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn message(UInt32, IOService *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
protected:
    // Local alias for slot 168: two signed 16.16 inputs and a signed 16.16
    // luminance result. Native implementation applies the PCC brightness scale.
    virtual SInt32 nativeSlot168(SInt32, SInt32) = 0;
};
static_assert(sizeof(IOService) == 136, "native IOService ABI");
static_assert(sizeof(AppleARMBacklight) == 880, "22H374 AppleARMBacklight ABI");
