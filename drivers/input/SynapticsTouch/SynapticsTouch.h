// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <GenericI2C.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include "TouchHID.h"

class SynapticsTouch : public IOHIDDevice {
    OSDeclareDefaultStructors(SynapticsTouch);
    GenericI2CDevice *fDevice = nullptr;
    struct Transport {
        GenericI2CDevice *device=nullptr;
        IOService *owner=nullptr;
        bool select(uint16_t);
        bool read(uint16_t,uint8_t *,size_t);
        bool write(uint16_t,const uint8_t *,size_t);
        void delayMS(unsigned);
    } fTransport;
    RMI4::Device<Transport> fSensor{fTransport};
    TouchHID::Transform fTransform;
    TouchHID::Delivery fDelivery;
    IOWorkLoop *fLoop=nullptr;
    IOCommandGate *fGate=nullptr;
    IOInterruptEventSource *fIRQ=nullptr;
    IOTimerEventSource *fTimer=nullptr;
    IOTimerEventSource *fDiagnosticTimer=nullptr;
    IOBufferMemoryDescriptor *fBuffer=nullptr;
    bool fOpen=false,fGateAdded=false,fIRQAdded=false,fTimerAdded=false,fPM=false;
    bool fRunning=false,fConfigured=false,fStopping=false,fHIDReady=false,fTrace=false;
    bool fDiagnosticEnabled=false,fDiagnosticActive=false,fDiagnosticTimerAdded=false;
    uint64_t fDiagnosticPulses=0,fDiagnosticReports=0;
    unsigned fErrors=0,fRecoveries=0,fLastContacts=0;
    uint64_t fInputReports=0;
    uint64_t fActiveFrames=0,fDownTransitions=0,fUpTransitions=0;
    uint16_t fMaxX=0,fMaxY=0;
    uint8_t fLast[TouchHID::ReportBytes]{};
    static void interrupt(OSObject *,IOInterruptEventSource *,int);
    static void timer(OSObject *,IOTimerEventSource *);
    static IOReturn power(OSObject *,void *,void *,void *,void *);
    static IOReturn snapshot(OSObject *,void *,void *,void *,void *);
    static IOReturn diagnosticTap(OSObject *,void *,void *,void *,void *);
    static void diagnosticRelease(OSObject *,IOTimerEventSource *);
    bool emit(const RMI4::Frame &);
    void sample();
    void cleanup();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool handleStart(IOService *) APPLE_KEXT_OVERRIDE;
    void handleStop(IOService *) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    OSString *newTransportString() const APPLE_KEXT_OVERRIDE;
    OSString *newManufacturerString() const APPLE_KEXT_OVERRIDE;
    OSString *newProductString() const APPLE_KEXT_OVERRIDE;
    OSNumber *newPrimaryUsageNumber() const APPLE_KEXT_OVERRIDE;
    OSNumber *newPrimaryUsagePageNumber() const APPLE_KEXT_OVERRIDE;
    IOReturn newReportDescriptor(IOMemoryDescriptor **) const APPLE_KEXT_OVERRIDE;
    IOReturn getReport(IOMemoryDescriptor *,IOHIDReportType,IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long,IOService *) APPLE_KEXT_OVERRIDE;
};
