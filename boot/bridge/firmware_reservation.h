// SPDX-License-Identifier: BSD-2-Clause
#ifndef BRIDGE_FIRMWARE_RESERVATION_H
#define BRIDGE_FIRMWARE_RESERVATION_H
#include "smem_reservation.h"

/* Complete coverage of a requested region by one EFI type, with no conflicting
 * or runtime descriptor. No allocation or EFI calls occur during this check. */
static inline int bridge_firmware_region(const void *map, size_t bytes, size_t stride,
                                         uint64_t address, uint64_t length, uint32_t type) {
    if (!map || stride < 40 || stride > 4096 || (stride & 7) || !bytes || bytes > 65536 ||
        bytes % stride || !length || address > UINT64_MAX - length) return 0;
    const unsigned char *raw = (const unsigned char *)map;
    const uint64_t end = address + length;
    for (size_t off = 0; off < bytes; off += stride) {
        const unsigned char *d = raw + off;
        const uint64_t start = smem_map_u64(d + 8), pages = smem_map_u64(d + 24);
        if ((start & 4095) || !pages || pages > (UINT64_MAX - start) / 4096) return 0;
        if (start < end && start + pages * 4096 > address &&
            (smem_map_u32(d) != type || (smem_map_u64(d + 32) >> 63))) return 0;
    }
    uint64_t covered = address;
    for (size_t scan = 0; scan < bytes / stride && covered < end; ++scan) {
        uint64_t next = covered;
        for (size_t off = 0; off < bytes; off += stride) {
            const unsigned char *d = raw + off;
            const uint64_t start = smem_map_u64(d + 8), finish = start + smem_map_u64(d + 24) * 4096;
            if (smem_map_u32(d) == type && start <= covered && finish > next) next = finish;
        }
        if (next == covered) return 0;
        covered = next;
    }
    return covered >= end;
}
#endif
