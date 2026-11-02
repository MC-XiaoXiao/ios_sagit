// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOTypes.h>
#include <libkern/c++/OSObject.h>
class IOUSBDeviceController;

class IOUSBDeviceControlRequest : public OSObject {
    OSDeclareDefaultStructors(IOUSBDeviceControlRequest);
protected:
    // OSObject has tail padding that this ABI uses for its state at +12.
    // Start the opaque reservation on an 8-byte boundary so a future subclass
    // cannot reuse our trailing bytes while native code stores its +152
    // work-loop pointer. The entire 160-byte native allocation stays reserved.
    alignas(UInt64) UInt8 nativeState[144];
public:
    enum tControlRequestState : UInt32;
    void free() APPLE_KEXT_OVERRIDE;
    virtual bool initWithControllerOptions(IOUSBDeviceController *, int inEndpoint, int outEndpoint, UInt32);
    virtual uintptr_t nativeSlot16();
    virtual uintptr_t nativeSlot17();
    virtual void stop();

    static IOUSBDeviceControlRequest *withControllerOptions(IOUSBDeviceController *, int, int, UInt32);
    void inputSetupPacket(UInt8 *);
    IOReturn processSetupPacket();
    void inputDataPacket(UInt8 *, UInt64);
    void reset();
};
static_assert(sizeof(IOUSBDeviceControlRequest) == 160, "iOS 18 control-request instance ABI");
