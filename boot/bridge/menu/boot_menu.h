// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <Uefi.h>
#include "menu_ui.h"

struct bridge_boot_menu_option {
    struct bridge_menu_label label;
    const CHAR16 *console_title;
    const EFI_GUID *firmware_file; // NULL selects the kernel handoff.
};
// Optional application host hooks. No transport state belongs to the menu.
struct bridge_menu_host {
    void *context;
    // 0: normal, 1: cancel countdown, 2: select kernel, -1: defer activation.
    int (*dispatch)(void *);
    EFI_STATUS (*leave)(void *, int firmware_app);
    void (*returned)(void *);
};
struct bridge_boot_menu_config {
    const CHAR16 *device;
    const struct bridge_boot_menu_option *options;
    unsigned count, initial_selection, seconds; // 1..6 options, 0..30 seconds.
    int escape_selection; // -1 disables the Escape shortcut.
    const struct bridge_menu_host *host;
};

EFI_STATUS bridge_boot_menu_configured(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
                                      const struct bridge_boot_menu_config *config);

// The configured entry point accepts any firmware's FV file GUIDs. Labels use
// printable ASCII and the Chinese glyph subset in assets/menu_font.h; extend
// tools/project/build/menu_assets.py when adding other Chinese characters.
// EFI_SUCCESS selects the kernel. All activity finishes before hardware handoff.
// This wrapper supplies the current EDK2 firmware's four default entries.
EFI_STATUS bridge_boot_menu(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, unsigned seconds);
EFI_STATUS bridge_boot_menu_hosted(EFI_HANDLE, EFI_SYSTEM_TABLE *, unsigned,
                                   const struct bridge_menu_host *);
