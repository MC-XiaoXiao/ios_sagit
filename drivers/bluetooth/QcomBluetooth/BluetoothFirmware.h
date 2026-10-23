// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "BluetoothH4.h"

namespace Bluetooth {
constexpr uint16_t DownloadOpcode = 0xfc00;
constexpr size_t FirmwareSegmentBytes = 243;
enum class FirmwareType : uint8_t { Patch = 1, NVM = 2 };
struct FirmwareView {
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    FirmwareType type = FirmwareType::Patch;
    uint8_t downloadMode = 0;
};
struct SoCVersion {
    uint32_t product = 0, soc = 0;
    uint16_t patch = 0, rom = 0;
};

inline Result parseVersionEvent(const uint8_t* p, size_t bytes, SoCVersion& version)
{
    if (!validPacket(p, bytes, false) || bytes != 17 || p[0] != Event || p[1] != 0xff || p[3] != 0
        || p[4] != 2)
        return Result::Invalid;
    SoCVersion found { read32(p + 5), read32(p + 13), read16(p + 9), read16(p + 11) };
    if (!found.soc || !found.rom)
        return Result::Invalid;
    version = found;
    return Result::Success;
}

// Structural parsing is separate from trust, board selection and upload.
// Assets must also be hash-pinned by their owner and match the real ROM/NVM.
// Container type 4 and ELF images are deliberately unsupported in this profile.
inline Result parseFirmware(const uint8_t* data, size_t bytes, FirmwareType type, FirmwareView& view)
{
    view = { };
    if (!data || bytes < 4 || bytes > 0xffffffU + 4U)
        return Result::Invalid;
    const uint32_t header = read32(data);
    if (uint8_t(header) != uint8_t(type))
        return Result::Unsupported;
    if ((header >> 8) != bytes - 4)
        return Result::Invalid;
    uint8_t mode = 0;
    if (type == FirmwareType::Patch) {
        if (bytes < 28)
            return Result::Invalid;
        // Do not invent semantics for the signing/total_size fields. The
        // declared code length must fit the enclosing, exact-size TLV.
        if (read32(data + 8) > bytes - 28)
            return Result::Invalid;
        mode = data[14];
        if (mode > 3)
            return Result::Unsupported;
    } else if (type == FirmwareType::NVM) {
        size_t pos = 4;
        if (pos == bytes)
            return Result::Invalid;
        while (pos < bytes) {
            if (bytes - pos < 12)
                return Result::Invalid;
            const size_t length = read16(data + pos + 2);
            if (length > bytes - pos - 12)
                return Result::Invalid;
            pos += 12 + length;
        }
    } else
        return Result::Unsupported;
    view = { data, bytes, type, mode };
    return Result::Success;
}

struct FirmwareSegment {
    size_t offset = 0, bytes = 0;
    bool last = false, vendorEvent = false, commandComplete = false;
};
inline Result nextFirmwareSegment(const FirmwareView& view, size_t offset, FirmwareSegment& segment)
{
    if (!view.data || !view.bytes || offset >= view.bytes || offset % FirmwareSegmentBytes
        || view.downloadMode > 3)
        return Result::Invalid;
    const size_t remaining = view.bytes - offset;
    const size_t bytes = remaining < FirmwareSegmentBytes ? remaining : FirmwareSegmentBytes;
    const bool last = bytes == remaining;
    // WCN3990 modes: bit 0 skips VSE; bit 1 skips CC on intermediate
    // segments. The final segment always needs the real vendor response.
    segment = { offset, bytes, last, last || !(view.downloadMode & 1), last || !(view.downloadMode & 2) };
    return Result::Success;
}
inline Result encodeFirmwareSegment(const FirmwareView& view, const FirmwareSegment& segment, uint8_t* output,
    size_t capacity, size_t& written)
{
    written = 0;
    FirmwareSegment expected;
    if (nextFirmwareSegment(view, segment.offset, expected) != Result::Success
        || segment.bytes != expected.bytes || segment.last != expected.last || !output
        || capacity < expected.bytes + 6)
        return Result::Invalid;
    output[0] = Command;
    write16(output + 1, DownloadOpcode);
    output[3] = uint8_t(expected.bytes + 2);
    output[4] = 0x1e;
    output[5] = uint8_t(expected.bytes);
    for (size_t i = 0; i < expected.bytes; ++i)
        output[6 + i] = view.data[expected.offset + i];
    written = expected.bytes + 6;
    return Result::Success;
}
inline Result validateDownloadEvent(const uint8_t* p, size_t bytes)
{
    if (!validPacket(p, bytes, false) || bytes != 6 || p[0] != Event || p[1] != 0xff || p[3] || p[4] != 4)
        return Result::Invalid;
    return p[5] ? Result::Fault : Result::Success;
}

// The transport bits are prepared offline, without changing address, RF or
// calibration tags. The kernel rechecks them before touching any resource.
inline bool transportNVM(const FirmwareView& view)
{
    if (!view.data || view.type != FirmwareType::NVM || view.bytes < 4)
        return false;
    bool hci = false, sleep = false;
    for (size_t pos = 4; pos < view.bytes;) {
        if (view.bytes - pos < 12)
            return false;
        const auto tag = read16(view.data + pos), length = read16(view.data + pos + 2);
        if (length > view.bytes - pos - 12)
            return false;
        const auto* p = view.data + pos + 12;
        if (tag == 17) {
            if (hci || length < 3 || !(p[0] & 0x80) || p[2] != 0x11)
                return false;
            hci = true;
        } else if (tag == 27) {
            if (sleep || !length || !(p[0] & 1))
                return false;
            sleep = true;
        }
        pos += 12 + length;
    }
    return hci && sleep;
}
inline bool localAddress(const FirmwareView& view, const uint8_t* canonical)
{
    if (!canonical || !view.data || view.type != FirmwareType::NVM || view.bytes < 4 || (canonical[0] & 1))
        return false;
    bool found = false;
    uint8_t any = 0, all = 0xff;
    for (unsigned i = 0; i < 6; ++i) {
        any |= canonical[i];
        all &= canonical[i];
    }
    if (!any || all == 0xff)
        return false;
    for (size_t pos = 4; pos < view.bytes;) {
        if (view.bytes - pos < 12)
            return false;
        const auto tag = read16(view.data + pos), length = read16(view.data + pos + 2);
        if (length > view.bytes - pos - 12)
            return false;
        if (tag == 2) {
            if (found || length != 6)
                return false;
            for (unsigned i = 0; i < 6; ++i)
                if (view.data[pos + 12 + i] != canonical[5 - i])
                    return false;
            found = true;
        }
        pos += 12 + length;
    }
    return found;
}
}
