// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "AudioOutputBackend.h"
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>

class QcomADSP : public AudioOutputBackend {
    OSDeclareDefaultStructors(QcomADSP);
    struct State;
    State *fState = nullptr;
    IOService *fOwner = nullptr;
    IOWorkLoop *fLoop = nullptr;
    IOCommandGate *fGate = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    IOInterruptEventSource *fIRQ = nullptr;
    bool fStarted = false, fProviderOpen = false, fPM = false, fPinned = false;
    bool fGateAdded = false, fTimerAdded = false, fIRQAdded = false;
    static IOReturn action(OSObject *, void *, void *, void *, void *);
    static void tick(OSObject *, IOTimerEventSource *);
    static void interrupt(OSObject *, IOInterruptEventSource *, int);
    AudioOutput::Result call(unsigned, void * = nullptr, void * = nullptr);
    void pin();
    void unpin();
    void cleanup();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    bool handleOpen(IOService *, IOOptionBits, void *) APPLE_KEXT_OVERRIDE;
    void handleClose(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    bool handleIsOpen(const IOService *) const APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    AudioOutput::Result outputConfiguration(AudioOutput::Configuration &) APPLE_KEXT_OVERRIDE;
    AudioOutput::Result prepareOutput(IOBufferMemoryDescriptor *, const AudioOutput::Configuration &) APPLE_KEXT_OVERRIDE;
    AudioOutput::Result startOutput(uint64_t) APPLE_KEXT_OVERRIDE;
    AudioOutput::Result stopOutput() APPLE_KEXT_OVERRIDE;
    AudioOutput::Result readPosition(AudioOutput::Position &) APPLE_KEXT_OVERRIDE;
    AudioOutput::Result releaseOutput() APPLE_KEXT_OVERRIDE;
};
