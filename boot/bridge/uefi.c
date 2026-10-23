// SPDX-License-Identifier: BSD-2-Clause
// UEFI types must be declared before protocol headers.
// clang-format off
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/Rng.h>
#include "bridge.h"
#include "watchdog.h"
#ifdef BRIDGE_FULL_MEMORY
#include "memory_ranges.h"
#endif
#ifdef BRIDGE_SMEM_HANDOFF
int bridge_smem_finalize(void *, const void *, size_t, size_t, uint64_t, uint64_t);
#endif
#ifdef BRIDGE_BASEBAND_HANDOFF
int bridge_baseband_prepare(EFI_BOOT_SERVICES *, void *);
void bridge_baseband_abort(EFI_BOOT_SERVICES *, void *);
int bridge_baseband_finalize(void *, const void *, size_t, size_t, uint64_t, uint64_t);
#endif
#ifdef BRIDGE_DISPLAY_PROBE
void bridge_display_probe(void);
#endif
#ifdef BRIDGE_TOUCH_PREPARE
int bridge_touch_prepare(EFI_BOOT_SERVICES *, void *);
int bridge_touch_finalize(void);
#endif
#ifdef BRIDGE_CPU_OSM_HANDOFF
int bridge_osm_prepare(EFI_BOOT_SERVICES *, void *);
int bridge_osm_finalize(void);
#endif
#ifdef BRIDGE_DISPLAY_HANDOFF
int bridge_display_prepare(EFI_BOOT_SERVICES *, void *, struct boot_args *, uint64_t);
int bridge_display_takeover(void);
#endif
#ifdef BRIDGE_DISPLAY_SMMU_HANDOFF
struct boot_memory_range;
int bridge_display_dma_prepare(EFI_BOOT_SERVICES *, void *);
uint64_t bridge_display_dma_next_arena(uint64_t, uint64_t);
int bridge_display_dma_exclude(struct boot_memory_range *, unsigned *);
#endif
// clang-format on
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
#define ARENA_SIZE BRIDGE_ARENA_SIZE
static unsigned char memory_map[65536] __attribute__((aligned(16)));
static uint64_t align_up(uint64_t n, uint64_t a) { return (n + a - 1) & ~(a - 1); }
#ifdef BRIDGE_DRIVER_IMAGE
extern const unsigned char driver_image_blob[], driver_image_end[];
#endif
#ifdef BRIDGE_GPU_PROBE
static void gpu_probe(const char *phase) {
    // Read only the always-accessible clock controllers. GX may be OFF:
    // touching the GPU/CP or its SMMU here can stall the interconnect.
    static const uint32_t registers[] = {
        0x05065000,0x05065004,0x0506500c,0x05065014,0x0506502c,0x05065038,0x05065130,
        0x05066000,0x05066004,0x05066008,0x05066020,0x05066030,0x05066034,
        0x05066050,0x05066054,0x05066070,0x05066074,0x05066090,0x05066094,
        0x05066098,0x05066100,0x05066104,0x05066120,0x05066124,
        0x050660a0,0x050660a4,
        0x00100000,0x00100004,0x0010000c,0x00100038,0x00171004,
        0x00171010,0x0017100c,0x00171018,0x00146040,0x0015200c,0x00152000
    };
    bridge_puts("[GPU probe] phase="); bridge_puts(phase); bridge_puts(" begin\n");
    for (unsigned i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
        uint32_t value = *(volatile const uint32_t *)(uintptr_t)registers[i];
        __asm__ volatile("dmb oshld" ::: "memory");
        bridge_puts("[GPU probe] address="); bridge_hex(registers[i]);
        bridge_puts(" value="); bridge_hex(value); bridge_puts("\n");
    }
    bridge_puts("[GPU probe] phase="); bridge_puts(phase); bridge_puts(" end\n");
}
#endif
#ifdef BRIDGE_FULL_MEMORY
#ifdef BRIDGE_GPU_COLD_START
int bridge_gpu_ram_safe(uint64_t,uint64_t);
#endif
static uint64_t full_memory_span;
static int publish_memory(void *tree, struct boot_args *ba, UINTN bytes, UINTN ds) {
    static struct boot_memory_range ranges[BOOT_MEMORY_MAX_RANGES];
    unsigned count = 0;
    uint64_t base = ba->physBase, arena_end = base + ARENA_SIZE;
#ifdef BRIDGE_DISPLAY_SMMU_HANDOFF
    if (bridge_display_dma_next_arena(base, ARENA_SIZE) != base) return -1;
#endif
#ifdef BRIDGE_GPU_COLD_START
    if (!bridge_gpu_ram_safe(base,ARENA_SIZE)) return -1;
#endif
    if (boot_memory_add(ranges, &count, base, arena_end)) return -1;
    for (UINTN off = 0; off < bytes; off += ds) {
        EFI_MEMORY_DESCRIPTOR *d = (void *)(memory_map + off);
        if (d->Type != EfiConventionalMemory || (d->Attribute & EFI_MEMORY_RUNTIME)) continue;
        uint64_t start = d->PhysicalStart, end = start + (d->NumberOfPages << 12);
        if (start < base) start = base;
#ifdef BRIDGE_GPU_COLD_START
        // EFI LoaderData holds every cold-start GPU allocation, including
        // PAS-only pages and alignment padding. Reject any contradictory
        // conventional range rather than publishing it to XNU's allocator.
        if (end > start && !bridge_gpu_ram_safe(start,end-start)) return -1;
#endif
        if (boot_memory_add(ranges, &count, start, end)) return -1;
    }
#ifdef BRIDGE_DISPLAY_SMMU_HANDOFF
    if (bridge_display_dma_exclude(ranges, &count) || !count) return -1;
#endif
    uint32_t size;
    unsigned char *data = dt_property(tree, "chosen", "boot-memory-ranges", &size);
    if (!data || size != BOOT_MEMORY_MAX_RANGES * 16) return -1;
    memset(data, 0, size);
    uint64_t usable = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t length = ranges[i].end - ranges[i].start;
        memcpy(data + i * 16, &ranges[i].start, 8);
        memcpy(data + i * 16 + 8, &length, 8);
        usable += length;
    }
    void *p = dt_property(tree, "chosen", "boot-memory-count", &size);
    if (!p || size != 4) return -1;
    memcpy(p, &count, 4);
    p = dt_property(tree, "chosen", "boot-memory-bootstrap-end", &size);
    if (!p || size != 8) return -1;
    memcpy(p, &arena_end, 8);
    uint64_t span = ranges[count - 1].end - base;
    p = dt_property(tree, "chosen", "dram-size", &size);
    if (!p || size != 8) return -1;
    memcpy(p, &span, 8);
    p = dt_property(tree, "memory", "reg", &size);
    if (!p || size != 16) return -1;
    memcpy((unsigned char *)p + 8, &span, 8);
    /* start.s only maps the bootstrap allocation. arm_vm_init expands the
     * physical aperture after parsing the whitelist, using its own page tables. */
    full_memory_span = span;
    ba->memSizeActual = usable;
    return 0;
}
#endif
// Public ChipInfo ABI prefix, through GetSerialNumber. The installed MSM8998
// firmware uses revision 0x10000; later revisions append to this prefix.
// ABI: simple-init/include/chipinfo.h. The installed GUID comes from
// SimpleInit.dec and the firmware driver's InstallMultipleProtocolInterfaces;
// the unused CHIPINFO_PROTOCOL_GUID macro in that header is different.
struct chip_info;
typedef EFI_STATUS (EFIAPI *chip_info_value)(struct chip_info *, UINT32 *);
struct chip_info {
    UINT64 revision;
    VOID *version_methods[2];
    chip_info_value get_chip_id;
    VOID *other_methods[5];
    chip_info_value get_serial_number;
};
_Static_assert(offsetof(struct chip_info, get_chip_id) == 24, "ChipInfo ID ABI");
_Static_assert(offsetof(struct chip_info, get_serial_number) == 72, "ChipInfo serial ABI");
static int platform_identity(EFI_BOOT_SERVICES *bs, void *tree) {
    UINT32 serial_size = 0, chip_size = 0;
    void *serial_property = dt_property(tree, "chosen", "unique-chip-id", &serial_size);
    void *chip_property = dt_property(tree, "chosen", "chip-id", &chip_size);
    if (!serial_property && !chip_property) return 0;
    if (!serial_property || serial_size != 8 || !chip_property || chip_size != 4) return -1;
    UINT64 existing_serial;
    UINT32 existing_chip;
    memcpy(&existing_serial, serial_property, sizeof(existing_serial));
    memcpy(&existing_chip, chip_property, sizeof(existing_chip));
    if (existing_serial && existing_chip) return 0;
    if (existing_serial || existing_chip) return -2;
    EFI_GUID guid = {0xb0760469, 0x970c, 0x487a, {0xa4, 0xb5, 0x28, 0xdb, 0x7b, 0x45, 0xce, 0xf1}};
    struct chip_info *info = 0;
    UINT32 serial = 0, chip = 0;
    EFI_STATUS status = bs->LocateProtocol(&guid, 0, (void **)&info);
    if (EFI_ERROR(status) || !info) return -3;
    bridge_puts("[XNU bridge] ChipInfo revision="); bridge_hex(info->revision);
    bridge_puts("\n");
    if (info->revision < 0x10000 || info->revision > 0x10002 ||
        !info->get_chip_id || !info->get_serial_number) return -4;
    if (EFI_ERROR(info->get_chip_id(info, &chip))) return -5;
    if (EFI_ERROR(info->get_serial_number(info, &serial))) return -6;
    if (bridge_set_platform_identity(tree, serial, chip)) return -7;
    bridge_puts("[XNU bridge] firmware ChipInfo identity supplied for native platform UUID\n");
    return 0;
}
#ifdef BRIDGE_UFS_HANDOFF
int bridge_ufs_prepare(EFI_BOOT_SERVICES *, void *);
int bridge_ufs_takeover(void);
#endif
#ifdef BRIDGE_USB_TOPOLOGY
int bridge_usb_topology_prepare(EFI_SYSTEM_TABLE *, void *);
int bridge_usb_topology_finish(void);
#endif
#ifdef BRIDGE_USB_RPM_SNAPSHOT
int bridge_usb_rpm_prepare(EFI_BOOT_SERVICES *, void *);
int bridge_usb_rpm_finish(void);
#endif
#ifdef BRIDGE_USB_FIRMWARE_STOP
#ifndef BRIDGE_USB_HARDWARE_STOP
#error USB firmware retirement requires post-EFI hardware DMA isolation
#endif
int bridge_usb_firmware_stop(EFI_BOOT_SERVICES *, void *);
int bridge_usb_firmware_stopped(void);
int bridge_usb_firmware_stop_finish(void);
static void usb_firmware_stop_abort(void) __attribute__((noreturn));
static void usb_firmware_stop_abort(void) {
    bridge_puts("[USB firmware] one-way retirement incomplete; retaining boot resources until reset\n");
    platform_watchdog_arm(BRIDGE_WATCHDOG_SECONDS);
    for (;;) __asm__ volatile("wfe");
}
#endif
#ifdef BRIDGE_USB_HARDWARE_STOP
#ifndef BRIDGE_USB_FIRMWARE_STOP
#error USB hardware stop requires pinned firmware software retirement
#endif
int bridge_usb_hardware_stop_prepare(EFI_BOOT_SERVICES *,void *);
int bridge_usb_hardware_stop_finish(void);
#endif
#ifdef BRIDGE_GPU_HANDOFF
int bridge_gpu_prepare(EFI_BOOT_SERVICES *, void *);
uint64_t bridge_gpu_next_arena(uint64_t, uint64_t);
int bridge_gpu_takeover(void *, uint64_t, uint64_t);
#endif
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    if (platform_console_init((void *)dt_blob))
        return EFI_UNSUPPORTED;
    struct kernel_info k;
    uint64_t el, mpidr;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    bridge_puts("\n[XNU bridge] EDK2 entry EL=");
    bridge_hex(el >> 2);
    bridge_puts(" MPIDR=");
    bridge_hex(mpidr);
    bridge_puts("\n");
    if (el != 4 && el != 8)
        return EFI_UNSUPPORTED;
    if (macho_info(kernel_blob, kernel_end - kernel_blob, &k)) {
        bridge_puts("Invalid Mach-O\n");
        return EFI_LOAD_ERROR;
    }
    EFI_STATUS status = bs->SetWatchdogTimer(0, 0, 0, 0);
    if (EFI_ERROR(status))
        return status;
#ifdef BRIDGE_GPU_PROBE
    gpu_probe("before-exit");
#endif
#ifdef BRIDGE_DISPLAY_SMMU_HANDOFF
    // Reserve firmware display tables before GPU/arena allocations can reuse
    // an incorrectly labelled conventional page. Unsupported formats publish
    // no mapper contract and keep the established framebuffer path.
    if (bridge_display_dma_prepare(bs, (void *)dt_blob)) return EFI_DEVICE_ERROR;
#endif
#ifdef BRIDGE_GPU_HANDOFF
    int gpu_status = bridge_gpu_prepare(bs, (void *)dt_blob);
    if (gpu_status) {
        bridge_puts("[GPU handoff] prepare failed: ");bridge_hex((uint64_t)(int64_t)gpu_status);bridge_puts("\n");
        return EFI_DEVICE_ERROR;
    }
#endif
    UINTN bytes = sizeof(memory_map), key, ds;
    UINT32 ver;
    status = bs->GetMemoryMap(&bytes, (void *)memory_map, &key, &ds, &ver);
    if (EFI_ERROR(status))
        return status;
    EFI_PHYSICAL_ADDRESS arena = 0;
    // Reserve the entire advertised arena: XNU cannot overwrite live EFI data.
    for (UINTN off = 0; off < bytes; off += ds) {
        EFI_MEMORY_DESCRIPTOR *d = (void *)(memory_map + off);
        if (d->Type != EfiConventionalMemory)
            continue;
        uint64_t start = align_up(d->PhysicalStart, 0x200000),
                 end = d->PhysicalStart + (d->NumberOfPages << 12);
        bridge_puts("[EFI free] ");
        bridge_hex(start);
        bridge_puts("..");
        bridge_hex(end);
        bridge_puts("\n");
#ifndef BRIDGE_FULL_MEMORY
        if (start < BRIDGE_MIN_PHYS)
            start = align_up(BRIDGE_MIN_PHYS, 0x200000);
#endif
#if defined(BRIDGE_GPU_HANDOFF) || defined(BRIDGE_DISPLAY_SMMU_HANDOFF)
        for (;;) {
#ifdef BRIDGE_DISPLAY_SMMU_HANDOFF
            uint64_t next = bridge_display_dma_next_arena(start, ARENA_SIZE);
#else
            uint64_t next = start;
#endif
#ifdef BRIDGE_GPU_HANDOFF
            next = bridge_gpu_next_arena(next, ARENA_SIZE);
#endif
            if (next == start) break;
            start = next;
            if (start > end || end - start < ARENA_SIZE) break;
        }
#endif
        if (start > end || end - start < ARENA_SIZE)
            continue;
        arena = start;
        status = bs->AllocatePages(AllocateAddress, EfiLoaderData, ARENA_SIZE >> 12, &arena);
        if (!EFI_ERROR(status))
            break;
        arena = 0;
    }
    if (!arena) {
        bridge_puts("No safe contiguous arena for requested capacity\n");
        return EFI_OUT_OF_RESOURCES;
    }
    struct boot_args *ba = 0;
    if (bridge_prepare((void *)arena, ARENA_SIZE, kernel_blob, kernel_end - kernel_blob, dt_blob,
                       dt_end - dt_blob, trust_cache_blob, trust_cache_end - trust_cache_blob,
                       mpidr, &ba, &k)) {
        bs->FreePages(arena, ARENA_SIZE >> 12);
        return EFI_LOAD_ERROR;
    }
    uint64_t dtpa = ba->deviceTreeP - k.virt + arena, bapa = (uintptr_t)ba,
             top = ba->topOfKernelData;
    void *tree = (void *)dtpa;
#ifdef BRIDGE_DRIVER_IMAGE
    const uint64_t driver_bytes = driver_image_end - driver_image_blob;
    const uint64_t driver_pa = align_up(top, 0x4000);
    uint32_t driver_property_bytes = 0;
    void *driver_property = dt_property(tree, "memory-map", "DriverImage", &driver_property_bytes);
    if (!driver_property || driver_property_bytes != 16 ||
        driver_bytes < 8 * 1024 * 1024 || driver_bytes > 64 * 1024 * 1024 ||
        (driver_bytes & 0x3fff) || driver_pa < arena ||
        driver_pa > arena + ARENA_SIZE || driver_bytes > arena + ARENA_SIZE - driver_pa) {
        bridge_puts("[XNU bridge] invalid driver image reservation\n");
        return EFI_LOAD_ERROR;
    }
    memcpy((void *)driver_pa, driver_image_blob, driver_bytes);
    const uint64_t driver_range[2] = {driver_pa, driver_bytes};
    memcpy(driver_property, driver_range, sizeof(driver_range));
    ba->topOfKernelData = top = driver_pa + driver_bytes;
    bridge_puts("[XNU bridge] DriverImage reserved address=");bridge_hex(driver_pa);
    bridge_puts(" bytes=");bridge_hex(driver_bytes);bridge_puts("\n");
#endif
    int identity_status = platform_identity(bs, tree);
    if (identity_status) {
        bridge_puts("[XNU bridge] firmware ChipInfo identity unavailable or invalid: ");
        bridge_hex((uint64_t)(int64_t)identity_status); bridge_puts("\n");
        bs->FreePages(arena, ARENA_SIZE >> 12);
        return EFI_DEVICE_ERROR;
    }
    EFI_GUID gopguid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    status = bs->LocateProtocol(&gopguid, 0, (void **)&gop);
    if (!EFI_ERROR(status) && gop && gop->Mode && gop->Mode->Info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *v = gop->Mode->Info;
        int supported =
            v->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ||
            (v->PixelFormat == PixelBitMask && v->PixelInformation.RedMask == 0xff0000 &&
             v->PixelInformation.GreenMask == 0xff00 && v->PixelInformation.BlueMask == 0xff);
        if (supported && v->PixelsPerScanLine <= 0x3fffffff &&
            !bridge_set_video(ba, gop->Mode->FrameBufferBase, gop->Mode->FrameBufferSize,
                              v->HorizontalResolution, v->VerticalResolution,
                              v->PixelsPerScanLine * 4)) {
            bridge_puts("[XNU bridge] framebuffer base=");
            bridge_hex(gop->Mode->FrameBufferBase);
            bridge_puts(" width=");
            bridge_hex(ba->video[3]);
            bridge_puts(" height=");
            bridge_hex(ba->video[4]);
            bridge_puts(" stride=");
            bridge_hex(ba->video[2]);
            bridge_puts(" BGRX8888\n");
        } else
            bridge_puts("[XNU bridge] GOP framebuffer format / geometry unsupported\n");
    } else
        bridge_puts("[XNU bridge] GOP framebuffer unavailable\n");
#ifdef BRIDGE_DISPLAY_PROBE
    bridge_display_probe();
#endif
#ifdef BRIDGE_DISPLAY_HANDOFF
    if (!gop || !gop->Mode || bridge_display_prepare(bs, tree, ba, gop->Mode->FrameBufferSize))
        return EFI_DEVICE_ERROR;
#endif
    UINT32 seedlen;
    void *seed = dt_property(tree, "chosen", "random-seed", &seedlen);
    EFI_GUID rngguid = EFI_RNG_PROTOCOL_GUID;
    EFI_RNG_PROTOCOL *rng = 0;
    if (!EFI_ERROR(bs->LocateProtocol(&rngguid, 0, (void **)&rng)) && rng && seed) {
        status = rng->GetRNG(rng, 0, seedlen, seed);
        if (EFI_ERROR(status)) {
            bridge_puts("Firmware RNG failed\n");
            return status;
        }
        bridge_puts("[XNU bridge] firmware RNG seed\n");
    } else
        bridge_puts("[XNU bridge] fresh host-generated build entropy\n");
    bridge_puts("[XNU bridge] physical arena=");
    bridge_hex(arena);
    bridge_puts(" bytes=");
    bridge_hex(ARENA_SIZE);
    bridge_puts(" virtual=");
    bridge_hex(k.virt);
    bridge_puts(" entry=");
    bridge_hex(arena + k.entry - k.virt);
    bridge_puts(" DT=");
    bridge_hex(dtpa);
    bridge_puts(" boot_args=");
    bridge_hex(bapa);
    bridge_puts(" top=");
    bridge_hex(top);
    bridge_puts("\n");
#ifdef BRIDGE_USB_TOPOLOGY
    int usb_topology_status = bridge_usb_topology_prepare(st, tree);
    if (usb_topology_status) {
        bridge_puts("[USB topology] installed ACPI topology/resource binding unavailable or invalid: ");
        bridge_hex((uint64_t)(int64_t)usb_topology_status);bridge_puts("\n");
        return EFI_DEVICE_ERROR;
    }
#endif
#ifdef BRIDGE_UFS_HANDOFF
    int ufs_status = bridge_ufs_prepare(bs, tree);
    if (ufs_status) {
        bridge_puts("[UFS handoff] prepare failed: ");bridge_hex((uint64_t)(int64_t)ufs_status);bridge_puts("\n");
        return EFI_DEVICE_ERROR;
    }
#endif
#ifdef BRIDGE_WATCHDOG_TEST
    platform_watchdog_arm(BRIDGE_WATCHDOG_SECONDS);
    bridge_puts("[XNU bridge] watchdog self-test begin\n");
    uint64_t t0, now, freq;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t0));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    for (unsigned int second = 0; second < BRIDGE_WATCHDOG_SECONDS + 3; second++) {
        do {
            __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
        } while (now - t0 < freq * second);
        bridge_puts("[WDT] second=");
        bridge_hex(second);
        bridge_puts(" count=");
        bridge_hex(platform_watchdog_read(0x0c));
        bridge_puts(" enable=");
        bridge_hex(platform_watchdog_read(8));
        bridge_puts(" bite=");
        bridge_hex(platform_watchdog_read(0x14));
        bridge_puts("\n");
    }
    platform_watchdog_arm(0);
    bridge_puts("[XNU bridge] watchdog did not reset; returning to firmware\n");
    bs->FreePages(arena, ARENA_SIZE >> 12);
    return EFI_DEVICE_ERROR;
#endif
#ifdef BRIDGE_USB_FIRMWARE_STOP
    if (bridge_usb_firmware_stop(bs, tree)) {
        bridge_puts("[USB firmware] event/client retirement failed\n");
        if (bridge_usb_firmware_stopped()) usb_firmware_stop_abort();
        return EFI_DEVICE_ERROR;
    }
#endif
#ifdef BRIDGE_USB_RPM_SNAPSHOT
    // Client Stop can change firmware RPM requests. Observe its final cache
    // after retirement, then compare it again across the EFI exit fence.
    if (bridge_usb_rpm_prepare(bs, tree)) {
        bridge_puts("[USB resources] pinned EFI RPM request capture failed\n");
#ifdef BRIDGE_USB_FIRMWARE_STOP
        if (bridge_usb_firmware_stopped()) usb_firmware_stop_abort();
#endif
        return EFI_DEVICE_ERROR;
    }
#endif
#ifdef BRIDGE_USB_HARDWARE_STOP
    if (bridge_usb_hardware_stop_prepare(bs,tree)) {
        bridge_puts("[USB hardware] stop output/binding unavailable\n");
        usb_firmware_stop_abort();
    }
#endif
#ifdef BRIDGE_BASEBAND_HANDOFF
    if (bridge_baseband_prepare(bs, tree)) {
#ifdef BRIDGE_USB_FIRMWARE_STOP
        if (bridge_usb_firmware_stopped()) usb_firmware_stop_abort();
#endif
        return EFI_OUT_OF_RESOURCES;
    }
#endif
#ifdef BRIDGE_TOUCH_PREPARE
    if (bridge_touch_prepare(bs, tree)) {
        bridge_puts("[Touch handoff] preflight failed; sensor remains disabled\n");
    }
#endif
#ifdef BRIDGE_CPU_OSM_HANDOFF
    int osm_status = bridge_osm_prepare(bs, tree);
    if (osm_status) {
        bridge_puts("[OSM handoff] preflight failed: ");
        bridge_hex((uint64_t)(int64_t)osm_status); bridge_puts("\n");
#ifdef BRIDGE_BASEBAND_HANDOFF
        bridge_baseband_abort(bs, tree);
#endif
#ifdef BRIDGE_USB_FIRMWARE_STOP
        if (bridge_usb_firmware_stopped()) usb_firmware_stop_abort();
#endif
        return EFI_UNSUPPORTED;
    }
#endif
    // No EFI allocation/console calls between the final map and ExitBootServices.
    for (unsigned int retry = 0; retry < 3; retry++) {
        bytes = sizeof(memory_map);
        status = bs->GetMemoryMap(&bytes, (void *)memory_map, &key, &ds, &ver);
        if (EFI_ERROR(status)) {
#ifdef BRIDGE_BASEBAND_HANDOFF
            bridge_baseband_abort(bs, tree);
#endif
#ifdef BRIDGE_USB_FIRMWARE_STOP
            if (bridge_usb_firmware_stopped()) usb_firmware_stop_abort();
#endif
            return status;
        }
#ifdef BRIDGE_SMEM_HANDOFF
        if (bridge_smem_finalize(tree, memory_map, bytes, ds, arena, ARENA_SIZE)) {
#ifdef BRIDGE_BASEBAND_HANDOFF
            bridge_baseband_abort(bs, tree);
#endif
#ifdef BRIDGE_USB_FIRMWARE_STOP
            if (bridge_usb_firmware_stopped()) usb_firmware_stop_abort();
#endif
            return EFI_DEVICE_ERROR;
        }
#endif
#ifdef BRIDGE_BASEBAND_HANDOFF
        if (bridge_baseband_finalize(tree, memory_map, bytes, ds, arena, ARENA_SIZE)) {
            bridge_baseband_abort(bs, tree);
#ifdef BRIDGE_USB_FIRMWARE_STOP
            if (bridge_usb_firmware_stopped()) usb_firmware_stop_abort();
#endif
            return EFI_DEVICE_ERROR;
        }
#endif
#ifdef BRIDGE_FULL_MEMORY
        if (publish_memory(tree, ba, bytes, ds)) {
#ifdef BRIDGE_BASEBAND_HANDOFF
            bridge_baseband_abort(bs, tree);
#endif
            return EFI_OUT_OF_RESOURCES;
        }
#endif
        status = bs->ExitBootServices(image, key);
        if (!EFI_ERROR(status))
            break;
        if (status != EFI_INVALID_PARAMETER) {
#ifdef BRIDGE_BASEBAND_HANDOFF
            bridge_baseband_abort(bs, tree);
#endif
#ifdef BRIDGE_USB_FIRMWARE_STOP
            if (bridge_usb_firmware_stopped()) usb_firmware_stop_abort();
#endif
            return status;
        }
    }
    if (EFI_ERROR(status)) {
#ifdef BRIDGE_BASEBAND_HANDOFF
        bridge_baseband_abort(bs, tree);
#endif
#ifdef BRIDGE_USB_FIRMWARE_STOP
        if (bridge_usb_firmware_stopped()) usb_firmware_stop_abort();
#endif
        return status;
    }
    bridge_puts("[XNU bridge] ExitBootServices OK; entering XNU\n");
#ifdef BRIDGE_FULL_MEMORY
    bridge_puts("[Boot memory] span="); bridge_hex(full_memory_span);
    bridge_puts(" usable="); bridge_hex(ba->memSizeActual); bridge_puts("\n");
#endif
    int watchdog_status = platform_watchdog_arm(BRIDGE_WATCHDOG_SECONDS);
    if (watchdog_status)
        bridge_puts("[XNU bridge] hardware watchdog unavailable on this platform\n");
    bridge_puts("[XNU bridge] watchdog seconds=");
    bridge_hex(BRIDGE_WATCHDOG_SECONDS);
    bridge_puts(" enable=");
    bridge_hex(platform_watchdog_read(8));
    bridge_puts("\n");
#ifdef BRIDGE_GPU_PROBE
    gpu_probe("after-exit");
#endif
#ifdef BRIDGE_USB_TOPOLOGY
    if (bridge_usb_topology_finish()) {
        bridge_puts("[USB topology] table or controller resources changed across EFI exit\n");
        for (;;) __asm__ volatile("wfe");
    }
#endif
#ifdef BRIDGE_USB_FIRMWARE_STOP
    if (bridge_usb_firmware_stop_finish()) {
        bridge_puts("[USB firmware] software producer restarted or changed across EFI exit\n");
        for (;;) __asm__ volatile("wfe");
    }
#endif
#ifdef BRIDGE_USB_RPM_SNAPSHOT
    if (bridge_usb_rpm_finish()) {
        bridge_puts("[USB resources] EFI RPM request cache or transport reset fence changed across exit\n");
        for (;;) __asm__ volatile("wfe");
    }
#endif
#ifdef BRIDGE_USB_HARDWARE_STOP
    if (bridge_usb_hardware_stop_finish()) {
        bridge_puts("[USB hardware] inherited halt/reset proof failed; retaining boot memory\n");
        for (;;) __asm__ volatile("wfe");
    }
#endif
#ifdef BRIDGE_DISPLAY_HANDOFF
    if (bridge_display_takeover()) {
        bridge_puts("[Display handoff] post-EFI validation failed\n");
        for (;;) __asm__ volatile("wfe");
    }
#endif

#ifdef BRIDGE_GPU_HANDOFF
    // The preflight snapshot remains available even when takeover fails.
    gpu_status = bridge_gpu_takeover(tree, arena, ARENA_SIZE);
    if (gpu_status) {
        bridge_puts("[GPU handoff] post-EFI validation failed: ");bridge_hex((uint64_t)(int64_t)gpu_status);bridge_puts("\n");
#ifdef BRIDGE_GPU_PROBE
        gpu_probe("takeover-failed");
#endif
        for (;;) __asm__ volatile("wfe");
    }
#endif

#ifdef BRIDGE_UFS_HANDOFF
    ufs_status = bridge_ufs_takeover();
    if (ufs_status) {
        bridge_puts("[UFS handoff] post-EFI validation failed: ");bridge_hex((uint64_t)(int64_t)ufs_status);bridge_puts("\n");
        // EFI services are gone; the independent watchdog returns to firmware.
        for (;;) __asm__ volatile("wfe");
    }
#endif

#ifdef BRIDGE_TOUCH_PREPARE
    int touch_status=bridge_touch_finalize();
    bridge_puts("[Touch handoff] result=");bridge_hex((uint64_t)(int64_t)touch_status);bridge_puts("\n");
#endif
#ifdef BRIDGE_CPU_OSM_HANDOFF
    osm_status = bridge_osm_finalize();
    if (osm_status) {
        bridge_puts("[OSM handoff] initialization failed: ");
        bridge_hex((uint64_t)(int64_t)osm_status); bridge_puts("\n");
        // Never enter XNU or re-enter EFI after a partial CPU clock/CPRh
        // transition. The independent recovery watchdog resets the platform.
        for (;;) __asm__ volatile("wfe");
    }
#endif
    // Clean loaded segments, tree and arguments to PoC before disabling the MMU.
#ifdef BRIDGE_USB_HARDWARE_STOP
    if (bridge_usb_hardware_stop_finish()) {
        bridge_puts("[USB hardware] reset isolation changed before XNU entry\n");
        for (;;) __asm__ volatile("wfe");
    }
#endif
    bridge_clean(arena, top);
    bridge_enter(ba, arena + k.entry - k.virt);
}
