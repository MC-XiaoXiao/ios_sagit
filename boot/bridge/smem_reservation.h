// SPDX-License-Identifier: BSD-2-Clause
#ifndef BRIDGE_SMEM_RESERVATION_H
#define BRIDGE_SMEM_RESERVATION_H
#include <stddef.h>
#include <stdint.h>

#define BRIDGE_SMEM_BASE UINT64_C(0x86000000)
#define BRIDGE_SMEM_BYTES UINT64_C(0x200000)

static inline uint64_t smem_map_u64(const unsigned char *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static inline uint32_t smem_map_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* Check the actual final EFI map, without EFI calls or allocations. SMEM is
 * shared RAM, never device memory or an address range inferred from a DT name.
 * Require complete EfiReservedMemoryType (0) coverage; even an overlapping
 * conventional/runtime descriptor invalidates that proof. This establishes
 * exclusion from the XNU allocator, not modem ownership or authentication. */
static inline int bridge_smem_reserved(const void *map, size_t bytes, size_t stride,
                                       uint64_t arena, uint64_t arena_bytes) {
    if (!map || stride < 40 || stride > 4096 || (stride & 7) || !bytes ||
        bytes > 65536 || bytes % stride || bytes / stride > 4096 ||
        !arena_bytes || arena > UINT64_MAX - arena_bytes) return 0;
    const uint64_t end = BRIDGE_SMEM_BASE + BRIDGE_SMEM_BYTES;
    if (arena < end && arena + arena_bytes > BRIDGE_SMEM_BASE) return 0;
    const unsigned char *raw = (const unsigned char *)map;
    for (size_t off = 0; off < bytes; off += stride) {
        const unsigned char *d = raw + off;
        const uint64_t start = smem_map_u64(d + 8), pages = smem_map_u64(d + 24);
        if ((start & 4095) || !pages || pages > (UINT64_MAX - start) / 4096) return 0;
        if (start < end && start + pages * 4096 > BRIDGE_SMEM_BASE && smem_map_u32(d) != 0)
            return 0;
    }
    /* EFI descriptors need not be sorted. Extend a coverage frontier and
     * reject holes; bounded scans also handle adjacent reserved descriptors. */
    uint64_t covered = BRIDGE_SMEM_BASE;
    for (size_t scan = 0; scan < bytes / stride && covered < end; ++scan) {
        uint64_t next = covered;
        for (size_t off = 0; off < bytes; off += stride) {
            const unsigned char *d = raw + off;
            if (smem_map_u32(d) != 0) continue;
            const uint64_t start = smem_map_u64(d + 8), finish = start + smem_map_u64(d + 24) * 4096;
            if (start <= covered && finish > next) next = finish;
        }
        if (next == covered) return 0;
        covered = next;
    }
    return covered >= end;
}
#endif
