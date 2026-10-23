// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandBoot.h"

namespace Baseband {
// CPU accesses use an independent ownership ledger. Updating Boot's logical
// state alone cannot grant writes into an MSS-only page. The Board initializes
// this ledger only after all three real SCM HLOS confirmations complete.
template<class IO> class MemoryAccess {
    IO &io;
    BootLayout layout{};
    Ownership owners[3]{Ownership::Unknown, Ownership::Unknown, Ownership::Unknown};
    bool initialized = false, pinned = false;
    Region region(unsigned i) const { return i == 0 ? layout.modem : i == 1 ? layout.mba : layout.metadata; }
    int index(uint64_t address, size_t bytes) const {
        if (!initialized || pinned || !bytes) return -1;
        for (unsigned i = 0; i < 3; ++i) {
            const auto r = region(i);
            if (address >= r.address && range(address - r.address, bytes, r.bytes)) return int(i);
        }
        return -1;
    }
    bool writable(int i) const { return i >= 0 && (owners[i] == Ownership::Host || owners[i] == Ownership::Shared); }
public:
    explicit MemoryAccess(IO &backend) : io(backend) {}
    Result confirm(const BootLayout &memory) {
        if (initialized || pinned || !memory.valid()) return Result::Invalid;
        layout = memory; initialized = true;
        for (auto &o : owners) o = Ownership::Host;
        return Result::OK;
    }
    Result assigned(const Region &r, Ownership from, Ownership to) {
        const int i = index(r.address, r.bytes);
        if (i < 0 || region(unsigned(i)).address != r.address || region(unsigned(i)).bytes != r.bytes ||
            owners[i] != from || to == Ownership::Unknown) return Result::Invalid;
        owners[i] = to; return Result::OK;
    }
    void quarantine() { pinned = true; for (auto &o : owners) o = Ownership::Unknown; }
    bool canClose() const {
        if (pinned) return false;
        if (!initialized) return true;
        for (auto o : owners) if (o != Ownership::Host) return false;
        return true;
    }
    bool copy(uint64_t address, const uint8_t *source, size_t bytes) {
        const auto i = index(address, bytes);
        return source && writable(i) && io.copy(unsigned(i), address - region(unsigned(i)).address, source, bytes);
    }
    bool zero(uint64_t address, size_t bytes) {
        if (!bytes) return !pinned && initialized;
        const auto i = index(address, bytes);
        return writable(i) && io.zero(unsigned(i), address - region(unsigned(i)).address, bytes);
    }
    bool publish(uint64_t address, size_t bytes) {
        const auto i = index(address, bytes);
        return writable(i) && io.publish(unsigned(i), address - region(unsigned(i)).address, bytes);
    }
};
} // namespace Baseband
