// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/usb/IOUSBDeviceController.h>
#include "DWC3Transfer.h"

// Native USB function controller. The role nub and all DMA/resource leases
// belong to QcomUSB; Apple Device Family owns configuration and callbacks.
class QcomUSBDevice : public IOUSBDeviceController {
    OSDeclareDefaultStructors(QcomUSBDevice);
    struct State;
    State *fState = nullptr;
    bool fFamilyStarted = false, fPinned = false, fPermanentPin = false;
    static void interrupt(OSObject *, class IOInterruptEventSource *, int);
    static void timer(OSObject *, class IOTimerEventSource *);
    static void worker(void *, void *);
    static void dmaWorker(void *, void *);
    void pin(bool recoverable=false);
    bool pinned(bool permanentOnly=false) const;
    void unpin();
    struct Completion {
        unsigned endpoint;
        UInt64 cookie;
        USB::Result result;
        UInt32 bytes;
    };
    IOReturn submitPrepared(unsigned physicalEndpoint, const DWC3::DMASegment *,
        unsigned count, UInt32 length, UInt64 cookie);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    void nativeSlot175(UInt32) APPLE_KEXT_OVERRIDE;
    IOReturn goOnBus(bool) APPLE_KEXT_OVERRIDE;
    bool connectedWithAccessoryCable() APPLE_KEXT_OVERRIDE;
    void handleStallEndpoint(int, bool) APPLE_KEXT_OVERRIDE;
    void handleAbortEndpoint(int) APPLE_KEXT_OVERRIDE;
    void handleSetAddressPreStatusPhase(int) APPLE_KEXT_OVERRIDE;
    void handleSetAddressPostStatusPhase(int) APPLE_KEXT_OVERRIDE;
    UInt32 getConnectionSpeed() APPLE_KEXT_OVERRIDE;
    UInt32 getMaxSpeedCapability() APPLE_KEXT_OVERRIDE;
    uintptr_t nativeSlot208() APPLE_KEXT_OVERRIDE;
    IOReturn createPipeGated(tCreatePipeParameters &, IOUSBDevicePipe *&) APPLE_KEXT_OVERRIDE;
    IOReturn provideEndpointIDsForConfiguration(OSArray *, UInt8 *) APPLE_KEXT_OVERRIDE;
    void prepareConfiguration(OSArray *) APPLE_KEXT_OVERRIDE;
    IOReturn activateEndpoint(int, int, int, int, tIOUSBDeviceInterfacePipeProperty) APPLE_KEXT_OVERRIDE;
    void deactivateEndpoint(int) APPLE_KEXT_OVERRIDE;
    IOBufferMemoryDescriptor *createIOBuffer(UInt32, UInt64) APPLE_KEXT_OVERRIDE;
    bool isEndpointStalled(int) APPLE_KEXT_OVERRIDE;
    void resetEndpointDataToggle(int) APPLE_KEXT_OVERRIDE;
    void nativeSlot199() APPLE_KEXT_OVERRIDE;
    IOReturn doRemoteWakeGated() APPLE_KEXT_OVERRIDE;
    IOReturn nativeSlot203(UInt32) APPLE_KEXT_OVERRIDE;
    IOReturn nativeSlot233(UInt32, UInt32) APPLE_KEXT_OVERRIDE;
    IOReturn ioGated(_IOUSBDeviceIORequestTag *) APPLE_KEXT_OVERRIDE;
    IOUSBDeviceControlRequest *createControlRequestTracker() APPLE_KEXT_OVERRIDE;
    void handleControlRequestStateChange(IOUSBDeviceControlRequest::tControlRequestState,
        IOUSBDeviceControlRequest::tControlRequestState) APPLE_KEXT_OVERRIDE;
};
