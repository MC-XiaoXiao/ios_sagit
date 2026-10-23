#ifndef _MI6_SM8150_H
#define _MI6_SM8150_H
#include <GenericARMPlatform.h>
#include <GenericARMIO.h>

class SM8150 : public GenericARMPlatform {
    OSDeclareDefaultStructors(SM8150);
protected:
    bool preparePlatform(IOService *provider) APPLE_KEXT_OVERRIDE;
};

class SM8150IO : public GenericARMIO {
    OSDeclareDefaultStructors(SM8150IO);
protected:
    bool validateDevice(IORegistryEntry *entry) APPLE_KEXT_OVERRIDE;
public:
    UInt64 physicalAddressLimit(void) const APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};
#endif
