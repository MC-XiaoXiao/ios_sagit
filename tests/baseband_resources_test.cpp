// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandResources.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <map>
#include <tuple>
using namespace Baseband;
using namespace ClockController;
struct Resources {
    struct Call { Controller kind; Operation op; uint32_t id; uint64_t value; unsigned set; };
    std::vector<Call> calls;
    std::map<std::tuple<Controller,uint32_t,unsigned>,uint64_t> votes;
    bool reset=true, leased=false, undersupply=false;
    uint64_t token=0;
    unsigned failAt=0,coldChecks=0;
    Result failure=Result::Unsupported;
    Result cold() { ++coldChecks; return reset ? Result::OK:Result::NotReady; }
    Result request(Controller kind,Operation op,uint32_t id,uint64_t &value,unsigned set) {
        calls.push_back({kind,op,id,value,set});
        assert(valid(targeted(kind,op,id,value,BasebandConsumer,set).request));
        if(calls.size()==failAt)return failure;
        if(op==RetainModemState) {
            assert(!leased && !value && !id && reset);leased=true;value=token=19;return Result::OK;
        }
        assert(leased);
        if(op==ValidateModemState || op==ReleaseModemState) {
            assert(!id && value==token);
            if(op==ReleaseModemState) {
                assert(reset);
                for(const auto &entry:votes)assert(!entry.second);
                leased=false;
            }
            return Result::OK;
        }
        auto &current=votes[{kind,id,set}];
        if(kind==GCC) {
            assert(op==RetainClock || op==ReleaseClock);
            if(op==RetainClock) { assert(!current);current=1; }
            else { assert(current==1 && reset);current=0; }
        } else {
            assert(op==SetDomainLevel || op==SetModemClockVote);
            if(!value)assert(reset);
            current=value;
            if(undersupply && value) --value;
        }
        return Result::OK;
    }
};
int main() {
    const ResourcePolicy policy{512,512,4};assert(policy.valid() && !ResourcePolicy{}.valid());
    assert(!(ResourcePolicy{256,512,4}.valid()) && !(ResourcePolicy{512,512,1}.valid()));
    Resources io;ResourceLease<Resources> lease(io,policy);
    assert(lease.acquire()==Result::OK && lease.ready() && lease.held());
    assert(io.calls.size()==15 && io.calls[0].op==RetainModemState && io.coldChecks==1);
    for(unsigned i=1;i<=4;++i)assert(io.calls[i].op==SetDomainLevel);
    for(unsigned i=5;i<=8;++i)assert(io.calls[i].op==SetModemClockVote);
    const uint32_t clocks[]={172,171,36,173,175,174};
    for(unsigned i=0;i<6;++i)assert(io.calls[i+9].op==RetainClock && io.calls[i+9].id==clocks[i]);
    assert(lease.acquire()==Result::Busy && io.calls.size()==15);
    io.reset=false;assert(lease.validate()==Result::OK);
    io.reset=true;assert(lease.release()==Result::OK && !lease.held() && !lease.ready() && !io.leased);
    assert(io.calls.back().op==ReleaseModemState);
    for(const auto &entry:io.votes)assert(!entry.second);
    // Every definite acquisition rejection unwinds only acknowledged steps.
    for(unsigned fail=1;fail<=15;++fail) {
        Resources broken;broken.failAt=fail;
        ResourceLease<Resources> b(broken,policy);
        assert(b.acquire()==Result::Unsupported && !b.held() && !b.quarantined() && !broken.leased);
        for(const auto &entry:broken.votes)assert(!entry.second);
    }
    // An ambiguous acquisition preserves prior votes and its exclusive token.
    for(unsigned fail=1;fail<=15;++fail) {
        Resources unknown;unknown.failAt=fail;unknown.failure=Result::Timeout;
        ResourceLease<Resources> u(unknown,policy);
        assert(u.acquire()==Result::Quarantined && u.quarantined() && unknown.calls.size()==fail);
        const auto calls=unknown.calls.size();
        assert(u.acquire()==Result::Quarantined && u.validate()==Result::Quarantined &&
               u.release()==Result::Quarantined && unknown.calls.size()==calls);
    }
    // Missing reset/drain proof refuses all vote releases and retains state.
    Resources running;ResourceLease<Resources> r(running,policy);
    assert(r.acquire()==Result::OK);running.reset=false;
    assert(r.release()==Result::Quarantined && running.leased && r.held());
    for(unsigned fail=16;fail<=31;++fail) {
        Resources broken;ResourceLease<Resources> b(broken,policy);
        assert(b.acquire()==Result::OK);broken.failAt=fail;broken.failure=Result::Timeout;
        assert(b.release()==Result::Quarantined && broken.calls.size()==fail && b.quarantined());
    }
    // An ACK reporting a lower aggregate is not a valid voltage lease.
    Resources low;low.undersupply=true;ResourceLease<Resources> l(low,policy);
    assert(l.acquire()==Result::Quarantined && low.calls.size()==2 && low.leased);
    Resources invalid;ResourceLease<Resources> bad(invalid,{0,256,4});
    assert(bad.acquire()==Result::Invalid && invalid.calls.empty());
    std::puts("baseband exclusive resource lease/order/failure unwind/vote retention checks passed");
}
