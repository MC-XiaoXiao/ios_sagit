// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "ADSPProtocol.h"
#include "../../baseband/QcomBaseband/BasebandSMEM.h"
#include <SMEMProtocol.h>

namespace ADSP {
struct MemoryLayout {
    Baseband::SMEMItem descriptor, tx, rx, smp;
    uint16_t smpHost = QcomSMEMProtocol::GlobalHost;
};
inline bool matchesMapping(const QcomSMEMProtocol::Request &r, const Baseband::SMEMItem &item) {
    return r.mapping && !(uintptr_t(r.mapping) & 7) && !r.flags &&
        r.offset == item.offset && r.length == item.bytes;
}
inline Result discoverMemory(const void *snapshot, size_t bytes, MemoryLayout &out) {
    out = {};
    if (!snapshot || bytes < 8192 || bytes > 16 * 1024 * 1024 || (bytes & 4095)) return Result::Invalid;
    // GLINK item IDs are per remote host in private partitions. A global v11
    // heap cannot establish that item 478 belongs to ADSP rather than modem.
    // Do not inherit the shared SMEM parser's legacy/global fallback here.
    if (read32(static_cast<const uint8_t *>(snapshot) + bytes - 4096) != 0x434f5424)
        return Result::Unsupported;
    Baseband::SMEMSnapshot view;
    Result result = view.bind(snapshot, bytes);
    if (result != Result::OK) return result;
    // The common parser permits global fallback when a host partition is
    // absent. GLINK IDs alone do not identify their remote host, so require
    // an actual AP/ADSP partition before any find(0, 2) lookup.
    const auto *toc = static_cast<const uint8_t *>(snapshot) + bytes - 4096;
    bool privatePartition = false;
    for (unsigned i = 0; i < read32(toc + 8); ++i) {
        const auto *entry = toc + 32 + 48 * i;
        const auto a = read16(entry + 12), b = read16(entry + 14);
        if (read32(entry + 4) && ((!a && b == 2) || (!b && a == 2))) privatePartition = true;
    }
    if (!privatePartition) return Result::Unsupported;
    Baseband::SMEMItem items[4]{};
    const uint16_t ids[] = {478, 479, 480, 429};
    uint16_t smpHost = 2;
    for (unsigned i = 0; result == Result::OK && i < 4; ++i) {
        result = view.find(0, 2, ids[i], items[i]);
        if (i == 3 && result == Result::NotReady) {
            smpHost = QcomSMEMProtocol::GlobalHost;
            result = view.find(smpHost, smpHost, ids[i], items[i]);
        }
        if (result == Result::OK && items[i].cached) result = Result::Unsupported;
        for (unsigned j = 0; result == Result::OK && j < i; ++j)
            if (Baseband::overlaps(items[i].offset, items[i].bytes, items[j].offset, items[j].bytes))
                result = Result::Invalid;
    }
    if (result != Result::OK) return result;
    if (items[0].bytes != 32 || (items[3].bytes != 340 && items[3].bytes != 344) ||
        items[1].bytes < 1024 || items[1].bytes > 65536 || (items[1].bytes & 7) ||
        items[2].bytes < 1024 || items[2].bytes > 65536 || (items[2].bytes & 7)) return Result::Invalid;
    out = {items[0], items[1], items[2], items[3], smpHost};
    return Result::OK;
}
class RemoteState {
    bool bound = false;
    uint32_t restart = 0;
    static Result parse(const void *data, size_t bytes, uint32_t &flags, uint32_t &state) {
        if (!data || (bytes != 340 && bytes != 344)) return Result::Invalid;
        const auto *p = static_cast<const uint8_t *>(data);
        if (read32(p) != 0x504d5324 || p[4] != 1 || !(read32(p + 4) & 0x100) ||
            read16(p + 8) != 2 || read16(p + 10) || read16(p + 12) != 16 ||
            read16(p + 14) > 16) return Result::Invalid;
        bool found = false;
        static constexpr char expected[16] = "slave-kernel";
        for (unsigned i = 0; i < read16(p + 14); ++i) {
            const auto *entry = p + 20 + i * 20;
            bool match = true;
            for (unsigned j = 0; j < 16; ++j) if (entry[j] != uint8_t(expected[j])) match = false;
            if (match) {
                if (found) return Result::Invalid;
                found = true; state = read32(entry + 16);
            }
        }
        if (!found) return Result::NotReady;
        flags = read32(p + 16);
        return !(state & 1) && (state & 2) ? Result::OK : Result::NotReady;
    }
public:
    Result bind(const void *data, size_t bytes) {
        if (bound) return Result::Busy;
        uint32_t flags = 0, state = 0;
        Result r = parse(data, bytes, flags, state);
        if (r == Result::OK) { restart = flags & 1; bound = true; }
        return r;
    }
    Result sample(const void *data, size_t bytes) const {
        if (!bound) return Result::NotReady;
        uint32_t flags = 0, state = 0;
        Result r = parse(data, bytes, flags, state);
        if (r != Result::OK) return r;
        return (flags & 1) == restart ? Result::OK : Result::Stale;
    }
};
}
