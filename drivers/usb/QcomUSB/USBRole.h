// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace USB {
// The provider increments connection on attachment/session loss, orientation
// or data/power-role change, including edges coalesced between Sample calls.
// Counter0 means no observation/fault. Never recycle a counter within a lease.
struct ConnectorState {
    bool attached = false, host = false, vbus = false, overCurrent = false;
    Orientation orientation = Orientation::Unknown;
    PowerRole power = PowerRole::None;
    uint8_t reserved[2]{};
    uint64_t connection = 0;
    bool valid() const {
        if (reserved[0] || reserved[1] || unsigned(orientation) > unsigned(Orientation::Reverse) ||
            unsigned(power) > unsigned(PowerRole::Sink)) return false;
        return attached ? connection && orientation != Orientation::Unknown && power != PowerRole::None :
            !host && orientation == Orientation::Unknown && power == PowerRole::None;
    }
    Role desired() const {
        if (!valid() || !attached || overCurrent || (power == PowerRole::Sink && !vbus)) return Role::None;
        return host ? Role::Host : Role::Device;
    }
    bool sameRoute(const ConnectorState &other) const {
        return connection == other.connection && attached == other.attached && host == other.host &&
            orientation == other.orientation && power == other.power;
    }
};
static_assert(sizeof(ConnectorState) == 16, "connector route identity ABI");

// Protected by the platform's notification lock in the kernel binding. A
// newer notice closes admission immediately, before the role worker can run.
// Matching live Sample is required to open it again. Notices carry no GPIO
// policy and may never make an older sample authoritative.
class ConnectorFence {
    ConnectorState sampled{};
    uint64_t notified = 0;
    uint64_t generation = 1;
    bool admitted = false, interrupted = false, exhausted = false;
    void close() {
        if (admitted) {
            if (generation == UINT64_MAX) exhausted = true;
            else ++generation;
        }
        admitted = false; interrupted = true;
    }
public:
    void notice(uint64_t connection) {
        if (!connection) close();
        else if (connection > notified) { notified = connection; close(); }
    }
    Result sample(const ConnectorState &signal, bool success) {
        if (exhausted) return Result::Invalid;
        if (!success) { close(); return Result::NotReady; }
        if (!signal.valid()) { close(); return Result::Invalid; }
        if (signal.connection < notified) { close(); return Result::Busy; }
        if (signal.connection == sampled.connection && !signal.sameRoute(sampled)) {
            close(); return Result::Invalid;
        }
        sampled = signal; notified = signal.connection;
        if (admitted && signal.desired() == Role::None) close();
        admitted = signal.desired() != Role::None;
        return Result::Success;
    }
    uint64_t epoch() const { return generation; }
    bool takeInterruption() { const bool value = interrupted; interrupted = false; return value; }
    bool accepts(const ConnectorState &signal, uint64_t epoch = 0) const {
        return !exhausted && admitted && (!epoch || epoch == generation) && signal.connection &&
            signal.connection == notified && signal.sameRoute(sampled);
    }
};

// Serialized, sleepable role worker. stop must prove halt and synchronously
// drain the old nub before configure may reset PHYs or change orientation.
// Aborted from safePower/configure/source(true)/start means a stale snapshot;
// roll back, then sample again. Source(false) or stop failure always faults.
template<class Backend> class RoleSwitch {
    Backend &backend;
    Role active = Role::None;
    ConnectorState route{}, candidate{}, observed{};
    uint64_t since = 0, lastTime = 0, generation = 0;
    bool fault = false, sampled = false, timeSeen = false, sleeping = false;

    Result nextEpoch() {
        if (generation == UINT64_MAX) { fault = true; return Result::Overflow; }
        ++generation; return Result::Success;
    }
    Result quiesce() {
        const bool off = backend.source(false) == Result::Success;
        bool idle = true;
        if (active != Role::None) {
            idle = backend.stop(active, generation);
            if (idle) { active = Role::None; route = {}; }
        }
        if (!off || !idle) { fault = true; return off ? Result::Timeout : Result::HardwareError; }
        auto r = nextEpoch();
        if (r != Result::Success) return r;
        r = backend.safePower(Role::None, {});
        if (r != Result::Success) { fault = true; return Result::HardwareError; }
        return Result::Success;
    }
    Result invalid() {
        const auto stopped = quiesce(); fault = true;
        return stopped == Result::Success ? Result::Invalid : stopped;
    }
    Result rollback(Role next, const ConnectorState &target, Result reason) {
        const bool off = backend.source(false) == Result::Success;
        const bool idle = backend.stop(next, generation);
        if (!idle) { active = next; route = target; }
        sampled = false;
        if (!off || !idle) { fault = true; return Result::HardwareError; }
        if (backend.safePower(Role::None, {}) != Result::Success) { fault = true; return Result::HardwareError; }
        if (reason == Result::Aborted) return Result::Busy;
        fault = true; return reason == Result::Success ? Result::HardwareError : reason;
    }
    Result activate(const ConnectorState &target) {
        const auto next = target.desired();
        auto r = backend.source(false);
        if (r != Result::Success) { fault = true; return Result::HardwareError; }
        r = nextEpoch();
        if (r != Result::Success) return r;
        r = backend.safePower(next, target);
        if (r != Result::Success) return rollback(next, target, r);
        r = backend.configure(next, target);
        if (r != Result::Success) return rollback(next, target, r);
        // Connector policy, independently of the data-role name, authorizes
        // sourcing. This layer does not negotiate power delivery itself.
        if (target.power == PowerRole::Source) {
            r = backend.source(true);
            if (r != Result::Success) return rollback(next, target, r);
        }
        r = backend.start(next, generation, target);
        if (r != Result::Success) return rollback(next, target, r);
        active = next; route = target; return Result::Success;
    }
public:
    static constexpr uint64_t DebounceUS = 20000;
    explicit RoleSwitch(Backend &hardware) : backend(hardware) {}
    Role current() const { return active; }
    uint64_t epoch() const { return generation; }
    uint64_t connection() const { return route.connection; }
    bool failed() const { return fault; }
    bool suspended() const { return sleeping; }
    bool accepts(uint64_t epoch) const { return !fault && active != Role::None && generation == epoch; }

    Result suspend() {
        if (fault) return Result::NotReady;
        if (sleeping) return Result::Success;
        const auto result=quiesce();
        if (result==Result::Success) { sleeping=true;sampled=false;candidate={}; }
        return result;
    }
    Result resume() {
        if (fault || !sleeping || active!=Role::None) return Result::NotReady;
        // No cached attachment is authoritative after a power cycle. The
        // next live Sample starts a new debounce and controller epoch.
        sleeping=false;sampled=timeSeen=false;candidate=observed={};return Result::Success;
    }

    Result update(ConnectorState signal, uint64_t nowUS, bool interrupted = false) {
        if (fault) return Result::NotReady;
        if (sleeping) return Result::NotReady;
        if (!signal.valid() || (timeSeen && nowUS < lastTime)) return invalid();
        lastTime = nowUS; timeSeen = true;
        if (signal.connection && signal.connection < observed.connection) return Result::Busy;
        if (signal.connection && signal.connection == observed.connection && !signal.sameRoute(observed)) return invalid();
        if (signal.connection > observed.connection) observed = signal;
        // A child can stop itself on a closed admission stamp even if a
        // temporary observation fault recovered to the same physical route.
        if (interrupted) {
            sampled = false;
            if (active != Role::None) {
                const auto stopped = quiesce();
                if (stopped != Result::Success) return stopped;
            }
        }
        const auto next = signal.desired();
        if (next == Role::None) {
            sampled = false; candidate = {};
            return active == Role::None ? Result::Success : quiesce();
        }
        if (active != Role::None && !signal.sameRoute(route)) {
            const auto stopped = quiesce();
            if (stopped != Result::Success) return stopped;
            sampled = false;
        }
        if (active != Role::None) return Result::Success;
        if (!sampled || !signal.sameRoute(candidate)) { candidate = signal; since = nowUS; sampled = true; }
        if (nowUS - since < DebounceUS) return Result::Busy;
        return activate(signal);
    }
    // Only after externally proven reset, DMA isolation and source-off.
    // Keep connector observation history within the same platform lease.
    Result resetAfterIsolation() {
        active = Role::None; route = candidate = {};
        sampled = timeSeen = sleeping = false; since = lastTime = 0;
        if (generation == UINT64_MAX) { fault = true; return Result::Overflow; }
        ++generation; fault = false; return Result::Success;
    }
};
}
