// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <QcomPMIC.h>
#include "ButtonState.h"
class QcomButtons : public IOHIDDevice {
    OSDeclareDefaultStructors(QcomButtons);
    QcomPMIC *fPMIC = nullptr;
    IOWorkLoop *fLoop = nullptr;
    IOCommandGate *fGate = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    IOTimerEventSource *fDiagnosticTimer = nullptr;
    IOInterruptEventSource *fInterrupts[3] = {};
    bool fAdded[3] = {}, fGateAdded=false, fTimerAdded=false, fOnline=false, fPM=false, fNeutralPending=false;
    IOBufferMemoryDescriptor *fReportBuffer = nullptr;
    QcomButton::State fState;
    UInt8 fReport=0;
    unsigned fErrors=0;
    bool fTrace=false;
    bool fDiagnosticEnabled=false,fDiagnosticActive=false,fDiagnosticTimerAdded=false;
    UInt64 fInputReports=0,fDiagnosticPulses=0;
    static void interruptAction(OSObject *, IOInterruptEventSource *, int);
    static void timerAction(OSObject *, IOTimerEventSource *);
    static void diagnosticRelease(OSObject *, IOTimerEventSource *);
    static IOReturn diagnosticPulse(OSObject *, void *, void *, void *, void *);
    static IOReturn snapshot(OSObject *, void *, void *, void *, void *);
    static IOReturn quiesce(OSObject *, void *, void *, void *, void *);
    static IOReturn resume(OSObject *, void *, void *, void *, void *);
    void sample();
    bool send(UInt8);
    void cleanup();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool handleStart(IOService *) APPLE_KEXT_OVERRIDE;
    void handleStop(IOService *) APPLE_KEXT_OVERRIDE;
    OSString *newTransportString() const APPLE_KEXT_OVERRIDE;
    OSString *newManufacturerString() const APPLE_KEXT_OVERRIDE;
    OSString *newProductString() const APPLE_KEXT_OVERRIDE;
    OSNumber *newPrimaryUsageNumber() const APPLE_KEXT_OVERRIDE;
    OSNumber *newPrimaryUsagePageNumber() const APPLE_KEXT_OVERRIDE;
    IOReturn newReportDescriptor(IOMemoryDescriptor **) const APPLE_KEXT_OVERRIDE;
    IOReturn getReport(IOMemoryDescriptor *, IOHIDReportType, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
};
