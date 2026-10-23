// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <BasebandProtocol.h>
#include <ClockControllerProtocol.h>

namespace Baseband {
struct ResourcePolicy {
    uint32_t cx = 0, mx = 0, qdss = 0;
    static bool level(uint32_t value) {
        const uint32_t allowed[] = {16, 32, 48, 64, 128, 192, 256, 320, 384, 416, 448, 512};
        for (auto v : allowed) if (value == v) return true;
        return false;
    }
    // Linux MSS boot asks both proxy domains for INT_MAX; MSM8998 rpmpd
    // clamps that to BINNING (512). Retain that initial boot demand and
    // the firmware's maximum QDSS state (4), rather than accepting an
    // unqualified lower corner before the modem has handed resources over.
    bool valid() const { return cx == 512 && mx == 512 && qdss == 4; }
};

// One Board owns this lease on its work loop. The policy must come from the
// admitted board profile; a compatible string never supplies voltage levels.
// All proxy clocks remain retained until reset/drain. A later handover may
// shorten that interval only after the actual modem SMP2P handover event.
template<class IO> class ResourceLease {
    IO &io;
    ResourcePolicy policy;
    uint64_t token = 0;
    uint16_t retained = 0;
    bool active = false, pinned = false;
    Result failure = Result::OK;
    static constexpr unsigned Steps = 14;
    static bool ambiguous(Result r) { return r == Result::IO || r == Result::Timeout || r == Result::Quarantined || r == Result::Stale; }
    Result quarantine(Result r) { pinned = true; failure = r; return Result::Quarantined; }
    Result state(ClockController::Operation op) {
        auto value = token;
        const auto r = io.request(ClockController::GCC, op, 0, value, 0);
        if (r != Result::OK) return r;
        if (op == ClockController::RetainModemState) {
            if (!value) return Result::Quarantined;
            token = value;
        } else if (value != token) return Result::Quarantined;
        if (op == ClockController::ReleaseModemState) token = 0;
        return Result::OK;
    }
    Result step(unsigned at, bool on) {
        using namespace ClockController;
        Controller controller = RPM;
        Operation operation = SetDomainLevel;
        uint32_t id = 0, set = 0;
        uint64_t value = 0;
        if (at < 4) { id = 0x1000 + at / 2; set = at % 2; value = on ? (at < 2 ? policy.cx : policy.mx) : 0; }
        else if (at < 8) { id = at < 6 ? 0x3000 : 0x3001; set = at % 2; value = on ? (at < 6 ? 1 : policy.qdss) : 0; operation = SetModemClockVote; }
        else {
            const uint32_t clocks[] = {172, 171, 36, 173, 175, 174};
            id = clocks[at - 8]; controller = GCC; operation = on ? RetainClock : ReleaseClock;
        }
        const auto requested = value;
        auto r = io.request(controller, operation, id, value, set);
        // Backends report their acknowledged aggregate. A successful RPC with
        // less than our requested floor does not cover the upcoming Q6 boot.
        if (r == Result::OK && controller == RPM && (value < requested ||
            (at < 4 ? (value && !ResourcePolicy::level(uint32_t(value))) || value > 512 : value > (at < 6 ? 1u : 4u))))
            r = Result::Quarantined;
        return r;
    }
    Result unwind() {
        if (pinned) return Result::Quarantined;
        if (!token) return Result::OK;
        auto r = state(ClockController::ValidateModemState);
        if (r != Result::OK) return quarantine(r);
        if ((r = io.cold()) != Result::OK) return quarantine(r);
        for (unsigned i = Steps; i-- > 0;) if (retained & (1u << i)) {
            if ((r = step(i, false)) != Result::OK) return quarantine(r);
            retained &= ~(1u << i);
        }
        if ((r = state(ClockController::ReleaseModemState)) != Result::OK) return quarantine(r);
        active = false; return Result::OK;
    }
public:
    ResourceLease(IO &backend, const ResourcePolicy &p) : io(backend), policy(p) {}
    bool quarantined() const { return pinned; }
    bool held() const { return token || retained || active; }
    bool ready() const { return active && !pinned; }
    Result lastFailure() const { return failure; }
    Result acquire() {
        if (pinned) return Result::Quarantined;
        if (held()) return Result::Busy;
        if (!policy.valid()) return Result::Invalid;
        auto r = state(ClockController::RetainModemState);
        if (r != Result::OK) return ambiguous(r) ? quarantine(r) : r;
        r = io.cold();
        for (unsigned i = 0; r == Result::OK && i < Steps; ++i) {
            r = step(i, true);
            if (r == Result::OK) retained |= 1u << i;
        }
        if (r != Result::OK) {
            failure = r;
            if (ambiguous(r)) return quarantine(r);
            return unwind() == Result::OK ? r : Result::Quarantined;
        }
        active = true; return Result::OK;
    }
    Result validate() {
        if (pinned) return Result::Quarantined;
        if (!active) return Result::NotReady;
        const auto r = state(ClockController::ValidateModemState);
        return r == Result::OK ? r : quarantine(r);
    }
    Result release() { return unwind(); }
};
} // namespace Baseband
