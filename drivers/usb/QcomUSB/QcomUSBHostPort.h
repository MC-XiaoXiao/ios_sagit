// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/usb/AppleUSBHostPort.h>
class QcomUSBHost;

// Controller-owned Native Family port. No independent personality: native
// controller createPorts uses the same attach/start ownership model as Apple's
// HCDs. Native code keeps its own workloop, enumeration policy and device nub.
class QcomUSBHostPort : public AppleUSBHostPort {
    OSDeclareDefaultStructors(QcomUSBHostPort);
    QcomUSBHost *fController=nullptr;
    UInt32 fNumber=0;
    bool fOpened=false,fStarted=false,fSuper=false;
    UInt8 fPublication=0; // 0=construction, 1=Native PortsCreated, 2=stopped
public:
    static QcomUSBHostPort *withController(QcomUSBHost *,UInt32,bool super);
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn message(UInt32,IOService *,void *) APPLE_KEXT_OVERRIDE;
    void signalHardwareChange();
    UInt32 getPortType() APPLE_KEXT_OVERRIDE;
    IOReturn initBusCurrent() APPLE_KEXT_OVERRIDE;
    IOReturn getCompanionPortGated(UInt32,AppleUSBHostPort *&) APPLE_KEXT_OVERRIDE;
    IOReturn powerOff() APPLE_KEXT_OVERRIDE;
    IOReturn powerOn() APPLE_KEXT_OVERRIDE;
    IOReturn suspend() APPLE_KEXT_OVERRIDE;
    IOReturn resume() APPLE_KEXT_OVERRIDE;
    IOUSBHostDevice *resetAndCreateDevice(UInt32 milliseconds) APPLE_KEXT_OVERRIDE;
    IOReturn getPortStatusGated(UInt32 &) APPLE_KEXT_OVERRIDE;
};
static_assert(sizeof(QcomUSBHostPort)==936,"native port plus controller and index state");
