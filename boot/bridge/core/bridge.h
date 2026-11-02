// SPDX-License-Identifier: BSD-2-Clause
#ifndef XNU_BRIDGE_H
#define XNU_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
#define BRIDGE_STATIC_ASSERT static_assert
#else
#define BRIDGE_STATIC_ASSERT _Static_assert
#endif
// Includes the two signed fingerprint application packets carried in Apple DT.
#define BRIDGE_MAX_DEVICE_TREE_SIZE 0x400000U
#ifndef BRIDGE_ARENA_SIZE
#define BRIDGE_ARENA_SIZE 0x10000000ULL
#endif
BRIDGE_STATIC_ASSERT(BRIDGE_ARENA_SIZE >= 0x4000000ULL && BRIDGE_ARENA_SIZE <= 0x40000000ULL &&
               !(BRIDGE_ARENA_SIZE & 0x1fffffULL), "arena capacity and alignment");
struct boot_args {
    uint16_t Revision, Version;
    uint64_t virtBase, physBase, memSize, topOfKernelData;
    uint64_t video[6];
    uint32_t machineType;
    uint64_t deviceTreeP;
    uint32_t deviceTreeLength;
    char CommandLine[1024];
    uint64_t bootFlags, memSizeActual;
};
BRIDGE_STATIC_ASSERT(offsetof(struct boot_args, virtBase) == 8, "boot ABI");
BRIDGE_STATIC_ASSERT(offsetof(struct boot_args, deviceTreeP) == 96, "DT ABI");
BRIDGE_STATIC_ASSERT(offsetof(struct boot_args, CommandLine) == 108, "command ABI");
BRIDGE_STATIC_ASSERT(offsetof(struct boot_args, bootFlags) == 1136, "flags ABI");
BRIDGE_STATIC_ASSERT(sizeof(struct boot_args) == 1152, "boot_args size");
#undef BRIDGE_STATIC_ASSERT
struct kernel_info {
    uint64_t virt, end, entry;
};
int macho_info(const void *, size_t, struct kernel_info *);
int macho_load(const void *, size_t, const struct kernel_info *, void *, size_t);
int bridge_set_video(struct boot_args *, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t);
int bridge_prepare(void *, size_t, const void *, size_t, const void *, size_t, const void *, size_t, uint64_t,
                   struct boot_args **, struct kernel_info *);
int fdt_get_video(const void *, struct boot_args *);
int fdt_find_arena(const void *, uint64_t, uint64_t, uint64_t, uint64_t *);
void bridge_clean(uint64_t, uint64_t);
void *dt_node_property(void *, const char *, uint32_t *);
void *dt_find_phandle(void *, uint32_t);
int platform_console_init(void *);
int platform_watchdog_arm(unsigned int);
uint32_t platform_watchdog_read(unsigned int);
void *dt_property(void *, const char *, const char *, uint32_t *);
int bridge_set_platform_identity(void *, uint32_t, uint32_t);
void bridge_puts(const char *);
void bridge_hex(uint64_t);
void bridge_enter(struct boot_args *, uint64_t) __attribute__((noreturn));
extern const unsigned char kernel_blob[], kernel_end[], dt_blob[], dt_end[];
extern const unsigned char trust_cache_blob[], trust_cache_end[];
#endif
