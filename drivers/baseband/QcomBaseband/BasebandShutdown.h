// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <BasebandProtocol.h>

namespace Baseband {
// Remote protocol ACKs only choose the graceful request path. Boot always
// follows this with AXI drain/reset before reclaiming RAM or clocks. Each
// protocol gets a bounded wait, as in qcom_sysmon + qcom_q6v5_request_stop.
template<class IO> class ShutdownSequence {
    IO &io;
    static bool normal(Result r) { return r == Result::OK || r == Result::Busy || r == Result::NotReady; }
public:
    explicit ShutdownSequence(IO &backend) : io(backend) {}
    Result run(uint64_t epoch, unsigned timeoutUS) {
        if (!epoch || !timeoutUS) return Result::Invalid;
        if (io.shutdownAborted()) return Result::NotReady;
        auto graceful = io.beginGraceful(epoch, io.shutdownNowUS(), timeoutUS);
        if (graceful == Result::OK) {
            graceful = Result::Timeout;
            const auto start = io.shutdownNowUS();
            for (unsigned polls = 0; polls <= timeoutUS / 1000; ++polls) {
                if (io.shutdownAborted()) { graceful = Result::IO; break; }
                bool ack = false;
                auto r = io.sampleStop(ack);
                if (!normal(r)) { graceful = r; break; }
                r = io.pumpStop(epoch, io.shutdownNowUS());
                if (!normal(r)) { graceful = r; break; }
                if (!io.gracefulPending()) { graceful = io.gracefulOutcome(); break; }
                if (io.shutdownNowUS() - start >= timeoutUS) break;
                io.shutdownDelayMS(1);
            }
        }
        io.recordGraceful(graceful);
        if (graceful == Result::OK) return Result::OK;
        if (graceful == Result::Quarantined) return graceful;
        if (io.shutdownAborted()) return Result::IO;

        bool ack = false;
        auto r = io.sampleStop(ack);
        if (!normal(r)) return r;
        bool cleared = r == Result::OK && !ack;
        r = io.stopBit(epoch, true); if (r != Result::OK) return r;
        Result result = Result::Timeout;
        const auto start = io.shutdownNowUS();
        bool transport = true;
        for (unsigned polls = 0; polls <= timeoutUS / 1000; ++polls) {
            if (io.shutdownAborted()) { result = Result::IO; break; }
            r = io.sampleStop(ack);
            if (!normal(r)) { result = r; break; }
            if (r == Result::OK && !ack) cleared = true;
            if (r == Result::OK && cleared && ack) { result = Result::OK; break; }
            // A broken optional/control transport does not prevent a fresh
            // SMP2P ACK. Continue NV servicing while that transport is usable.
            if (transport) {
                r = io.pumpStop(epoch, io.shutdownNowUS());
                if (r == Result::Quarantined) { result = r; break; }
                if (!normal(r)) transport = false;
            }
            if (io.shutdownNowUS() - start >= timeoutUS) break;
            io.shutdownDelayMS(1);
        }
        const auto clearedRequest = io.stopBit(epoch, false);
        return clearedRequest == Result::OK ? result : clearedRequest;
    }
};
} // namespace Baseband
