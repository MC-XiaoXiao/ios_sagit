#ifndef _MI6_GENERIC_ARM_IO_H
#define _MI6_GENERIC_ARM_IO_H
#include <AppleARMPlatform/AppleARMIO.h>
#include <IOKit/IOPlatformExpert.h>
#include <ClockControllerProtocol.h>

class GenericARMIO : public AppleARMIO {
    OSDeclareDefaultStructors(GenericARMIO);
protected:
    virtual bool validateDevice(IORegistryEntry *entry);
    IOReturn clockRequest(ClockController::Request &request);
public:
    virtual UInt64 physicalAddressLimit(void) const;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *provider, IOOptionBits options) APPLE_KEXT_OVERRIDE;
    bool handlePlatformError(vm_offset_t far) APPLE_KEXT_OVERRIDE;
    const char *getClockName(UInt32 id, UInt32 *options, UInt32 count) APPLE_KEXT_OVERRIDE;
    UInt64 getClockFrequency(UInt32 id, UInt32 *options, UInt32 count) APPLE_KEXT_OVERRIDE;
    UInt64 getClockFrequency(const char *name, UInt32 *options, UInt32 count) APPLE_KEXT_OVERRIDE;
    UInt64 getClockFrequency(const OSSymbol *name, UInt32 *options, UInt32 count) APPLE_KEXT_OVERRIDE;
    IOReturn enableDeviceClock(UInt32 id, UInt32 enable) APPLE_KEXT_OVERRIDE;
    IOReturn enableDevicePower(UInt32 id, UInt32 state, UInt32 *result) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn enableDevicePinGroup(UInt32 *groups, UInt32 count) APPLE_KEXT_OVERRIDE;
    IOService *createDeviceWithProvider(AppleARMIO *bus, IORegistryEntry *entry) APPLE_KEXT_OVERRIDE;
    void processDevice(IOService *nub) APPLE_KEXT_OVERRIDE;
    const char *deleteList(void) APPLE_KEXT_OVERRIDE;
    const char *excludeList(void) APPLE_KEXT_OVERRIDE;
    bool compareNubName(const IOService *nub, OSString *name, OSString **matched = nullptr) const APPLE_KEXT_OVERRIDE;
    IOReturn getNubResources(IOService *nub) APPLE_KEXT_OVERRIDE;
    bool isDMACoherent(IOService *nub, UInt32 options) APPLE_KEXT_OVERRIDE;
    UInt64 getIODeviceClocks(UInt32 *ids, bool enable, UInt32 count) APPLE_KEXT_OVERRIDE;
};
#endif
