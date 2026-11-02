// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBDeviceInterface.h>

// A Device Family function driver. It never accesses DWC3 or EP0 directly.
class QcomUSBConsole : public IOService {
    OSDeclareDefaultStructors(QcomUSBConsole);
    struct State;
    static State *sData;
    static void peerSessionChanged();
    State *fState = nullptr;
    bool fStarted = false;
    static void timer(OSObject *, class IOTimerEventSource *);
    static void completed(void *, void *, IOReturn, UInt32);
    static UInt32 descriptors(void *, UInt8 *, UInt32, UInt32, UInt32, UInt32, UInt32, void *);
    static bool control(OSObject *, const IOUSBDeviceFunctionSetup *, IOReturn *,
        IOMemoryDescriptor **, UInt64 *, IOUSBDeviceControlCompletion *);
    static bool controlGated(OSObject *, const IOUSBDeviceFunctionSetup *, IOReturn *,
        IOMemoryDescriptor **, UInt64 *, IOUSBDeviceControlCompletion *);
    static void controlCompleted(OSObject *, void *, IOReturn, UInt64, IOMemoryDescriptor *);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn message(UInt32, IOService *, void *) APPLE_KEXT_OVERRIDE;
};
