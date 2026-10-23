/* Subclass-only ABI declaration; implementation belongs to Apple's Kext.
 * Pinned to the arm64 AppleARMPlatform 1.0.2 in the project's ipad7c cache.
 * See drivers/README.md before changing the provider or XNU headers.
 */
#ifndef _MI6_APPLE_ARM_PE_H
#define _MI6_APPLE_ARM_PE_H

#include <IOKit/IOPlatformExpert.h>

class AppleARMPE : public IODTPlatformExpert {
    OSDeclareDefaultStructors(AppleARMPE);

private:
    // The native class adds 56 bytes and no virtual slots to IODTPlatformExpert.
    // These bytes are owned exclusively by the native constructors/destructors.
    UInt8 applePrivateState[56];

public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
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
};

static_assert(sizeof(IOService) == 136, "IOService ABI mismatch");
static_assert(sizeof(IODTPlatformExpert) == 240, "IODTPlatformExpert ABI mismatch");
static_assert(sizeof(AppleARMPE) == 296, "AppleARMPE ABI mismatch");

#endif
