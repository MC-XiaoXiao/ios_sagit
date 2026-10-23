// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <libkern/c++/OSObject.h>
#include <IOKit/usb/IOUSBDeviceTypes.h>

// Version-locked opaque views of Family-owned objects, not subclass headers.
// All storage is reserved; constructors/destructors are unavailable and no
// virtual operations are declared. Only the audited nonvirtual calls below
// are legal. The actual objects retain their native metaclasses and vtables.
class IOUSBDevicePipe final {
    class MetaClass : public OSMetaClass { OSObject *alloc() const APPLE_KEXT_OVERRIDE; };
    alignas(UInt64) [[maybe_unused]] UInt8 opaque[120];
    IOUSBDevicePipe() = delete;
    IOUSBDevicePipe(const IOUSBDevicePipe &) = delete;
    IOUSBDevicePipe &operator=(const IOUSBDevicePipe &) = delete;
    ~IOUSBDevicePipe() = delete;
public:
    static const MetaClass gMetaClass;
    int getDirection(); // native 0=OUT, 1=IN
};
class IOUSBDeviceInterface final {
    class MetaClass : public OSMetaClass { OSObject *alloc() const APPLE_KEXT_OVERRIDE; };
    alignas(UInt64) [[maybe_unused]] UInt8 opaque[312];
    IOUSBDeviceInterface() = delete;
    IOUSBDeviceInterface(const IOUSBDeviceInterface &) = delete;
    IOUSBDeviceInterface &operator=(const IOUSBDeviceInterface &) = delete;
    ~IOUSBDeviceInterface() = delete;
public:
    static const MetaClass gMetaClass;
    int getAlternateSettingCount() const;
    int getPipeCountForAlternateSetting(UInt8);
    IOUSBDevicePipe *getPipeForAlternateSetting(int, UInt8);
    static IOUSBDeviceInterface *fromObject(OSObject *object) {
        return reinterpret_cast<IOUSBDeviceInterface *>(
            OSMetaClassBase::safeMetaCast(object, &gMetaClass));
    }
};
static_assert(sizeof(IOUSBDevicePipe) == 120, "native pipe opaque storage");
static_assert(sizeof(IOUSBDeviceInterface) == 312, "native interface opaque storage");
