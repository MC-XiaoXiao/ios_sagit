// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include "USBPhyProtocol.h"

// AppleARMIODevice matched, independently owned USB PHY service. Platform
// resource acquisition uses open + USBPhyRequest; this service does not own
// DWC3/xHCI DMA or infer connector wiring from a USB controller name.
class QcomUSBPHY : public IOService {
    OSDeclareDefaultStructors(QcomUSBPHY);
    struct State;
    State *fState = nullptr;
    bool fPinned = false, fStarted = false;
    void pin();
protected:
    bool handleOpen(IOService *, IOOptionBits, void *) APPLE_KEXT_OVERRIDE;
    void handleClose(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    bool handleIsOpen(const IOService *) const APPLE_KEXT_OVERRIDE;
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
};
