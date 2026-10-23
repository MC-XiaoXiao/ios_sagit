// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandMemoryIO.h"
#include "BasebandMemoryAccess.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/crypto/sha2.h>

namespace Baseband {
Result readMemoryHandoff(MemoryHandoff &out) {
    auto *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    auto *memory = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (!chosen || !memory) { OSSafeReleaseNULL(chosen); OSSafeReleaseNULL(memory); return Result::NotReady; }
    auto *data = OSDynamicCast(OSData, chosen->getProperty("qcom,baseband-handoff"));
    auto r = data ? out.decode(data->getBytesNoCopy(), data->getLength()) : Result::NotReady;
    const char *names[] = {"QcomMPSS", "QcomMBA", "QcomBBMetadata", "QcomBBFirmware", "QcomSMEM"};
    const Region regions[] = {out.layout.modem, out.layout.mba, out.layout.metadata, out.image, {0x86000000, 0x200000}};
    if (r == Result::OK) for (unsigned i = 0; i < 5; ++i) {
        data = OSDynamicCast(OSData, memory->getProperty(names[i]));
        if (!data || data->getLength() != 16 || read64(data->getBytesNoCopy()) != regions[i].address ||
            read64(static_cast<const uint8_t *>(data->getBytesNoCopy()) + 8) != regions[i].bytes) { r = Result::Invalid; break; }
    }
    chosen->release(); memory->release(); return r;
}
Result copyFirmware(const MemoryHandoff &record, OSData *&mba, OSData *&modem) {
    if (mba || modem || !record.valid()) return Result::Invalid;
    auto *memory = IOMemoryDescriptor::withPhysicalAddress(record.image.address, record.image.bytes,
        kIODirectionOut | kIOMemoryMapperNone);
    auto *map = memory ? memory->map(kIOMapAnywhere | kIOMapReadOnly | kIOMapDefaultCache) : nullptr;
    Result r = Result::NoMemory;
    if (map && map->getVirtualAddress() && map->getLength() == record.image.bytes) {
        const auto *p = reinterpret_cast<const uint8_t *>(map->getVirtualAddress());
        mba = OSData::withBytes(p + record.mbaOffset, unsigned(record.mbaBytes));
        modem = OSData::withBytes(p + record.modemOffset, unsigned(record.modemBytes));
        if (mba && modem) {
            SHA256_CTX hash; uint8_t actual[32]; bool equal = true;
            SHA256_Init(&hash); SHA256_Update(&hash, mba->getBytesNoCopy(), mba->getLength()); SHA256_Final(actual, &hash);
            for (unsigned i = 0; i < 32; ++i) equal &= actual[i] == record.mbaHash[i];
            SHA256_Init(&hash); SHA256_Update(&hash, modem->getBytesNoCopy(), modem->getLength()); SHA256_Final(actual, &hash);
            for (unsigned i = 0; i < 32; ++i) equal &= actual[i] == record.modemHash[i];
            r = equal ? Result::OK : Result::Invalid;
        }
    }
    OSSafeReleaseNULL(map); OSSafeReleaseNULL(memory);
    if (r != Result::OK) { OSSafeReleaseNULL(mba); OSSafeReleaseNULL(modem); }
    return r;
}
struct NativeMemory::Context {
    IOMemoryDescriptor *memory[3]{};
    IOMemoryMap *maps[3]{};
    BootLayout layout;
    MemoryAccess<Context> access{*this};
    explicit Context(const BootLayout &m) : layout(m) {}
    ~Context() { for (unsigned i = 0; i < 3; ++i) { OSSafeReleaseNULL(maps[i]); OSSafeReleaseNULL(memory[i]); } }
    bool copy(unsigned index, uint64_t offset, const uint8_t *source, size_t bytes) {
        memcpy(reinterpret_cast<void *>(maps[index]->getVirtualAddress() + offset), source, bytes); return true;
    }
    bool zero(unsigned index, uint64_t offset, size_t bytes) {
        bzero(reinterpret_cast<void *>(maps[index]->getVirtualAddress() + offset), bytes); return true;
    }
    bool publish(unsigned, uint64_t, size_t) {
        // Every destination mapping is Normal Non-Cacheable. CPU writes have
        // no dirty WB lines; order/complete them before SCM or RMB publication.
        __asm__ volatile("dsb sy" ::: "memory"); return true;
    }
};
NativeMemory::~NativeMemory() { close(); }
Result NativeMemory::bindConfirmed(const BootLayout &memory) {
    if (context || !memory.valid()) return Result::Invalid;
    context = new Context(memory); if (!context) return Result::NoMemory;
    const Region regions[] = {memory.modem, memory.mba, memory.metadata};
    for (unsigned i = 0; i < 3; ++i) {
        context->memory[i] = IOMemoryDescriptor::withPhysicalAddress(regions[i].address, regions[i].bytes,
            kIODirectionInOut | kIOMemoryMapperNone);
        context->maps[i] = context->memory[i] ? context->memory[i]->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
        if (!context->maps[i] || !context->maps[i]->getVirtualAddress() || context->maps[i]->getLength() != regions[i].bytes) {
            close(); return Result::NoMemory;
        }
    }
    return context->access.confirm(memory);
}
Result NativeMemory::assigned(const Region &r, Ownership from, Ownership to) {
    return context ? context->access.assigned(r, from, to) : Result::NotReady;
}
void NativeMemory::quarantine() { if (context) context->access.quarantine(); }
bool NativeMemory::copy(uint64_t address, const uint8_t *p, size_t bytes) { return context && context->access.copy(address, p, bytes); }
bool NativeMemory::zero(uint64_t address, size_t bytes) { return context && context->access.zero(address, bytes); }
bool NativeMemory::publish(uint64_t address, size_t bytes) { return context && context->access.publish(address, bytes); }
Result NativeMemory::metadata(const FirmwareImage &image, const Region &region) {
    if (!context || region.address != context->layout.metadata.address || region.bytes != context->layout.metadata.bytes ||
        !image.valid() || image.metadataBytes() > region.bytes) return Result::Invalid;
    // Assemble into ordinary HLOS memory first. The final write still passes
    // through the same per-region ownership and length checks as every segment.
    auto *data = OSData::withCapacity(unsigned(image.metadataBytes()));
    if (!data || !data->appendBytes(nullptr, unsigned(image.metadataBytes()))) { OSSafeReleaseNULL(data); return Result::NoMemory; }
    auto r = image.metadata(const_cast<void *>(data->getBytesNoCopy()), data->getLength());
    if (r == Result::OK && (!copy(region.address, static_cast<const uint8_t *>(data->getBytesNoCopy()), data->getLength()) ||
        !zero(region.address + data->getLength(), region.bytes - data->getLength()) || !publish(region.address, region.bytes))) r = Result::IO;
    data->release(); return r;
}
bool NativeMemory::close() {
    if (!context) return true;
    if (!context->access.canClose()) return false;
    delete context; context = nullptr; return true;
}
} // namespace Baseband
