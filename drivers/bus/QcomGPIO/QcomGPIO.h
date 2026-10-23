// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "TLMM.h"
#include "TLMMPinmux.h"

class QcomGPIO : public IOInterruptController {
    OSDeclareDefaultStructors(QcomGPIO);
    IOMemoryMap *fMap = nullptr;
    const OSSymbol *fName = nullptr;
    IOService *fParent = nullptr;
    bool fPublished = false, fParentRegistered = false, fFault = false;
    bool fOwned[QcomTLMM::PinCount] = {};
    uint32_t fMuxAllowed[QcomTLMM::PinCount]{};
    QcomTLMM::PinmuxLeases fMuxLeases;
    IOReturn pinmux(Pinmux::Request &);
    uint32_t fTrigger[QcomTLMM::PinCount] = {};
    QcomTLMM::SummaryGuard fSummary;
    struct Registers {
        volatile uint8_t *base = nullptr;
        uint32_t read32(uint32_t);
        void write32(uint32_t, uint32_t);
    } fRegisters;
    bool decode(IOService *, int, uint32_t &, uint32_t &) const;
    bool owns(IOService *, int, uint32_t &) const;
    void cleanup();
    static void parentInterrupt(OSObject *, void *, IOService *, int);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn registerInterrupt(IOService *, int, void *, IOInterruptHandler, void *) APPLE_KEXT_OVERRIDE;
    IOReturn unregisterInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
    IOReturn enableInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
    IOReturn disableInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
    IOReturn causeInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
    IOReturn getInterruptType(IOService *, int, int *) APPLE_KEXT_OVERRIDE;
    IOInterruptAction getInterruptHandlerAddress() APPLE_KEXT_OVERRIDE;
    IOReturn handleInterrupt(void *, IOService *, int) APPLE_KEXT_OVERRIDE;
    bool vectorCanBeShared(IOInterruptVectorNumber, IOInterruptVector *) APPLE_KEXT_OVERRIDE;
    void disableVectorHard(IOInterruptVectorNumber, IOInterruptVector *) APPLE_KEXT_OVERRIDE;
    void enableVector(IOInterruptVectorNumber, IOInterruptVector *) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
};
