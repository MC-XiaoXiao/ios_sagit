// SPDX-License-Identifier: BSD-2-Clause
#include "bridge.h"
#include "smem_reservation.h"

static void put64(unsigned char *p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

/* Called after every final GetMemoryMap, immediately before ExitBootServices.
 * No allocation, console access, SMEM access or device writes are allowed. */
int bridge_smem_finalize(void *tree, const void *map, size_t bytes, size_t stride,
                         uint64_t arena, uint64_t arena_bytes) {
    uint32_t length = 0;
    unsigned char *record = dt_property(tree, "memory-map", "QcomSMEM", &length);
    if (!record || length != 16) return -1;
    put64(record, 0); put64(record + 8, 0);
    if (!bridge_smem_reserved(map, bytes, stride, arena, arena_bytes)) return -1;
    put64(record, BRIDGE_SMEM_BASE); put64(record + 8, BRIDGE_SMEM_BYTES);
    return 0;
}
