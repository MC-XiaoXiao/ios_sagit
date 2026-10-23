// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include "WLANLifecycle.h"
#include "WLANFirmwareAssets.h"
class OSData;

// Platform operations will bind RPM/clock/NoC, QRTR/WLFW and a device
// IOMapper here. An IORegistry boolean is never evidence of ownership or
// firmware readiness. The initial backend deliberately acquires nothing.
class WLANPlatformBackend {
public:
    WLAN::Result acquire(WLAN::Resource, uint64_t) { return WLAN::Result::NotReady; }
    WLAN::Result release(WLAN::Resource, uint64_t) { return WLAN::Result::NotReady; }
    bool uncertain(WLAN::Resource, uint64_t) const { return false; }
};

class QcomWLAN : public IOService {
    OSDeclareDefaultStructors(QcomWLAN);
    IOWorkLoop *fWorkLoop = nullptr;
    IOCommandGate *fGate = nullptr;
    bool fActive = false, fPMInitialized = false;
    uint32_t fMissing = WLAN::InitialMissing;
    OSData *fBoard = nullptr, *fFirmwareConfiguration = nullptr;
    WLAN::FirmwareAssets fFirmwareAssets;
    WLANPlatformBackend fPlatform;
    WLAN::Lifecycle<WLANPlatformBackend> fLifecycle{fPlatform};
    static IOReturn gatedRequest(OSObject *, void *, void *, void *, void *);
    static IOReturn gatedStop(OSObject *, void *, void *, void *, void *);
    IOReturn perform(uint32_t, uint64_t *, uint64_t);
    void publishState();
    void cleanup();
    static bool validProvider(IOService *);
public:
    IOService *probe(IOService *, SInt32 *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
    IOReturn request(uint32_t, uint64_t *, uint64_t = 0);
};
