// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/usb/AppleUSBRequest.h>
class IOUSBHostDevice;
class IOUSBHostPipe;
class IOUSBHostIOSource;

class AppleUSBHostRequest : public AppleUSBRequest {
    OSDeclareDefaultStructors(AppleUSBHostRequest);
    UInt8 nativeState[8];
public:
    void free() APPLE_KEXT_OVERRIDE; // 14
    void setNextRequest(AppleUSBRequest *) APPLE_KEXT_OVERRIDE; // 16
    void complete() APPLE_KEXT_OVERRIDE; // 21
    void cancel() APPLE_KEXT_OVERRIDE; // 22
    virtual IOUSBHostDevice *getDevice() const; // 50
    virtual IOUSBHostPipe *getPipe() const; // 51
    virtual IOUSBHostIOSource *getIOSource() const; // 52
};
static_assert(sizeof(AppleUSBHostRequest)==64,"iOS18 host request instance ABI");
