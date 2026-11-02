// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <libkern/c++/OSObject.h>
#include <IOKit/usb/IOUSBDeviceTypes.h>
class IOService;
struct IOUSBDeviceFunctionSetup { UInt8 type, request; UInt16 value, index, length; };
struct IOUSBDeviceControlCompletion {
    OSObject *target;
    void (*action)(OSObject *, void *, IOReturn, UInt64, IOMemoryDescriptor *);
    void *parameter;
};
static_assert(sizeof(IOUSBDeviceFunctionSetup) == 8, "native SETUP packet ABI");
static_assert(sizeof(IOUSBDeviceControlCompletion) == 24, "native control completion ABI");
using IOUSBDeviceControlHandler = bool (*)(OSObject *, const IOUSBDeviceFunctionSetup *,
    IOReturn *, IOMemoryDescriptor **, UInt64 *, IOUSBDeviceControlCompletion *);
using IOUSBDeviceDescriptorHandler = UInt32 (*)(void *, UInt8 *, UInt32, UInt32,
    UInt32, UInt32, UInt32, void *);

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
    // Native slot-16 dispatcher; asynchronous regular I/O, both directions.
    IOReturn nativeIO(IOMemoryDescriptor *, UInt64, IOUSBDeviceCompletion *, UInt64 *, UInt32);
    // Audited slot-25 abort body. Caller must hold the controller work-loop
    // gate. Slot26 resets DATA toggles and must never substitute for abort.
    IOReturn nativeAbortGated();
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
    int nativeAddAlternateSetting(int, int, int);
    IOUSBDevicePipe *nativeCreatePipe(int, int, int, int, int, int);
    IOReturn nativeSetDescription(const char *);
    IOReturn nativeFinalize();
    IOReturn nativeSetDescriptorHandler(IOUSBDeviceDescriptorHandler, void *);
    IOReturn nativeSetControlHandler(OSObject *, IOUSBDeviceControlHandler);
    IOReturn nativeClearControlHandler(OSObject *);
    IOService *service() { return reinterpret_cast<IOService *>(this); }
    static IOUSBDeviceInterface *fromObject(OSObject *object) {
        return reinterpret_cast<IOUSBDeviceInterface *>(
            OSMetaClassBase::safeMetaCast(object, &gMetaClass));
    }
};
static_assert(sizeof(IOUSBDevicePipe) == 120, "native pipe opaque storage");
static_assert(sizeof(IOUSBDeviceInterface) == 312, "native interface opaque storage");
