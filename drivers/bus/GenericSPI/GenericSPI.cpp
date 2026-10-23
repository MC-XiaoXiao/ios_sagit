// SPDX-License-Identifier: BSD-2-Clause
#include "GenericSPI.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <machine/machine_routines.h>

OSDefineMetaClassAndAbstractStructors(GenericSPIController, IOService);
OSDefineMetaClassAndStructors(GenericSPIDevice, IOService);

bool GenericSPIController::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fLock = IOLockAlloc();
    fDevices = OSArray::withCapacity(4);
    return fLock && fDevices;
}
bool GenericSPIDevice::configure(IORegistryEntry *entry, const SPI::Configuration &config) {
    if (!SPI::valid(config) || !init(entry, gIODTPlane)) return false;
    fConfig = config;
    return setProperty("SPIChipSelect", config.chipSelect, 32) &&
           setProperty("SPIFrequency", config.frequency, 32) &&
           setProperty("SPIMode", config.mode, 32);
}
static bool word(IORegistryEntry *entry, const char *name, uint32_t &value) {
    auto *data = OSDynamicCast(OSData, entry->getProperty(name));
    if (!data || data->getLength() != sizeof(value)) return false;
    memcpy(&value, data->getBytesNoCopy(), sizeof(value));
    return true;
}
bool GenericSPIController::publishDevices(IOService *provider) {
    auto *children = provider->getChildIterator(gIODTPlane);
    if (!children) return false;
    bool used[4] = {}, ok = true;
    IORegistryEntry *entry;
    while ((entry = OSDynamicCast(IORegistryEntry, children->getNextObject()))) {
        auto *status = OSDynamicCast(OSData, entry->getProperty("status"));
        if (entry->getProperty("status") && !status) { ok = false; break; }
        if (status && !((status->getLength() == 5 && !memcmp(status->getBytesNoCopy(), "okay", 5)) ||
                        (status->getLength() == 3 && !memcmp(status->getBytesNoCopy(), "ok", 3)))) continue;
        // #size-cells=0 children carry a chip-select, converted to LE64 by the bridge.
        auto *reg = OSDynamicCast(OSData, entry->getProperty("reg"));
        uint64_t cs = 0;
        SPI::Configuration config;
        if (!reg || reg->getLength() != sizeof(cs)) { ok = false; break; }
        memcpy(&cs, reg->getBytesNoCopy(), sizeof(cs));
        if (cs >= 4 || used[cs] ||
            !word(entry, "spi-max-frequency", config.frequency) ||
            !word(entry, "spi-mode", config.mode)) { ok = false; break; }
        config.chipSelect = cs;
        if (!SPI::valid(config) || !accepts(config)) { ok = false; break; }
        used[cs] = true;
        auto *device = new GenericSPIDevice;
        ok = device && device->configure(entry, config) && fDevices->setObject(device);
        OSSafeReleaseNULL(device);
        if (!ok) break;
    }
    children->release();
    unsigned attached = 0;
    if (ok) for (; attached < fDevices->getCount(); ++attached)
        if (!static_cast<GenericSPIDevice *>(fDevices->getObject(attached))->attach(this)) { ok = false; break; }
    if (!ok) {
        for (unsigned i = 0; i < attached; ++i)
            static_cast<GenericSPIDevice *>(fDevices->getObject(i))->detach(this);
        fDevices->flushCollection();
        return false;
    }
    IOLockLock(fLock); fOnline = true; IOLockUnlock(fLock);
    for (unsigned i = 0; i < fDevices->getCount(); ++i)
        static_cast<GenericSPIDevice *>(fDevices->getObject(i))->registerService();
    registerService();
    return true;
}
IOReturn GenericSPIController::submit(GenericSPIDevice *device, SPI::Transaction &t) {
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    if (!fLock || !device || device->getProvider() != this) return kIOReturnBadArgument;
    // No caller may redirect a request to another chip-select or retune the bus.
    const auto &c = device->configuration();
    if (t.config.chipSelect != c.chipSelect || t.config.frequency != c.frequency ||
        t.config.mode != c.mode || !SPI::valid(t)) return kIOReturnBadArgument;
    IOLockLock(fLock);
    const auto result = fOnline ? execute(t) : SPI::Result::NotReady;
    IOLockUnlock(fLock);
    switch (result) {
    case SPI::Result::Success: return kIOReturnSuccess;
    case SPI::Result::Invalid: return kIOReturnBadArgument;
    case SPI::Result::Unsupported: return kIOReturnUnsupported;
    case SPI::Result::NotReady: return kIOReturnNotReady;
    case SPI::Result::Timeout: return kIOReturnTimeout;
    default: return kIOReturnIOError;
    }
}
IOReturn GenericSPIDevice::transfer(IOService *client, const uint8_t *tx, uint8_t *rx,
                                   size_t length, uint32_t timeoutUS) {
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    if (!client || !isOpen(client)) return kIOReturnNotOpen;
    auto *controller = OSDynamicCast(GenericSPIController, getProvider());
    SPI::Transaction t{fConfig, tx, rx, length, timeoutUS};
    return controller ? controller->submit(this, t) : kIOReturnNotAttached;
}
bool GenericSPIDevice::compareName(OSString *name, OSString **matched) const {
    return IODTCompareNubName(this, name, matched);
}
bool GenericSPIController::requestTerminate(IOService *, IOOptionBits) { return false; }
void GenericSPIController::stop(IOService *provider) {
    // Keep the bus usable until clients have restored their device registers.
    if (fDevices) for (unsigned i = 0; i < fDevices->getCount(); ++i) {
        auto *device = static_cast<GenericSPIDevice *>(fDevices->getObject(i));
        device->terminate(kIOServiceSynchronous);
        if (device->getProvider() == this) device->detach(this);
    }
    if (fLock) { IOLockLock(fLock); fOnline = false; IOLockUnlock(fLock); }
    if (fDevices) fDevices->flushCollection();
    IOService::stop(provider);
}
void GenericSPIController::free() {
    OSSafeReleaseNULL(fDevices);
    if (fLock) { IOLockFree(fLock); fLock = nullptr; }
    IOService::free();
}
