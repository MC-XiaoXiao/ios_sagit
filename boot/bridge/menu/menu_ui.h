// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

#define BRIDGE_MENU_MAX_ITEMS 6

enum bridge_menu_icon { MENU_ICON_BOOT, MENU_ICON_USB, MENU_ICON_SHELL, MENU_ICON_FIRMWARE };
struct bridge_menu_label {
    const uint16_t *title;
    const uint16_t *detail;
    enum bridge_menu_icon icon;
};
struct bridge_menu_view {
    const uint16_t *device;
    const struct bridge_menu_label *items;
    unsigned count, selected;
    int remaining; // -1 means automatic boot was cancelled.
    unsigned total_seconds;
    int returned;
};
struct bridge_menu_surface {
    uint32_t *pixels; // GOP Blt pixels: little-endian B, G, R, reserved.
    unsigned width, height, stride; // stride is in pixels, independent of width.
};

// CPU renderer, independent of EFI protocols and board hardware. No allocation.
// Call synchronously: one bounded glyph cache is shared between redraws.
int bridge_menu_render(struct bridge_menu_surface *surface, const struct bridge_menu_view *view);
