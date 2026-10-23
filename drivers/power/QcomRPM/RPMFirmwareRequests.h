// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace QcomPower {
// An observation of the pinned EFI NPA request cache, not an RPM ACK or a
// resource lease. In particular, an absent key does not establish a zero
// hardware vote, and an empty message queue does not establish success:
// this firmware retires error replies through the ordinary completion path.
// Its channelUp byte is a connection-seen latch, not current channel state:
// the pinned callback sets it on connection and never clears it on close.
constexpr const char *FirmwareRequestsProperty="qcom,usb-firmware-rpm";
struct FirmwareRequestValue {
    uint32_t present=0,bytes=0;
    uint64_t value=0;
};
struct FirmwareRequest {
    uint32_t type=0,resource=0,key=0,presence=0; // bit0: resource; bit1: key
    // Presence describes this selected key. A resource-only observation
    // can still contain other keys; it does not prove an empty RPM cache.
    // Active and next requests share the firmware's submitted cache slot.
    FirmwareRequestValue activeOrNextSubmitted{},activeRequested{};
    FirmwareRequestValue sleepSubmitted{},sleepRequested{},nextRequested{};
    uint32_t dirty[3]{},resourceDirty[3]{},reserved[2]{};
};
struct FirmwareRequests {
    uint32_t version=2,bytes=sizeof(FirmwareRequests),count=0,grants=0;
    uint32_t initialized=0,channelUp=0,autocomplete=0,reserved=0;
    uint32_t queueCounts[3]{},setDirty[3]{},padding[2]{};
    FirmwareRequest rows[160]{};
};
static_assert(sizeof(FirmwareRequestValue)==16 && sizeof(FirmwareRequest)==128 &&
    offsetof(FirmwareRequests,rows)==64 && sizeof(FirmwareRequests)==20544,
    "EFI RPM request observation ABI");
}
