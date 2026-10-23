// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerHardware.h>
namespace Adreno {
struct NoCContract {
    enum : uint32_t { ExclusivePort = 1, RoutingPinned = 2, Required = 3 };
    uint32_t version, flags;
    uint64_t average, peak;
    uint32_t port, reserved;
    bool valid() const {
        return version == 1 && flags == Required && port == 1 && !reserved && average && average <= peak &&
               peak <= uint64_t(UINT32_MAX - 999) * 8;
    }
};
static_assert(sizeof(NoCContract) == 32, "GPU NoC handoff ABI");
// BIMC GFX3D is AP-owned: program master port 1 locally in bypass mode.
// Its DDR path uses the shared BIMC clock and EBI RPM slave, not a remote
// GFX master vote. Backend requires actual CP halt and VBIF drain for writes.
template <class Backend> class GPUInterconnect {
    using Status = QcomPower::Status;
    Backend &backend;
    bool clockHeld = false, bandwidthHeld = false, qosHeld = false, granted = false, uncertain = false;
    uint32_t saved = 0;
    uint64_t average = 0, peakRate = 0;
    Status changed(Status s) {
        if (s == QcomPower::TimedOut || s == QcomPower::Stale || s == QcomPower::Failed)
            uncertain = true;
        return s;
    }
    Status mode(uint32_t value) {
        const auto before = backend.read(Enable);
        if (before == UINT32_MAX)
            return QcomPower::Failed;
        backend.write(Enable, (before & ~1U) | (value & 1));
        const auto after = backend.read(Enable);
        return after != UINT32_MAX && (after & 1) == (value & 1) ? QcomPower::OK : QcomPower::Failed;
    }

  public:
    // BIMC base 0x01000000; M_REG_BASE 0x8000 + port1 * 0x4000 + BKE_EN 0x300.
    static constexpr uint32_t Enable = 0xc300;
    explicit GPUInterconnect(Backend &b) : backend(b) {}
    bool empty() const { return !uncertain && !clockHeld && !bandwidthHeld && !qosHeld; }
    Status acquire(const NoCContract &c) {
        using namespace QcomPower;
        if (!empty())
            return Busy;
        if (!c.valid())
            return Invalid;
        average = c.average;
        peakRate = c.peak / 8 + (c.peak % 8 != 0);
        if (peakRate < 19200000)
            peakRate = 19200000;
        peakRate = ((peakRate + 999) / 1000) * 1000;
        uint64_t maximum = 0;
        auto s = backend.limit(0x2002, true, maximum);
        if (s != OK)
            return s;
        if (peakRate > maximum)
            return Invalid;
        s = backend.limit(0x2104, false, maximum);
        if (s != OK)
            return s;
        if (average > maximum)
            return Invalid;
        s = backend.isolated();
        if (s != OK)
            return s;
        // Sleep peers remain at their declared boot floors. The shared RPM
        // policy derives the additional BIMC floor from all average traffic.
        s = changed(backend.vote(0x2002, 1, true, 0));
        if (s != OK)
            return s;
        s = changed(backend.vote(0x2104, 1, false, 0));
        if (s != OK)
            return s;
        s = changed(backend.vote(0x2002, 0, true, peakRate));
        if (s != OK)
            return s;
        clockHeld = true;
        s = changed(backend.vote(0x2104, 0, false, average));
        if (s != OK)
            return s;
        bandwidthHeld = true;
        saved = backend.read(Enable);
        if (saved == UINT32_MAX)
            return Failed;
        qosHeld = true;
        s = mode(0);
        if (s != OK)
            return s;
        granted = true;
        return check();
    }
    Status check() {
        using namespace QcomPower;
        if (uncertain)
            return Stale;
        if (!granted || !qosHeld)
            return NotReady;
        uint64_t value = 0;
        auto s = backend.get(0x2002, true, value);
        if (s != OK)
            return s;
        if (value < peakRate)
            return Stale;
        s = backend.get(0x2104, false, value);
        if (s != OK)
            return s;
        if (value < average)
            return Stale;
        const auto current = backend.read(Enable);
        return current != UINT32_MAX && !(current & 1) ? OK : Stale;
    }
    Status release() {
        using namespace QcomPower;
        granted = false;
        if (uncertain)
            return Stale;
        if (empty())
            return OK;
        auto s = backend.isolated();
        if (s != OK)
            return s;
        if (qosHeld) {
            s = mode(saved);
            if (s != OK)
                return s;
            qosHeld = false;
        }
        if (bandwidthHeld) {
            s = changed(backend.vote(0x2104, 0, false, 0));
            if (s != OK)
                return s;
            bandwidthHeld = false;
        }
        if (clockHeld) {
            s = changed(backend.vote(0x2002, 0, true, 0));
            if (s != OK)
                return s;
            clockHeld = false;
        }
        return OK;
    }
};
} // namespace Adreno
