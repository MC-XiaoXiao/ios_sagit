// SPDX-License-Identifier: BSD-2-Clause
#include "USBControllerResources.h"
#include "USBInterconnect.h"
#include "USBDirectDMA.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

using USB::Result;
namespace CC=ClockController;
struct Backend {
    struct Event { CC::Operation op; unsigned id; };
    std::vector<Event> events;
    std::map<unsigned,unsigned> refs;
    std::map<unsigned,bool> pins;
    uint64_t core=19200000, mock=19200000;
    unsigned calls=0, failAt=0, haltCount=0;
    bool domain=false, reset=false, halted=false, dmaStarted=true, failHalt=false, applied=false;
    bool profiled=false,proof=true,handoffCapability=true,domainCapability=true,baselines=false,prepareApplied=false,revokeReset=false,badPrepareValue=false;
    uint64_t cxFloor=256,cxAcknowledged=256,changedCore=0;
    unsigned bootCalls=0,bootFailAt=0,prepareCalls=0,preparedHz=0,undervoltageEvents=0;
    Result prepareResult=Result::Success;
    Result failure=Result::NotReady;
    bool bootStopped() {return proof;}
    Result clockRequest(CC::Controller controller,CC::Operation op,unsigned id,uint64_t &value) {
        if(++bootCalls==bootFailAt)return Result::NotReady;
        if(controller==CC::GCC)return request(op,id,value);
        assert(controller==CC::RPM && !value);
        auto r=CC::targeted(controller,op,id,value,CC::USBControllerConsumer);assert(CC::valid(r.request));
        events.push_back({op,0x80000000u|id});
        if(op==CC::GetCapabilities) {
            assert(!id);value=(handoffCapability ? uint64_t(CC::ResourceHandoff):0)|(domainCapability ? uint64_t(CC::Domains):0);
            return Result::Success;
        }
        if(op==CC::GetDomainLevel) {
            assert(id==0x1000 && baselines);value=cxAcknowledged;
            if(changedCore)core=changedCore;
            return Result::Success;
        }
        assert(op==CC::PrepareUSBResources);++prepareCalls;
        preparedHz=id;
        if(cxFloor<QcomPower::usbCoreCXLevel(id))return Result::Unsupported;
        if(prepareResult==Result::Success || prepareApplied) {
            baselines=true;
            // Independent FMAX model observes the rate still clocking the
            // hardware when the persistent AP policy reaches RPM.
            const unsigned required=core<=66670000 ? 64u:core<=133330000 ? 128u:core<=200000000 ? 256u:384u;
            if(cxFloor<required)++undervoltageEvents;
        }
        if(revokeReset)reset=false;
        if(badPrepareValue)value=1;
        return prepareResult;
    }
    Result prepareClocks(unsigned coreHz) {return profiled ? USB::prepareControllerBootResources(*this,coreHz):Result::Success;}
    Result request(CC::Operation op, unsigned id, uint64_t &value) {
        events.push_back({op,id});
        if(profiled && op!=CC::GetCapabilities && op!=CC::GetResetState && op!=CC::GetRate)assert(baselines);
        const bool fail=++calls==failAt;
        if (fail && !applied) return failure;
        switch (op) {
        case CC::GetCapabilities:value=CC::Clocks|CC::Domains|CC::Resets|CC::ClockHandoff;break;
        case CC::RetainDomain:assert(id==2 && !domain);domain=true;break;
        case CC::ReleaseDomain:assert(id==2 && reset);domain=false;break;
        case CC::RetainClock:
            assert(domain);
            // The controller-local branch cannot finish its hardware
            // handshake while BCR is asserted (observed on the device).
            if (reset && (id==116 || id==117 || id==118)) return Result::Timeout;
            ++refs[id];break;
        case CC::ReleaseClock:assert(refs[id] && reset);--refs[id];break;
        case CC::AdoptClock:assert((halted && refs[id]) || reset);pins[id]=false;break;
        case CC::GetResetState:value=reset; if (reset) { halted=true;dmaStarted=false; }break;
        case CC::AssertReset:assert((halted || !dmaStarted) && domain);reset=true;break;
        case CC::DeassertReset:assert(reset && domain);reset=false;halted=false;break;
        case CC::SetRate:
            assert(reset);
            for (auto entry:refs) assert(!entry.second);
            if (id==163) core=value; else { assert(id==164);mock=value; } break;
        case CC::GetRate:value=(id==116 || id==163) ? core:mock;break;
        default:assert(false);
        }
        return fail ? failure : Result::Success;
    }
    Result halt() {
        ++haltCount;
        assert(domain);
        for (auto id:{71,116,32,118,117}) assert(refs[id]);
        if (failHalt) return Result::Timeout;
        halted=true;dmaStarted=false; return Result::Success;
    }
    bool noReferences() const {
        if (domain) return false;
        for (auto entry:refs) if (entry.second) return false;
        return true;
    }
};
static void lifecycle() {
    Backend peer; USB::ControllerResources<Backend> resources(peer);
    assert(resources.empty() && resources.acquire(123)==Result::Invalid && peer.events.empty());
    assert(resources.acquire(120000000)==Result::Success && resources.inReset() && resources.coreHz()==120000000);
    assert(peer.haltCount==1 && !resources.empty());
    assert(resources.acquire(60000000)==Result::Busy);
    assert(resources.activate()==Result::Success && !resources.inReset());
    peer.dmaStarted=true;
    assert(resources.isolate()==Result::Success && peer.haltCount==2 && resources.inReset());
    assert(resources.release()==Result::Success && resources.empty() && peer.noReferences());
    assert(resources.acquire(60000000)==Result::Success && resources.coreHz()==60000000);
    assert(resources.release()==Result::Success);
    peer.reset=false;peer.halted=false; // another warm firmware incarnation
    peer.failHalt=true;
    assert(resources.acquire(150000000)==Result::Timeout && !peer.reset && !resources.empty());
    assert(resources.release()==Result::Timeout && !peer.noReferences());
    peer.failHalt=false; assert(resources.release()==Result::Success);
}
static void failures() {
    Backend normal; USB::ControllerResources<Backend> reference(normal);
    assert(reference.acquire(120000000)==Result::Success);
    const auto count=normal.calls;
    for (bool apply:{false,true}) for (unsigned n=1;n<=count;++n) {
        Backend b; USB::ControllerResources<Backend> r(b);b.failAt=n;b.applied=apply;
        // Read/no-side-effect and acknowledged rejections are recoverable.
        // A timed-out clock operation is deliberately quarantined.
        b.failure=Result::Timeout;
        assert(r.acquire(120000000)!=Result::Success);
        const auto before=b.events.size(); const auto cleaned=r.release();
        if (r.failed()) assert(cleaned==Result::HardwareError && !r.empty() && b.events.size()==before);
        else if (cleaned==Result::Success) assert(r.empty() && b.noReferences());
        else assert(cleaned==Result::NotReady && !r.empty()); // partial inherited clock access
    }
    Backend reset; USB::ControllerResources<Backend> r(reset);
    assert(r.acquire(120000000)==Result::Success);
    reset.failAt=reset.calls+1;reset.failure=Result::Timeout;reset.applied=true;
    assert(r.activate()==Result::Timeout && !r.inReset());
    // A partially deasserted reset must be asserted again before teardown.
    assert(r.release()==Result::Success && reset.reset && reset.noReferences());
}

static void deferredLocalClocks() {
    Backend b;b.reset=true;USB::ControllerResources<Backend> r(b);
    assert(r.acquire(60000000)==Result::Success && r.inReset());
    assert(b.refs[71]==1 && b.refs[32]==1);
    for (unsigned id:{116,117,118}) assert(!b.refs[id]);
    const auto begin=b.events.size();
    assert(r.activate()==Result::Success && !b.reset);
    assert(b.events[begin].op==CC::DeassertReset);
    const unsigned local[]={117,118,116};
    for (unsigned i=0;i<3;++i) {
        assert(b.events[begin+1+i].op==CC::RetainClock && b.events[begin+1+i].id==local[i]);
        assert(b.refs[local[i]]==1);
    }
    assert(r.isolate()==Result::Success && r.inReset());
    assert(r.activate()==Result::Success);
    for (unsigned id:local) assert(b.refs[id]==1); // no duplicate holds on reactivation
    assert(r.release()==Result::Success && b.noReferences());
    for (unsigned step=2;step<=4;++step) for (bool applied:{false,true}) {
        Backend p;p.reset=true;USB::ControllerResources<Backend> partial(p);
        assert(partial.acquire(60000000)==Result::Success);
        p.failAt=p.calls+step;p.failure=Result::Timeout;p.applied=applied;
        assert(partial.activate()==Result::Timeout && partial.failed());
        const auto writes=p.events.size();
        assert(partial.release()==Result::HardwareError && !partial.empty() && p.events.size()==writes);
    }
}
static void protocol() {
    USBInterconnect::Request r;
    assert(USBInterconnect::valid(r));
    r.action=USBInterconnect::Action::Acquire;assert(!USBInterconnect::valid(r));
    r.averageBytesPerSecond=240000000;r.peakBytesPerSecond=800000000;assert(USBInterconnect::valid(r));
    r.lease=1;assert(!USBInterconnect::valid(r));
    r.action=USBInterconnect::Action::Release;r.averageBytesPerSecond=r.peakBytesPerSecond=0;
    assert(USBInterconnect::valid(r));r.flags=USBInterconnect::Granted;assert(!USBInterconnect::valid(r));
    auto clock=CC::targeted(CC::GCC,CC::AdoptClock,116,0,CC::USBControllerConsumer);
    assert(CC::valid(clock.request));
    clock.request.version=1;assert(!CC::valid(clock.request));
    auto prepare=CC::targeted(CC::RPM,CC::PrepareUSBResources,60000000,0,CC::USBControllerConsumer);
    assert(CC::valid(prepare.request));
    for(unsigned hz:{120000000u,150000000u}) {prepare.request.id=hz;assert(CC::valid(prepare.request));}
    prepare.request.id=60000000;
    for(unsigned n=0;n<6;++n) {
        auto bad=prepare;
        switch(n) {
        case 0:bad.controller=CC::GCC;break;
        case 1:bad.consumer=CC::USB2PHYConsumer;break;
        case 2:bad.flags=1;break;
        case 3:bad.request.id=1;break;
        case 4:bad.request.value=1;break;
        case 5:bad.request.version=1;break;
        }
        assert(!CC::valid(bad.request));
    }
    prepare.request.id=0;assert(!CC::valid(prepare.request)); // old unqualified request
}
static void preparedBoot() {
    Backend b;b.profiled=true;b.reset=true;USB::ControllerResources<Backend> resources(b);
    assert(resources.acquire(60000000)==Result::Success && b.prepareCalls==1 && b.baselines);
    assert(b.events.size()>8 && b.events[0].op==CC::GetResetState && b.events[1].op==CC::GetRate &&
        b.events[1].id==163 && b.events[2].op==CC::GetCapabilities && b.events[2].id==0x80000000u &&
        b.events[3].op==CC::PrepareUSBResources && b.events[3].id==(0x80000000u|60000000u) &&
        b.events[4].op==CC::GetDomainLevel && b.events[4].id==0x80001000u &&
        b.events[5].op==CC::GetRate && b.events[5].id==163 && b.events[6].op==CC::GetResetState &&
        b.events[8].op==CC::RetainDomain);
    assert(resources.release()==Result::Success && b.baselines);
    b.baselines=false;assert(resources.acquire(60000000)==Result::Success && b.prepareCalls==2);
    assert(resources.release()==Result::Success);
    for(unsigned failure=0;failure<14;++failure) {
        Backend p;p.profiled=true;p.reset=true;
        if(failure==0)p.proof=false;
        else if(failure==1)p.reset=false;
        else if(failure==2)p.handoffCapability=false;
        else if(failure==3)p.revokeReset=true;
        else if(failure==4)p.badPrepareValue=true;
        else if(failure==5)p.prepareResult=Result::Busy;
        else if(failure==6)p.prepareResult=Result::HardwareError;
        else if(failure==7) {p.prepareResult=Result::Timeout;p.prepareApplied=true;}
        else if(failure==8)p.prepareResult=Result::Unsupported;
        else if(failure==9)p.prepareResult=Result::NotReady;
        else if(failure==10)p.domainCapability=false;
        else if(failure==11)p.cxFloor=48;
        else if(failure==12)p.cxAcknowledged=48;
        else p.cxAcknowledged=513;
        USB::ControllerResources<Backend> r(p);
        assert(r.acquire(60000000)!=Result::Success && r.empty() && !p.domain && p.refs.empty());
        const auto events=p.events.size();assert(r.release()==Result::Success && p.events.size()==events);
        for(const auto &event:p.events)assert(event.op==CC::GetResetState || event.op==CC::GetCapabilities ||
            event.op==CC::PrepareUSBResources || event.op==CC::GetDomainLevel || event.op==CC::GetRate);
        if(failure==0)assert(p.events.empty() && !p.bootCalls);
        if(failure==7)assert(p.baselines); // RPM retains an unknown applied baseline; GCC was untouched
    }
    for(unsigned phase=1;phase<=7;++phase) {
        Backend p;p.profiled=true;p.reset=true;p.bootFailAt=phase;
        USB::ControllerResources<Backend> r(p);
        assert(r.acquire(60000000)==Result::NotReady && r.empty() && !p.domain && p.refs.empty());
    }
    const unsigned rates[]={60000000,120000000,150000000},levels[]={64,128,256};
    for(unsigned inherited=0;inherited<4;++inherited)for(unsigned i=0;i<3;++i) {
        const unsigned initial=inherited ? rates[inherited-1]:19200000;
        const unsigned covered=inherited && inherited-1>i ? inherited-1:i;
        Backend p;p.profiled=true;p.reset=true;p.core=initial;p.cxFloor=p.cxAcknowledged=levels[covered];
        USB::ControllerResources<Backend> r(p);
        assert(r.acquire(rates[i])==Result::Success && p.preparedHz==rates[covered] && !p.undervoltageEvents);
        assert(r.release()==Result::Success);
        p.core=initial;p.cxFloor=covered ? levels[covered-1]:48;
        assert(r.acquire(rates[i])==Result::Unsupported && r.empty() && p.noReferences() && !p.undervoltageEvents);
    }
    const uint64_t unknownRates[]={0,1,59999999,150000001,UINT64_C(0x100000000),UINT64_MAX};
    for(uint64_t unknown:unknownRates) {
        Backend p;p.profiled=true;p.reset=true;p.core=unknown;USB::ControllerResources<Backend> r(p);
        assert(r.acquire(60000000)!=Result::Success && r.empty() && p.noReferences() && !p.prepareCalls);
    }
    {
        Backend p;p.profiled=true;p.reset=true;p.core=60000000;p.changedCore=120000000;
        USB::ControllerResources<Backend> r(p);
        assert(r.acquire(60000000)==Result::HardwareError && r.empty() && p.noReferences() && !p.undervoltageEvents);
    }
    puts("USB prepared boot: inherited/target clock coverage, stable rate and BCR across AP ACKs before GCC mutation, retry and unknown-ACK retention passed");
}
struct DirectBackend {
    USB::DirectDMAWindow window{1,32,0,0,uint64_t(1)<<36};
    bool mapper=false,reset=true,fabric=true,changeOnRead=false,route=true;
    unsigned reads=0,routeChecks=0;
    Result readDMAWindow(USB::DirectDMAWindow &out) {
        ++reads;
        if (mapper) return Result::Unsupported;
        out=window;
        if (changeOnRead && reads%2==0) out.length>>=1;
        return Result::Success;
    }
    Result dmaAccessible() { return fabric ? Result::Success : Result::Timeout; }
    Result dmaRoute() {assert(fabric);++routeChecks;return route ? Result::Success:Result::HardwareError;}
    Result dmaIsolated() { return reset ? Result::Success : Result::Busy; }
};
static void directDMA() {
    uint8_t wire[32]{};wire[0]=1;wire[4]=32;wire[28]=16; // length=2^36
    USB::DirectDMAWindow decoded{};
    assert(USB::DirectDMAWindow::decode(wire,32,decoded) && decoded.valid(36));
    assert(!USB::DirectDMAWindow::decode(nullptr,32,decoded));
    assert(!USB::DirectDMAWindow::decode(wire,31,decoded));
    assert(!decoded.valid(32) && !decoded.valid(64));
    for (unsigned offset:{0u,4u,8u,16u,24u,31u}) {
        wire[offset]^=2;assert(USB::DirectDMAWindow::decode(wire,32,decoded) && !decoded.valid(36));wire[offset]^=2;
    }
    DirectBackend peer;USB::DirectDMA<DirectBackend> dma(peer);
    assert(dma.check()==Result::NotReady && dma.release()==Result::Success);
    peer.mapper=true;assert(dma.acquire(36)==Result::Unsupported && !dma.owned());peer.mapper=false;
    peer.reset=false;assert(dma.acquire(36)==Result::Busy && !dma.owned());peer.reset=true;
    peer.fabric=false;assert(dma.acquire(36)==Result::Timeout && !dma.owned());peer.fabric=true;
    peer.changeOnRead=true;peer.reads=0;
    assert(dma.acquire(36)==Result::HardwareError && !dma.owned());peer.changeOnRead=false;
    assert(dma.acquire(36)==Result::Success && dma.owned() && !dma.faulted());
    assert(dma.acquire(36)==Result::Busy);
    peer.reset=false;assert(dma.check()==Result::Success);
    assert(dma.release()==Result::Busy && dma.owned());
    peer.window.physicalBase=4096;
    assert(dma.check()==Result::HardwareError && dma.faulted());
    peer.window.physicalBase=0;
    assert(dma.check()==Result::HardwareError); // restoring metadata cannot reopen admission
    peer.reset=true;peer.mapper=true;
    assert(dma.release()==Result::Unsupported && dma.owned());peer.mapper=false;
    peer.fabric=false;assert(dma.release()==Result::Timeout && dma.owned());peer.fabric=true;
    assert(dma.release()==Result::Success && !dma.owned() && !dma.faulted());
    assert(dma.acquire(36)==Result::Success);
    peer.fabric=false;assert(dma.check()==Result::Timeout && dma.faulted());peer.fabric=true;
    assert(dma.check()==Result::HardwareError && dma.release()==Result::Success);
    // A distinct address-width epoch is admitted only after complete release.
    peer.window.length=uint64_t(1)<<32;
    assert(dma.acquire(36)==Result::Unsupported && dma.acquire(32)==Result::Success);
    assert(dma.release()==Result::Success);
    peer.window.length=uint64_t(1)<<36;peer.route=false;
    assert(dma.acquire(36)==Result::HardwareError && !dma.owned());peer.route=true;
    assert(dma.acquire(36)==Result::Success);peer.route=false;
    assert(dma.check()==Result::HardwareError && dma.faulted() && dma.release()==Result::HardwareError && dma.owned());
    peer.route=true;assert(dma.release()==Result::Success && !dma.owned());
    const auto before=peer.routeChecks;peer.fabric=false;
    assert(dma.acquire(36)==Result::Timeout && peer.routeChecks==before);peer.fabric=true;
}
int main() {
    lifecycle();failures();deferredLocalClocks();protocol();directDMA();preparedBoot();
    std::puts("USB resources: inherited halt, clock adoption, reset/rate sequencing, partial leases, NoC and explicit direct-DMA ownership passed");
}
