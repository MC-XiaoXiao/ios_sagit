// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/usb/IOUSBHostIOSource.h>
#include <IOKit/usb/StandardUSB.h>
class IOUSBHostInterface;

class IOUSBHostPipe : public IOUSBHostIOSource {
    OSDeclareDefaultStructors(IOUSBHostPipe);
    // Native speed occupies inherited tail padding at 92. Treat both classes'
    // storage as opaque; appending a guessed C++ field there would move it.
    UInt8 nativePipeState[88];
public:
    struct StandardUSBDescriptors {
        UInt16 bcdUSB;
        const StandardUSB::EndpointDescriptor *descriptor;
        const StandardUSB::SuperSpeedEndpointCompanionDescriptor *ssCompanionDescriptor;
        const StandardUSB::SuperSpeedPlusIsochronousEndpointCompanionDescriptor *sspCompanionDescriptor;
    };
    enum tGetEndpointDescriptorOptions : UInt32 {
        kGetEndpointDescriptorOriginal=0, kGetEndpointDescriptorCurrentPolicy=1
    };
    void free() APPLE_KEXT_OVERRIDE; // 14
    uintptr_t nativeSlot17() APPLE_KEXT_OVERRIDE;
    uintptr_t nativeSlot19() APPLE_KEXT_OVERRIDE;
    uintptr_t nativeSlot21() APPLE_KEXT_OVERRIDE;
    uintptr_t nativeSlot23() APPLE_KEXT_OVERRIDE;
    uintptr_t nativeSlot24() APPLE_KEXT_OVERRIDE;
    uintptr_t nativeSlot29() APPLE_KEXT_OVERRIDE;
    uintptr_t nativeSlot30() APPLE_KEXT_OVERRIDE;
    uintptr_t nativeSlot32() APPLE_KEXT_OVERRIDE;
    uintptr_t nativeSlot33() APPLE_KEXT_OVERRIDE;
    uintptr_t nativeSlot34() APPLE_KEXT_OVERRIDE;
    virtual bool initWithDescriptorsAndOwners(const StandardUSB::EndpointDescriptor *,
        const StandardUSB::SuperSpeedEndpointCompanionDescriptor *,AppleUSBHostController *,
        IOUSBHostDevice *,IOUSBHostInterface *,UInt8,UInt16); // 37
    virtual const StandardUSB::EndpointDescriptor *getEndpointDescriptor(
        tGetEndpointDescriptorOptions=kGetEndpointDescriptorCurrentPolicy); // 38
    virtual const StandardUSB::SuperSpeedEndpointCompanionDescriptor *getSuperSpeedEndpointCompanionDescriptor(
        tGetEndpointDescriptorOptions=kGetEndpointDescriptorCurrentPolicy); // 39
    virtual IOReturn adjustPipe(const StandardUSB::EndpointDescriptor *,
        const StandardUSB::SuperSpeedEndpointCompanionDescriptor *); // 40
    virtual uintptr_t nativeSlot41(); // 41
    virtual uintptr_t nativeSlot42(); // 42
    virtual IOReturn clearStall(bool withRequest); // 43
    virtual uintptr_t nativeSlot44(); // 44
    virtual IOReturn getDescriptors(StandardUSBDescriptors *,
        tGetEndpointDescriptorOptions=kGetEndpointDescriptorCurrentPolicy); // 45
    virtual IOReturn adjustPipeGated(const StandardUSB::EndpointDescriptor *,
        const StandardUSB::SuperSpeedEndpointCompanionDescriptor *); // 46
    virtual uintptr_t nativeSlot47(); // 47
    virtual uintptr_t nativeSlot48(); // 48
    virtual IOReturn clearStallGated(bool withRequest); // 49
    virtual IOReturn adjustPipeGatedV2(StandardUSBDescriptors *); // 50
    virtual uintptr_t nativeSlot51(); // 51
    virtual uintptr_t nativeSlot52(); // 52
    virtual uintptr_t nativeSlot53(); // 53
    virtual uintptr_t nativeSlot54(); // 54
    virtual uintptr_t nativeSlot55(); // 55
    virtual uintptr_t nativeSlot56(); // 56
    virtual uintptr_t nativeSlot57(); // 57
    virtual uintptr_t nativeSlot58(); // 58
    virtual uintptr_t nativeSlot59(); // 59
    virtual uintptr_t nativeSlot60(); // 60
    virtual uintptr_t nativeSlot61(); // 61
    virtual uintptr_t nativeSlot62(); // 62
    virtual uintptr_t nativeSlot63(); // 63
    virtual uintptr_t nativeSlot64(); // 64
    virtual UInt8 getSpeed() const; // 65
    virtual UInt16 getDeviceAddress() const; // 66
};
static_assert(sizeof(IOUSBHostPipe)==184,"iOS18 pipe instance ABI");
static_assert(sizeof(IOUSBHostPipe::StandardUSBDescriptors)==32,"native descriptor collection ABI");
static_assert(__builtin_offsetof(IOUSBHostPipe::StandardUSBDescriptors,descriptor)==8,"native endpoint pointer");
static_assert(__builtin_offsetof(IOUSBHostPipe::StandardUSBDescriptors,ssCompanionDescriptor)==16,"native SS pointer");
static_assert(__builtin_offsetof(IOUSBHostPipe::StandardUSBDescriptors,sspCompanionDescriptor)==24,"native SSP pointer");
