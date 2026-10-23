#include "SDM835.h"
#include <ARMDeviceTree.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>

#define super GenericARMPlatform
OSDefineMetaClassAndStructors(SDM835, super);
#undef super
#define super GenericARMIO
OSDefineMetaClassAndStructors(SDM835IO, super);

bool SDM835::preparePlatform(IOService *provider)
{
    // Early XNU's SDM835 CPU_ON/OFF path is SMC-only. Refuse a contradictory DT.
    IORegistryEntry *psci = provider->childFromPath("psci", gIODTPlane);
    if (!psci) return false;
    OSData *method = OSDynamicCast(OSData, psci->getProperty("method"));
    bool valid = method && ARMPlatformConfig::conduit(method->getBytesNoCopy(),
        method->getLength()) == ARMPlatformConfig::kSMC;
    psci->release();
    if (!valid) return false;

    IORegistryEntry *cpus = provider->childFromPath("cpus", gIODTPlane);
    if (!cpus) return false;
    OSIterator *children = cpus->getChildIterator(gIODTPlane);
    cpus->release();
    if (!children) return false;
    uint32_t seen = 0;
    IORegistryEntry *cpu;
    while ((cpu = OSDynamicCast(IORegistryEntry, children->getNextObject()))) {
        OSData *type = OSDynamicCast(OSData, cpu->getProperty("device_type"));
        if (!type || !ARMPlatformConfig::stringEquals(type->getBytesNoCopy(), type->getLength(), "cpu"))
            continue;
        OSData *reg = OSDynamicCast(OSData, cpu->getProperty("reg"));
        if (!reg || !ARMPlatformConfig::addSDM835CPU(reg->getBytesNoCopy(), reg->getLength(), seen)) {
            valid = false;
            break;
        }
    }
    children->release();
    // Firmware may harvest CPUs; don't invent the absent cores. cpus=N remains
    // XNU's responsibility. At least one firmware CPU must be present.
    return valid && seen && GenericARMPlatform::preparePlatform(provider);
}

bool SDM835IO::validateDevice(IORegistryEntry *entry)
{
    if (!GenericARMIO::validateDevice(entry)) return false;
    if (!ARMDeviceMatches(entry, "qcom,msm-uartdm-v1.4")) return true;
    OSData *reg = OSDynamicCast(OSData, entry->getProperty("reg"));
    // Keep the IOKit description consistent with this board's early console.
    return reg && reg->getLength() == 16 &&
        ARMPlatformConfig::readLE64(reg->getBytesNoCopy()) == 0x0c1b0000 &&
        ARMPlatformConfig::readLE64(static_cast<const uint8_t *>(reg->getBytesNoCopy()) + 8) == 0x1000;
}

bool SDM835IO::start(IOService *provider)
{
    OSData *ranges = OSDynamicCast(OSData, provider->getProperty("ranges"));
    // This project uses an identity, zero-based SoC window. Never silently
    // reinterpret Linux big-endian cells or another Qualcomm board's window.
    if (!ranges || ranges->getLength() != 24 ||
        ARMPlatformConfig::readLE64(ranges->getBytesNoCopy()) != 0 ||
        ARMPlatformConfig::readLE64(static_cast<const uint8_t *>(ranges->getBytesNoCopy()) + 8) != 0 ||
        ARMPlatformConfig::readLE64(static_cast<const uint8_t *>(ranges->getBytesNoCopy()) + 16) != 0x100000000ULL)
        return false;
    return GenericARMIO::start(provider);
}

UInt64 SDM835IO::physicalAddressLimit(void) const { return 1ULL << 40; }
