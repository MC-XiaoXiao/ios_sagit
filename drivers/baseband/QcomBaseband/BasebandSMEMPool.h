// SPDX-License-Identifier: GPL-2.0-only
// Serialized live allocator, Linux v6.12 drivers/soc/qcom/smem.c wire ABI.
#pragma once
#include "BasebandSMEM.h"

namespace Baseband {
// IO lock/unlock serializes all AP clients, then acquires TCSR hardware lock 3.
// Metadata reads happen while locked. Payloads/FIFO cursors are deliberately
// not copied: another processor may update them independently of this lock.
// write() changes unpublished bytes; store32() is an aligned publication.
// A failed publication is ambiguous and permanently quarantines this object.
template<class IO> class SMEMPool {
    IO &io;
    uint32_t bytes;
    uint32_t physical;
    bool pinned = false;
    uint8_t toc[4096]{};
    static constexpr uint16_t Global = 0xfffe;
    static constexpr uint32_t Items = 512, LegacyTOC = 208, LegacyHeader = 8400;
    struct Partition {
        uint32_t offset = 0, size = 0, line = 0, uncached = 0, cached = 0;
        bool legacy = false;
    };
    Result read(uint32_t offset, void *p, size_t n) {
        return range(offset, n, bytes) && io.read(offset, p, n) ? Result::OK : Result::IO;
    }
    Result table(uint16_t host, Partition &out) {
        if (bytes < 12288 || (bytes & 4095) || bytes > 16 * 1024 * 1024 ||
            (host >= 20 && host != Global) || host == 0) return Result::Invalid;
        auto r = read(bytes - 4096, toc, sizeof(toc)); if (r != Result::OK) return r;
        if (read32(toc) != 0x434f5424) {
            uint8_t h[208]; r = read(0, h, sizeof(h)); if (r != Result::OK) return r;
            if ((read32(h + 92) >> 16) != 11 || read32(h + 192) != 1 || read32(h + 204))
                return Result::Unsupported;
            const uint32_t free = read32(h + 196), available = read32(h + 200);
            if (free < LegacyHeader || (free & 7) || !range(free, available, bytes)) return Result::Invalid;
            out = {0, bytes, 8, free, available, true}; return Result::OK;
        }
        if (read32(toc + 4) != 1) return Result::Unsupported;
        const auto count = read32(toc + 8);
        if (!count || count > (4096 - 32) / 48) return Result::Invalid;
        const uint8_t *privatePart = nullptr, *globalPart = nullptr;
        for (unsigned i = 0; i < count; ++i) {
            const auto *p = toc + 32 + 48 * i;
            const uint32_t at = read32(p), size = read32(p + 4), line = read32(p + 16);
            if (!size) continue;
            if (!at || (at & 7) || size < 32 || !range(at, size, bytes - 4096) ||
                line < 8 || line > 4096 || (line & (line - 1))) return Result::Invalid;
            for (unsigned j = 0; j < i; ++j) {
                const auto *old = toc + 32 + 48 * j;
                if (overlaps(at, size, read32(old), read32(old + 4))) return Result::Invalid;
            }
            const auto a = read16(p + 12), b = read16(p + 14);
            if (a == Global && b == Global) {
                if (globalPart) return Result::Invalid;
                globalPart = p;
            }
            if (host != Global && ((a == 0 && b == host) || (b == 0 && a == host))) {
                if (privatePart) return Result::Invalid;
                privatePart = p;
            }
        }
        const auto *selected = privatePart ? privatePart : globalPart;
        if (!selected) return Result::NotReady;
        out.offset = read32(selected); out.size = read32(selected + 4); out.line = read32(selected + 16);
        uint8_t h[32]; r = read(out.offset, h, sizeof(h)); if (r != Result::OK) return r;
        if (read32(h) != 0x54525024 || read16(h + 4) != read16(selected + 12) ||
            read16(h + 6) != read16(selected + 14) || read32(h + 8) != out.size) return Result::Invalid;
        out.uncached = read32(h + 12); out.cached = read32(h + 16);
        if (out.uncached < 32 || (out.uncached & 7) || out.uncached > out.cached ||
            out.cached > out.size || (out.cached & (out.line - 1))) return Result::Invalid;
        return Result::OK;
    }
    Result find(const Partition &part, uint16_t item, SMEMItem &out) {
        out = {};
        unsigned found = 0;
        uint8_t seen[Items / 8]{};
        auto mark = [&](uint16_t id) {
            if (id >= Items || (seen[id / 8] & (1u << (id % 8)))) return false;
            seen[id / 8] |= 1u << (id % 8); return true;
        };
        if (part.legacy) {
            // Read just the TOC, not the entire live payload heap. The
            // 4 KiB span arrays also detect overlap between dynamic items.
            uint32_t offsets[Items]{}, sizes[Items]{};
            for (unsigned i = 0; i < Items; ++i) {
                uint8_t e[16]; auto r = read(LegacyTOC + i * 16, e, sizeof(e));
                if (r != Result::OK) return r;
                if (!read32(e)) continue;
                const auto offset = read32(e + 4), size = read32(e + 8);
                if (read32(e) != 1 || !size || ((offset | size) & 7) ||
                    !range(offset, size, part.uncached) ||
                    ((read32(e + 12) & ~3u) && (read32(e + 12) & ~3u) != physical) ||
                    (i >= 8 && offset < LegacyHeader)) return Result::Invalid;
                for (unsigned j = 8; j < i; ++j)
                    if (i >= 8 && overlaps(offset, size, offsets[j], sizes[j])) return Result::Invalid;
                offsets[i] = offset; sizes[i] = size;
                if (i == item) { found = 1; out = {offset, size, false}; }
            }
        } else {
            uint32_t at = 32;
            while (at < part.uncached) {
                uint8_t e[16];
                if (!range(at, sizeof(e), part.uncached)) return Result::Invalid;
                auto r = read(part.offset + at, e, sizeof(e)); if (r != Result::OK) return r;
                const auto size = read32(e + 4);
                const auto pad = read16(e + 8), gap = read16(e + 10), id = read16(e + 2);
                if (read16(e) != 0xa5a5 || !mark(id) || !size || (size & 7) || (gap & 7) || pad > size ||
                    !range(at + 16, uint64_t(gap) + size, part.uncached)) return Result::Invalid;
                if (id == item) { ++found; out = {part.offset + at + 16 + gap, size - pad, false}; }
                at += 16 + gap + size;
            }
            if (at != part.uncached) return Result::Invalid;
            at = part.size;
            const uint32_t header = (16 + part.line - 1) & ~(part.line - 1);
            while (at > part.cached) {
                uint8_t e[16];
                if (at - part.cached < header) return Result::Invalid;
                const uint32_t h = at - header;
                auto r = read(part.offset + h, e, sizeof(e)); if (r != Result::OK) return r;
                const auto size = read32(e + 4);
                const auto pad = read16(e + 8), gap = read16(e + 10), id = read16(e + 2);
                if (read16(e) != 0xa5a5 || !mark(id) || !size || (size & (part.line - 1)) ||
                    pad > size || size > h - part.cached || gap != header - 16) return Result::Invalid;
                if (id == item) { ++found; out = {part.offset + h - size, size - pad, true}; }
                at = h - size;
            }
            if (at != part.cached) return Result::Invalid;
        }
        return found ? Result::OK : Result::NotReady;
    }
    bool zero(uint32_t offset, uint32_t length) {
        const uint8_t zeros[64]{};
        while (length) {
            const auto n = length > sizeof(zeros) ? sizeof(zeros) : length;
            if (!io.write(offset, zeros, n)) return false;
            offset += n; length -= n;
        }
        return true;
    }
    Result allocate(const Partition &part, uint16_t item, uint32_t size, SMEMItem &out) {
        const uint32_t aligned = (size + 7) & ~7u;
        const uint32_t need = aligned + (part.legacy ? 0 : 16);
        const uint32_t available = part.legacy ? part.cached : part.cached - part.uncached;
        if (need > available) return Result::NoMemory;
        const uint32_t offset = part.offset + part.uncached + (part.legacy ? 0 : 16);
        if (!range(offset, aligned, bytes)) return Result::Invalid;
        // All new payload/padding is initialized before any peer can find it.
        if (!zero(offset, aligned)) return Result::IO;
        uint8_t header[16]{};
        if (part.legacy) {
            write32(header + 4, offset); write32(header + 8, aligned);
            const uint32_t entry = LegacyTOC + item * 16;
            if (!io.write(entry + 4, header + 4, 12)) return Result::IO;
            io.release();
            if (!io.store32(entry, 1) || !io.store32(196, part.uncached + aligned) ||
                !io.store32(200, part.cached - aligned)) { pinned = true; return Result::Quarantined; }
        } else {
            write16(header, 0xa5a5); write16(header + 2, item); write32(header + 4, aligned);
            write16(header + 8, uint16_t(aligned - size));
            if (!io.write(part.offset + part.uncached, header, sizeof(header))) return Result::IO;
            io.release();
            if (!io.store32(part.offset + 12, part.uncached + need)) { pinned = true; return Result::Quarantined; }
        }
        io.release();
        out = {offset, part.legacy ? aligned : size, false}; return Result::OK;
    }
public:
    SMEMPool(IO &backend, uint32_t length, uint32_t base = 0x86000000) : io(backend), bytes(length), physical(base) {}
    bool quarantined() const { return pinned; }
    Result item(uint16_t host, uint16_t id, uint32_t allocateBytes, SMEMItem &out, bool &created) {
        out = {}; created = false;
        if (pinned) return Result::Quarantined;
        if (id >= Items || (allocateBytes && (id < 8 || allocateBytes > 65536))) return Result::Invalid;
        auto r = io.lock(); if (r != Result::OK) return r;
        io.acquire();
        Partition part{};
        r = table(host, part);
        if (r == Result::OK) r = find(part, id, out);
        if (r == Result::OK && allocateBytes && (out.cached ||
            (part.legacy ? out.bytes != ((allocateBytes + 7) & ~7u) : out.bytes != allocateBytes))) r = Result::Invalid;
        if (r == Result::NotReady && allocateBytes && part.size) {
            r = allocate(part, id, allocateBytes, out); created = r == Result::OK;
        }
        // Even a read must not report success after losing/unlocking another
        // processor's hardware lock. The native backend never busts locks.
        if (!io.unlock()) { pinned = true; r = Result::Quarantined; }
        if (r != Result::OK) { out = {}; created = false; }
        return r;
    }
};
} // namespace Baseband
