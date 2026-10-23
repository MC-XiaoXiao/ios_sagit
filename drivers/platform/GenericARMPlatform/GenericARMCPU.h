#ifndef _GENERIC_ARM_CPU_H
#define _GENERIC_ARM_CPU_H
#include <IOKit/IOCPU.h>

// Modern XNU manages CPU_ON, CPU_OFF, timer and IPI in its machine layer.
// This IOKit service supplies the existing CPU topology to that layer.
class GenericARMCPU : public IOCPU {
    OSDeclareDefaultStructors(GenericARMCPU);
    const OSSymbol *fCPUName;
    IOCPUInterruptController *fInterruptController;
    bool fInterruptEnabled;
    uint32_t fPhysicalID;
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free(void) APPLE_KEXT_OVERRIDE;
    void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
    void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
    kern_return_t startCPU(vm_offset_t start, vm_offset_t arg) APPLE_KEXT_OVERRIDE;
    void haltCPU(void) APPLE_KEXT_OVERRIDE;
    void signalCPU(IOCPU *target) APPLE_KEXT_OVERRIDE;
    const OSSymbol *getCPUName(void) APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *provider, IOOptionBits options) APPLE_KEXT_OVERRIDE;
};
#endif
