#include "GenericARMIO.h"
#include "ARMPlatformConfig.h"
#include "ARMDeviceTree.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <machine/machine_routines.h>

#define super AppleARMIO
OSDefineMetaClassAndStructors(GenericARMIO, super);

bool GenericARMIO::validateDevice(IORegistryEntry *entry)
{
    // Native AppleARMIODevice interprets these arrays as Apple's gate/pin
    // protocols. They are not Qualcomm clocks or Linux phandle tuples.
    const char *keys[] = {"clock-ids", "clock-gates", "power-gates", "pin-groups"};
    for (const char *key : keys)
        if (entry->getProperty(key)) return false;
    OSObject *property = entry->getProperty("reg");
    if (!property) return true; // Logical services need not have MMIO.
    OSData *reg = OSDynamicCast(OSData, property);
    return reg && ARMPlatformConfig::validReg(reg->getBytesNoCopy(), reg->getLength(), physicalAddressLimit());
}

UInt64 GenericARMIO::physicalAddressLimit(void) const { return 1ULL << 48; }

bool GenericARMIO::start(IOService *provider)
{
    IOLog("GenericARMIO: starting Apple arm-io bus %s\n", provider->getName());
    if (!ARMDeviceHasTwoCells(provider)) return false;
    IORegistryEntry *parent = provider->copyParentEntry(gIODTPlane);
    if (!parent) return false;
    bool parentCells = ARMDeviceHasTwoCells(parent);
    parent->release();
    if (!parentCells) return false;
    OSData *ranges = OSDynamicCast(OSData, provider->getProperty("ranges"));
    if (!ranges || !ARMPlatformConfig::validRanges(ranges->getBytesNoCopy(),
            ranges->getLength(), physicalAddressLimit())) return false;
    // Prepare every child before starting asynchronous matching, so malformed
    // DTs cannot leave a partially published bus behind.
    OSArray *nubs = OSArray::withCapacity(8);
    OSIterator *children = provider->getChildIterator(gIODTPlane);
    if (!nubs || !children) {
        OSSafeReleaseNULL(nubs);
        OSSafeReleaseNULL(children);
        return false;
    }
    bool ok = true;
    IORegistryEntry *entry;
    while ((entry = OSDynamicCast(IORegistryEntry, children->getNextObject()))) {
        OSObject *statusProperty = entry->getProperty("status");
        OSData *status = OSDynamicCast(OSData, statusProperty);
        if (statusProperty && !status) { ok = false; break; }
        if (status && !ARMPlatformConfig::enabled(status->getBytesNoCopy(), status->getLength()))
            continue;
        if (!validateDevice(entry)) { IOLog("GenericARMIO: invalid child %s\n", entry->getName()); ok = false; break; }
        IOService *nub = createDeviceWithProvider(this, entry);
        if (!nub) { IOLog("GenericARMIO: failed to create child %s\n", entry->getName()); ok = false; break; }
        ok = nubs->setObject(nub);
        nub->release();
        if (!ok) break;
    }
    children->release();
    unsigned int attached = 0;
    if (ok) {
        for (; attached < nubs->getCount(); ++attached) {
            IOService *nub = OSDynamicCast(IOService, nubs->getObject(attached));
            if (!nub->attach(this)) { ok = false; break; }
        }
    }
    // IOPlatformIO installs the global SError provider. Do this only once all
    // fallible bus preparation is finished; this foundational Kext is resident.
    if (ok) ok = IOPlatformIO::start(provider);
    if (!ok) {
        for (unsigned int i = 0; i < attached; ++i)
            OSDynamicCast(IOService, nubs->getObject(i))->detach(this);
        nubs->release();
        return false;
    }
    for (unsigned int i = 0; i < nubs->getCount(); ++i)
        OSDynamicCast(IOService, nubs->getObject(i))->registerService();
    nubs->release();
    registerService();
    IOLog("GenericARMIO: Apple arm-io bus published\n");
    return true;
}

bool GenericARMIO::handlePlatformError(vm_offset_t /*far*/)
{
    return false; // Let XNU report unhandled SError; don't swallow hardware faults.
}

bool GenericARMIO::requestTerminate(IOService *, IOOptionBits) { return false; }

bool GenericARMIO::compareNubName(const IOService *nub, OSString *name, OSString **matched) const
{
    return IODTCompareNubName(nub, name, matched);
}

IOReturn GenericARMIO::getNubResources(IOService *nub)
{
    IOLog("GenericARMIO: resolving resources for %s\n", nub->getName());
    if (nub->getProvider() != this) return kIOReturnNotAttached;
    OSData *reg = OSDynamicCast(OSData, nub->getProperty("reg"));
    if (!reg) return nub->getProperty("reg") ? kIOReturnBadArgument : kIOReturnSuccess;
    if (!ARMPlatformConfig::validReg(reg->getBytesNoCopy(), reg->getLength(), physicalAddressLimit()))
        return kIOReturnBadArgument;
    if (!nub->getDeviceMemory()) {
        OSArray *memory = IODTResolveAddressing(nub, "reg", nullptr);
        if (!memory || memory->getCount() != reg->getLength() / 16) {
            IOLog("GenericARMIO: resource resolution failed for %s (%u/%u)\n",
                nub->getName(), memory ? memory->getCount() : 0, reg->getLength()/16);
            return kIOReturnNoResources;
        }
    }
    // IODTPlatformExpert's initial DT mapping supplies interrupt properties;
    // AppleARMGIC registers the SPI controller separately. XNU owns SGI/PPI.
    return kIOReturnSuccess;
}

IOService *GenericARMIO::createDeviceWithProvider(AppleARMIO *bus, IORegistryEntry *entry)
{
    if (bus != this || !validateDevice(entry)) return nullptr;
    OSData *status = OSDynamicCast(OSData, entry->getProperty("status"));
    if (entry->getProperty("status") && (!status ||
            !ARMPlatformConfig::enabled(status->getBytesNoCopy(), status->getLength()))) return nullptr;
    // Preserve the real AppleARMIODevice metaclass and all of its dependencies.
    return AppleARMIO::createDeviceWithProvider(bus, entry);
}

void GenericARMIO::processDevice(IOService *) {}
const char *GenericARMIO::deleteList(void) { return "()"; }
const char *GenericARMIO::excludeList(void) { return "()"; }
IOReturn GenericARMIO::clockRequest(ClockController::Request &request)
{
    if (!ClockController::valid(request)) return kIOReturnBadArgument;
    UInt32 kind = request.version == 1 ? ClockController::GCC :
        reinterpret_cast<ClockController::TargetedRequest &>(request).controller;
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    // Nonblocking discovery avoids a startup cycle: the controller itself is
    // published below this bus. Scope the match to our own provider hierarchy.
    OSDictionary *matching = IOService::serviceMatching("GenericClockController");
    if (!matching) return kIOReturnNoMemory;
    OSIterator *services = IOService::getMatchingServices(matching);
    matching->release();
    if (!services) return kIOReturnNotReady;
    IOService *service, *controller = nullptr;
    while ((service = OSDynamicCast(IOService, services->getNextObject()))) {
        IOService *nub = service->getProvider();
        auto *controllerKind = OSDynamicCast(OSNumber, service->getProperty("ClockControllerKind"));
        if (nub && nub->getProvider() == this && controllerKind && controllerKind->unsigned32BitValue() == kind) {
            if (controller) { controller->release(); services->release(); return kIOReturnExclusiveAccess; }
            controller = service; controller->retain();
        }
    }
    services->release();
    if (!controller) return kIOReturnNotReady;
    const OSSymbol *function = OSSymbol::withCStringNoCopy(ClockController::Function);
    IOReturn result = function ? controller->callPlatformFunction(function, false,
        &request, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    OSSafeReleaseNULL(function); controller->release();
    return result;
}

IOReturn GenericARMIO::callPlatformFunction(const OSSymbol *name, bool wait,
    void *p1, void *p2, void *p3, void *p4)
{
    if (!name) return kIOReturnBadArgument;
    if (!name->isEqualTo(ClockController::Function))
        return IOPlatformIO::callPlatformFunction(name, wait, p1, p2, p3, p4);
    auto *r = static_cast<ClockController::Request *>(p1);
    if (!r || p2 || p3 || p4 || !ClockController::valid(*r)) return kIOReturnBadArgument;
    return clockRequest(*r);
}

const char *GenericARMIO::getClockName(UInt32 id, UInt32 *, UInt32 count)
{
    if (count) return nullptr;
    ClockController::Request r = {1, sizeof(r), ClockController::GetName, id, 0, nullptr};
    return clockRequest(r) == kIOReturnSuccess ? r.name : nullptr;
}
UInt64 GenericARMIO::getClockFrequency(UInt32 id, UInt32 *, UInt32 count)
{
    if (count) return 0;
    ClockController::Request r = {1, sizeof(r), ClockController::GetRate, id, 0, nullptr};
    return clockRequest(r) == kIOReturnSuccess ? r.value : 0;
}
UInt64 GenericARMIO::getClockFrequency(const char *name, UInt32 *, UInt32 count)
{
    if (!name || count) return 0;
    ClockController::Request r = {1, sizeof(r), ClockController::FindClock, 0, 0, name};
    if (clockRequest(r) != kIOReturnSuccess) return 0;
    return getClockFrequency(r.id, nullptr, 0);
}
UInt64 GenericARMIO::getClockFrequency(const OSSymbol *name, UInt32 *options, UInt32 count)
{
    return name ? getClockFrequency(name->getCStringNoCopy(), options, count) : 0;
}
IOReturn GenericARMIO::enableDeviceClock(UInt32 id, UInt32 enable)
{
    if (enable > 1) return kIOReturnBadArgument;
    ClockController::Request r = {1, sizeof(r), enable ? ClockController::RetainClock :
        ClockController::ReleaseClock, id, 0, nullptr};
    return clockRequest(r);
}
IOReturn GenericARMIO::enableDevicePower(UInt32 id, UInt32 state, UInt32 *result)
{
    if (result) *result = 0xffffffffU;
    if (state > 1) return kIOReturnBadArgument;
    ClockController::Request r = {1, sizeof(r), state ? ClockController::RetainDomain :
        ClockController::ReleaseDomain, id, 0, nullptr};
    IOReturn status = clockRequest(r);
    if (status == kIOReturnSuccess && result) {
        r.operation = ClockController::GetDomainState;
        status = clockRequest(r);
        if (status == kIOReturnSuccess) *result = static_cast<UInt32>(r.value);
    }
    return status;
}
IOReturn GenericARMIO::enableDevicePinGroup(UInt32 *, UInt32) { return kIOReturnUnsupported; }
bool GenericARMIO::isDMACoherent(IOService *, UInt32) { return false; }
UInt64 GenericARMIO::getIODeviceClocks(UInt32 *, bool, UInt32) { return 0; }
