// SPDX-License-Identifier: BSD-2-Clause
#include "bridge.h"
#include "watchdog.h"
extern unsigned char raw_image_start[], raw_image_end[];
void raw_main(const void *firmware_fdt) __attribute__((noreturn));
void raw_main(const void *firmware_fdt) {
    uint64_t arena = 0, mpidr;
    struct boot_args *ba = 0;
    struct kernel_info k;
    if (platform_console_init((void *)dt_blob))
        for (;;)
            __asm__ volatile("wfe");
    bridge_puts("\n[XNU bridge] ARM64 Image entry\n");
    platform_watchdog_arm(BRIDGE_WATCHDOG_SECONDS);
#ifdef BRIDGE_UFS_HANDOFF
    // Warm ownership verification requires live EFI protocols and a successful
    // ExitBootServices fence. Raw ARM64 Image entry has neither.
    bridge_puts("[XNU bridge] UFS handoff requires the EFI entry point\n");
    for (;;) __asm__ volatile("wfe");
#endif
#ifdef BRIDGE_GPU_HANDOFF
    bridge_puts("[XNU bridge] GPU handoff requires the EFI producer fence\n");
    for (;;) __asm__ volatile("wfe");
#endif
#ifdef BRIDGE_DRIVER_IMAGE
    bridge_puts("[XNU bridge] driver image reservation requires the EFI entry point\n");
    for (;;) __asm__ volatile("wfe");
#endif
#ifdef BRIDGE_USB_FIRMWARE_STOP
    bridge_puts("[XNU bridge] USB firmware retirement requires the EFI entry point\n");
    for (;;) __asm__ volatile("wfe");
#endif
    if (fdt_find_arena(firmware_fdt, BRIDGE_ARENA_SIZE, (uintptr_t)raw_image_start,
                       (uintptr_t)raw_image_end, &arena)) {
        bridge_puts("[XNU bridge] no safe arena in firmware FDT\n");
        for (;;)
            __asm__ volatile("wfe");
    }
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    if (bridge_prepare((void *)arena, BRIDGE_ARENA_SIZE, kernel_blob, kernel_end - kernel_blob, dt_blob,
                       dt_end - dt_blob, trust_cache_blob, trust_cache_end - trust_cache_blob,
                       mpidr, &ba, &k)) {
        bridge_puts("[XNU bridge] invalid kernel / XNU DT\n");
        for (;;)
            __asm__ volatile("wfe");
    }
    if (!fdt_get_video(firmware_fdt, ba)) {
        bridge_puts("[XNU bridge] firmware simple-framebuffer passed to XNU\n");
    }
    bridge_puts("[XNU bridge] raw physical arena=");
    bridge_hex(arena);
    bridge_puts(" bytes=");
    bridge_hex(BRIDGE_ARENA_SIZE);
    bridge_puts("\n");
    bridge_clean(arena, ba->topOfKernelData);
    bridge_enter(ba, arena + k.entry - k.virt);
}
