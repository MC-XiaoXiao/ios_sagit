// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/IOService.h>
class QcomSCM : public IOService {
    OSDeclareDefaultStructors(QcomSCM);
    struct State;
    State *fState = nullptr;
    bool fStarted = false, fProviderOpen = false, fPinned = false;
    bool quarantined() const;
    void latchQuarantine();
    void pin();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    bool handleOpen(IOService *, IOOptionBits, void *) APPLE_KEXT_OVERRIDE;
    void handleClose(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    bool handleIsOpen(const IOService *) const APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
};
