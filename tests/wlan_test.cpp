// SPDX-License-Identifier: BSD-2-Clause
#include <WLANProtocol.h>
#include <WLANDriverPolicy.h>
#include "WLANLifecycle.h"
#include "WLANCopyEngine.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

using namespace WLAN;
struct Backend {
    Resource failAcquire = Resource(0), failRelease = Resource(0);
    bool partial = false;
    bool uncertain(Resource resource,uint64_t) const { return partial && resource == failAcquire; }
    std::vector<unsigned> calls;
    Result acquire(Resource r, uint64_t epoch) {
        assert(epoch); calls.push_back(r); return r == failAcquire ? Result::NotReady : Result::Success;
    }
    Result release(Resource r, uint64_t epoch) {
        assert(epoch); calls.push_back(0x100U | r); return r == failRelease ? Result::Timeout : Result::Success;
    }
};
static constexpr Resource resources[] = {Power,Clock,Interconnect,DMA,Firmware,CopyEngines};

static void lifecycle() {
    for (unsigned failure = 0; failure < 6; ++failure) {
        Backend backend; backend.failAcquire = resources[failure]; Lifecycle<Backend> life(backend);
        assert(life.start() == Result::Busy); life.attach();
        assert(life.start() == Result::NotReady);
        assert(life.currentState() == State::WaitingForResources && !life.resources() && !life.accepts(1));
        assert(backend.calls.size() == 2 * failure + 1);
        for (unsigned n = 0; n <= failure; ++n) assert(backend.calls[n] == resources[n]);
        for (unsigned n = 0; n < failure; ++n) assert(backend.calls[failure + 1 + n] == (0x100U | resources[failure - 1 - n]));
        backend.failAcquire = Resource(0);
        assert(life.start() == Result::Success && life.epoch() == 2 && life.resources() == AllResources);
        assert(!life.accepts(1) && life.accepts(2));
        assert(life.detach() == Result::Success && life.currentState() == State::Detached);
    }
    for (unsigned failure = 0; failure < 6; ++failure) {
        Backend backend; Lifecycle<Backend> life(backend); life.attach(); assert(life.start() == Result::Success);
        backend.calls.clear(); backend.failRelease = resources[failure];
        assert(life.stop() == Result::Timeout && life.currentState() == State::Fault);
        uint32_t held = 0; for (unsigned n = 0; n <= failure; ++n) held |= resources[n];
        assert(life.resources() == held && backend.calls.size() == 6 - failure);
        assert(life.start() == Result::Busy && !life.accepts(1));
        backend.failRelease = Resource(0);
        assert(life.stop() == Result::Success && !life.resources());
    }
    Backend backend; Lifecycle<Backend> life(backend); life.attach(); assert(life.start() == Result::Success);
    const auto epoch = life.epoch();
    life.firmwareCrashed(epoch + 1); assert(life.accepts(epoch));
    life.firmwareCrashed(epoch); assert(life.currentState() == State::Fault && life.resources() == AllResources);
    assert(life.stop() == Result::Success); assert(life.start() == Result::Success);
    assert(life.epoch() == epoch + 1 && !life.accepts(epoch));
    Backend partial; partial.failAcquire = Firmware; partial.failRelease = DMA;
    Lifecycle<Backend> failed(partial); failed.attach();
    assert(failed.start() == Result::Timeout && failed.currentState() == State::Fault);
    assert(failed.resources() == (Power | Clock | Interconnect | DMA));
    assert(failed.detach() == Result::Timeout && failed.resources());
    partial.failRelease = Resource(0); assert(failed.detach() == Result::Success && !failed.resources());
    Backend shared; shared.failAcquire = Firmware; shared.failRelease = Firmware; shared.partial = true;
    Lifecycle<Backend> quarantine(shared); quarantine.attach();
    assert(quarantine.start() == Result::Timeout && (quarantine.resources() & Firmware));
    assert(quarantine.resources() == (Power | Clock | Interconnect | DMA | Firmware));
    shared.failRelease = Resource(0); assert(quarantine.detach() == Result::Success);
    assert(driverAdmission(1,false,InitialMissing,false) == Result::NotReady);
    assert(driverAdmission(2,true,0,true) == Result::Unsupported);
    assert(driverAdmission(1,true,0,false) == Result::Unsupported);
    assert(driverAdmission(1,true,NativeInterfaceABI,true) == Result::Unsupported);
    assert(driverAdmission(1,true,DeviceMapper,true) == Result::NotReady);
    assert(driverAdmission(1,true,0,true) == Result::Success);
}

static void copyEngines() {
    constexpr uint64_t limit = uint64_t(1) << 35;
    assert(validDMA(limit - 16,16,8)); assert(!validDMA(limit - 16,17));
    assert(!validDMA(0,32)); assert(!validDMA(0x1000,UINT64_MAX)); assert(!validDMA(0x1001,16,8));
    assert(!validDMA(0x1000,16,3));
    CEDescriptor d = {}; assert(encodeDescriptor(d,0x712345678ULL,0x234,0xabc) == Result::Success);
    assert(read32(d.bytes) == 0x12345678 && read32(d.bytes + 4) == 7);
    assert(read16(d.bytes + 8) == 0x234 && read16(d.bytes + 10) == 0xabc0 && read32(d.bytes + 12) == 0);
    assert(encodeDescriptor(d,0x1000,1,0x1000) == Result::Invalid);
    assert(encodeDescriptor(d,limit - 1,2,0) == Result::Invalid);
    assert(encodeDescriptor(d,0x1000,65536,0) == Result::Invalid);
    assert(Pipes[4].sourceEntries == 2048 && Pipes[6].maximumBytes == 0 && Pipes[11].destinationEntries == 512);
    CopyEngineRing<4> ring; unsigned index = 99; uint64_t cookie = 99;
    assert(ring.submit(1,1,index) == Result::NotReady); assert(ring.begin(1) == Result::Success);
    for (unsigned i = 0; i < 3; ++i) assert(ring.submit(1,i + 1,index) == Result::Success && index == i);
    assert(ring.submit(1,4,index) == Result::Busy);
    assert(ring.complete(2,0,cookie) == Result::NotReady && ring.pending() == 3);
    assert(ring.complete(1,2,cookie) == Result::Invalid);
    assert(ring.complete(1,0,cookie) == Result::Success && cookie == 1);
    assert(ring.submit(1,2,index) == Result::Invalid);
    assert(ring.submit(1,4,index) == Result::Success && index == 3);
    for (unsigned i = 1; i < 4; ++i) assert(ring.complete(1,i,cookie) == Result::Success && cookie == i + 1);
    assert(ring.submit(1,5,index) == Result::Success && index == 0);
    ring.timeout(); assert(ring.isQuarantined() && ring.pending() == 1);
    assert(ring.begin(2) == Result::Busy && ring.complete(1,0,cookie) == Result::NotReady);
    assert(ring.retireAfterIsolation(2) == Result::NotReady);
    assert(ring.retireAfterIsolation(1) == Result::Success && !ring.pending());
    assert(ring.begin(1) == Result::Invalid);
    assert(ring.begin(2) == Result::Success && ring.submit(1,6,index) == Result::NotReady);
    for (unsigned n = 0; n < 10000; ++n) {
        assert(ring.submit(2,n + 1,index) == Result::Success);
        assert(ring.complete(2,index,cookie) == Result::Success && cookie == n + 1);
    }
}

int main() {
    lifecycle(); copyEngines();
    assert(validCall(GetInfo,0,InfoScalars)); assert(!validCall(GetInfo,1,InfoScalars));
    assert(validCall(SetPower,1,0,1)); assert(!validCall(SetPower,1,0,2));
    assert(!validCall(3,0,0)); assert(!validCall(GetStatus,0,StatusScalars - 1));
    puts("WLAN: resource rollback, DMA quarantine, generations, CE ownership and client admission passed");
}
