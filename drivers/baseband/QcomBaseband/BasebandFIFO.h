// SPDX-License-Identifier: GPL-2.0-only
// GLINK SMEM SPSC ring, based on Linux v6.12 qcom_glink_smem.c.
#pragma once
#include <BasebandProtocol.h>

namespace Baseband {
// Backend provides atomic 32-bit cursor reads/writes, byte copies, acquire /
// release barriers and a mailbox kick. The retained SMEM mapping is uncached;
// substituting ordinary cached RAM without cache maintenance is unsupported.
// All operations are serialized by the channel owner's work loop.
template<class IO> class FIFO {
    IO &io;
    uint32_t length;
    bool failed = false;
    Result cursors(uint32_t &head, uint32_t &tail) {
        if (failed) return Result::Quarantined;
        if (length < 32 || (length & 7) || length > 1024 * 1024 ||
            !io.head(head) || !io.tail(tail)) return Result::Invalid;
        io.acquire();
        if (head >= length || tail >= length || (head & 7) || (tail & 7)) {
            failed = true; return Result::Invalid;
        }
        return Result::OK;
    }
    bool read(uint32_t at, void *data, size_t bytes) {
        const size_t first = bytes < length - at ? bytes : length - at;
        return io.read(at, data, first) && (first == bytes ||
            io.read(0, static_cast<uint8_t *>(data) + first, bytes - first));
    }
    bool write(uint32_t at, const void *data, size_t bytes) {
        const size_t first = bytes < length - at ? bytes : length - at;
        return io.write(at, data, first) && (first == bytes ||
            io.write(0, static_cast<const uint8_t *>(data) + first, bytes - first));
    }
public:
    FIFO(IO &backend, uint32_t capacity) : io(backend), length(capacity) {}
    bool quarantined() const { return failed; }
    Result available(size_t &bytes) {
        bytes = 0; uint32_t h, t; const auto r = cursors(h, t);
        if (r != Result::OK) return r;
        bytes = h >= t ? h - t : length - t + h; return Result::OK;
    }
    Result peek(size_t offset, void *buffer, size_t bytes) {
        uint32_t h, t; auto r = cursors(h, t);
        if (r != Result::OK) return r;
        const size_t n = h >= t ? h - t : length - t + h;
        if ((bytes && !buffer) || !range(offset, bytes, n)) return Result::NotReady;
        if (!bytes) return Result::OK;
        return read(uint32_t((t + offset) % length), buffer, bytes) ? Result::OK : Result::IO;
    }
    Result consume(size_t bytes) {
        uint32_t h, t; auto r = cursors(h, t);
        if (r != Result::OK) return r;
        const size_t n = h >= t ? h - t : length - t + h;
        if (!bytes || (bytes & 7) || bytes > n) return Result::Invalid;
        io.release();
        if (!io.setTail(uint32_t((t + bytes) % length))) {
            failed = true; return Result::Quarantined;
        }
        return Result::OK;
    }
    Result push(const void *buffer, size_t bytes, bool readNotification = false) {
        uint32_t h, t; auto r = cursors(h, t);
        if (r != Result::OK) return r;
        if (!buffer || bytes < 8 || bytes > length - 8) return Result::Invalid;
        const size_t rounded = (bytes + 7) & ~size_t(7);
        const size_t space = t <= h ? length - h + t : t - h;
        // Reserve a full slot to distinguish empty/full plus a slot for
        // READ_NOTIF when normal producers are blocked.
        const size_t reserve = readNotification ? 8 : 16;
        if (readNotification && (bytes != 8 || read16(buffer) != 13))
            return Result::Invalid;
        if (space < reserve || rounded > space - reserve) return Result::Busy;
        if (!write(h, buffer, bytes)) return Result::IO;
        const uint8_t zero[8]{};
        if (rounded > bytes && !write(uint32_t((h + bytes) % length), zero, rounded - bytes))
            return Result::IO;
        io.release();
        if (!io.setHead(uint32_t((h + rounded) % length)) || !io.kick()) {
            // Publishing the head may already have made the packet visible.
            // Never retry it as a new send after an ambiguous mailbox failure.
            failed = true; return Result::Quarantined;
        }
        return Result::OK;
    }
};
} // namespace Baseband
