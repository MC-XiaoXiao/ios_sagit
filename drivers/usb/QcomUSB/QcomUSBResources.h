// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>

// Shares the controller's AppleARMIODevice through a separate IOKit matching
// category. The QcomUSB client holds the provider open; this service owns its
// resource lease and never constructs a USB Family controller directly.
class QcomUSBResources : public IOService {
    OSDeclareDefaultStructors(QcomUSBResources);
    struct State;
    State *fState = nullptr;
    bool fStarted = false, fPublished = false, fPinned = false;
    static void poll(OSObject *, class IOTimerEventSource *);
    static void notify(void *, void *);
    void pin();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn message(UInt32, IOService *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
};
