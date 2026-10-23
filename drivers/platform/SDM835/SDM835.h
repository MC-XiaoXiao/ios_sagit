#ifndef _MI6_SDM835_H
#define _MI6_SDM835_H
#include <GenericARMPlatform.h>
#include <GenericARMIO.h>

class SDM835 : public GenericARMPlatform {
    OSDeclareDefaultStructors(SDM835);
protected:
    bool preparePlatform(IOService *provider) APPLE_KEXT_OVERRIDE;
};

class SDM835IO : public GenericARMIO {
    OSDeclareDefaultStructors(SDM835IO);
protected:
    bool validateDevice(IORegistryEntry *entry) APPLE_KEXT_OVERRIDE;
public:
    UInt64 physicalAddressLimit(void) const APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};
#endif
