#ifndef _MI6_GENERIC_ARM_PLATFORM_H
#define _MI6_GENERIC_ARM_PLATFORM_H
#include <AppleARMPlatform/AppleARMPE.h>
#include "ARMPlatformConfig.h"

class GenericARMPlatform : public AppleARMPE {
    OSDeclareDefaultStructors(GenericARMPlatform);
    ARMPlatformConfig::PSCIConduit fConduit;

protected:
    virtual bool preparePlatform(IOService *provider);

public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    bool configure(IOService *provider) APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *provider, IOOptionBits options) APPLE_KEXT_OVERRIDE;
    IOService *createNub(IORegistryEntry *entry) APPLE_KEXT_OVERRIDE;
    const char *deleteList(void) APPLE_KEXT_OVERRIDE;
    const char *excludeList(void) APPLE_KEXT_OVERRIDE;
    bool platformAdjustService(IOService *service) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *name, bool wait,
        void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;
    OSString *createSystemSerialNumberString(OSData *data) APPLE_KEXT_OVERRIDE;
    void getUTCTimeOfDay(clock_sec_t *secs, clock_nsec_t *nsecs) APPLE_KEXT_OVERRIDE;
    void setUTCTimeOfDay(clock_sec_t secs, clock_nsec_t nsecs) APPLE_KEXT_OVERRIDE;
    void PMInstantiatePowerDomains(void) APPLE_KEXT_OVERRIDE;
    void PMRegisterDevice(IOService *nub, IOService *device) APPLE_KEXT_OVERRIDE;
    int haltRestart(unsigned int type) APPLE_KEXT_OVERRIDE;
};
#endif
