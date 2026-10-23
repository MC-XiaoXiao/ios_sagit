// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBDMA.h"

namespace USB {
// Policy notifications belong to one bus incarnation. DMA completions keep
// their independent request cookies and must still drain after invalidation.
class DeviceSession {
    uint64_t serial=1;
    bool paused=false;
public:
    uint64_t generation() const { return serial; }
    bool accepts(uint64_t value) const { return serial && value==serial; }
    bool suspended() const { return paused; }
    bool suspend(bool value) {
        if (!serial || value==paused) return false;
        paused=value;return true;
    }
    bool advance() {
        paused=false;
        if (!serial) return false;
        if (serial==UINT64_MAX) { serial=0;return false; }
        ++serial;return true;
    }
};

// Bounded request ownership shared by the native adapter and offline tests.
// Except execute(), every operation requires the controller gate. A separate
// DMA worker executes a ticket outside that gate, then commits it under the
// gate. Tickets never contain a Family request or callback target.
template<class API, unsigned Slots = 64> class DeviceRequests {
    static_assert(Slots && Slots <= 128, "bounded request pool");
public:
    enum class Phase : uint8_t { Free, Queued, Preparing, Prepared, Active,
        Retiring, Cleaning, Complete, Pinned };
    struct View {
        uint64_t cookie = 0, scope = 0, deadline = 0, bus = 0;
        uint32_t length = 0, capacity = 0;
        unsigned endpoint = 0, packet = 0;
        Phase phase = Phase::Free;
        uint64_t lane=0;
    };
    struct Completion {
        void *context = nullptr;
        uint64_t cookie = 0;
        Result result = Result::NotReady;
        uint32_t bytes = 0;
    };
    struct Work {
        uint64_t cookie = 0;
        DeviceBounce<API> *dma = nullptr;
        typename API::Memory *buffer = nullptr;
        unsigned packet = 0, endpoint = 0;
        uint32_t length = 0, actual = 0;
        bool preparing = false, copyBack = false;
        uint64_t offset=0;
    };
    struct WorkResult { Result result; bool empty, released; };
private:
    struct Node {
        DeviceBounce<API> *dma = nullptr;
        typename API::Memory *buffer = nullptr;
        void *context = nullptr;
        View view{};
        Result result = Result::NotReady;
        uint32_t actual = 0;
        bool terminal = false, announced = false, ownershipPinned = false;
        CleanupRecovery recovery;
        uint64_t offset=0;
    } nodes[Slots];
    API &api;
    uint64_t serial = 0, working = 0;
    Node *find(uint64_t cookie) {
        if (!cookie) return nullptr;
        auto &n = nodes[(cookie - 1) % Slots];
        return n.view.cookie == cookie && n.view.phase != Phase::Free ? &n : nullptr;
    }
    Node *first(uint64_t lane) {
        Node *found = nullptr;
        for (auto &n : nodes) if (n.view.phase != Phase::Free && n.view.lane == lane &&
            (!found || n.view.cookie < found->view.cookie)) found = &n;
        return found;
    }
    void terminal(Node &n, Result result, uint32_t bytes = 0) {
        // Completion already reported by the hardware remains immutable while
        // its OUT copy/unmap runs. An abort still waits for that completion.
        if (n.terminal) return;
        n.terminal = true; n.result = result;
        n.actual = result == Result::Success ? bytes : 0;
    }
    void reset(Node &n) {
        auto *dma = n.dma;
        n = {}; n.dma = dma;
    }
public:
    explicit DeviceRequests(API &operations) : api(operations) {}
    DeviceRequests(const DeviceRequests &) = delete;
    DeviceRequests &operator=(const DeviceRequests &) = delete;
    bool attach(unsigned slot, DeviceBounce<API> *dma) {
        if (slot >= Slots || !dma || nodes[slot].dma || dma->currentState() != DMAState::Empty) return false;
        nodes[slot].dma = dma; return true;
    }
    // Retain before returning success: the mapper may not run until after the
    // submitting function has dropped its own reference. Only takeCompletion
    // returns the opaque context; workers never access it.
    // Ordinary I/O uses the endpoint's FIFO lane. Periodic frame owners may
    // give independent nonzero lanes so mapping can prefetch several service
    // intervals; that owner still controls their ordered hardware publication.
    Result admit(unsigned ep, typename API::Memory *buffer, uint32_t length,
                 unsigned packet, uint64_t scope, uint64_t deadline,
                 void *context, uint64_t &cookie,uint64_t offset=0,uint64_t lane=0) {
        cookie = 0;
        if (!context || !ep || ep >= 32 || !scope || length > DMARegion<API>::MaximumBytes ||
            ((length || offset) && !buffer) ||
            (buffer && (offset>api.length(buffer) || length>api.length(buffer)-offset)) ||
            !packet || packet > 1024)
            return Result::Invalid;
        if (serial > UINT64_MAX / Slots - 1) return Result::NotReady;
        const uint64_t ordering=lane ? lane : ep;
        for (const auto &n:nodes) if (n.view.phase!=Phase::Free && n.view.lane==ordering && n.view.endpoint!=ep)
            return Result::Invalid;
        for (unsigned i = 0; i < Slots; ++i) {
            auto &n = nodes[i];
            if (!n.dma || n.view.phase != Phase::Free) continue;
            cookie = (++serial - 1) * Slots + i + 1;
            n.buffer = buffer; n.context = context;
            n.view = {cookie, scope, deadline, 0, length, 0, ep, packet, Phase::Queued};
            n.view.lane=ordering;n.offset=offset;
            if (buffer) api.retainMemory(buffer);
            return Result::Success;
        }
        return Result::Busy;
    }
    bool inspect(uint64_t cookie, View &view) {
        auto *n = find(cookie); if (!n) return false;
        view = n->view; return true;
    }
    bool cancellable(uint64_t cookie) {
        auto *n=find(cookie);return n && !n->terminal;
    }
    bool prepared(unsigned ep, View &view) {
        auto *n = first(ep);
        if (!n || n->view.phase != Phase::Prepared || n->terminal) return false;
        view = n->view; return true;
    }
    bool preparedRequest(uint64_t cookie,View &view) {
        auto *n=find(cookie);
        if (!n || n!=first(n->view.lane) || n->view.phase!=Phase::Prepared || n->terminal) return false;
        view=n->view;return true;
    }
    uint64_t active(unsigned ep) {
        for (auto &n : nodes) if (n.view.endpoint == ep && n.view.phase == Phase::Active) return n.view.cookie;
        return 0;
    }
    bool stopping(unsigned ep, Result &reason) {
        auto *n = find(active(ep));
        if (!n || !n->terminal) return false;
        reason = n->result; return true;
    }
    Result publish(uint64_t cookie, uint64_t epoch) {
        auto *n = find(cookie);
        if (!n || n != first(n->view.lane) || n->view.phase != Phase::Prepared || n->terminal) return Result::NotReady;
        auto result = n->dma->publish(epoch);
        if (result == Result::Success) n->view.phase = Phase::Active;
        return result;
    }
    Result completed(uint64_t cookie, uint64_t epoch, Result result, uint32_t bytes, bool partial=false) {
        auto *n = find(cookie);
        if (!n || n->view.phase != Phase::Active) return Result::NotReady;
        const auto retired = n->dma->retire(epoch);
        if (retired != Result::Success) return retired;
        if (bytes > n->view.length) { result = Result::Overflow; bytes = 0; }
        terminal(*n, result, bytes);
        // A periodic OUT TRB may prove a valid prefix before an overrun.
        // Ordinary transfers keep their existing all-or-error copy policy.
        if (partial && n->result==Result::Overflow) n->actual=bytes;
        n->view.phase = Phase::Retiring;
        return Result::Success;
    }
    bool cancel(uint64_t cookie, Result reason = Result::Aborted) {
        auto *n = find(cookie);
        if (!n || n->terminal || reason == Result::Success || reason == Result::Pending) return false;
        terminal(*n, reason);
        if (n->view.phase == Phase::Queued || n->view.phase == Phase::Prepared) n->view.phase = Phase::Retiring;
        // Preparing commits to Retiring when its ticket returns. Active waits
        // for a real TD/END completion or whole-controller isolation proof.
        return true;
    }
    void cancelEndpoint(unsigned ep, Result reason = Result::Aborted) {
        for (auto &n : nodes) if (n.view.phase != Phase::Free && n.view.endpoint == ep) cancel(n.view.cookie, reason);
    }
    void expire(uint64_t now) {
        for (auto &n : nodes) if (n.view.deadline && now >= n.view.deadline) cancel(n.view.cookie, Result::Timeout);
    }
    // A failed hardware stop pins storage and the resource lease. A native
    // error completion can then detach its context: no later worker copies
    // into client memory or reads a freed Family tag/callback target.
    void quarantine(unsigned ep) {
        for (auto &n : nodes) if (n.view.endpoint == ep && n.view.phase == Phase::Active) {
            terminal(n, Result::HardwareError); n.result = Result::HardwareError; n.actual = 0;
            n.dma->quarantine(); n.ownershipPinned = true; n.view.phase = Phase::Pinned;
        }
    }
    void isolated() {
        for (auto &n : nodes) if (n.view.phase != Phase::Free) {
            cancel(n.view.cookie);
            if (n.view.phase == Phase::Active || (n.view.phase == Phase::Pinned && n.ownershipPinned)) {
                n.dma->retireAfterIsolation(); n.ownershipPinned = false; n.view.phase = Phase::Retiring;
            }
        }
    }
    // No native tag/context is inspected by execute(). A cancelled mapping
    // may finish after a replacement SETUP, but can never publish to hardware.
    bool beginWork(Work &work) {
        if (working) return false;
        Node *chosen = nullptr;
        for (auto &n : nodes) if (n.view.phase == Phase::Retiring &&
            (!chosen || n.view.cookie < chosen->view.cookie)) chosen = &n;
        if (!chosen) for (auto &n : nodes) if (n.view.phase == Phase::Queued && &n == first(n.view.lane) &&
            (!chosen || n.view.cookie < chosen->view.cookie)) chosen = &n;
        if (!chosen) return false;
        auto &n = *chosen;
        const bool preparing = n.view.phase == Phase::Queued;
        n.view.phase = preparing ? Phase::Preparing : Phase::Cleaning;
        working = n.view.cookie;
        work = {working, n.dma, n.buffer, n.view.packet, n.view.endpoint,
            n.view.length, n.actual, preparing, n.result == Result::Success ||
                (n.result==Result::Overflow && n.actual)};
        work.offset=n.offset;
        return true;
    }
    bool needsWork() {
        if (working) return false;
        for (auto &n : nodes) if (n.view.phase == Phase::Retiring ||
            (n.view.phase == Phase::Queued && &n == first(n.view.lane))) return true;
        return false;
    }
    bool workPending() {return working || needsWork();}
    WorkResult execute(const Work &work) {
        if (work.preparing) {
            auto r = work.dma->prepare(work.buffer, work.length, !(work.endpoint & 1), work.packet,work.offset);
            return {r, work.dma->currentState() == DMAState::Empty, false};
        }
        const auto result = work.dma->currentState() == DMAState::Empty ? Result::Success :
            work.dma->finish(work.actual, work.copyBack);
        const bool empty = work.dma->currentState() == DMAState::Empty;
        if (empty && work.buffer) api.releaseMemory(work.buffer);
        return {result, empty, empty};
    }
    bool commit(const Work &work, WorkResult result) {
        auto *n = find(work.cookie);
        if (!n || working != work.cookie || n->view.phase != (work.preparing ? Phase::Preparing : Phase::Cleaning)) return false;
        working = 0;
        if (work.preparing) {
            if (result.result != Result::Success) terminal(*n, result.result);
            if (n->terminal) n->view.phase = Phase::Retiring;
            else {
                n->view.bus = n->dma->busAddress(); n->view.capacity = n->dma->capacity();
                n->view.phase = Phase::Prepared;
            }
        } else {
            if (result.released) n->buffer = nullptr;
            if (!result.empty) {
                n->result = Result::HardwareError; n->actual = 0; n->view.phase = Phase::Pinned;
                n->recovery.failed(n->dma->cleanupRetryable());
            } else {
                if (result.result != Result::Success) { n->result = result.result; n->actual = 0; }
                if (!n->context) reset(*n);
                else n->view.phase = Phase::Complete;
            }
        }
        return true;
    }
    uint64_t notice() {
        Node *chosen = nullptr;
        for (auto &n : nodes) if (n.terminal && n.context && !n.announced &&
            (!chosen || n.view.cookie < chosen->view.cookie)) chosen = &n;
        return chosen ? chosen->view.cookie : 0;
    }
    bool announce(uint64_t cookie) {
        auto *n = find(cookie);
        if (!n || !n->terminal || !n->context || n->announced) return false;
        n->announced = true; return true;
    }
    Result takeCompletion(uint64_t cookie, Completion &completion) {
        auto *n = find(cookie);
        if (!n || !n->context) return Result::Invalid;
        if (n->view.phase != Phase::Complete && n->view.phase != Phase::Pinned) return Result::Pending;
        completion = {n->context, cookie, n->result, n->actual};
        n->context = nullptr;
        if (n->view.phase == Phase::Complete) reset(*n);
        return Result::Success;
    }
    bool retryCleanup(uint64_t cookie,bool rootDetached=false) {
        auto *n = find(cookie);
        if (!n || n->view.phase != Phase::Pinned || n->ownershipPinned || !n->recovery.retry(rootDetached)) return false;
        n->view.phase = Phase::Retiring; return true;
    }
    unsigned retryIsolatedCleanup(bool rootDetached) {
        unsigned count=0;
        for (auto &n:nodes) if (n.view.phase==Phase::Pinned && retryCleanup(n.view.cookie,rootDetached)) ++count;
        return count;
    }
    uint64_t pending(unsigned ep) {
        Node *chosen = nullptr;
        for (auto &n : nodes) if (n.context && n.view.endpoint == ep &&
            (!chosen || n.view.cookie < chosen->view.cookie)) chosen = &n;
        return chosen ? chosen->view.cookie : 0;
    }
    bool empty() const {
        for (const auto &n : nodes) if (n.view.phase != Phase::Free) return false;
        return true;
    }
    bool pinned() const {
        for (const auto &n : nodes) if (n.view.phase == Phase::Pinned) return true;
        return false;
    }
};
}
