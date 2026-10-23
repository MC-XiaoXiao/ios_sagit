// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSSet.h>

class QcomSMEM : public IOService {
    OSDeclareDefaultStructors(QcomSMEM);
    struct State;
    State *fState = nullptr;
    IOWorkLoop *fLoop = nullptr;
    IOCommandGate *fGate = nullptr;
    OSSet *fClients = nullptr;
    IOService *fModem = nullptr, *fADSP = nullptr;
    bool fActive = false, fPinned = false;
    static IOReturn action(OSObject *, void *, void *, void *, void *);
    static IOReturn deactivate(OSObject *, void *, void *, void *, void *);
    bool scoped(const IOService *) const;
    void pin();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
    bool handleOpen(IOService *, IOOptionBits, void *) APPLE_KEXT_OVERRIDE;
    void handleClose(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    bool handleIsOpen(const IOService *) const APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
};
