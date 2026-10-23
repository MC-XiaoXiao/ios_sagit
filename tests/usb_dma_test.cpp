// SPDX-License-Identifier: BSD-2-Clause
#include <cstdio>
#include "usb_memory_model.h"
#include "USBHostDMA.h"

struct ConsumingMemoryAPI : MemoryAPI {
    static constexpr bool CompleteFailurePreservesPreparation=false;
    unsigned completeCalls=0;
    Memory *consumeFailure=nullptr;
    Result complete(Memory *memory) {
        ++completeCalls;
        const auto result=MemoryAPI::complete(memory);
        if (memory==consumeFailure) {consumeFailure=nullptr;return Result::HardwareError;}
        return result;
    }
};
static void consumedMemoryCompletion() {
    ConsumingMemoryAPI api;ConsumingMemoryAPI::Memory *pinned=nullptr;
    {
        USB::DMARegion<ConsumingMemoryAPI> region(api);
        assert(region.allocate(4096,64)==Result::Success);
        pinned=api.memories.back();api.consumeFailure=pinned;
        assert(region.release()==Result::HardwareError && !region.cleanupRetryable());
        const auto calls=api.completeCalls;
        assert(region.release()==Result::HardwareError && api.completeCalls==calls);
        assert(region.currentState()==DMAState::CleanupFailed && pinned->references==1 && !pinned->prepares);
    }
    // Fixture disposal after the owner is gone. Production retains this MD
    // because an aggregate completion error has no safe replay/clear proof.
    api.releaseMemory(pinned);
    auto *client=api.allocate(64,64);
    {
        USB::DeviceBounce<ConsumingMemoryAPI> transfer(api);
        assert(transfer.prepare(client,64,true,64)==Result::Success);
        api.consumeFailure=client;
        assert(transfer.finish(64,true)==Result::HardwareError && !transfer.cleanupRetryable());
        const auto calls=api.completeCalls,copies=api.copies;
        assert(transfer.finish(64,true)==Result::HardwareError && api.completeCalls==calls && api.copies==copies);
        assert(client->references==2 && !client->prepares);
    }
    api.releaseMemory(client);api.releaseMemory(client);
    USB::CleanupRecovery recovery;
    assert(!recovery.retry(true));recovery.failed(true);
    assert(!recovery.retry(false) && recovery.retry(true) && !recovery.retry(true));
    recovery.failed(true);assert(!recovery.retry(true));
    USB::CleanupRecovery invariant;invariant.failed(false);invariant.failed(true);
    assert(!invariant.retry(true));
}

static void regionOwnership() {
    MemoryAPI api; USB::DMARegion<MemoryAPI> ring(api);
    api.coherentFlag = false;
    assert(ring.allocate(4096, 64) == Result::NotReady && !api.allocationCalls);
    api.coherentFlag = true; api.mapperFlag = false;
    assert(ring.allocate(4096, 64) == Result::NotReady && !api.allocationCalls);
    api.bypassFlag = true;
    assert(ring.allocate(4096, 64) == Result::Success);
    for (unsigned i = 0; i < ring.capacity(); ++i) assert(static_cast<uint8_t *>(ring.bytes())[i] == 0);
    assert(ring.publish(9) == Result::Success);
    assert(ring.release() == Result::Busy && api.releases == 0);
    assert(ring.retire(8) == Result::NotReady);
    ring.quarantine(); assert(ring.retire(9) == Result::NotReady);
    assert(ring.release() == Result::Busy);
    ring.retireAfterIsolation();
    api.clearFailures = 1;
    assert(ring.release() == Result::HardwareError);
    assert(ring.currentState() == DMAState::CleanupFailed && api.releases == 0);
    assert(ring.publish(10) == Result::NotReady);
    assert(ring.release() == Result::Success && api.releases == 1);
    assert(ring.currentState() == DMAState::Empty);
}

static void allocationFailures() {
    for (int failure = 1; failure <= 6; ++failure) {
        MemoryAPI api; api.fail = failure; USB::DMARegion<MemoryAPI> ring(api);
        assert(ring.allocate(256, 64) != Result::Success);
        assert(ring.currentState() == DMAState::Empty && api.memories.empty() && api.commands.empty());
    }
    MemoryAPI api; USB::DMARegion<MemoryAPI> ring(api);
    for (uint64_t address : {UINT64_C(0xfffffff0), UINT64_C(0x100000000), UINT64_MAX}) {
        api.bits = 32; api.bus = address;
        assert(ring.allocate(256, 64) == Result::Invalid);
        assert(ring.currentState() == DMAState::Empty);
    }
    assert(ring.allocate(0, 64) == Result::Invalid);
    assert(ring.allocate(256, 24) == Result::Invalid);
    assert(ring.allocate(1024 * 1024 + 1, 64) == Result::Invalid);
}

static void bounceCopies() {
    MemoryAPI api; auto *client = api.allocate(128, 64);
    std::memset(client->data, 0xa5, 128);
    USB::DeviceBounce<MemoryAPI> request(api);
    assert(request.prepare(client, 65, true, 64) == Result::Success);
    assert(request.capacity() == 128 && client->references == 2 && client->prepares == 1);
    assert(request.publish(2) == Result::Success);
    std::memset(request.bytes(), 0x7b, 128); // hardware's final whole packet
    assert(request.finish(65, true) == Result::Busy);
    assert(request.retire(1) == Result::NotReady);
    assert(request.retire(2) == Result::Success);
    assert(request.finish(129, true) == Result::Overflow);
    assert(request.finish(65, true) == Result::Success);
    for (unsigned i = 0; i < 128; ++i) assert(static_cast<uint8_t *>(client->data)[i] == (i < 65 ? 0x7b : 0xa5));
    assert(client->references == 1 && client->prepares == 0);
    assert(request.prepare(client, 64, false, 64) == Result::Success);
    assert(request.capacity() == 64 && std::memcmp(request.bytes(), client->data, 64) == 0);
    assert(request.publish(3) == Result::Success);
    request.quarantine();
    assert(request.finish(0, false) == Result::Busy);
    request.retireAfterIsolation();
    assert(request.finish(0, false) == Result::Success);
    assert(request.prepare(nullptr, 0, true, 64) == Result::Success && request.capacity() == 64);
    assert(request.finish(0, false) == Result::Success);
    assert(request.prepare(nullptr, 0, false, 64) == Result::Success && request.capacity() == 0);
    assert(request.publish(4) == Result::Success && request.retire(4) == Result::Success);
    assert(request.finish(0, false) == Result::Success);
    api.releaseMemory(client);
}

struct EmptyDescriptorAPI : MemoryAPI {
    unsigned emptyPrepareCalls=0;
    Result prepare(Memory *memory,bool writes,bool reads) {
        if (!memory->size) {++emptyPrepareCalls;return Result::Invalid;}
        return MemoryAPI::prepare(memory,writes,reads);
    }
};
static void nativeEmptyDescriptor() {
    EmptyDescriptorAPI api;
    auto *client=api.allocate(64,64);
    client->size=0; // Native Mux's non-null MD with no logical client bytes.
    for (bool receive:{false,true}) {
        USB::DeviceBounce<EmptyDescriptorAPI> request(api);
        assert(request.prepare(client,0,receive,512)==Result::Success);
        assert(request.capacity()==(receive ? 512u:0u));
        assert(client->references==2 && !client->prepares && !api.emptyPrepareCalls);
        assert(request.publish(7)==Result::Success && request.retire(7)==Result::Success);
        assert(request.finish(1,true)==Result::Overflow);
        assert(request.finish(0,true)==Result::Success);
        assert(client->references==1 && !client->prepares && !api.copies);
    }
    api.releaseMemory(client);
}

static void cleanupRetries() {
    MemoryAPI api; auto *client = api.allocate(64, 64);
    USB::DeviceBounce<MemoryAPI> request(api);
    assert(request.prepare(client, 64, true, 64) == Result::Success);
    std::memset(request.bytes(), 0x5c, 64);
    // Fail ring memory complete, retry, then fail the client's complete after
    // ring memory was released. Retrying must neither copy from freed memory
    // nor invoke the client's writeBytes more than once.
    api.completeFailures = 1;
    assert(request.finish(64, true) == Result::HardwareError && api.copies == 1);
    api.completeFailures = 0;
    assert(request.finish(64, true) == Result::Success && api.copies == 1);
    assert(request.prepare(client, 64, true, 64) == Result::Success);
    api.failComplete = client;
    assert(request.finish(64, true) == Result::HardwareError);
    const unsigned copies = api.copies;
    assert(request.currentState() == DMAState::CleanupFailed);
    assert(request.finish(64, true) == Result::Success && api.copies == copies);
    for (int failure : {2, 7}) {
        api.fail = failure;
        assert(request.prepare(client, 64, false, 64) != Result::Success);
        assert(request.currentState() == DMAState::Empty && client->references == 1 && !client->prepares);
    }
    api.fail = 0;
    assert(request.prepare(client, 64, true, 64) == Result::Success);
    api.fail = 8;
    assert(request.finish(64, true) == Result::HardwareError);
    assert(request.currentState() == DMAState::Empty && client->references == 1);
    api.fail = 0; api.releaseMemory(client);
}

static void hostFamilyPreparation() {
    MemoryAPI api;auto *client=api.allocate(128,64);
    // Native Family, rather than HostBounce, owns this pair.
    assert(api.prepare(client,true,true)==Result::Success);
    std::memset(client->data,0xa5,128);
    USB::HostBounce<MemoryAPI> request(api);
    assert(request.prepareFamilyBuffer(client,65,true)==Result::Success);
    assert(client->references==2 && client->prepares==1 && request.capacity()==65);
    assert(request.publish(31)==Result::Success);
    assert(request.finish(64,true)==Result::Busy);
    assert(request.retire(30)==Result::NotReady && request.retire(31)==Result::Success);
    std::memset(request.bytes(),0x69,65);
    api.clearFailures=1;
    assert(request.finish(64,true)==Result::HardwareError && !api.copies);
    assert(request.finish(63,true)==Result::Invalid); // retry cannot change a completion
    api.completeFailures=1;
    assert(request.finish(64,true)==Result::HardwareError && api.copies==1);
    assert(client->prepares==1 && client->references==2);
    assert(request.finish(64,true)==Result::Success && api.copies==1);
    for (unsigned i=0;i<128;++i) assert(static_cast<uint8_t *>(client->data)[i]==(i<64 ? 0x69 : 0xa5));
    assert(client->references==1 && client->prepares==1);
    assert(request.prepareFamilyBuffer(client,65,false)==Result::Success);
    assert(std::memcmp(request.bytes(),client->data,65)==0);
    assert(request.publish(32)==Result::Success);
    request.quarantine();assert(request.finish(0,false)==Result::Busy);
    request.retireAfterIsolation();assert(request.finish(0,false)==Result::Success);
    assert(client->prepares==1);
    for (int failure=1;failure<=7;++failure) {
        api.fail=failure;
        assert(request.prepareFamilyBuffer(client,64,false)!=Result::Success);
        assert(request.currentState()==DMAState::Empty && client->references==1 && client->prepares==1);
    }
    api.fail=0;
    assert(request.prepareFamilyBuffer(client,64,true)==Result::Success);
    api.fail=8;
    assert(request.finish(64,true)==Result::HardwareError && request.currentState()==DMAState::Empty);
    api.fail=0;
    assert(request.prepareFamilyBuffer(nullptr,1,true)==Result::Invalid);
    assert(request.prepareFamilyBuffer(client,129,true)==Result::Invalid);
    assert(request.prepareFamilyBuffer(nullptr,0,true)==Result::Success);
    assert(request.publish(33)==Result::Success && request.retire(33)==Result::Success);
    assert(request.finish(0,true)==Result::Success);
    assert(client->prepares==1 && api.complete(client)==Result::Success);
    api.releaseMemory(client);
}

int main() {
    consumedMemoryCompletion();
    regionOwnership(); allocationFailures(); bounceCopies(); cleanupRetries();
    nativeEmptyDescriptor();
    hostFamilyPreparation();
    std::puts("USB DMA: leases, address limits, tails, quarantine, cleanup retries and native Host prepared-buffer ownership passed");
}
