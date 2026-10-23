// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandBoot.h"

namespace Baseband {
// Native Board and offline fault tests share this ordering. Hooks perform
// real operations; no default-success implementation is linked into the KEXT.
template<class IO> class Admission {
    IO &io;
    bool retained = false, pinned = false;
    Result pin() { pinned = true; io.pin(); return Result::Quarantined; }
public:
    explicit Admission(IO &backend) : io(backend) {}
    bool held() const { return retained; }
    bool quarantined() const { return pinned; }
    Result acquire(const BootLayout &layout) {
        if (pinned) return Result::Quarantined;
        if (retained) return Result::Busy;
        if (!layout.valid()) return Result::Invalid;
        auto r = io.acquireResources();
        if (r != Result::OK) return r == Result::Quarantined || io.hasAmbiguity() ? pin() : r;
        retained = true;
        r = io.probeSecure();
        const Region regions[] = {layout.modem, layout.mba, layout.metadata};
        for (const auto &region : regions) if (r == Result::OK) r = io.confirmHost(region);
        if (r == Result::OK) r = io.mapConfirmedMemory();
        if (r == Result::OK) r = io.initializeSignals();
        if (r == Result::OK) return r;
        if (r == Result::Quarantined || io.hasAmbiguity()) return pin();
        io.closeSignals();
        if (!io.closeMemory() || io.releaseResources() != Result::OK) return pin();
        retained = false; return r;
    }
    // Boot calls this after reset/drain and successful Host reclamation of
    // every target. The CPU ownership ledger checks that independently.
    Result release() {
        if (pinned) return Result::Quarantined;
        if (!retained) return Result::NotReady;
        io.disconnectRoutes(); io.closeSignals();
        if (!io.closeMemory() || io.releaseResources() != Result::OK) return pin();
        retained = false; return Result::OK;
    }
};
} // namespace Baseband
