// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <SPIProtocol.h>
class GenericSPIDevice;

// Local transport API. This does not claim the private AppleSPIMC ABI.
class GenericSPIController : public IOService {
    OSDeclareAbstractStructors(GenericSPIController);
    IOLock *fLock = nullptr;
    OSArray *fDevices = nullptr;
    bool fOnline = false;
protected:
    virtual SPI::Result execute(SPI::Transaction &) = 0;
    virtual bool accepts(const SPI::Configuration &) const = 0;
    bool publishDevices(IOService *);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn submit(GenericSPIDevice *, SPI::Transaction &);
};

class GenericSPIDevice : public IOService {
    OSDeclareDefaultStructors(GenericSPIDevice);
    SPI::Configuration fConfig;
public:
    bool configure(IORegistryEntry *, const SPI::Configuration &);
    bool compareName(OSString *, OSString **) const APPLE_KEXT_OVERRIDE;
    const SPI::Configuration &configuration() const { return fConfig; }
    IOReturn transfer(IOService *, const uint8_t *, uint8_t *, size_t, uint32_t timeoutUS = 2000);
};
