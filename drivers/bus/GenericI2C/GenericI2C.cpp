// SPDX-License-Identifier: BSD-2-Clause
#include "GenericI2C.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <machine/machine_routines.h>

OSDefineMetaClassAndAbstractStructors(GenericI2CController, IOService);
OSDefineMetaClassAndStructors(GenericI2CDevice, IOService);

bool GenericI2CController::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fLock = IOLockAlloc();
    fDevices = OSArray::withCapacity(4);
    return fLock && fDevices;
}
bool GenericI2CDevice::configure(IORegistryEntry *entry, uint16_t address) {
    if (!init(entry, gIODTPlane)) return false;
    fAddress = address;
    return setProperty("I2CAddress", address, 32);
}
bool GenericI2CController::publishDevices(IOService *provider) {
    auto *children = provider->getChildIterator(gIODTPlane);
    if (!children) return false;
    bool addresses[128] = {};
    bool ok = true;
    IORegistryEntry *entry;
    while ((entry = OSDynamicCast(IORegistryEntry, children->getNextObject()))) {
        auto *status = OSDynamicCast(OSData, entry->getProperty("status"));
        if (entry->getProperty("status") && !status) { ok=false;break; }
        if (status && !((status->getLength() == 5 && !memcmp(status->getBytesNoCopy(), "okay", 5)) ||
                       (status->getLength() == 3 && !memcmp(status->getBytesNoCopy(), "ok", 3)))) continue;
        // Apple DT converts a child with #size-cells=0 to one LE64 address.
        auto *reg = OSDynamicCast(OSData, entry->getProperty("reg"));
        uint64_t address = 0;
        if (!reg || reg->getLength() != sizeof(address)) { ok = false; break; }
        memcpy(&address, reg->getBytesNoCopy(), sizeof(address));
        if (address < 8 || address > 0x77 || addresses[address]) { ok = false; break; }
        addresses[address] = true;
        auto *device = new GenericI2CDevice;
        ok = device && device->configure(entry, address) && fDevices->setObject(device);
        OSSafeReleaseNULL(device);
        if (!ok) break;
    }
    children->release();
    unsigned attached = 0;
    if (ok) for (; attached < fDevices->getCount(); ++attached)
        if (!static_cast<GenericI2CDevice *>(fDevices->getObject(attached))->attach(this)) { ok = false; break; }
    if (!ok) {
        for (unsigned i = 0; i < attached; ++i)
            static_cast<GenericI2CDevice *>(fDevices->getObject(i))->detach(this);
        fDevices->flushCollection();
        return false;
    }
    IOLockLock(fLock); fOnline = true; IOLockUnlock(fLock);
    for (unsigned i = 0; i < fDevices->getCount(); ++i)
        static_cast<GenericI2CDevice *>(fDevices->getObject(i))->registerService();
    registerService();
    return true;
}
IOReturn GenericI2CController::submit(GenericI2CDevice *device, I2C::Transaction &t) {
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    if (!fLock || !device || device->getProvider() != this || t.address != device->address() ||
        !I2C::valid(t)) return kIOReturnBadArgument;
    IOLockLock(fLock);
    auto result = fOnline ? execute(t) : I2C::Result::NotReady;
    IOLockUnlock(fLock);
    switch (result) {
    case I2C::Result::Success: return kIOReturnSuccess;
    case I2C::Result::Invalid: return kIOReturnBadArgument;
    case I2C::Result::Unsupported: return kIOReturnUnsupported;
    case I2C::Result::NotReady: return kIOReturnNotReady;
    case I2C::Result::Nack: return kIOReturnNoDevice;
    case I2C::Result::Timeout: return kIOReturnTimeout;
    default: return kIOReturnIOError;
    }
}
IOReturn GenericI2CDevice::transfer(IOService *client, I2C::Message *messages, unsigned count, uint32_t timeoutUS) {
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    if (!client || !isOpen(client)) return kIOReturnNotOpen;
    auto *controller = OSDynamicCast(GenericI2CController, getProvider());
    I2C::Transaction transaction{fAddress, messages, count, timeoutUS};
    return controller ? controller->submit(this, transaction) : kIOReturnNotAttached;
}
bool GenericI2CDevice::compareName(OSString *name, OSString **matched) const {
    return IODTCompareNubName(this, name, matched);
}
bool GenericI2CController::requestTerminate(IOService *, IOOptionBits) { return false; }
void GenericI2CController::quiesceBus() {
    if (fLock) { IOLockLock(fLock); fOnline = false; IOLockUnlock(fLock); }
}
void GenericI2CController::stop(IOService *provider) {
    // Clients must be able to restore their registers while stopping.
    if (fDevices) for (unsigned i = 0; i < fDevices->getCount(); ++i) {
        auto *device = static_cast<GenericI2CDevice *>(fDevices->getObject(i));
        device->terminate(kIOServiceSynchronous);
        if (device->getProvider()==this) device->detach(this);
    }
    quiesceBus();
    if (fDevices) fDevices->flushCollection();
    IOService::stop(provider);
}
void GenericI2CController::free() {
    OSSafeReleaseNULL(fDevices);
    if (fLock) { IOLockFree(fLock); fLock = nullptr; }
    IOService::free();
}
