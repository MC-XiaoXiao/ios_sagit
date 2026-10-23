// SPDX-License-Identifier: BSD-2-Clause
// Minimal declarations for the iOS 18 arm64 provider. Instance/virtual layout
// and each imported symbol are pinned by apple-hid-abi.json. Public API names
// follow Apple's IOHIDFamily/IOHIDDevice.h; opaque slots keep native behavior.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
enum IOHIDReportType { kIOHIDReportTypeInput=0, kIOHIDReportTypeOutput=1, kIOHIDReportTypeFeature=2 };
class IOHIDDevice : public IOService {
    OSDeclareDefaultStructors(IOHIDDevice);
    alignas(8) UInt8 fNativeState[72];
public:
    using IOService::setProperty;
    kern_return_t Dispatch(const IORPC) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool init(OSDictionary * = nullptr) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    bool handleOpen(IOService *, IOOptionBits, void *) APPLE_KEXT_OVERRIDE;
    void handleClose(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    bool handleIsOpen(const IOService *) const APPLE_KEXT_OVERRIDE;
    bool willTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    bool matchPropertyTable(OSDictionary *, SInt32 *) APPLE_KEXT_OVERRIDE;
    IOReturn message(UInt32, IOService *, void * = nullptr) APPLE_KEXT_OVERRIDE;
    bool setProperty(const OSSymbol *, OSObject *) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
    IOReturn configureReport(IOReportChannelList *, IOReportConfigureAction, void *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn updateReport(IOReportChannelList *, IOReportUpdateAction, void *, void *) APPLE_KEXT_OVERRIDE;
    virtual bool handleStart(IOService *);
    virtual void handleStop(IOService *);
    virtual bool publishProperties(IOService *);
    virtual OSString *newTransportString() const;
    virtual OSString *newManufacturerString() const;
    virtual OSString *newProductString() const;
    virtual OSNumber *newVendorIDNumber() const;
    virtual OSNumber *newProductIDNumber() const;
    virtual OSNumber *newVersionNumber() const;
    virtual OSNumber *newSerialNumber() const;
    virtual OSNumber *newPrimaryUsageNumber() const;
    virtual OSNumber *newPrimaryUsagePageNumber() const;
    virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const = 0;
    virtual IOReturn handleReport(IOMemoryDescriptor *, IOHIDReportType = kIOHIDReportTypeInput, IOOptionBits = 0);
    virtual IOReturn getReport(IOMemoryDescriptor *, IOHIDReportType, IOOptionBits);
    virtual IOReturn setReport(IOMemoryDescriptor *, IOHIDReportType, IOOptionBits = 0);
    virtual IOMemoryDescriptor *getMemoryWithCurrentElementValues() const;
    virtual void nativeSlot185();
    virtual void nativeSlot186();
    virtual void nativeSlot187();
    virtual void nativeSlot188();
    virtual void nativeSlot189();
    virtual void nativeSlot190();
    virtual void nativeSlot191();
    virtual void nativeSlot192();
    virtual void nativeSlot193();
    virtual void nativeSlot194();
    virtual void nativeSlot195();
    virtual void nativeSlot196();
    virtual void nativeSlot197();
    virtual void nativeSlot198();
    virtual void nativeSlot199();
    virtual void nativeSlot200();
    virtual void nativeSlot201();
    virtual void nativeSlot202();
    virtual void nativeSlot203();
    virtual void nativeSlot204();
    virtual void nativeSlot205();
    virtual void nativeSlot206();
};
static_assert(sizeof(IOHIDDevice)==208, "iOS 18 arm64 HID device ABI");
