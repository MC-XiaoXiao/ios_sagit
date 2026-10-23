// SPDX-License-Identifier: GPL-2.0-only
// SMEM v11 global heap / v12 private partitions and SMP2P v1, Linux v6.12.
#pragma once
#include <BasebandProtocol.h>

namespace Baseband {
struct SMEMItem { size_t offset = 0, bytes = 0; bool cached = false; };
// Parses an immutable snapshot taken while holding the SMEM hardware lock.
// Offsets are relative to that snapshot. This does not allocate remotely
// shared items or claim their ownership from a device-tree property.
class SMEMSnapshot {
    const uint8_t *data = nullptr;
    size_t bytes = 0;
    unsigned version = 0;
    static constexpr size_t LegacyTOC = 208, LegacyItems = 512, LegacyHeader = LegacyTOC + 16 * LegacyItems;
    Result partition(size_t offset, size_t length, uint32_t cacheline,
                     uint16_t host0, uint16_t host1, uint16_t item, SMEMItem &out) const {
        if (length < 32 || !range(offset, length, bytes - 4096))
            return Result::Invalid;
        const auto *p = data + offset;
        if (read32(p) != 0x54525024 || read16(p + 4) != host0 ||
            read16(p + 6) != host1 || read32(p + 8) != length)
            return Result::Invalid;
        const uint32_t uncached = read32(p + 12), cached = read32(p + 16);
        if (uncached < 32 || uncached > cached || cached > length ||
            (uncached & 7) || (cached & (cacheline - 1)))
            return Result::Invalid;
        unsigned found = 0;
        SMEMItem selected{};
        size_t pos = 32;
        while (pos < uncached) {
            if (!range(pos, 16, uncached)) return Result::Invalid;
            const auto *e = p + pos;
            const uint32_t n = read32(e + 4);
            const uint16_t pad = read16(e + 8), gap = read16(e + 10);
            if (read16(e) != 0xa5a5 || !n || (n & 7) || (gap & 7) ||
                pad > n || !range(pos + 16, uint64_t(gap) + n, uncached))
                return Result::Invalid;
            if (read16(e + 2) == item) {
                ++found; selected = {offset + pos + 16 + gap, n - pad, false};
            }
            pos += 16 + gap + n;
        }
        if (pos != uncached) return Result::Invalid;
        // Cached entries grow downwards. Their data precedes the header.
        pos = length;
        const size_t header = (16 + cacheline - 1) & ~size_t(cacheline - 1);
        while (pos > cached) {
            if (pos - cached < header) return Result::Invalid;
            const size_t at = pos - header;
            const auto *e = p + at;
            const uint32_t n = read32(e + 4);
            const uint16_t pad = read16(e + 8);
            if (read16(e) != 0xa5a5 || !n || (n & (cacheline - 1)) ||
                pad > n || n > at - cached || read16(e + 10) != header - 16)
                return Result::Invalid;
            if (read16(e + 2) == item) {
                ++found; selected = {offset + at - n, n - pad, true};
            }
            pos = at - n;
        }
        if (pos != cached || found > 1) return Result::Invalid;
        if (!found) return Result::NotReady;
        out = selected; return Result::OK;
    }
public:
    Result bind(const void *snapshot, size_t length, uint32_t physical = 0x86000000) {
        data = nullptr; bytes = 0; version = 0;
        if (!snapshot || length < 8192 || (length & 4095) || length > 16 * 1024 * 1024)
            return Result::Invalid;
        const auto *p = static_cast<const uint8_t *>(snapshot);
        const auto *toc = p + length - 4096;
        if (read32(toc) != 0x434f5424) {
            // Stock SBLs may expose the legacy global allocator. Private
            // partitions are absent; Linux routes every host through this TOC.
            if (length < LegacyHeader || read32(p + 192) != 1 || read32(p + 204) ||
                (read32(p + 64 + 7 * 4) >> 16) != 11) return Result::Unsupported;
            const uint32_t free = read32(p + 196), available = read32(p + 200);
            if (free < LegacyHeader || (free & 7) || !range(free, available, length)) return Result::Invalid;
            for (unsigned i = 0; i < LegacyItems; ++i) {
                const auto *entry = p + LegacyTOC + 16 * i;
                if (!read32(entry)) continue;
                const uint32_t offset = read32(entry + 4), size = read32(entry + 8), aux = read32(entry + 12);
                // A separate auxiliary region needs a separate mapping and
                // physical reservation. Never reinterpret it as the main heap.
                if (read32(entry) != 1 || !size || (offset & 7) || (size & 7) ||
                    (i >= 8 && offset < LegacyHeader) || !range(offset, size, free) ||
                    ((aux & ~3u) && (aux & ~3u) != physical)) return Result::Invalid;
                for (unsigned j = 8; j < i; ++j) {
                    const auto *old = p + LegacyTOC + 16 * j;
                    if (i >= 8 && read32(old) && overlaps(offset, size, read32(old + 4), read32(old + 8))) return Result::Invalid;
                }
            }
            data = p; bytes = length; version = 11; return Result::OK;
        }
        if (read32(toc + 4) != 1) return Result::Unsupported;
        const uint32_t count = read32(toc + 8);
        if (!count || count > (4096 - 32) / 48) return Result::Invalid;
        for (uint32_t i = 0; i < count; ++i) {
            const auto *e = toc + 32 + 48 * i;
            const uint32_t start = read32(e), size = read32(e + 4), line = read32(e + 16);
            if (!size) continue;
            if (!start || (start & 7) || size < 32 ||
                !range(start, size, length - 4096) ||
                line < 8 || line > 4096 || (line & (line - 1)))
                return Result::Invalid;
            for (uint32_t j = 0; j < i; ++j) {
                const auto *old = toc + 32 + 48 * j;
                if (overlaps(start, size, read32(old), read32(old + 4)))
                    return Result::Invalid;
            }
        }
        data = p; bytes = length; version = 12; return Result::OK;
    }
    Result find(uint16_t local, uint16_t remote, uint16_t item, SMEMItem &out) const {
        out = {};
        if (!data) return Result::NotReady;
        if (version == 11) {
            if (item >= LegacyItems) return Result::Invalid;
            const auto *entry = data + LegacyTOC + 16 * item;
            if (!read32(entry)) return Result::NotReady;
            out = {read32(entry + 4), read32(entry + 8), false}; return Result::OK;
        }
        const auto *toc = data + bytes - 4096;
        const uint32_t count = read32(toc + 8);
        const uint8_t *selected = nullptr;
        const uint8_t *global = nullptr;
        for (uint32_t i = 0; i < count; ++i) {
            const auto *e = toc + 32 + 48 * i;
            if (!read32(e + 4)) continue;
            const uint16_t a = read16(e + 12), b = read16(e + 14);
            if (a == 0xfffe && b == 0xfffe) {
                if (global) return Result::Invalid;
                global = e;
            }
            if ((a == local && b == remote) || (a == remote && b == local)) {
                if (selected) return Result::Invalid;
                selected = e;
            }
        }
        if (!selected) selected = global;
        if (!selected) return Result::NotReady;
        return partition(read32(selected), read32(selected + 4), read32(selected + 16),
                         read16(selected + 12), read16(selected + 14), item, out);
    }
};

namespace SMP2P {
constexpr size_t Bytes = 340, EntryBytes = 20, MaxEntries = 16;
constexpr uint32_t Magic = 0x504d5324, SSRFeature = 1;
constexpr uint16_t AP = 0, Modem = 1;
constexpr uint32_t OutboundItem = 435, InboundItem = 428;
constexpr uint32_t Fatal = 1, Ready = 2, Handover = 4, StopAck = 8, ShutdownAck = 128;
struct Observation {
    uint32_t state = 0, rising = 0, falling = 0, outboundFlags = 0;
    bool restart = false, negotiatedSSR = false;
};
class Monitor {
    uint32_t previous = 0;
    bool initialized = false, restartDone = false;
public:
    void reset() { previous = 0; initialized = false; restartDone = false; }
    Result sample(const void *buffer, size_t bytes, Observation &out) {
        out = {};
        // v11 SMEM reports its 8-byte allocation size (344) rather than the
        // 340-byte SMP2P structure; v12 subtracts its padding_data field.
        if (!buffer || (bytes != Bytes && bytes != ((Bytes + 7) & ~size_t(7)))) return Result::Invalid;
        const auto *p = static_cast<const uint8_t *>(buffer);
        if (read32(p) != Magic || p[4] != 1 || read16(p + 8) != Modem ||
            read16(p + 10) != AP || read16(p + 12) != MaxEntries ||
            read16(p + 14) > MaxEntries) return Result::Invalid;
        const uint32_t features = read32(p + 4) >> 8;
        const unsigned count = read16(p + 14);
        bool found = false;
        uint32_t current = 0;
        const char expected[16] = "slave-kernel";
        for (unsigned i = 0; i < count; ++i) {
            const auto *e = p + 20 + EntryBytes * i;
            bool equal = true;
            for (unsigned j = 0; j < 16; ++j)
                if (e[j] != uint8_t(expected[j])) equal = false;
            if (equal) {
                if (found) return Result::Invalid;
                found = true; current = read32(e + 16);
            }
        }
        if (!found) return Result::NotReady;
        const bool done = (read32(p + 16) & 1) != 0, ssr = features & SSRFeature;
        out.restart = initialized && ssr && done != restartDone;
        out.negotiatedSSR = ssr;
        out.outboundFlags = ssr && done ? 2 : 0;
        // First observation also reports asserted lines. The lifecycle must
        // distinguish cold boot admission from a retained-running endpoint.
        const uint32_t old = out.restart ? 0 : previous;
        out.state = current; out.rising = current & ~old; out.falling = old & ~current;
        previous = current; restartDone = done; initialized = true;
        return Result::OK;
    }
};
inline Result initialize(void *buffer, size_t bytes) {
    if (!buffer || bytes != Bytes) return Result::Invalid;
    auto *p = static_cast<uint8_t *>(buffer);
    for (size_t i = 0; i < bytes; ++i) p[i] = 0;
    write32(p, Magic); write32(p + 4, 1 | (SSRFeature << 8));
    write16(p + 8, AP); write16(p + 10, Modem);
    write16(p + 12, MaxEntries); write16(p + 14, 1);
    const char name[] = "master-kernel";
    for (size_t i = 0; i < sizeof(name); ++i) p[20 + i] = name[i];
    return Result::OK;
}
} // namespace SMP2P
} // namespace Baseband
