// SPDX-License-Identifier: BSD-2-Clause
#include <array>
#include <cstdio>
#include "usb_memory_model.h"
#include "USBDeviceRequests.h"

struct Fixture {
    static constexpr unsigned Slots = 4;
    MemoryAPI api;
    USB::DeviceRequests<MemoryAPI, Slots> queue{api};
    using Queue = decltype(queue);
    using Phase = Queue::Phase;
    std::array<USB::DeviceBounce<MemoryAPI> *, Slots> buffers{};
    MemoryAPI::Memory *client = api.allocate(128, 64);
    unsigned context = 0;
    Fixture() {
        for (unsigned i = 0; i < Slots; ++i) {
            buffers[i] = new USB::DeviceBounce<MemoryAPI>(api);
            assert(queue.attach(i, buffers[i]));
        }
        std::memset(client->data, 0xa5, 128);
    }
    ~Fixture() {
        assert(queue.empty());
        for (auto *buffer : buffers) { assert(buffer->currentState() == DMAState::Empty); delete buffer; }
        api.releaseMemory(client);
    }
    uint64_t admit(unsigned ep = 2, unsigned length = 65, uint64_t scope = 3, uint64_t deadline = 0) {
        uint64_t cookie = 0;
        assert(queue.admit(ep, client, length, 64, scope, deadline, &context, cookie) == Result::Success);
        return cookie;
    }
    Queue::Work begin() { Queue::Work work; assert(queue.beginWork(work)); return work; }
    void commit(const Queue::Work &work) { assert(queue.commit(work, queue.execute(work))); }
    void work() { commit(begin()); }
    Queue::View view(uint64_t cookie) { Queue::View v; assert(queue.inspect(cookie, v)); return v; }
    Queue::Completion complete(uint64_t cookie, Result status, uint32_t actual = 0) {
        Queue::Completion completion;
        assert(queue.takeCompletion(cookie, completion) == Result::Success);
        assert(completion.cookie == cookie && completion.context == &context);
        assert(completion.result == status && completion.bytes == actual);
        return completion;
    }
};

static void orderedQueues() {
    Fixture f;
    const auto out = f.admit(), second = f.admit(), in = f.admit(3, 64);
    assert(f.client->references == 4); // own retain exists before preparation
    f.work();
    assert(f.view(out).phase == Fixture::Phase::Prepared);
    auto work = f.begin(); assert(work.cookie == in); // second OUT cannot overtake
    f.commit(work);
    Fixture::Queue::View ready;
    assert(f.queue.prepared(2, ready) && ready.cookie == out && ready.capacity == 128);
    assert(f.queue.publish(out, 9) == Result::Success);
    assert(f.queue.publish(in, 9) == Result::Success);
    Fixture::Queue::Work blocked;
    assert(!f.queue.beginWork(blocked));
    std::memset(f.buffers[(out - 1) % Fixture::Slots]->bytes(), 0x7b, 128);
    assert(f.queue.completed(out, 8, Result::Success, 65) == Result::NotReady);
    assert(f.queue.completed(out, 9, Result::Success, 65) == Result::Success);
    assert(f.queue.notice() == out && f.queue.announce(out));
    Fixture::Queue::Completion pending;
    assert(f.queue.takeCompletion(out, pending) == Result::Pending);
    f.work(); f.complete(out, Result::Success, 65);
    for (unsigned i = 0; i < 128; ++i) assert(static_cast<uint8_t *>(f.client->data)[i] == (i < 65 ? 0x7b : 0xa5));
    f.work(); assert(f.queue.prepared(2, ready) && ready.cookie == second);
    f.queue.cancelEndpoint(2); f.work(); f.complete(second, Result::Aborted);
    assert(f.queue.completed(in, 9, Result::Success, 64) == Result::Success);
    f.work(); f.complete(in, Result::Success, 64);
    assert(f.client->references == 1 && !f.client->prepares && f.api.copies == 1);
}

static void replacementWhileMapping() {
    Fixture f;
    const auto old = f.admit(1, 18, 41);
    auto mapping = f.begin(); assert(mapping.cookie == old);
    // A new SETUP arrives while IOMemoryDescriptor::prepare is outside gate.
    assert(f.queue.cancel(old));
    assert(f.queue.notice() == old && f.queue.announce(old));
    const auto next = f.admit(1, 18, 42);
    Fixture::Queue::Completion done;
    assert(f.queue.takeCompletion(old, done) == Result::Pending);
    f.commit(mapping);
    assert(f.view(old).phase == Fixture::Phase::Retiring);
    assert(f.queue.publish(old, 7) == Result::NotReady);
    auto cleanup = f.begin(); assert(cleanup.cookie == old && !cleanup.preparing && !cleanup.copyBack);
    f.commit(cleanup); f.complete(old, Result::Aborted);
    f.work();
    assert(f.view(next).phase == Fixture::Phase::Prepared && f.view(next).scope == 42);
    assert(f.queue.completed(old, 7, Result::Success, 18) == Result::NotReady);
    f.queue.cancelEndpoint(1); f.work(); f.complete(next, Result::Aborted);
}

static void asyncEndAndQuarantine() {
    Fixture f;
    const auto active = f.admit(), queued = f.admit();
    f.work(); assert(f.queue.publish(active, 7) == Result::Success);
    f.queue.cancelEndpoint(2);
    // END command issuance is insufficient to unmap the active allocation.
    f.work(); f.complete(queued, Result::Aborted);
    assert(f.view(active).phase == Fixture::Phase::Active);
    Fixture::Queue::Completion c;
    assert(f.queue.takeCompletion(active, c) == Result::Pending);
    assert(f.queue.completed(active, 7, Result::Success, 65) == Result::Success);
    f.work(); f.complete(active, Result::Aborted);
    assert(f.api.copies == 0);
    const auto unsafe = f.admit();
    f.work(); assert(f.queue.publish(unsafe, 8) == Result::Success);
    const auto releases = f.api.releases;
    f.queue.quarantine(2);
    f.complete(unsafe, Result::HardwareError);
    assert(!f.queue.empty() && f.queue.pinned() && f.api.releases == releases);
    assert(!f.queue.retryCleanup(unsafe)); // no ownership proof
    f.queue.isolated(); f.work();
    assert(f.api.copies == 0 && f.queue.empty()); // no callback/copy after detachment
}

static void failuresAndReuse() {
    Fixture f;
    for (int failure = 1; failure <= 7; ++failure) {
        f.api.fail = failure;
        const auto cookie = f.admit(3, 64);
        f.work(); assert(f.view(cookie).phase == Fixture::Phase::Retiring);
        f.work();
        Fixture::Queue::Completion c;
        assert(f.queue.takeCompletion(cookie, c) == Result::Success && c.result != Result::Success);
        assert(f.client->references == 1 && !f.client->prepares);
    }
    f.api.fail = 0;
    const auto first = f.admit(); f.work();
    assert(f.queue.publish(first, 1) == Result::Success);
    assert(f.queue.completed(first, 1, Result::Success, 65) == Result::Success);
    auto cleanup = f.begin();
    assert(!f.queue.cancel(first)); // already valid completion; cleanup may copy
    f.api.clearFailures = 1;
    f.commit(cleanup);
    assert(f.queue.pinned() && f.api.copies == 0);
    f.complete(first, Result::HardwareError);
    assert(!f.queue.retryCleanup(first));
    assert(f.queue.retryCleanup(first,true)); f.work();
    assert(f.queue.empty() && f.api.copies == 0);
    const auto reused = f.admit(); assert(reused != first);
    Fixture::Queue::Completion c;
    assert(f.queue.takeCompletion(first, c) == Result::Invalid);
    assert(!f.queue.announce(first) && !f.queue.cancel(first));
    f.queue.cancel(reused); f.work(); f.complete(reused, Result::Aborted);
    for (unsigned n=0;n<16;++n) {
        const auto next=f.admit();f.work();
        assert(f.queue.publish(next,9)==Result::Success);
        assert(f.queue.completed(next,9,Result::Success,65)==Result::Success);
        f.api.clearFailures=1;f.work();f.complete(next,Result::HardwareError);
        assert(f.queue.retryIsolatedCleanup(false)==0 && f.queue.retryIsolatedCleanup(true)==1);
        f.work();assert(f.queue.empty());
    }
}

static void limitsAndTimeouts() {
    Fixture f;
    uint64_t cookie = 99;
    assert(f.queue.admit(0, f.client, 1, 64, 3, 0, &f.context, cookie) == Result::Invalid && !cookie);
    assert(f.queue.admit(32, f.client, 1, 64, 3, 0, &f.context, cookie) == Result::Invalid);
    assert(f.queue.admit(2, f.client, 129, 64, 3, 0, &f.context, cookie) == Result::Invalid);
    assert(f.queue.admit(2, f.client, 65, 1025, 3, 0, &f.context, cookie) == Result::Invalid);
    std::array<uint64_t, Fixture::Slots> ids;
    for (auto &id : ids) id = f.admit(2, 64, 3, 10);
    assert(f.queue.admit(2, f.client, 64, 64, 3, 10, &f.context, cookie) == Result::Busy);
    f.queue.expire(9); assert(!f.queue.notice());
    f.queue.expire(10);
    for (auto id : ids) { f.work(); f.complete(id, Result::Timeout); }
    const auto zero = f.admit(1, 0); f.work();
    assert(f.view(zero).capacity == 0 && f.queue.publish(zero, 2) == Result::Success);
    assert(f.queue.completed(zero, 2, Result::Success, 0) == Result::Success);
    f.work(); f.complete(zero, Result::Success);
    const auto overrun = f.admit(); f.work();
    assert(f.queue.publish(overrun, 2) == Result::Success);
    const auto copies = f.api.copies;
    assert(f.queue.completed(overrun, 2, Result::Success, 66) == Result::Success);
    f.work(); f.complete(overrun, Result::Overflow);
    assert(f.api.copies == copies);
    // Full-speed interrupt MPS=3 is legal and needs an 18-byte OUT tail
    // allocation for a 17-byte client request, rather than bit-mask rounding.
    assert(f.queue.admit(2, f.client, 17, 3, 3, 0, &f.context, cookie) == Result::Success);
    f.work(); assert(f.view(cookie).capacity == 18);
    assert(f.queue.publish(cookie, 3) == Result::Success);
    assert(f.queue.completed(cookie, 3, Result::Success, 17) == Result::Success);
    f.work(); f.complete(cookie, Result::Success, 17);
}

static void isolatedCleanupBound() {
    MemoryAPI api;auto *client=api.allocate(128,64);
    USB::DeviceBounce<MemoryAPI> buffer(api);unsigned context=0;
    {
        USB::DeviceRequests<MemoryAPI,1> queue(api);assert(queue.attach(0,&buffer));
        uint64_t cookie=0;
        assert(queue.admit(2,client,65,64,1,0,&context,cookie)==Result::Success);
        decltype(queue)::Work work;
        assert(queue.beginWork(work));assert(queue.commit(work,queue.execute(work)));
        assert(queue.publish(cookie,1)==Result::Success && queue.completed(cookie,1,Result::Success,65)==Result::Success);
        api.clearFailures=2;
        assert(queue.beginWork(work));assert(queue.commit(work,queue.execute(work)) && queue.pinned());
        decltype(queue)::Completion done;
        assert(queue.takeCompletion(cookie,done)==Result::Success && done.context==&context);
        assert(queue.retryIsolatedCleanup(false)==0 && !queue.needsWork());
        assert(queue.retryIsolatedCleanup(true)==1);
        assert(queue.beginWork(work));assert(queue.commit(work,queue.execute(work)) && queue.pinned());
        assert(queue.retryIsolatedCleanup(true)==0 && !queue.workPending());
        assert(queue.takeCompletion(cookie,done)==Result::Invalid && !api.copies);
    }
    // Dispose the mock after the queue itself has gone. Production keeps the
    // unknown cleanup owner pinned; no second recovery job/callback is queued.
    assert(buffer.finish(0,false)==Result::Success);
    api.releaseMemory(client);api.releaseMemory(client);
}

static void resetAndDisconnectIncarnations() {
    Fixture f;USB::DeviceSession session;
    assert(session.advance());const auto old=session.generation();
    const auto active=f.admit(2,65,old);f.work();assert(f.queue.publish(active,7)==Result::Success);
    const auto preparing=f.admit(3,64,old);auto work=f.begin();
    assert(session.suspend(true) && session.suspended() && !session.suspend(true));
    // Reset/disconnect invalidates queued SETUP/connect/suspend notices
    // before halting, including while a DMA prepare is outside the gate.
    assert(session.advance() && !session.suspended() && !session.accepts(old));
    f.queue.cancelEndpoint(2);f.queue.cancelEndpoint(3);
    Fixture::Queue::Completion completion;
    assert(f.queue.takeCompletion(active,completion)==Result::Pending);
    f.queue.isolated();f.commit(work);f.work();f.work();
    f.complete(active,Result::Aborted);f.complete(preparing,Result::Aborted);
    assert(f.queue.empty() && !f.api.copies);
    const auto resetNotice=session.generation();assert(session.accepts(resetNotice));
    assert(session.advance() && !session.accepts(resetNotice) && !session.suspended());
    // The new incarnation can run despite the preceding link having been
    // suspended. Old request-cookie completions can never finish its I/O.
    const auto next=f.admit(2,64,session.generation());f.work();
    assert(f.queue.publish(next,8)==Result::Success);
    assert(f.queue.completed(active,7,Result::Success,65)==Result::NotReady);
    assert(f.queue.completed(next,8,Result::Success,64)==Result::Success);
    f.work();f.complete(next,Result::Success,64);
}

int main() {
    orderedQueues(); replacementWhileMapping(); asyncEndAndQuarantine(); failuresAndReuse(); limitsAndTimeouts();
    resetAndDisconnectIncarnations();
    isolatedCleanupBound();
    std::puts("USB requests: ordered endpoints, mapping cancellation, asynchronous END, quarantine, cleanup faults, reuse and timeout passed");
}
