// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <libkern/c++/OSObject.h>
#include <IOKit/IOTypes.h>
#include <IOKit/usb/IOUSBHostIsochronous.h>
class AppleUSBHostController;
class IOUSBHostDevice;

// iOS18 arm64 Family object. Unknown slots are dispatch reservations, not
// callable interfaces. The Family owns its device workloop, gates and timers.
class IOUSBHostIOSource : public OSObject {
    OSDeclareDefaultStructors(IOUSBHostIOSource);
    UInt8 nativeState[80];
public:
    void free() APPLE_KEXT_OVERRIDE; // 14
    virtual bool initWithOwners(AppleUSBHostController *,IOUSBHostDevice *); // 15
    virtual uintptr_t nativeSlot16(); // 16
    virtual uintptr_t nativeSlot17(); // 17
    virtual uintptr_t nativeSlot18(); // 18
    virtual uintptr_t nativeSlot19(); // 19
    virtual uintptr_t nativeSlot20(); // 20
    virtual uintptr_t nativeSlot21(); // 21
    virtual IOUSBHostDevice *getDevice() const; // 22
    virtual uintptr_t nativeSlot23(); // 23
    virtual uintptr_t nativeSlot24(); // 24
    virtual uintptr_t nativeSlot25(); // 25
    virtual uintptr_t nativeSlot26(); // 26
    virtual IOReturn getState(UInt32 *) const; // 27
    virtual uintptr_t nativeSlot28(); // 28
    virtual uintptr_t nativeSlot29(); // 29
    virtual uintptr_t nativeSlot30(); // 30
    virtual uintptr_t nativeSlot31(); // 31
    virtual uintptr_t nativeSlot32(); // 32
    virtual uintptr_t nativeSlot33(); // 33
    virtual uintptr_t nativeSlot34(); // 34
    virtual uintptr_t nativeSlot35(); // 35
    virtual uintptr_t nativeSlot36(); // 36
};
static_assert(sizeof(IOUSBHostIOSource)==96,"iOS18 IO source instance ABI");
