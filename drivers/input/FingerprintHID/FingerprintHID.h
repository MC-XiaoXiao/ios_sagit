// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread_call.h>
#include "HomeWorker.h"
#include "SecureApp.h"

// Apple IOHIDDevice supplies parsing, IOHIDInterface publication and native
// event delivery. Chip Kexts select a measured firmware protocol in handleStart.
class FingerprintHID : public IOHIDDevice {
    OSDeclareAbstractStructors(FingerprintHID);
    IOService *fGPIO=nullptr, *fClock=nullptr;
    IOWorkLoop *fLoop=nullptr;
    IOCommandGate *fGate=nullptr;
    IOInterruptEventSource *fIRQ=nullptr;
    IOTimerEventSource *fTimer=nullptr;
    IOTimerEventSource *fDiagnosticTimer=nullptr;
    IOBufferMemoryDescriptor *fReportBuffer=nullptr;
    HomeSecureApp fApp;
    thread_call_t fWorker=nullptr;
    QcomHomeProtocol::WorkerQueue fWork;
    struct WorkResult {
        QcomHomeProtocol::WorkerQueue::Work work;
        bool success=false,blocked=false,down=false,changed=false;
        uint64_t durationUS=0;
    };
    uint32_t fPowerPin=0,fResetPin=0,fIRQPin=0;
    unsigned fErrors=0;
    uint64_t fInputReports=0,fSensorSamples=0;
    uint64_t fIRQRequests=0,fTimerRequests=0,fMaxSecureUS=0;
    bool fTrace=false,fStopping=false,fQuarantined=false;
    bool fDiagnosticEnabled=false,fDiagnosticActive=false,fDiagnosticTimerAdded=false;
    uint64_t fDiagnosticPulses=0;
    bool fGateAdded=false,fIRQAdded=false,fTimerAdded=false,fOnline=false,fPM=false;
    bool fBusVote=false,fCoreVote=false,fPowered=false;
    QcomHomeProtocol::State fState;
    IOReturn gpio(const char *,uint32_t,uintptr_t);
    bool clock(unsigned,uint32_t,uint64_t=0);
    bool prepareResources(IOService *);
    bool resetSensor();
    bool initializeSensor(IOService *);
    bool configureSensor(bool);
    bool sampleSensor(bool &,bool &);
    bool report(bool);
    void sample();
    void cleanup();
    void drainWorker();
    static void secureWorker(thread_call_param_t,thread_call_param_t);
    static IOReturn takeWork(OSObject *,void *,void *,void *,void *);
    static IOReturn completeWork(OSObject *,void *,void *,void *,void *);
    static IOReturn shutdown(OSObject *,void *,void *,void *,void *);
    static void irqAction(OSObject *,IOInterruptEventSource *,int);
    static void timerAction(OSObject *,IOTimerEventSource *);
    static void diagnosticRelease(OSObject *,IOTimerEventSource *);
    static IOReturn diagnosticPulse(OSObject *,void *,void *,void *,void *);
    static IOReturn resume(OSObject *,void *,void *,void *,void *);
    static IOReturn quiesce(OSObject *,void *,void *,void *,void *);
    static IOReturn snapshot(OSObject *,void *,void *,void *,void *);
protected:
    QcomHomeProtocol::Variant fVariant=QcomHomeProtocol::Variant::None;
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *,IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    bool handleStart(IOService *) APPLE_KEXT_OVERRIDE;
    void handleStop(IOService *) APPLE_KEXT_OVERRIDE;
    OSString *newTransportString() const APPLE_KEXT_OVERRIDE;
    OSString *newManufacturerString() const APPLE_KEXT_OVERRIDE;
    OSString *newProductString() const APPLE_KEXT_OVERRIDE;
    OSNumber *newPrimaryUsageNumber() const APPLE_KEXT_OVERRIDE;
    OSNumber *newPrimaryUsagePageNumber() const APPLE_KEXT_OVERRIDE;
    IOReturn newReportDescriptor(IOMemoryDescriptor **) const APPLE_KEXT_OVERRIDE;
    IOReturn getReport(IOMemoryDescriptor *,IOHIDReportType,IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long,IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
};
