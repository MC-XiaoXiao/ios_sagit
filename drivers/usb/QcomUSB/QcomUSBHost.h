// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/usb/AppleUSBHostController.h>
#include <IOKit/IOLocks.h>
#include "XHCICommands.h"
#include "XHCIFrames.h"
class QcomUSBController;
class QcomUSBHostPort;

// Native Host Family adapter. Hardware ownership belongs to the generation-
// scoped QcomUSBController provider. Bulk streams are an optional capability;
// this adapter uses ordinary bulk rings and explicitly rejects stream setup.
// Its role nub exists only after the platform has acquired real resources.
class QcomUSBHost : public AppleUSBHostController {
    OSDeclareDefaultStructors(QcomUSBHost);
    struct State;
    State *fState=nullptr;
    // Published once before entering Native start; retained until free.
    // getRawMicroFrameCount can run concurrently with stop and never reads
    // the mutable State pointer or waits for a workloop gate.
    QcomUSBController *fFrameProvider=nullptr;
    IOSimpleLock *fFrameUpdateLock=nullptr;
    XHCI::FrameEvents fFrameEvents;
    UInt32 fLastMicroFrame=0;
    bool fFrameReadFailed=false;
    USB::Result recordFrameRollover(UInt32); // caller holds fFrameUpdateLock with IRQs disabled
    USB::Result observeFrameEvents();
    bool beginFrameEvents(XHCI::TRB *,unsigned,UInt64);
    void endFrameEvents();
    bool observedFrameRollover(UInt64);
    friend class QcomUSBHostPort;
    enum class PortOperation { PowerOn,PowerOff,Suspend,Resume,Reset };
    IOReturn portOperation(UInt32,PortOperation);
    IOReturn portStatus(UInt32,UInt32 &);
    void signalPort(unsigned);
    IOReturn resolveCompanion(QcomUSBHostPort *,UInt32,AppleUSBHostPort *&);
    bool fFamilyStarted=false, fPinned=false;
    bool releaseHardware();
    void pin();
    static bool interruptFilter(OSObject *,IOFilterInterruptEventSource *);
    static void interruptAction(OSObject *,IOInterruptEventSource *,int);
    static void timerAction(OSObject *,class IOTimerEventSource *);
    static void releaseWorker(void *,void *);
    static void deviceWorker(void *,void *);
    static void requestWorker(void *,void *);
    static void commandReleaseWorker(void *,void *);
    static void ttWorker(void *,void *);
    static void closeWorker(void *,void *);
protected:
    // Called while holding the Native controller gate (not its IRQ thread).
    // Caller owns a prepared payload throughout the call. A timeout keeps the
    // queue's retain until controller isolation; the caller must not free DMA
    // from an unsuccessful command as if that were a hardware completion.
    IOReturn executeCommand(const XHCI::Command &,OSObject *,XHCI::CommandRecord &,UInt32 timeoutUS=1000000);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn powerDown(unsigned long) APPLE_KEXT_OVERRIDE;
    IOReturn powerUp(unsigned long) APPLE_KEXT_OVERRIDE;
    IODMACommand *getDMACommand(UInt32 alignment,UInt32 options) APPLE_KEXT_OVERRIDE;
    IOReturn createPorts() APPLE_KEXT_OVERRIDE;
    AppleUSBHostPort *getCompanionPort(AppleUSBHostPort *,UInt32 speed) APPLE_KEXT_OVERRIDE;
    IOUSBHostDevice *createDevice(UInt32 speed,UInt32 location,UInt32 options) APPLE_KEXT_OVERRIDE;
    void nativeSlot193(IOUSBHostDevice *) APPLE_KEXT_OVERRIDE;
    IOReturn registerHubGated(IOUSBHostDevice *,const StandardUSB::HubDescriptor *,tRegisterHubFlags) APPLE_KEXT_OVERRIDE;
    IOReturn registerSuperSpeedHubGated(IOUSBHostDevice *,const StandardUSB::SuperSpeedHubDescriptor *) APPLE_KEXT_OVERRIDE;
    IOReturn unregisterHubGated(IOUSBHostDevice *) APPLE_KEXT_OVERRIDE;
    IOReturn createPipeGated(IOUSBHostPipe::StandardUSBDescriptors &,IOUSBHostDevice *,IOUSBHostInterface *,IOUSBHostPipe *&) APPLE_KEXT_OVERRIDE;
    IOReturn adjustPipeGated(IOUSBHostPipe *,IOUSBHostPipe::StandardUSBDescriptors &) APPLE_KEXT_OVERRIDE;
    IOReturn closePipeGated(IOUSBHostPipe *) APPLE_KEXT_OVERRIDE;
    IOReturn ioGated(AppleUSBHostRequest *) APPLE_KEXT_OVERRIDE;
    IOReturn abortPipeGated(IOUSBHostPipe *,IOReturn,IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn clearPipeStallGated(IOUSBHostPipe *) APPLE_KEXT_OVERRIDE;
    IOReturn suspendPipesGated(IOUSBHostDevice *) APPLE_KEXT_OVERRIDE;
    IOReturn resumePipesGated(IOUSBHostDevice *) APPLE_KEXT_OVERRIDE;
    IOReturn enableStreamsGated(IOUSBHostPipe *) APPLE_KEXT_OVERRIDE;
    IOReturn disableStreamsGated(IOUSBHostPipe *) APPLE_KEXT_OVERRIDE;
    IOReturn createStreamGated(IOUSBHostDevice *,IOUSBHostPipe *,UInt32,IOUSBHostStream *&) APPLE_KEXT_OVERRIDE;
    IOReturn abortStreamGated(IOUSBHostStream *,IOReturn) APPLE_KEXT_OVERRIDE;
    IOReturn closeStreamGated(IOUSBHostStream *) APPLE_KEXT_OVERRIDE;
    bool filterOccurred(IOFilterInterruptEventSource *) APPLE_KEXT_OVERRIDE;
    void interruptOccurred(IOInterruptEventSource *,int) APPLE_KEXT_OVERRIDE;
    UInt64 getRawMicroFrameCount() APPLE_KEXT_OVERRIDE;
    UInt64 makeFrameNumber(UInt64,UInt64) const APPLE_KEXT_OVERRIDE;
    UInt64 getFrameNumber(UInt64 * = nullptr) APPLE_KEXT_OVERRIDE;
    UInt64 getFrameRollover(UInt64 * = nullptr) APPLE_KEXT_OVERRIDE;
};
