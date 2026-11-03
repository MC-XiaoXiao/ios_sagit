/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _DISPLAY_BACKLIGHT_H
#define _DISPLAY_BACKLIGHT_H
#include <stdint.h>

namespace DisplayBacklight {
constexpr const char *Function = "DisplayBacklight";
enum Operation : uint32_t { Claim, Release, Query, SetBrightness, SetPower };
// In-kernel platform function: p1=Request*, p2=the claiming IOService*.
// A single display owns changes; no unprivileged SPMI register interface.
struct Request {
    uint32_t version = 1, size = sizeof(Request);
    Operation operation = Query;
    uint32_t identifier = 0, value = 0;
    uint32_t brightness = 0, appliedBrightness = 0, power = 0, valid = 0;
};
// Coherent IORegistry snapshot, shared by the driver and diagnostic clients.
// Invalid state contains last confirmed values, not a hardware readback proof.
constexpr const char *StateProperty = "DisplayBacklightState";
struct State {
    uint32_t version = 1, size = sizeof(State);
    uint32_t brightness = 0, appliedBrightness = 0, power = 0, valid = 0;
    uint32_t lastError = 0, reserved = 0;
};
static_assert(sizeof(State) == 32, "backlight snapshot ABI");
}
#endif
