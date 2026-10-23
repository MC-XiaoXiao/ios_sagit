// SPDX-License-Identifier: GPL-2.0-only
#include <Uefi.h>
extern "C" {
#include "bridge.h"
void *memcpy(void *, const void *, size_t);
}
#include "firmware_reservation.h"
#include "BasebandMemory.h"

extern "C" const unsigned char baseband_firmware_blob[], baseband_firmware_end[];
using Baseband::MemoryHandoff;
using Baseband::Region;
using Baseband::Result;
namespace {
struct State {
    MemoryHandoff record;
    bool allocated = false;
    unsigned char map[65536] __attribute__((aligned(16)));
} state;
int allocate(EFI_BOOT_SERVICES *bs, uint64_t lower, uint64_t upper, uint64_t length, Region &out) {
    UINTN bytes = sizeof(state.map), key = 0, stride = 0; UINT32 version = 0;
    if (EFI_ERROR(bs->GetMemoryMap(&bytes, reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(state.map), &key, &stride, &version)) ||
        stride < 40 || (stride & 7) || bytes > sizeof(state.map) || bytes % stride || !length || (length & 0x3fff)) return -1;
    for (UINTN off = 0; off < bytes; off += stride) {
        const auto *d = state.map + off;
        if (smem_map_u32(d) != EfiConventionalMemory || smem_map_u64(d + 32) >> 63) continue;
        auto start = smem_map_u64(d + 8); const auto pages = smem_map_u64(d + 24);
        if ((start & 4095) || !pages || pages > (UINT64_MAX - start) / 4096) return -1;
        uint64_t end = start + pages * 4096;
        if (start < lower) start = lower;
        if (end > upper) end = upper;
        if (start > UINT64_MAX - 0x3fff) continue;
        start = (start + 0x3fff) & ~UINT64_C(0x3fff);
        if (start >= end || length > end - start) continue;
        EFI_PHYSICAL_ADDRESS address = start;
        if (EFI_ERROR(bs->AllocatePages(AllocateAddress, EfiLoaderData, length / 4096, &address))) continue;
        if (address != start) { bs->FreePages(address, length / 4096); return -1; }
        out = {start, length}; return 0;
    }
    return -1;
}
void clear(void *tree) {
    uint32_t bytes = 0;
    auto *record = static_cast<unsigned char *>(dt_property(tree, "chosen", "qcom,baseband-handoff", &bytes));
    if (record && (bytes == MemoryHandoff::Bytes || bytes == MemoryHandoff::CatalogRecordBytes)) Baseband::write32(record + 8, 0);
    const char *names[] = {"QcomMPSS", "QcomMBA", "QcomBBMetadata", "QcomBBFirmware", "QcomRMTFS"};
    for (auto name : names) {
        auto *p = static_cast<unsigned char *>(dt_property(tree, "memory-map", name, &bytes));
        if (p && bytes == 16) { Baseband::write64(p, 0); Baseband::write64(p + 8, 0); }
    }
}
}
extern "C" int bridge_baseband_prepare(EFI_BOOT_SERVICES *bs, void *tree) {
    if (!bs || !tree || state.allocated) return -1;
    uint32_t bytes = 0;
    const auto *record = dt_property(tree, "chosen", "qcom,baseband-handoff", &bytes);
    if (state.record.decode(record, bytes, false) != Result::OK ||
        uint64_t(baseband_firmware_end - baseband_firmware_blob) != state.record.imageBytes()) return -1;
    if (allocate(bs, 0xa0000000, 0xc0000000, 0x4000, state.record.layout.metadata)) return -1;
    if (allocate(bs, 0x80000000, UINT64_C(1) << 32, state.record.imageBytes(), state.record.image)) {
        bs->FreePages(state.record.layout.metadata.address, state.record.layout.metadata.bytes / 4096);
        state.record.layout.metadata = {}; return -1;
    }
    memcpy(reinterpret_cast<void *>(state.record.image.address), baseband_firmware_blob, state.record.image.bytes);
    // Staging memory is separate from the bootstrap arena. Clean its WB EFI
    // mapping before a later XNU CPU mapping reads the immutable source bytes.
    bridge_clean(state.record.image.address, state.record.image.bytes);
    state.allocated = true; clear(tree); return 0;
}
extern "C" void bridge_baseband_abort(EFI_BOOT_SERVICES *bs, void *tree) {
    if (!bs || !tree) return;
    clear(tree);
    if (state.allocated) {
        bs->FreePages(state.record.image.address, state.record.image.bytes / 4096);
        bs->FreePages(state.record.layout.metadata.address, state.record.layout.metadata.bytes / 4096);
        state.allocated = false;
    }
    state.record.layout.metadata = {}; state.record.image = {}; state.record.arena = {}; state.record.flags = 0;
    uint32_t length = 0;
    auto *p = dt_property(tree, "chosen", "qcom,baseband-handoff", &length);
    if (p && length == state.record.wireBytes()) state.record.encode(p, length, false);
}
extern "C" int bridge_baseband_finalize(void *tree, const void *map, size_t bytes, size_t stride,
                                         uint64_t arena, uint64_t arena_bytes) {
    clear(tree);
    if (!state.allocated) return -1;
    auto record = state.record;
    record.arena = {arena, arena_bytes}; record.flags = MemoryHandoff::Verified;
    if (!record.valid() || !bridge_smem_reserved(map, bytes, stride, arena, arena_bytes) ||
        !bridge_firmware_region(map, bytes, stride, 0x88f00000, 0x200000, EfiReservedMemoryType) ||
        !bridge_firmware_region(map, bytes, stride, record.layout.modem.address, record.layout.modem.bytes, EfiReservedMemoryType) ||
        !bridge_firmware_region(map, bytes, stride, record.layout.mba.address, record.layout.mba.bytes, EfiReservedMemoryType) ||
        !bridge_firmware_region(map, bytes, stride, record.layout.metadata.address, record.layout.metadata.bytes, EfiLoaderData) ||
        !bridge_firmware_region(map, bytes, stride, record.image.address, record.image.bytes, EfiLoaderData) ||
        !bridge_firmware_region(map, bytes, stride, arena, arena_bytes, EfiLoaderData)) return -1;
    uint32_t length = 0;
    void *p = dt_property(tree, "chosen", "qcom,baseband-handoff", &length);
    if (!p || length != record.wireBytes()) return -1;
    const char *names[] = {"QcomMPSS", "QcomMBA", "QcomBBMetadata", "QcomBBFirmware", "QcomRMTFS"};
    const Region ranges[] = {record.layout.modem, record.layout.mba, record.layout.metadata, record.image, {0x88f00000, 0x200000}};
    for (unsigned i = 0; i < 5; ++i) {
        auto *range = static_cast<unsigned char *>(dt_property(tree, "memory-map", names[i], &length));
        if (!range || length != 16) { clear(tree); return -1; }
        Baseband::write64(range, ranges[i].address); Baseband::write64(range + 8, ranges[i].bytes);
    }
    if (record.encode(p, record.wireBytes()) != Result::OK) { clear(tree); return -1; }
    return 0;
}
