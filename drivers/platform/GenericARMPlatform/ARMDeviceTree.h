#ifndef _MI6_ARM_DEVICE_TREE_H
#define _MI6_ARM_DEVICE_TREE_H
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSData.h>
#include "ARMPlatformConfig.h"

inline bool ARMDeviceHasTwoCells(IORegistryEntry *entry)
{
    OSData *address = OSDynamicCast(OSData, entry->getProperty("#address-cells"));
    OSData *size = OSDynamicCast(OSData, entry->getProperty("#size-cells"));
    return address && size && address->getLength() == 4 && size->getLength() == 4 &&
        ARMPlatformConfig::readLE32(address->getBytesNoCopy()) == 2 &&
        ARMPlatformConfig::readLE32(size->getBytesNoCopy()) == 2;
}

inline bool ARMDeviceMatches(IORegistryEntry *entry, const char *compatible)
{
    OSString *name = OSString::withCString(compatible);
    if (!name) return false;
    bool result = IODTCompareNubName(entry, name, static_cast<OSString **>(nullptr));
    name->release();
    return result;
}
#endif
