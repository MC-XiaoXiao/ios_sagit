// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <WLANProtocol.h>
namespace WLAN {
struct BoardView { const uint8_t *data = nullptr; uint32_t length = 0; };
// ath10k board-2 container, selected by an exact caller-pinned identity. Never
// fall back to the first entry or to calibration from another board.
inline Result selectBoard(const void *buffer,size_t bytes,const char *identity,size_t identityBytes,BoardView &out) {
    out = {};
    constexpr char magic[] = "QCA-ATH10K-BOARD";
    if (!buffer || !identity || !identityBytes || identityBytes > 200 || bytes < sizeof(magic) || bytes > 16 * 1024 * 1024)
        return Result::Invalid;
    const auto *p = static_cast<const uint8_t *>(buffer);
    for (unsigned i = 0; i < sizeof(magic); ++i) if (p[i] != uint8_t(magic[i])) return Result::Invalid;
    size_t position = (sizeof(magic) + 3) & ~size_t(3);
    bool selected = false; BoardView candidate{};
    while (position < bytes) {
        if (bytes - position < 8) return Result::Invalid;
        const uint32_t type = read32(p + position), length = read32(p + position + 4);
        position += 8;
        if (length > bytes - position || ((uint64_t(length) + 3) & ~3ULL) > bytes - position) return Result::Invalid;
        if (type == 0) {
            size_t offset = 0; bool named = false, match = false, dataSeen = false;
            while (offset < length) {
                if (length - offset < 8) return Result::Invalid;
                const auto *record = p + position + offset;
                const uint32_t kind = read32(record), count = read32(record + 4);
                offset += 8;
                const uint64_t padded = (uint64_t(count) + 3) & ~3ULL;
                if (count > length - offset || padded > length - offset) return Result::Invalid;
                if (kind == 0) {
                    if (named || dataSeen) return Result::Invalid;
                    named = true; match = count == identityBytes;
                    if (match) for (unsigned i = 0; i < count; ++i) match &= record[8 + i] == uint8_t(identity[i]);
                } else if (kind == 1) {
                    if (!named || dataSeen || !count || count > 1024 * 1024) return Result::Invalid;
                    dataSeen = true;
                    if (match) {
                        if (selected) return Result::Invalid;
                        selected = true; candidate = {record + 8,count};
                    }
                }
                offset += size_t(padded);
            }
            if (!named || !dataSeen) return Result::Invalid;
        }
        position += (size_t(length) + 3) & ~size_t(3);
    }
    if (!selected) return Result::NotReady;
    out = candidate;
    return Result::Success;
}
}
