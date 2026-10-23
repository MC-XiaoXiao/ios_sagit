// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandTFTPIO.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/crypto/sha2.h>
namespace Baseband {
Result readTFTPFiles(TFTP::Files &files, const MemoryHandoff &record) {
    if (!record.valid()) return Result::Invalid;
    if (record.version == MemoryHandoff::CatalogVersion) {
        auto *memory = IOMemoryDescriptor::withPhysicalAddress(record.image.address, record.image.bytes,
            kIODirectionOut | kIOMemoryMapperNone);
        auto *map = memory ? memory->map(kIOMapAnywhere | kIOMapReadOnly | kIOMapDefaultCache) : nullptr;
        OSData *copy = nullptr;
        Result r = Result::NoMemory;
        if (map && map->getVirtualAddress() && map->getLength() == record.image.bytes) {
            const auto *p = reinterpret_cast<const uint8_t *>(map->getVirtualAddress());
            copy = OSData::withBytes(p + record.filesOffset, unsigned(record.filesBytes));
        }
        if (copy) {
            SHA256_CTX hash; uint8_t actual[32];
            SHA256_Init(&hash); SHA256_Update(&hash, copy->getBytesNoCopy(), copy->getLength()); SHA256_Final(actual, &hash);
            bool equal = true; for (unsigned i = 0; i < 32; ++i) equal &= actual[i] == record.filesHash[i];
            r = equal ? files.bind(copy->getBytesNoCopy(), copy->getLength()) : Result::Invalid;
        }
        OSSafeReleaseNULL(copy); OSSafeReleaseNULL(map); OSSafeReleaseNULL(memory); return r;
    }
    auto *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) return Result::NotReady;
    auto *property = chosen->getProperty("qcom,tftp-files");
    auto *data = OSDynamicCast(OSData, property);
    const auto r = property && !data ? Result::Invalid :
        files.bind(data ? data->getBytesNoCopy() : nullptr, data ? data->getLength() : 0);
    chosen->release(); return r;
}
} // namespace Baseband
