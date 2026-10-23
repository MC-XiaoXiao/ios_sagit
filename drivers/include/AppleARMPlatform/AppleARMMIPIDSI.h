/* Subclass ABI for the pinned iOS 18 arm64 AppleARMPlatform provider. */
#ifndef _APPLE_ARM_MIPI_DSI_H
#define _APPLE_ARM_MIPI_DSI_H
#include <IOKit/IOService.h>

class AppleARMMIPIDSIController : public IOService {
    OSDeclareAbstractStructors(AppleARMMIPIDSIController);
    UInt8 applePrivateState[8];
public:
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
    // iOS 18 slots 168..180; AppleARMMIPIDSIDevice forwards to these slots.
    virtual IOReturn enableInterface(bool) = 0;
    virtual IOReturn enableHighSpeedMode(bool) = 0;
    virtual IOReturn enableVideo(bool) = 0;
    virtual IOReturn sendShortCommand(UInt8, UInt8, UInt8) = 0;
    virtual IOReturn sendLongCommand(UInt8, const UInt8 *, UInt64) = 0;
    virtual IOReturn receiveShortCommand(UInt8, UInt8 *) = 0;
    virtual IOReturn receiveLongCommand(UInt8, UInt8 *, UInt64 *) = 0;
protected:
    // Unnamed optional hooks in this stripped image. Do not call these from
    // C++; their native signatures have not been established.
    virtual IOReturn reservedSlot175();
    virtual UInt32 nativeSlot176();
    virtual IOReturn nativeSlot177();
public:
    virtual IOService *newDeviceWithRegistryEntry(IORegistryEntry *, IOService *);
    virtual void publishBelow(IORegistryEntry *);
    virtual bool compareNubName(const IOService *, OSString *, OSString **) const;
};
static_assert(sizeof(IOService) == 136, "IOService ABI mismatch");
static_assert(sizeof(AppleARMMIPIDSIController) == 144, "iOS 18 MIPI ABI mismatch");
class AppleARMMIPIDSIDevice : public IOService {
    OSDeclareAbstractStructors(AppleARMMIPIDSIDevice);
    UInt8 nativeController[8];
public:
    virtual bool initDeviceWithRegistryEntry(IORegistryEntry *, IOService *); // 168
    virtual IOReturn enableInterface(bool); // 169
    virtual IOReturn enableHighSpeedMode(bool); // 170
    virtual IOReturn enableVideo(bool); // 171
    virtual IOReturn sendShortCommand(UInt8, UInt8, UInt8); // 172
    virtual IOReturn sendLongCommand(UInt8, const UInt8 *, UInt64); // 173
    virtual IOReturn receiveShortCommand(UInt8, UInt8 *); // 174
    virtual IOReturn receiveLongCommand(UInt8, UInt8 *, UInt64 *); // 175
protected:
    virtual void nativeSlot176();
};
static_assert(sizeof(AppleARMMIPIDSIDevice) == 144, "iOS 18 MIPI device ABI mismatch");
#endif
