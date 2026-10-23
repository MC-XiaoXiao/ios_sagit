/* Subclass ABI for the pinned iOS 18 arm64 AppleARMPlatform provider.
 * This declares the native class; it does not replace its metaclass or Kext.
 */
#ifndef _MI6_APPLE_ARM_IO_H
#define _MI6_APPLE_ARM_IO_H
#include <IOKit/platform/IOPlatformIO.h>
class AppleARMFunction;

class AppleARMIO : public IOPlatformIO {
    OSDeclareAbstractStructors(AppleARMIO);
    UInt8 applePrivateState[80];
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    bool handlePlatformError(vm_offset_t far) APPLE_KEXT_OVERRIDE;
    // Order is part of the private ABI: slots 169..185 in the pinned cache.
    virtual const char *getClockName(UInt32 id, UInt32 *options, UInt32 count);
    virtual UInt64 getClockFrequency(UInt32 id, UInt32 *options, UInt32 count);
    virtual UInt64 getClockFrequency(const char *name, UInt32 *options, UInt32 count);
    virtual UInt64 getClockFrequency(const OSSymbol *name, UInt32 *options, UInt32 count);
    virtual IOReturn enableDeviceClock(UInt32 id, UInt32 enable);
    virtual IOReturn enableDevicePower(UInt32 id, UInt32 state, UInt32 *result);
    virtual IOReturn enableDevicePinGroup(UInt32 *groups, UInt32 count);
    virtual IOService *createDeviceWithProvider(AppleARMIO *bus, IORegistryEntry *entry);
    virtual void processDevice(IOService *nub);
    virtual void publishBelow(IORegistryEntry *root);
    virtual const char *deleteList(void);
    virtual const char *excludeList(void);
    virtual bool compareNubName(const IOService *nub, OSString *name, OSString **matched = nullptr) const;
    virtual IOReturn getNubResources(IOService *nub);
    virtual bool isDMACoherent(IOService *nub, UInt32 options);
    virtual void registerAltErrorHandler(AppleARMFunction *handler);
    virtual UInt64 getIODeviceClocks(UInt32 *ids, bool enable, UInt32 count);
};
static_assert(sizeof(IOPlatformIO) == 136, "IOPlatformIO ABI mismatch");
static_assert(sizeof(AppleARMIO) == 216, "AppleARMIO ABI mismatch");
#endif
