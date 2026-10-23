// SPDX-License-Identifier: GPL-2.0-only
// Outbound GLINK edge cleanup, Linux v6.12 qcom_glink_ssr.c.
#pragma once
#include <BasebandProtocol.h>

namespace Baseband { namespace SSR {
constexpr const char *ChannelName = "glink_ssr";
constexpr uint64_t TimeoutUS = 1000000;
enum class Edge : unsigned { ADSP, SLPI, WCNSS };
// GLINK edge labels differ from sysmon subsystem labels on MSM8998.
constexpr const char *EdgeNames[] = {"lpass", "dsps", "wcnss"};
enum class Phase : unsigned { Idle, Queued, Waiting, Complete, Failed };
// A cleanup ACK describes another edge's transport cleanup, never physical
// reset, AXI drain or memory ownership. The actual stop owner must attest its
// edge/generation before cleanup() can queue a notification. No userspace API
// accepts such a witness, and the native owner invents no foreign lifecycle.
template<class Link> class Client {
    Link &link;
    uint64_t epoch = 0, lastTime = 0, deadline = 0, subjectEpoch = 0, completedEpochs[3]{};
    uint32_t sequence = 0, discarded = 0;
    uint16_t channel = 0;
    unsigned subject = 0;
    bool active = false;
    Phase phase = Phase::Idle;
    Result result = Result::NotReady;
    uint8_t request[48]{};
    bool pending() const { return phase == Phase::Queued || phase == Phase::Waiting; }
    void finish(Result r) { result = r; phase = r == Result::OK ? Phase::Complete : Phase::Failed; }
public:
    explicit Client(Link &endpoint) : link(endpoint) {}
    Result start(uint64_t generation, uint64_t now) {
        if (active || !generation || generation <= epoch) return Result::Invalid;
        epoch = generation; lastTime = now; active = true; phase = Phase::Idle; result = Result::NotReady;
        sequence = discarded = channel = 0; for (auto &e : completedEpochs) e = 0;
        return Result::OK;
    }
    template<class StopOwner> Result cleanup(uint64_t generation, Edge edge, uint64_t stoppedGeneration,
                                             StopOwner &owner, uint64_t now, uint64_t timeout = TimeoutUS) {
        if (!active || generation != epoch) return Result::Stale;
        if (pending()) return Result::Busy;
        const auto index = unsigned(edge);
        if (index >= 3 || !stoppedGeneration || now < lastTime || !timeout || timeout > UINT64_MAX - now) return Result::Invalid;
        if (stoppedGeneration <= completedEpochs[index]) return Result::Stale;
        if (!link.connected(ChannelName)) return Result::NotReady;
        const auto proof = owner.confirmedStopped(edge, stoppedGeneration);
        if (proof != Result::OK) return proof;
        if (sequence == UINT32_MAX) return Result::NoMemory;
        channel = link.channelIdentity(ChannelName); if (!channel) return Result::NotReady;
        for (auto &b : request) b = 0;
        write32(request + 8, ++sequence);
        size_t n = 0; for (; EdgeNames[index][n]; ++n) request[16 + n] = uint8_t(EdgeNames[index][n]);
        write32(request + 12, uint32_t(n)); // Version=0, DO_CLEANUP=0, excluding NUL from name_len.
        subject = index; subjectEpoch = stoppedGeneration;
        deadline = now + timeout; lastTime = now; phase = Phase::Queued; result = Result::Busy; return Result::OK;
    }
    Result progress(uint64_t generation, uint64_t now) {
        if (!active || generation != epoch) return Result::Stale;
        if (now < lastTime) { finish(Result::Stale); return Result::OK; }
        lastTime = now;
        if (pending() && now >= deadline) finish(Result::Timeout);
        if (pending() && link.channelIdentity(ChannelName) != channel) finish(Result::Stale);
        const uint8_t *data = nullptr; size_t bytes = 0;
        auto r = link.datagramOn(epoch, ChannelName, data, bytes);
        if (r == Result::OK) {
            const bool valid = bytes >= 12 && !read32(data) && read32(data + 4) == 1 &&
                read32(data + 8) == sequence && phase == Phase::Waiting;
            if (valid) finish(Result::OK);
            else if (discarded != UINT32_MAX) ++discarded;
            r = link.acceptOn(epoch, ChannelName); if (r != Result::OK) return r;
        } else if (r != Result::NotReady) return r;
        if (phase != Phase::Queued) return Result::NotReady;
        r = link.sendOn(epoch, ChannelName, request, sizeof(request), now);
        if (r == Result::OK) { phase = Phase::Waiting; completedEpochs[subject] = subjectEpoch; }
        else if (r != Result::Busy && r != Result::NotReady) finish(r);
        return r;
    }
    Result outcome() const { return pending() ? Result::Busy : result; }
    Phase state() const { return phase; }
    uint32_t discardedResponses() const { return discarded; }
    void stopped() { active = false; phase = Phase::Idle; result = Result::NotReady; }
};
}} // namespace Baseband::SSR
