// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandShutdown.h"
#include <cassert>
#include <cstdio>
using namespace Baseband;
struct Board {
    uint64_t now=0;
    unsigned pumps=0,samples=0,set=0,clear=0,gracePumps=2,stopSamples=3;
    bool aborted=false,requested=false,retainedAck=false,neverClear=false,transportBroken=false;
    Result begin=Result::OK,grace=Result::OK,observed=Result::NotReady,clearResult=Result::OK;
    uint64_t shutdownNowUS() const { return now; }
    bool shutdownAborted() const { return aborted; }
    void shutdownDelayMS(unsigned ms) { now+=ms*1000; }
    Result beginGraceful(uint64_t epoch,uint64_t start,unsigned timeout) { assert(epoch==1 && start==now && timeout);return begin; }
    bool gracefulPending() const { return pumps<gracePumps; }
    Result gracefulOutcome() const { return grace; }
    void recordGraceful(Result r) { observed=r; }
    Result sampleStop(bool &ack) {
        ++samples;ack=retainedAck;
        if(requested) {
            if(neverClear)ack=true;
            else ack=samples>=stopSamples;
        }
        return Result::OK;
    }
    Result pumpStop(uint64_t epoch,uint64_t time) {
        assert(epoch==1 && time==now);++pumps;return transportBroken?Result::IO:Result::OK;
    }
    Result stopBit(uint64_t epoch,bool value) {
        assert(epoch==1);requested=value;if(value)++set;else ++clear;return value?Result::OK:clearResult;
    }
};
int main() {
    Board protocol;ShutdownSequence<Board> a(protocol);
    assert(a.run(1,5000)==Result::OK && protocol.observed==Result::OK && !protocol.set && !protocol.clear && protocol.pumps==2);
    Board absent;absent.begin=Result::NotReady;ShutdownSequence<Board> b(absent);
    assert(b.run(1,5000)==Result::OK && absent.set==1 && absent.clear==1 && !absent.requested && absent.pumps);
    Board rejected;rejected.grace=Result::IO;rejected.stopSamples=6;ShutdownSequence<Board> c(rejected);
    assert(c.run(1,5000)==Result::OK && rejected.observed==Result::IO && rejected.set==1 && rejected.clear==1);
    Board stale;stale.begin=Result::NotReady;stale.retainedAck=stale.neverClear=true;ShutdownSequence<Board> d(stale);
    assert(d.run(1,3000)==Result::Timeout && stale.set==1 && stale.clear==1);
    Board fresh;fresh.begin=Result::NotReady;fresh.retainedAck=true;fresh.stopSamples=4;ShutdownSequence<Board> e(fresh);
    assert(e.run(1,5000)==Result::OK && fresh.set==1 && fresh.clear==1 && fresh.samples==4);
    Board timed;timed.gracePumps=1000;timed.stopSamples=12;ShutdownSequence<Board> f(timed);
    assert(f.run(1,5000)==Result::OK && timed.observed==Result::Timeout && timed.now>=5000 && timed.clear==1);
    Board broken;broken.begin=Result::NotReady;broken.transportBroken=true;ShutdownSequence<Board> g(broken);
    assert(g.run(1,5000)==Result::OK && broken.set==1 && broken.clear==1 && broken.pumps==1);
    Board unknown;unknown.begin=Result::Quarantined;ShutdownSequence<Board> h(unknown);
    assert(h.run(1,5000)==Result::Quarantined && !unknown.set);
    Board stopUnknown;stopUnknown.begin=Result::NotReady;stopUnknown.clearResult=Result::Quarantined;
    ShutdownSequence<Board> i(stopUnknown);assert(i.run(1,5000)==Result::Quarantined);
    Board watchdog;watchdog.aborted=true;ShutdownSequence<Board> j(watchdog);
    assert(j.run(1,5000)==Result::NotReady && !watchdog.pumps && !watchdog.set);
    std::puts("baseband real graceful ACK, serviced SMP fallback, retained-ACK rejection and bounded stop checks passed");
}
