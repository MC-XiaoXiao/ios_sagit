// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandRMTFSIO.h"
#include <IOBufferAllocation.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/storage/IOMedia.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
static_assert(sizeof(IOStorage) == 144 && sizeof(IOMedia) == 208, "iPad iOS 18 storage declarations");

namespace Baseband {
namespace {
constexpr Region Shared{RMTFS::SharedBase, RMTFS::SharedBytes};
Result status(IOReturn r) {
    switch (r) {
    case kIOReturnSuccess: return Result::OK;
    case kIOReturnNotResponding: case kIOReturnTimeout: case kIOReturnOffline: return Result::Quarantined;
    case kIOReturnNoMemory: return Result::NoMemory;
    case kIOReturnUnsupported: case kIOReturnNotWritable: return Result::Unsupported;
    case kIOReturnBusy: return Result::Busy;
    case kIOReturnBadArgument: return Result::Invalid;
    case kIOReturnNotReady: case kIOReturnNotAttached: return Result::NotReady;
    default: return Result::IO;
    }
}
bool identity(IOMedia *media, IOService *owner, const RMTFS::MediaPolicy &policy) {
    if (!media || media->isInactive() || media->isWhole() || !media->isFormatted() ||
        media->getBase() != policy.start || media->getSize() != policy.bytes ||
        ((policy.access & RMTFS::Writable) && !media->isWritable())) return false;
    auto *text = OSDynamicCast(OSString, media->getProperty(kIOMediaUUIDKey));
    uint8_t actual[16];
    if (!text || !RMTFS::uuid(text->getCStringNoCopy(), text->getLength(), actual) || memcmp(actual, policy.uuid, 16)) return false;
    // Accept only direct GPT partitions of this SoC's QcomUFS logical units.
    // Files, disk images, external disks, whole disks and stacked/aliased media
    // cannot acquire a write path just by publishing a matching label/UUID.
    auto *scheme = media->getProvider();
    auto *whole = scheme && !scheme->isInactive() && scheme->metaCast("IOGUIDPartitionScheme") ? OSDynamicCast(IOMedia, scheme->getProvider()) : nullptr;
    auto *driver = whole && whole->isWhole() && !whole->getBase() && !whole->isInactive() ? whole->getProvider() : nullptr;
    auto *block = driver && !driver->isInactive() && driver->metaCast("IOBlockStorageDriver") ? driver->getProvider() : nullptr;
    auto *ufs = block && !block->isInactive() && block->metaCast("QcomUFSBlockDevice") ? block->getProvider() : nullptr;
    auto *lun = block ? OSDynamicCast(OSNumber, block->getProperty("UFS LUN")) : nullptr;
    auto *onub = owner->getProvider(), *unub = ufs ? ufs->getProvider() : nullptr;
    const auto blockBytes = media->getPreferredBlockSize();
    return ufs && !ufs->isInactive() && ufs->metaCast("QcomUFS") && lun && lun->unsigned32BitValue() == policy.lun &&
        onub && unub && onub->getProvider() && onub->getProvider() == unub->getProvider() &&
        blockBytes >= RMTFS::SectorBytes && blockBytes <= RMTFS::TransferBytes && !(blockBytes & (blockBytes - 1)) &&
        !(policy.start % blockBytes) && !(policy.bytes % blockBytes) && range(policy.start, policy.bytes, whole->getSize());
}
}
Result readRMTFSPolicy(RMTFS::StoragePolicy &out, uint64_t epoch) {
    out = {};
    auto *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    auto *memory = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (!chosen || !memory) { OSSafeReleaseNULL(chosen); OSSafeReleaseNULL(memory); return Result::NotReady; }
    auto *data = OSDynamicCast(OSData, chosen->getProperty("qcom,rmtfs-storage-policy"));
    auto r = data ? out.decode(data->getBytesNoCopy(), data->getLength()) : Result::NotReady;
    data = OSDynamicCast(OSData, memory->getProperty("QcomRMTFS"));
    if (r == Result::OK && (!epoch || out.epoch != epoch || !data || data->getLength() != 16 ||
        read64(data->getBytesNoCopy()) != Shared.address ||
        read64(static_cast<const uint8_t *>(data->getBytesNoCopy()) + 8) != Shared.bytes)) r = Result::Invalid;
    chosen->release(); memory->release();
    if (r != Result::OK) out = {};
    return r;
}
struct NativeRMTFSStorage::Context {
    IOService *owner;
    RMTFS::StoragePolicy policy;
    IOMedia *media[RMTFS::PartitionCount]{};
    bool opened[RMTFS::PartitionCount]{}, dirty[RMTFS::PartitionCount]{};
    unsigned refs[RMTFS::PartitionCount]{};
    IOBufferMemoryDescriptor *buffer = nullptr;
    IOMemoryDescriptor *memory = nullptr;
    IOMemoryMap *mapping = nullptr;
    NativeAssignment *secure = nullptr;
    const uint32_t *abort;
    bool pinned = false, prepared = false;
    RMTFS::StorageBlocks<Context> blocks{*this};
    RMTFS::SharedRegion<Context> shared{*this};
    Context(IOService *o, const RMTFS::StoragePolicy &p, const uint32_t *a) : owner(o), policy(p), abort(a) { owner->retain(); }
    ~Context() {
        for (unsigned i = 0; i < RMTFS::PartitionCount; ++i) {
            if (opened[i]) media[i]->close(owner);
            OSSafeReleaseNULL(media[i]);
        }
        OSSafeReleaseNULL(buffer); owner->release();
    }
    Result pin() { pinned = true; shared.quarantine(); owner->setProperty("BasebandRMTFSQuarantined", true); return Result::Quarantined; }
    bool interrupted() const { return __atomic_load_n(abort, __ATOMIC_ACQUIRE) != 0; }
    Result confirm() { return secure ? secure->confirmHost(Shared) : Result::NotReady; }
    Result assign(Ownership from, Ownership to) { return secure ? secure->assign(Shared, from, to) : Result::NotReady; }
    Result map() {
        if (memory || mapping) return Result::Busy;
        memory = IOMemoryDescriptor::withPhysicalAddress(Shared.address, Shared.bytes, kIODirectionInOut | kIOMemoryMapperNone);
        mapping = memory ? memory->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
        return mapping && mapping->getVirtualAddress() && mapping->getLength() == Shared.bytes ? Result::OK : Result::NoMemory;
    }
    bool unmap() { OSSafeReleaseNULL(mapping); OSSafeReleaseNULL(memory); return true; }
    Result geometry(unsigned part, uint64_t &bytes, uint32_t &block, bool &writable, bool flush = false) {
        if (pinned || shared.quarantined()) return Result::Quarantined;
        if (!flush && interrupted()) return Result::NotReady;
        if (part >= RMTFS::PartitionCount || !refs[part] || !shared.ready() || !opened[part]) return Result::NotReady;
        if (!identity(media[part], owner, policy.media[part])) return Result::Stale;
        bytes = policy.media[part].bytes; block = uint32_t(media[part]->getPreferredBlockSize());
        writable = policy.media[part].access & RMTFS::Writable; return Result::OK;
    }
    Result transfer(unsigned part, bool write, uint64_t offset, void *data, size_t bytes, size_t &actual) {
        actual = 0;
        uint64_t capacity = 0; uint32_t block = 0; bool writable = false;
        auto r = geometry(part, capacity, block, writable); if (r != Result::OK) return r;
        if (!bytes || bytes > RMTFS::TransferBytes || !data || (offset % block) || (bytes % block) ||
            !range(offset, bytes, capacity) || (write && !writable) || prepared) return Result::Invalid;
        const auto direction = write ? kIODirectionOut : kIODirectionIn;
        buffer->setLength(bytes); buffer->setDirection(direction);
        if (buffer->getLength() != bytes) return Result::IO;
        if (write) memcpy(buffer->getBytesNoCopy(), data, bytes);
        r = status(buffer->prepare(direction));
        if (r != Result::OK) return r == Result::Quarantined ? pin() : r;
        prepared = true; UInt64 completed = 0;
        if (write) dirty[part] = true;
        const auto native = write ? media[part]->write(owner, offset, buffer, nullptr, &completed) :
            media[part]->read(owner, offset, buffer, nullptr, &completed);
        r = status(native); actual = size_t(completed);
        if (r == Result::Quarantined || completed > bytes) return pin();
        // I/O returned a completed status. Pair our prepare even on a known
        // short/error result; the storage provider owns its own nested prepare.
        if (buffer->complete(direction) != kIOReturnSuccess) return pin();
        prepared = false;
        if (!write && r == Result::OK && completed == bytes) memcpy(data, buffer->getBytesNoCopy(), bytes);
        return r;
    }
};
NativeRMTFSStorage::~NativeRMTFSStorage() { close(); }
Result NativeRMTFSStorage::bind(IOService *owner, const RMTFS::StoragePolicy &policy, const uint32_t *abort) {
    if (context || !owner || !owner->metaCast("QcomBaseband") || !policy.valid() || !abort) return Result::Invalid;
    context = new Context(owner, policy, abort); if (!context) return Result::NoMemory;
    auto &c = *context;
    auto *match = IOService::serviceMatching(kIOMediaClass);
    auto iterator = match ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    Result r = iterator ? Result::OK : Result::NotReady;
    if (iterator) while (auto *object = iterator->getNextObject()) {
        auto *media = OSDynamicCast(IOMedia, object);
        for (unsigned i = 0; i < RMTFS::PartitionCount; ++i) if (identity(media, owner, policy.media[i])) {
            if (c.media[i]) { r = Result::Busy; break; } // Duplicate UUID/range is not an implicit selection.
            c.media[i] = media; media->retain();
        }
        if (r != Result::OK) break;
    }
    if (r == Result::OK) for (unsigned i = 0; i < RMTFS::PartitionCount; ++i) {
        if (!c.media[i]) { r = Result::NotReady; break; }
        const IOStorageAccess access = kIOStorageAccessExclusiveLock |
            ((policy.media[i].access & RMTFS::Writable) ? kIOStorageAccessReaderWriter : kIOStorageAccessReader);
        if (!c.media[i]->open(owner, 0, access)) { r = Result::Busy; break; }
        c.opened[i] = true;
        if (!identity(c.media[i], owner, policy.media[i])) { r = Result::Stale; break; }
    }
    if (r == Result::OK) {
        c.buffer = IOKitMemory::allocateBuffer(IOKitMemory::bufferOptions(true), RMTFS::TransferBytes, RMTFS::TransferBytes);
        if (!c.buffer) r = Result::NoMemory;
    }
    if (r != Result::OK) close();
    return r;
}
Result NativeRMTFSStorage::acquireShared(NativeAssignment &secure) {
    if (!context) return Result::NotReady;
    context->secure = &secure;
    const auto r = context->shared.acquire();
    return r == Result::Quarantined ? context->pin() : r;
}
Result NativeRMTFSStorage::retireShared(bool remoteStopped) {
    if (!context) return Result::OK;
    const auto r = context->shared.retire(remoteStopped);
    return r == Result::Quarantined ? context->pin() : r;
}
void NativeRMTFSStorage::quarantine() { if (context) context->pin(); }
bool NativeRMTFSStorage::quarantined() const { return context && (context->pinned || context->shared.quarantined()); }
bool NativeRMTFSStorage::ready() const { return context && !context->pinned && !context->interrupted() && context->shared.ready(); }
Result NativeRMTFSStorage::open(unsigned part) {
    if (!ready()) return quarantined() ? Result::Quarantined : Result::NotReady;
    auto &c = *context;
    if (part >= RMTFS::PartitionCount || !c.opened[part] || c.refs[part] == UINT32_MAX) return Result::Invalid;
    if (!identity(c.media[part], c.owner, c.policy.media[part])) return Result::Stale;
    ++c.refs[part]; return Result::OK;
}
Result NativeRMTFSStorage::sync(unsigned part) {
    if (!context) return Result::NotReady;
    auto &c = *context;
    uint64_t bytes = 0; uint32_t block = 0; bool writable = false;
    // A watchdog suppresses further shared-memory/disk data transfers. A
    // completed prefix still needs a real cache flush before handles close.
    auto r = c.geometry(part, bytes, block, writable, true); if (r != Result::OK) return r;
    if (!writable) return Result::Unsupported;
    r = status(c.media[part]->synchronize(c.owner, 0, 0, 0));
    if (r == Result::Quarantined) return c.pin();
    if (r == Result::OK) c.dirty[part] = false;
    return r;
}
Result NativeRMTFSStorage::close(unsigned part) {
    if (!context || part >= RMTFS::PartitionCount || !context->refs[part]) return Result::Invalid;
    if (quarantined()) return Result::Quarantined;
    if (context->refs[part] == 1 && context->dirty[part]) { const auto r = sync(part); if (r != Result::OK) return r; }
    --context->refs[part]; return Result::OK;
}
Result NativeRMTFSStorage::size(unsigned part, uint64_t &bytes) {
    bytes = 0; if (!context) return Result::NotReady;
    uint32_t block = 0; bool writable = false; return context->geometry(part, bytes, block, writable);
}
Result NativeRMTFSStorage::read(unsigned part, uint64_t offset, void *buffer, size_t bytes) {
    return context ? context->blocks.read(part, offset, buffer, bytes) : Result::NotReady;
}
Result NativeRMTFSStorage::write(unsigned part, uint64_t offset, const void *buffer, size_t bytes) {
    return context ? context->blocks.write(part, offset, buffer, bytes) : Result::NotReady;
}
Result NativeRMTFSStorage::readShared(uint64_t physical, void *data, size_t bytes) {
    if (!ready()) return quarantined() ? Result::Quarantined : Result::NotReady;
    if (!data || physical < Shared.address || !range(physical - Shared.address, bytes, Shared.bytes)) return Result::Invalid;
    __asm__ volatile("dmb oshld" ::: "memory");
    memcpy(data, reinterpret_cast<const void *>(context->mapping->getVirtualAddress() + physical - Shared.address), bytes);
    return Result::OK;
}
Result NativeRMTFSStorage::writeShared(uint64_t physical, const void *data, size_t bytes) {
    if (!ready()) return quarantined() ? Result::Quarantined : Result::NotReady;
    if (!data || physical < Shared.address || !range(physical - Shared.address, bytes, Shared.bytes)) return Result::Invalid;
    memcpy(reinterpret_cast<void *>(context->mapping->getVirtualAddress() + physical - Shared.address), data, bytes);
    __asm__ volatile("dsb oshst" ::: "memory"); return Result::OK;
}
bool NativeRMTFSStorage::close() {
    if (!context) return true;
    if (quarantined() || context->prepared || !context->shared.canClose()) return false;
    for (unsigned i = 0; i < RMTFS::PartitionCount; ++i) if (context->refs[i] || context->dirty[i]) return false;
    delete context; context = nullptr; return true;
}
} // namespace Baseband
