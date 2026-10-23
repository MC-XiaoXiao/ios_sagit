// SPDX-License-Identifier: BSD-2-Clause
#pragma once
// EFI Qualcomm Clock protocol prefix, through CalculateClockFrequency.
typedef struct bridge_clock_protocol BridgeClock;
struct bridge_clock_protocol {
    UINT64 version;
    EFI_STATUS (EFIAPI *get_id)(BridgeClock *,const CHAR8 *,UINTN *);
    EFI_STATUS (EFIAPI *enable)(BridgeClock *,UINTN);
    void *disable,*is_enabled;
    EFI_STATUS (EFIAPI *is_on)(BridgeClock *,UINTN,BOOLEAN *);
    void *set_frequency;
    EFI_STATUS (EFIAPI *get_frequency)(BridgeClock *,UINTN,UINT32 *);
    EFI_STATUS (EFIAPI *calculate_frequency)(BridgeClock *,UINTN,UINT32 *);
};
