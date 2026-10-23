// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandSMEM.h"

namespace Baseband { namespace SMP2P {
// A retained READY=1 is not a new boot completion. Preserve a pre-reset
// baseline and require a cleared/reinitialized peer or an observed SSR change.
class ReadyGate {
    bool fresh = false, running = false;
public:
    void baseline(bool present, const Observation &before) {
        fresh = !present || !(before.state & Ready); running = false;
    }
    void invalidated() { fresh = true; running = false; }
    Result observe(const Observation &now) {
        if (now.restart || !(now.state & Ready)) fresh = true;
        if (now.rising & Fatal) { running = false; return Result::IO; }
        running = fresh && (now.state & Ready) && !(now.state & Fatal);
        return Result::OK;
    }
    bool ready() const { return running; }
};
// Item IO provides aligned read32/store32, release/acquire barriers and one
// outgoing mailbox notification. A controller owns the item for its lifetime.
template<class IO> Result publish(IO &io) {
    uint8_t image[Bytes]{};
    const auto r = initialize(image, sizeof(image));
    if (r != Result::OK) return r;
    // Invalidate the old version before replacing an AP-owned retained item.
    // Publish version/features only after the complete header and entry exist.
    if (!io.store32(4, 0)) return Result::Quarantined;
    io.release();
    for (unsigned offset = 0; offset < Bytes; offset += 4) {
        if (offset == 4) continue;
        if (!io.store32(offset, read32(image + offset))) return Result::Quarantined;
    }
    io.release();
    if (!io.store32(4, read32(image + 4))) return Result::Quarantined;
    io.release();
    return io.notify() ? Result::OK : Result::Quarantined;
}
template<class IO> Result snapshot(IO &io, void *buffer, size_t bytes) {
    if (!buffer || bytes != Bytes) return Result::Invalid;
    auto *out = static_cast<uint8_t *>(buffer);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint32_t version = 0, counts = 0, flags = 0;
        if (!io.read32(4, version) || !io.read32(12, counts) || !io.read32(16, flags)) return Result::IO;
        io.acquire();
        if (!(version & 255)) return Result::NotReady;
        for (unsigned offset = 0; offset < Bytes; offset += 4) {
            uint32_t value = 0;
            // Values are read as aligned words, never as four torn byte loads.
            if (!io.read32(offset, value)) return Result::IO;
            write32(out + offset, value);
        }
        io.acquire();
        uint32_t afterVersion = 0, afterCounts = 0, afterFlags = 0;
        if (!io.read32(4, afterVersion) || !io.read32(12, afterCounts) || !io.read32(16, afterFlags)) return Result::IO;
        if (version == afterVersion && counts == afterCounts && flags == afterFlags &&
            read32(out + 4) == version && read32(out + 12) == counts && read32(out + 16) == flags)
            return Result::OK;
    }
    return Result::Busy;
}
template<class IO> Result acknowledge(IO &io, uint32_t flags) {
    if (flags & ~2u) return Result::Invalid;
    uint32_t before = 0;
    if (!io.read32(16, before)) return Result::IO;
    const uint32_t next = (before & ~2u) | flags;
    if (before == next) return Result::OK;
    if (!io.store32(16, next)) return Result::Quarantined;
    io.release(); return io.notify() ? Result::OK : Result::Quarantined;
}
template<class IO> Result stop(IO &io, bool requested) {
    uint32_t before = 0;
    if (!io.read32(36, before)) return Result::IO;
    const uint32_t next = requested ? before | 1 : before & ~1u;
    if (before == next) return Result::OK;
    if (!io.store32(36, next)) return Result::Quarantined;
    io.release(); return io.notify() ? Result::OK : Result::Quarantined;
}
}} // namespace Baseband::SMP2P
