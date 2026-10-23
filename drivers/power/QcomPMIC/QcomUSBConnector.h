// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/IOService.h>

// A separate matching category permits the PMIC input and connector clients
// to coexist. The provider owns SPMI arbitration and the interrupt domain.
class QcomUSBConnector : public IOService {
    OSDeclareDefaultStructors(QcomUSBConnector);
    struct State;
    State *fState = nullptr;
    bool fStarted = false, fPinned = false, fPublished = false;
    void pin();
    static void interrupt(OSObject *, void *, IOService *, int);
    static void notify(void *, void *);
    static void poll(OSObject *, class IOTimerEventSource *);
protected:
    bool handleOpen(IOService *, IOOptionBits, void *) APPLE_KEXT_OVERRIDE;
    void handleClose(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    bool handleIsOpen(const IOService *) const APPLE_KEXT_OVERRIDE;
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
};
