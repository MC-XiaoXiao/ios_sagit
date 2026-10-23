#include "SM8150.h"
#include "SM8150Config.h"
#include <ARMDeviceTree.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>

#define super GenericARMPlatform
OSDefineMetaClassAndStructors(SM8150, super);
#undef super
#define super GenericARMIO
OSDefineMetaClassAndStructors(SM8150IO, super);

static bool cpuProperty(IORegistryEntry *cpu, const char *name, uint32_t value)
{
    OSData *data = OSDynamicCast(OSData, cpu->getProperty(name));
    return data && data->getLength() == 4 &&
        ARMPlatformConfig::readLE32(data->getBytesNoCopy()) == value;
}

bool SM8150::preparePlatform(IOService *provider)
{
    IORegistryEntry *psci = provider->childFromPath("psci", gIODTPlane);
    if (!psci) return false;
    OSData *method = OSDynamicCast(OSData, psci->getProperty("method"));
    bool valid = method && ARMPlatformConfig::conduit(method->getBytesNoCopy(),
        method->getLength()) == ARMPlatformConfig::kSMC;
    psci->release();
    if (!valid) return false; // The SM8150 machine layer uses PSCI through SMC.

    IORegistryEntry *cpus = provider->childFromPath("cpus", gIODTPlane);
    if (!cpus) return false;
    OSIterator *children = cpus->getChildIterator(gIODTPlane);
    cpus->release();
    if (!children) return false;
    uint32_t seen = 0;
    IORegistryEntry *cpu;
    while ((cpu = OSDynamicCast(IORegistryEntry, children->getNextObject()))) {
        OSData *type = OSDynamicCast(OSData, cpu->getProperty("device_type"));
        if (!type || !ARMPlatformConfig::stringEquals(type->getBytesNoCopy(),
                type->getLength(), "cpu")) continue;
        OSObject *statusProperty = cpu->getProperty("status");
        OSData *status = OSDynamicCast(OSData, statusProperty);
        if (statusProperty && !status) { valid = false; break; }
        if (status && !ARMPlatformConfig::enabled(status->getBytesNoCopy(),
                status->getLength())) continue;
        OSData *reg = OSDynamicCast(OSData, cpu->getProperty("reg"));
        OSData *enable = OSDynamicCast(OSData, cpu->getProperty("enable-method"));
        if (!reg || !SM8150Config::addCPU(reg->getBytesNoCopy(), reg->getLength(), seen) ||
            !enable || !ARMPlatformConfig::stringEquals(enable->getBytesNoCopy(),
                enable->getLength(), "psci")) { valid = false; break; }
        uint32_t affinity = ARMPlatformConfig::readLE32(reg->getBytesNoCopy());
        // These are XNU scheduling groups, not physical DSU cluster numbers.
        if (!cpuProperty(cpu, "die-cluster-id", SM8150Config::schedulerCluster(affinity)) ||
            !cpuProperty(cpu, "cluster-core-id", SM8150Config::schedulerCore(affinity)) ||
            !cpuProperty(cpu, "cluster-type", affinity < 0x400 ? 0x45 : 0x50) ||
            !cpuProperty(cpu, "l2-cache-id", affinity >> 8)) {
            valid = false;
            break;
        }
    }
    children->release();
    return valid && seen && GenericARMPlatform::preparePlatform(provider);
}

bool SM8150IO::validateDevice(IORegistryEntry *entry)
{
    if (!GenericARMIO::validateDevice(entry)) return false;
    // Do not admit the predecessor's UARTDM backend on a GENI serial engine.
    if (ARMDeviceMatches(entry, "qcom,msm-uartdm") ||
        ARMDeviceMatches(entry, "qcom,msm-uartdm-v1.4")) return false;
    if (!ARMDeviceMatches(entry, "arm,gic-v3") &&
        !ARMDeviceMatches(entry, "ARM,gicv3")) return true;
    OSData *reg = OSDynamicCast(OSData, entry->getProperty("reg"));
    return reg && SM8150Config::gicRegisters(reg->getBytesNoCopy(), reg->getLength());
}

bool SM8150IO::start(IOService *provider)
{
    OSData *ranges = OSDynamicCast(OSData, provider->getProperty("ranges"));
    if (!ranges || !SM8150Config::identityRanges(ranges->getBytesNoCopy(),
            ranges->getLength())) return false;
    return GenericARMIO::start(provider);
}

// This is the admitted peripheral MMIO window, not a limit on installed RAM.
UInt64 SM8150IO::physicalAddressLimit(void) const { return 1ULL << 32; }
