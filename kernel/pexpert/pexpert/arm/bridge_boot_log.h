/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _PEXPERT_ARM_BRIDGE_BOOT_LOG_H_
#define _PEXPERT_ARM_BRIDGE_BOOT_LOG_H_
#include <stdint.h>
#define BRIDGE_BOOT_LOG_BYTES (256u * 1024u)
#define BRIDGE_BOOT_LOG_MAGIC 0x474c424du /* MBLG, little endian */
/* An opaque /chosen property. Both producers and consumers are little endian. */
struct bridge_boot_log {
    uint32_t magic, version, used, dropped;
    uint8_t bytes[BRIDGE_BOOT_LOG_BYTES];
};
static inline int bridge_boot_log_valid(const struct bridge_boot_log *log, unsigned size) {
    return log && size == sizeof(*log) && log->magic == BRIDGE_BOOT_LOG_MAGIC &&
        log->version == 1 && log->used <= BRIDGE_BOOT_LOG_BYTES;
}
static inline void bridge_boot_log_putc(struct bridge_boot_log *log, uint8_t c) {
    log->magic = BRIDGE_BOOT_LOG_MAGIC; log->version = 1;
    if (log->used < BRIDGE_BOOT_LOG_BYTES) log->bytes[log->used++] = c;
    else if (log->dropped != UINT32_MAX) log->dropped++;
}
#endif
