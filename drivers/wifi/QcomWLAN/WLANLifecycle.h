// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <WLANProtocol.h>

namespace WLAN {
// Backend operations are synchronous and serialized by the owner. Real DMA
// allocation and firmware waits require a sleepable worker outside the gate.
// acquire() either rolls back, or reports uncertain(resource, epoch) so even
// partially acquired firmware/DMA is included in the reverse unwind.
// release(CopyEngines) proves all CE DMA and callbacks have stopped;
// release(Firmware) proves the remote processor cannot access host memory.
// Neither a timer expiry nor an MMIO status bit is such a proof.
template<class Backend> class Lifecycle {
    Backend &backend;
    State state = State::Detached;
    uint32_t held = 0;
    uint64_t generation = 0;
    Result last = Result::Success;
    static constexpr Resource order[6] = {Power, Clock, Interconnect, DMA, Firmware, CopyEngines};
    Result unwind() {
        for (unsigned i = 6; i; --i) {
            const Resource resource = order[i - 1];
            if (!(held & resource)) continue;
            const Result result = backend.release(resource, generation);
            if (result != Result::Success) {
                // Keep this resource and every prerequisite. In particular,
                // an uncertain DMA stop must never free memory or power off.
                state = State::Fault; return last = result;
            }
            held &= ~uint32_t(resource);
        }
        state = State::WaitingForResources;
        return Result::Success;
    }
public:
    explicit Lifecycle(Backend &operations) : backend(operations) {}
    Lifecycle(const Lifecycle &) = delete;
    Lifecycle &operator=(const Lifecycle &) = delete;
    State currentState() const { return state; }
    uint32_t resources() const { return held; }
    uint64_t epoch() const { return generation; }
    Result lastResult() const { return last; }
    void attach() { if (state == State::Detached) state = State::WaitingForResources; }
    Result start() {
        if (state == State::TransportReady) return Result::Success;
        if (state != State::WaitingForResources || held) return Result::Busy;
        if (generation == UINT64_MAX) return last = Result::Fault;
        ++generation; state = State::Starting;
        for (const Resource resource : order) {
            const Result result = backend.acquire(resource, generation);
            if (result != Result::Success) {
                if (backend.uncertain(resource,generation)) held |= resource;
                const Result cleanup = unwind();
                return last = cleanup == Result::Success ? result : cleanup;
            }
            held |= resource;
        }
        state = State::TransportReady; return last = Result::Success;
    }
    Result stop() {
        if (state == State::Detached) return Result::NotReady;
        if (state == State::Starting || state == State::Stopping) return Result::Busy;
        state = State::Stopping; return last = unwind();
    }
    Result detach() {
        if (state == State::Detached) return Result::Success;
        const Result result = stop();
        if (result == Result::Success) state = State::Detached;
        return result;
    }
    // A crash invalidates protocol events immediately. Resource ownership is
    // retained until stop() obtains the backend's actual stop/isolation proof.
    void firmwareCrashed(uint64_t epoch) {
        if (epoch != generation || !(held & Firmware)) return;
        state = State::Fault; last = Result::Fault;
    }
    bool accepts(uint64_t epoch) const {
        return state == State::TransportReady && epoch && epoch == generation;
    }
};
}
