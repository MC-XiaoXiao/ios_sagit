// SPDX-License-Identifier: GPL-2.0-only
// MSM8998 MSS TCSR ports from Linux v6.12 msm8998.dtsi/qcom_q6v5_mss.c.
#pragma once
#include <BasebandProtocol.h>

namespace Baseband { namespace AXI {
constexpr uint64_t Base = 0x01f60000;
constexpr uint32_t Bytes = 0x20000, ResetID = 108;
constexpr uint32_t Ports[] = {0x3000, 0x5000, 0x4000}; // Q6, modem, non-cache.
constexpr uint32_t Request = 0, Acknowledge = 4, Idle = 8;

inline bool registerOffset(uint32_t offset, bool write) {
    for (auto port : Ports)
        if (offset == port + Request || (!write && (offset == port + Acknowledge || offset == port + Idle)))
            return true;
    return false;
}

// The Board holds CX/MX, proxy/active clocks and exclusive MSS reset ownership
// throughout this operation. A complete reset/drain proof is required before
// reclaiming modem pages. Unlike a best-effort shutdown, a failed halt never
// permits freeing memory or dropping the resource lease. All requests stay
// asserted until every port acknowledges and the dedicated reset is read back.
template<class IO> class Controller {
    IO &io;
    bool pinned = false;
    Result failure = Result::OK;
    Result quarantine(Result why) { pinned = true; failure = why; return Result::Quarantined; }
    Result state(uint32_t reg, bool &set) {
        uint32_t value = 0;
        if (!io.read(reg, value)) return Result::IO;
        if (value > 1) return Result::Invalid;
        set = value != 0; return Result::OK;
    }
    Result wait(uint32_t port, bool requireAck) {
        const auto start = io.nowUS();
        for (unsigned poll = 0; poll <= 1000; ++poll) {
            bool idle = false, ack = !requireAck;
            auto r = state(port + Idle, idle);
            if (r == Result::OK && requireAck) r = state(port + Acknowledge, ack);
            if (r != Result::OK) return r;
            if (idle && ack) return Result::OK;
            if (io.nowUS() - start >= 100000 || poll == 1000) break;
            io.delayUS(100);
        }
        return Result::Timeout;
    }
public:
    explicit Controller(IO &backend) : io(backend) {}
    bool quarantined() const { return pinned; }
    Result lastFailure() const { return failure; }
    // Read-only admission: seeing an idle bus without asserted reset is not
    // proof of a stopped remote. No inherited running modem is taken over.
    Result cold() {
        if (pinned) return Result::Quarantined;
        bool reset = false;
        auto r = io.resetState(reset);
        if (r != Result::OK) return r;
        if (!reset) return Result::NotReady;
        for (auto port : Ports) {
            bool idle = false;
            r = state(port + Idle, idle);
            if (r != Result::OK) return r;
            if (!idle) return Result::NotReady;
        }
        return Result::OK;
    }
    Result quiesceAndReset() {
        if (pinned) return Result::Quarantined;
        bool reset = false;
        auto r = io.resetState(reset);
        if (r != Result::OK) return quarantine(r);
        if (reset) {
            for (auto port : Ports) if ((r = wait(port, false)) != Result::OK) return quarantine(r);
        } else {
            // Request all three before waiting: no port is reopened while another
            // can still submit transactions. A failed store may already be visible.
            for (auto port : Ports)
                if (!io.write(port + Request, 1)) return quarantine(Result::IO);
            for (auto port : Ports)
                if ((r = wait(port, true)) != Result::OK) return quarantine(r);
            if ((r = io.reset(true)) != Result::OK) return quarantine(r);
            if ((r = io.resetState(reset)) != Result::OK || !reset)
                return quarantine(r == Result::OK ? Result::IO : r);
            for (auto port : Ports)
                if ((r = wait(port, false)) != Result::OK) return quarantine(r);
        }
        // The request bit clears only with MSS held in reset and every AXI
        // transaction drained. Verify the writes before releasing clocks.
        for (auto port : Ports) {
            if (!io.write(port + Request, 0)) return quarantine(Result::IO);
            bool requested = true;
            if ((r = state(port + Request, requested)) != Result::OK || requested)
                return quarantine(r == Result::OK ? Result::IO : r);
        }
        return Result::OK;
    }
};
}} // namespace Baseband::AXI
