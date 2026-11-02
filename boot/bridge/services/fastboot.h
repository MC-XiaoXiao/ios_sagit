// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <Uefi.h>

// EDK2 EmbeddedPkg's public protocols. Keep the pinned ABI (no EFIAPI).
struct bridge_fastboot_transport {
    EFI_STATUS (*Start)(EFI_EVENT);
    EFI_STATUS (*Stop)(VOID);
    EFI_STATUS (*Send)(UINTN, CONST VOID *, EFI_EVENT *);
    EFI_STATUS (*Receive)(UINTN *, VOID **);
};
struct bridge_fastboot_platform {
    EFI_STATUS (*Init)(VOID);
    VOID (*UnInit)(VOID);
    EFI_STATUS (*FlashPartition)(CHAR8 *, UINTN, VOID *);
    EFI_STATUS (*ErasePartition)(CHAR8 *);
    EFI_STATUS (*GetVar)(CHAR8 *, CHAR8 *);
    EFI_STATUS (*DoOemCommand)(CHAR8 *);
};
enum bridge_payload_id { BRIDGE_KERNEL, BRIDGE_DT, BRIDGE_RAMDISK, BRIDGE_TRUSTCACHE, BRIDGE_PAYLOAD_COUNT };
struct bridge_payload { const void *data; UINTN size; };
struct bridge_fastboot {
    EFI_SYSTEM_TABLE *system;
    struct bridge_fastboot_transport *transport;
    struct bridge_fastboot_platform *platform;
    EFI_EVENT receive_event, fatal_event, timeout_event;
    struct bridge_payload defaults[BRIDGE_PAYLOAD_COUNT], inputs[BRIDGE_PAYLOAD_COUNT];
    void *owned[BRIDGE_PAYLOAD_COUNT], *download;
    UINTN expected, received;
    UINTN reserved_bytes; // Other arena residents supplied by the bridge entry.
    volatile unsigned idle_seconds;
    int running, initialized, hold, boot_requested, receiving, failed;
};

// The bridge owns this service. It has no graphics or input dependency.
EFI_STATUS bridge_fastboot_start(struct bridge_fastboot *, EFI_SYSTEM_TABLE *,
                                 const struct bridge_payload[BRIDGE_PAYLOAD_COUNT]);
EFI_STATUS bridge_fastboot_resume(struct bridge_fastboot *);
EFI_STATUS bridge_fastboot_suspend(struct bridge_fastboot *);
void bridge_fastboot_destroy(struct bridge_fastboot *);
void bridge_fastboot_poll(struct bridge_fastboot *);
int bridge_fastboot_busy(const struct bridge_fastboot *);
// Stop USB before hardware handoff. Inputs stay valid until destroy.
EFI_STATUS bridge_fastboot_prepare_boot(struct bridge_fastboot *);

// Bounded Apple DT validation, also used before calling the bridge's DT walker.
int bridge_payload_dt_valid(const void *, UINTN);
