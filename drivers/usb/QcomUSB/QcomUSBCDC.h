// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>

// Optional, exclusive CDC ACM console personality. The shared role nub still
// owns clocks, PHY, connector, DMA isolation and admission.
class QcomUSBCDC : public IOService {
    OSDeclareDefaultStructors(QcomUSBCDC);
    struct State;
    State *fState = nullptr;
    bool fStarted = false, fConsoleClaimed = false;
    static void interrupt(OSObject *, class IOInterruptEventSource *, int);
    static void timer(OSObject *, class IOTimerEventSource *);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
};
