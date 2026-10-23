// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/usb/StandardUSB.h>
class IOMemoryDescriptor;
struct IOUSBHostCompletion;

// An opaque view of the actual Family-owned device, not a subclass header.
// The pinned helper allocates its real Native metaclass and full vtable. Only
// audited direct getters, internal device requests and IOService operations
// are exposed. No private Native fields are read by the HCD.
class IOUSBHostDevice final {
    alignas(UInt64) UInt8 nativeObject[336];
    IOUSBHostDevice()=delete;
    IOUSBHostDevice(const IOUSBHostDevice &)=delete;
    IOUSBHostDevice &operator=(const IOUSBHostDevice &)=delete;
    ~IOUSBHostDevice()=delete;
public:
    UInt16 getDeviceAddress() const;
    UInt8 getSpeed() const;
    // Native slot207, called directly through its audited implementation.
    // Client, setup, raw buffer, MD, byte count, completion, timeout(ms).
    IOReturn internalDeviceRequest(IOService *,StandardUSB::DeviceRequest &,void *,IOMemoryDescriptor *,
        UInt32 *,IOUSBHostCompletion *,UInt32);
    IOService *service() { return reinterpret_cast<IOService *>(this); }
};
static_assert(sizeof(IOUSBHostDevice)==336,"iOS18 Native device opaque view");
