// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <QcomSPMI.h>
#include "PMICInputs.h"
#include "PMICUSB.h"
#include <InputIRQState.h>

class QcomPMIC : public IOInterruptController {
    OSDeclareDefaultStructors(QcomPMIC);
    QcomSPMI *fSPMI = nullptr;
    IOWorkLoop *fLoop = nullptr;
    IOCommandGate *fGate = nullptr;
    IOInterruptEventSource *fInterrupt = nullptr;
    const OSSymbol *fName = nullptr;
    bool fGateAdded = false, fIRQAdded = false, fPublished = false, fFault = false, fSuspended = false, fPM = false;
    unsigned fCount = 0, fButtonCount = 0;
    InputIRQ::SummaryGuard fSummary;
    struct Entry { QcomPMICInput::IRQ irq; UInt16 apid; bool enabled; } fEntries[QcomPMICInput::MaxIRQs] = {};
    QcomPMICInput::Button fButtons[3] = {};
    PMICUSB::Config fUSBConfig{};
    IOService *fUSBClient = nullptr;
    struct Bus {
        QcomSPMI *spmi = nullptr;
        IOReturn error = kIOReturnSuccess;
        bool read(unsigned, unsigned, uint8_t *, unsigned);
        bool write(unsigned, unsigned, const uint8_t *, unsigned);
    } fBus;
    enum Operation { Register, Unregister, Enable, Disable, PrepareButtons, ReadButtons, GPIORead, GPIOConfigure,
                     ClaimUSB, ReleaseUSB, TransferUSB, Suspend, Resume };
    struct USBTransfer { UInt16 address; UInt8 *bytes; unsigned count; bool write; };
    struct Request { Operation op; IOService *nub; int source; void *target; IOInterruptHandler handler; void *ref; void *data; };
    static IOReturn dispatch(OSObject *, void *, void *, void *, void *);
    static void interruptAction(OSObject *, IOInterruptEventSource *, int);
    IOReturn perform(Request &);
    int decode(IOService *, int) const;
    void drain();
    void fault();
    void cleanup();
    bool publishButtonSources();
    unsigned ownedMask(unsigned) const;
    void updateParent();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn registerInterrupt(IOService *, int, void *, IOInterruptHandler, void *) APPLE_KEXT_OVERRIDE;
    IOReturn unregisterInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
    IOReturn enableInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
    IOReturn disableInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
    IOReturn getInterruptType(IOService *, int, int *) APPLE_KEXT_OVERRIDE;
    IOReturn causeInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
    unsigned buttonCount() const { return fButtonCount; }
    bool buttonConfig(unsigned, QcomPMICInput::Button &) const;
    IOReturn prepareButtons();
    IOReturn readButtons(UInt32 &mask);
    bool usbConfiguration(PMICUSB::Config &) const;
    IOReturn claimUSB(IOService *, bool acquire);
    IOReturn usbTransfer(IOService *, UInt16 address, UInt8 *, unsigned count, bool write);
};
