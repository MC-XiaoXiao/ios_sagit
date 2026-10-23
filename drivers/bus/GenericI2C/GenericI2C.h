// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <I2CProtocol.h>
class GenericI2CDevice;

// Like AppleARMIICController/AppleARMIICDevice, the controller publishes
// address-scoped nubs. This is our explicit API, not an Apple private ABI.
class GenericI2CController : public IOService {
    OSDeclareAbstractStructors(GenericI2CController);
    IOLock *fLock = nullptr;
    bool fOnline = false;
    OSArray *fDevices = nullptr;
protected:
    virtual I2C::Result execute(I2C::Transaction &) = 0;
    bool publishDevices(IOService *);
    void quiesceBus();
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn submit(GenericI2CDevice *, I2C::Transaction &);
};

class GenericI2CDevice : public IOService {
    OSDeclareDefaultStructors(GenericI2CDevice);
    uint16_t fAddress = 0;
public:
    bool configure(IORegistryEntry *, uint16_t);
    bool compareName(OSString *, OSString **) const APPLE_KEXT_OVERRIDE;
    uint16_t address() const { return fAddress; }
    IOReturn transfer(IOService *, I2C::Message *, unsigned, uint32_t timeoutUS = 20000);
};
