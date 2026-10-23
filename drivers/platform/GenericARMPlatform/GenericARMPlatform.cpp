#include "GenericARMPlatform.h"
#include "ARMDeviceTree.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <machine/machine_routines.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOCPU.h>
#include <IOKit/rtc/IORTCController.h>

#define super AppleARMPE
OSDefineMetaClassAndStructors(GenericARMPlatform, super);

bool GenericARMPlatform::preparePlatform(IOService *provider)
{
    fConduit = ARMPlatformConfig::kNoPSCI;
    if (!ARMDeviceHasTwoCells(provider)) return false;
    IORegistryEntry *psci = provider->childFromPath("psci", gIODTPlane);
    if (!psci) return true;
    OSData *status = OSDynamicCast(OSData, psci->getProperty("status"));
    OSData *method = OSDynamicCast(OSData, psci->getProperty("method"));
    bool supported = ARMDeviceMatches(psci, "arm,psci-0.2") || ARMDeviceMatches(psci, "arm,psci-1.0");
    if (supported && (!psci->getProperty("status") || status) && ARMPlatformConfig::enabled(status ? status->getBytesNoCopy() : nullptr,
            status ? status->getLength() : 0) && method)
        fConduit = ARMPlatformConfig::conduit(method->getBytesNoCopy(), method->getLength());
    psci->release();
    return fConduit != ARMPlatformConfig::kNoPSCI;
}

bool GenericARMPlatform::start(IOService *provider)
{
    if (!preparePlatform(provider)) return false;
    // Keep the native metaclass/ABI, but use XNU's platform setup. The native
    // AppleARMPE::start waits for Apple-only RTC/PMGR services on this cache.
    // configure() remains IODTPlatformExpert's CPU/top-level nub publisher.
    if (!IOPlatformExpert::start(provider)) return false;
    setProperty("ARMPlatformFirmwarePower", fConduit != ARMPlatformConfig::kNoPSCI);
    registerService();
    return true;
}

bool GenericARMPlatform::configure(IOService *provider)
{
    IOLog("GenericARMPlatform: configuring Apple DT nubs\n");
    const ml_topology_info_t *topology = ml_get_topology_info();
    if (!topology || topology->version != CPU_TOPOLOGY_VERSION || !topology->num_cpus)
        panic("GenericARMPlatform: missing XNU CPU topology");
    IOCPUInterruptController *controller = new IOCPUInterruptController;
    if (!controller || controller->initCPUInterruptController(topology->max_cpu_id + 1,
            topology->num_cpus) != kIOReturnSuccess ||
        !setProperty("GenericARMCPUInterruptController", controller))
        panic("GenericARMPlatform: cannot initialize CPU interrupt bookkeeping");
    controller->release();
    // This controller tracks local CPU initialization only. Don't register it
    // under the peripheral parent's name: SDM835 uses Apple's Passthru there.
    bool ok = IODTPlatformExpert::configure(provider);
    IOLog("GenericARMPlatform: DT nub configuration %s\n", ok ? "complete" : "failed");
    return ok;
}

bool GenericARMPlatform::requestTerminate(IOService *, IOOptionBits) { return false; }

IOService *GenericARMPlatform::createNub(IORegistryEntry *entry)
{
    OSObject *property = entry->getProperty("status");
    OSData *status = OSDynamicCast(OSData, property);
    if (property && (!status || !ARMPlatformConfig::enabled(status->getBytesNoCopy(), status->getLength())))
        return nullptr;
    bool isCPU = ARMDeviceMatches(entry, "cpu");
    if (isCPU) {
        OSData *reg = OSDynamicCast(OSData, entry->getProperty("reg"));
        if (!reg || reg->getLength() != 4 ||
            ml_get_cpu_number(ARMPlatformConfig::readLE32(reg->getBytesNoCopy())) < 0)
            return nullptr;
    }
    IOService *nub = IODTPlatformExpert::createNub(entry);
    IOLog("GenericARMPlatform: nub %s %s\n", entry->getName(gIODTPlane), nub ? "created" : "failed");
    if (nub && isCPU && !nub->setProperty("GenericARMPlatformCPU", true)) {
        nub->release();
        return nullptr;
    }
    return nub;
}

// IODTFindMatchingEntries interprets a null list as matching ALL children.
const char *GenericARMPlatform::deleteList(void) { return "()"; }
const char *GenericARMPlatform::excludeList(void)
{
    return "(cpus, chosen, memory, options, aliases)";
}

bool GenericARMPlatform::platformAdjustService(IOService *service)
{
    return IOPlatformExpert::platformAdjustService(service);
}

IOReturn GenericARMPlatform::callPlatformFunction(const OSSymbol *name, bool wait,
    void *p1, void *p2, void *p3, void *p4)
{
    if (!name) return kIOReturnBadArgument;
    return IOPlatformExpert::callPlatformFunction(name, wait, p1, p2, p3, p4);
}

OSString *GenericARMPlatform::createSystemSerialNumberString(OSData *data)
{
    if (!data || !data->getLength()) return nullptr;
    const char *text = static_cast<const char *>(data->getBytesNoCopy());
    unsigned int length = data->getLength();
    if (text[length - 1] || strnlen(text, length) != length - 1) return nullptr;
    return OSString::withCString(text);
}

void GenericARMPlatform::getUTCTimeOfDay(clock_sec_t *secs, clock_nsec_t *nsecs)
{
    OSObject *object = getResourceService()->copyProperty("IORTC");
    IORTC *clock = OSDynamicCast(IORTC, object);
    if (clock) clock->getUTCTimeOfDay(secs, nsecs);
    else IOPlatformExpert::getUTCTimeOfDay(secs, nsecs);
    OSSafeReleaseNULL(object);
}
void GenericARMPlatform::setUTCTimeOfDay(clock_sec_t secs, clock_nsec_t nsecs)
{
    OSObject *object = getResourceService()->copyProperty("IORTC");
    IORTC *clock = OSDynamicCast(IORTC, object);
    if (clock) clock->setUTCTimeOfDay(secs, nsecs);
    else IOPlatformExpert::setUTCTimeOfDay(secs, nsecs);
    OSSafeReleaseNULL(object);
}
void GenericARMPlatform::PMInstantiatePowerDomains(void)
{
    IOPlatformExpert::PMInstantiatePowerDomains();
    // Firmware power-off/reset and WFI idle do not imply suspend/resume support.
    if (root) root->setSleepSupported(0);
}
void GenericARMPlatform::PMRegisterDevice(IOService *nub, IOService *device)
{
    IOPlatformExpert::PMRegisterDevice(nub, device);
}

int GenericARMPlatform::haltRestart(unsigned int type)
{
    if (type == kPEPanicSync) return 0;
    uint32_t function;
    switch (type) {
    case kPEHaltCPU:
    case kPEUPSDelayHaltCPU: function = 0x84000008; break; // PSCI SYSTEM_OFF
    case kPERestartCPU:
    case kPEPanicRestartCPU:
    case kPEPanicRestartCPUNoCallouts: function = 0x84000009; break; // SYSTEM_RESET
    default: return IOPlatformExpert::haltRestart(type);
    }
    if (fConduit == ARMPlatformConfig::kNoPSCI) {
        kprintf("GenericARMPlatform: shutdown type=%u has no PSCI conduit\n", type);
        return -1;
    }
    kprintf("GenericARMPlatform: PSCI %s via %s, shutdown type=%u\n",
        function == 0x84000008 ? "SYSTEM_OFF" : "SYSTEM_RESET",
        fConduit == ARMPlatformConfig::kSMC ? "SMC" : "HVC", type);
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = 0;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    if (fConduit == ARMPlatformConfig::kSMC) {
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
            : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                "x12", "x13", "x14", "x15", "x16", "x17", "memory", "cc");
    } else {
        __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
            : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                "x12", "x13", "x14", "x15", "x16", "x17", "memory", "cc");
    }
    // A successful reset/off never returns; even a firmware success code here
    // is failure to complete shutdown. Do not report a completed operation.
    IOLog("GenericARMPlatform: PSCI shutdown returned %lld\n", (long long)x0);
    return -1;
}
