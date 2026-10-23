// SPDX-License-Identifier: BSD-2-Clause
#include "XHCIContexts.h"
#include "XHCICommands.h"
#include <array>
#include <cassert>
#include <functional>
#include <map>

namespace {
using USB::Result;using USB::Speed;
struct Order {
    unsigned reads=0,writes=0;
    void dmaReadBarrier() { ++reads; }
    void dmaWriteBarrier() { ++writes; }
};
void contexts() {
    for (unsigned native=0;native<4;++native) {
        uint8_t encoded=255;const auto speed=XHCI::speedFromFamily(native);
        assert(speed!=Speed::Unknown && XHCI::speedToFamily(speed,encoded) && encoded==native);
    }
    uint8_t invalid=255;
    assert(XHCI::speedFromFamily(4)==Speed::Unknown && !XHCI::speedToFamily(Speed::Unknown,invalid) && invalid==255);
    for (unsigned stride:{32u,64u}) {
        alignas(64) std::array<uint32_t,(33*64+64)/4> words;
        words.fill(0xa5a5a5a5);XHCI::InputContext input;
        assert(input.bind(words.data(),33*stride,0x100000,stride,36,32,2)==Result::Success);
        for (unsigned n=0;n<33*stride/4;++n) assert(!words[n]);
        for (unsigned n=33*stride/4;n<words.size();++n) assert(words[n]==0xa5a5a5a5);
        XHCI::PortProtocol protocol{};protocol.major=2;protocol.speeds[1]=Speed::Full;
        protocol.speeds[2]=Speed::Low;protocol.speeds[3]=Speed::High;
        XHCI::SlotConfiguration slot;slot.route=0x4321;slot.rootPort=2;slot.speed=Speed::High;slot.speedID=3;slot.entries=7;
        assert(input.slot(slot,protocol)==Result::Success);
        const unsigned at=stride/4;
        assert(words[at]==(0x4321|3u<<20|7u<<27) && words[at+1]==(2u<<16) && !words[at+3]);
        XHCI::EndpointConfiguration ep;ep.type=XHCI::EndpointType::Control;ep.maxPacket=64;
        ep.speed=Speed::High;ep.dequeue=0x900001000;
        assert(input.endpoint(ep)==Result::Success);
        assert(words[2*at+1]==(64u<<16|4u<<3|6) && words[2*at+2]==0x1001 && words[2*at+3]==9);
        assert(words[2*at+4]==8 && words[1]==3);
        ep.type=XHCI::EndpointType::Interrupt;ep.address=0x82;ep.interval=16;ep.maxPacket=1024|(2u<<11);
        assert(input.endpoint(ep)==Result::Success);
        const unsigned dci=5;
        assert(words[(dci+1)*at]==(15u<<16));
        assert(words[(dci+1)*at+1]==(1024u<<16|2u<<8|7u<<3|6));
        assert(words[(dci+1)*at+4]==(3072u<<16|3072));
        auto unchanged=words;
        for (unsigned bad=0;bad<7;++bad) {
            auto candidate=ep;
            switch (bad) {
            case 0:candidate.maxPacket|=1u<<11;break; // reserved 4 transactions
            case 1:candidate.interval=17;break;
            case 2:candidate.dequeue=0;break;
            case 3:candidate.address=0x92;break;
            case 4:candidate.dequeue|=1;break;
            case 5:candidate.companion=true;break;
            case 6:candidate.speed=Speed::Low;break;
            }
            assert(input.endpoint(candidate)==Result::Invalid && words==unchanged);
        }
        assert(input.drop(1)==Result::Invalid && input.drop(dci)==Result::Success);
        assert((words[0]&(1u<<dci)) && !(words[1]&(1u<<dci)));
        assert(input.endpoint(ep,true)==Result::Success && (words[0]&(1u<<dci)) && (words[1]&(1u<<dci)));
        assert(input.freeze(19)==Result::Success && input.clear()==Result::Busy && input.slot(slot,protocol)==Result::Busy);
        assert(input.endpoint(ep)==Result::Busy && input.drop(5)==Result::Busy);
        assert(input.retire(18)==Result::Invalid && input.frozen() && input.retire(19)==Result::Success);
        auto badSlot=slot;badSlot.route=0x101;unchanged=words;
        assert(input.slot(badSlot,protocol)==Result::Invalid && words==unchanged);
        badSlot=slot;badSlot.ttHub=1;assert(input.slot(badSlot,protocol)==Result::Invalid);
        badSlot=slot;badSlot.speed=Speed::Full;badSlot.speedID=1;badSlot.ttHub=7;badSlot.ttPort=3;badSlot.multiTT=true;
        assert(input.slot(badSlot,protocol)==Result::Success && words[at+2]==(7|3u<<8));
        assert(input.clear()==Result::Success);
        XHCI::PortProtocol ss{};ss.major=3;ss.speeds[4]=Speed::Super;
        slot={};slot.rootPort=1;slot.speed=Speed::Super;slot.speedID=4;slot.hub=true;slot.ports=4;
        assert(input.slot(slot,ss)==Result::Success && words[at]&(1u<<26));
        ep={};ep.type=XHCI::EndpointType::Isochronous;ep.speed=Speed::Super;ep.address=0x81;
        ep.dequeue=0x200000;ep.maxPacket=1024;ep.interval=1;ep.companion=true;
        ep.burst=15;ep.attributes=2;ep.bytesPerInterval=49152;
        assert(input.endpoint(ep)==Result::Success && words[4*at]==(2u<<8));
        assert(words[4*at+1]==(1024u<<16|15u<<8|5u<<3));
        assert(words[4*at+4]==(49152u<<16|49152));
        ep.largeESIT=true;assert(input.endpoint(ep)==Result::Success && !words[4*at]);
        ep.bytesPerInterval=49153;unchanged=words;
        assert(input.endpoint(ep)==Result::Invalid && words==unchanged);
        ep={};ep.type=XHCI::EndpointType::Bulk;ep.speed=Speed::Super;ep.address=0x81;
        ep.dequeue=0x200000;ep.maxPacket=1024;ep.companion=true;ep.burst=15;ep.attributes=16;
        assert(input.endpoint(ep)==Result::Success && !(words[4*at]&(31u<<10)));
        ep.maxPacket|=1u<<11;assert(input.endpoint(ep)==Result::Invalid);
        // Read-only output context uses the selected context stride, never
        // reads endpoint state as proof that DMA can be freed.
        XHCI::DeviceContext output;Order order;
        assert(output.bind(words.data(),32*stride,stride)==Result::Success);
        words[0]=3u<<20|5u<<27;words[1]=2u<<16;words[3]=2u<<27|42;
        XHCI::SlotSnapshot state{};assert(output.slot(order,state)==Result::Success);
        assert(state.address==42 && state.state==2 && state.rootPort==2 && state.entries==5 && state.speedID==3);
        words[at]=3;words[at+2]=0x3001;words[at+3]=9;
        unsigned epState=0;uint64_t dequeue=0;
        assert(output.endpoint(order,1,epState,dequeue)==Result::Success && epState==3 && dequeue==0x900003001);
        assert(order.reads==2);
    }
}
struct Commands {
    alignas(64) std::array<XHCI::TRB,16> memory{};
    Order order;
    XHCI::ProducerRing<Order> commandRing{order};
    std::map<uintptr_t,unsigned> refs;
    Result bell=Result::Success;
    bool admission=true,reset=false,resources=true;
    std::function<void()> onDoorbell;
    Commands() { assert(commandRing.initialize(memory.data(),0x10000,16,36)); }
    XHCI::ProducerRing<Order> &ring() { return commandRing; }
    bool admitted(uint64_t epoch) { return admission && epoch==7; }
    Result doorbell() { if (onDoorbell) onDoorbell();return bell; }
    void retain(uintptr_t owner) { ++refs[owner]; }
    void release(uintptr_t owner) { assert(refs[owner]);if (!--refs[owner]) refs.erase(owner); }
    Result isolation(uint64_t epoch) { assert(epoch==7);return !resources ? Result::NotReady : reset ? Result::Success : Result::Busy; }
};
using Queue=XHCI::CommandQueue<Commands,8>;
XHCI::TRB event(const XHCI::CommandRecord &r,unsigned code,unsigned slot) {
    return XHCI::trb(r.address,code<<24,XHCI::CommandEvent<<10|slot<<24|1);
}
void release(Queue &q,Commands &b,uint64_t ticket,uintptr_t owner) {
    uintptr_t ref=0;assert(q.acknowledge(ticket,owner,ref)==Result::Success && ref==owner);b.release(ref);
}
void commands() {
    Commands b;Queue q(b);assert(q.initialize(7,32,64,36)==Result::Success);
    uint64_t ticket=0;XHCI::Command enable;enable.type=XHCI::EnableSlot;enable.endpoint=3;
    assert(q.submit(enable,10,1,100,ticket)==Result::Success && ticket==1 && b.refs[10]==1);
    assert((b.memory[0].control&(31u<<16))==(3u<<16));
    XHCI::CommandRecord record{},done{};assert(q.copy(ticket,record));
    assert(q.complete(event(record,1,7),6,done)==Result::Aborted && q.outstanding()==1);
    assert(q.complete(event(record,1,7),7,done)==Result::Success && done.slot==7 && done.result==Result::Success);
    uintptr_t ref=0;assert(q.acknowledge(ticket,11,ref)==Result::Invalid && !ref);release(q,b,ticket,10);
    assert(q.acknowledge(ticket,10,ref)==Result::Invalid);
    // Repeated hardware completions cross the actual command segment Link.
    for (unsigned i=0;i<5000;++i) {
        XHCI::Command noop;assert(q.submit(noop,11,i+2,100,ticket)==Result::Success);
        assert(q.copy(ticket,record) && q.complete(event(record,1,0),7,done)==Result::Success);
        release(q,b,ticket,11);
    }
    assert(!q.outstanding() && !q.retained() && b.refs.empty());
    std::array<uint64_t,8> held{};
    for (auto &t:held) {
        XHCI::Command noop;assert(q.submit(noop,12,6000,100,t)==Result::Success);
        assert(q.copy(t,record) && q.complete(event(record,1,0),7,done)==Result::Success);
    }
    XHCI::Command noop;assert(q.submit(noop,12,6000,100,ticket)==Result::Busy && !ticket && q.retained()==8);
    for (auto t:held) release(q,b,t,12);
    // Metadata and its strong owner are installed before ringing doorbell.
    b.onDoorbell=[&] { assert(q.copy(ticket,record) && b.refs[13]);assert(q.complete(event(record,1,0),7,done)==Result::Success); };
    assert(q.submit(noop,13,7000,100,ticket)==Result::Success);b.onDoorbell={};release(q,b,ticket,13);
    assert(b.refs.empty());
}
void commandFailures() {
    for (unsigned kind=0;kind<7;++kind) {
        Commands b;Queue q(b);assert(q.initialize(7,32,32,36)==Result::Success);
        XHCI::Command command;uint64_t first=0,second=0;
        if (kind==0) b.bell=Result::Timeout;
        const auto submitted=q.submit(command,20,10,100,first);
        assert(first && submitted==(kind==0 ? Result::Timeout : Result::Success));
        XHCI::CommandRecord record{},done{};assert(q.copy(first,record));
        if (kind==1) {
            assert(q.submit(command,21,11,1,second)==Result::Success);
            assert(q.expire(12)==Result::Timeout);
        } else if (kind==2) {
            auto bad=event(record,1,0);bad.lo+=16;assert(q.complete(bad,7,done)==Result::HardwareError);
        } else if (kind==3) assert(q.complete(event(record,24,0),7,done)==Result::HardwareError);
        else if (kind==4) assert(q.complete(event(record,1,1),7,done)==Result::HardwareError);
        else if (kind==5) { b.admission=false;assert(q.expire(11)==Result::Aborted); }
        else if (kind==6) assert(q.complete(event(record,13,0),7,done)==Result::HardwareError);
        assert(q.faulted());uintptr_t owner=0;
        assert(q.acknowledge(first,20,owner)==Result::Busy && !owner);
        assert(q.complete(event(record,1,0),7,done)==Result::HardwareError);
        assert(q.isolate()==Result::Busy && b.refs[20]);b.reset=true;b.resources=false;
        assert(q.isolate()==Result::NotReady && b.refs[20]);b.resources=true;
        assert(q.isolate()==Result::Success && !b.ring().pending());
        release(q,b,first,20);if (second) release(q,b,second,21);
        assert(b.refs.empty() && q.initialize(8,32,32,36)==Result::Busy);
        assert(q.submit(command,20,1000,100,first)==Result::NotReady);
    }
    Commands b;Queue q(b);assert(q.initialize(7,32,64,36)==Result::Success);
    XHCI::Command enable;enable.type=XHCI::EnableSlot;
    uint64_t ticket=0;assert(q.submit(enable,30,1,100,ticket)==Result::Success);
    XHCI::CommandRecord record{},done{};assert(q.copy(ticket,record));
    assert(q.complete(event(record,9,0),7,done)==Result::Success && done.result==Result::Busy);
    release(q,b,ticket,30);
    XHCI::TRB encoded{};XHCI::Command command;command.type=XHCI::StopEndpoint;command.slot=7;command.endpoint=3;command.option=true;
    assert(command.encode(32,64,36,encoded)==Result::Success && (encoded.control&(1u<<23)));
    command.type=XHCI::SetDequeue;command.option=false;command.parameter=0x200001;
    assert(command.encode(32,64,36,encoded)==Result::Success);
    command.parameter|=2;assert(command.encode(32,64,36,encoded)==Result::Invalid);
    command={};command.type=XHCI::AddressDevice;command.slot=1;command.parameter=(1ULL<<36)-64;
    assert(command.encode(32,64,36,encoded)==Result::Invalid);
    command.parameter=0x200000;command.option=true;
    assert(command.encode(32,64,36,encoded)==Result::Success && (encoded.control&(1u<<9)));
    command.type=XHCI::EvaluateContext;assert(command.encode(32,64,36,encoded)==Result::Invalid);
}
void contextCommandLifecycle() {
    for (bool failure:{false,true}) {
        alignas(64) std::array<uint32_t,33*64/4> words{};XHCI::InputContext input;
        assert(input.bind(words.data(),sizeof(words),0x200000,64,36,32,2)==Result::Success);
        Commands b;Queue q(b);assert(q.initialize(7,32,64,36)==Result::Success);
        uint64_t ticket=0;uintptr_t released=0;
        assert(input.submit(q,XHCI::AddressDevice,7,false,50,1,100,ticket)==Result::Invalid && !ticket);
        XHCI::PortProtocol protocol{};protocol.major=2;protocol.speeds[3]=Speed::High;
        XHCI::SlotConfiguration slot;slot.rootPort=1;slot.speedID=3;slot.speed=Speed::High;
        XHCI::EndpointConfiguration ep;ep.speed=Speed::High;ep.maxPacket=64;ep.dequeue=0x300000;
        assert(input.slot(slot,protocol)==Result::Success && input.endpoint(ep)==Result::Success);
        if (failure) b.bell=Result::Timeout;
        assert(input.submit(q,XHCI::AddressDevice,7,false,50,1,100,ticket)==(failure ? Result::Timeout : Result::Success));
        assert(ticket && input.frozen() && input.clear()==Result::Busy && b.refs[50]);
        assert(input.finish(q,50,released)==Result::Busy && !released);
        if (failure) {
            assert(q.isolate()==Result::Busy && input.frozen());b.reset=true;assert(q.isolate()==Result::Success);
        } else {
            XHCI::CommandRecord record{},completed{};assert(q.copy(ticket,record));
            assert(q.complete(event(record,1,7),7,completed)==Result::Success);
        }
        assert(input.finish(q,51,released)==Result::Invalid && input.frozen());
        assert(input.finish(q,50,released)==Result::Success && released==50 && !input.frozen());
        b.release(released);assert(b.refs.empty() && input.clear()==Result::Success);
    }
}
}
static void familyLocations() {
    XHCI::FamilyLocation location;
    assert(XHCI::decodeFamilyLocation(0xa5345678,0xa5000000,4,location)==Result::Success);
    assert(location.bus==0xa5 && location.rootPort==3 && location.route==0x87654 && location.depth==5);
    assert(XHCI::decodeFamilyLocation(0x1f100000,0x1f000000,2,location)==Result::Success);
    assert(location.rootPort==1 && !location.route && !location.depth);
    assert(XHCI::decodeFamilyLocation(0x00212000,0,2,location)==Result::Success);
    assert(!location.bus && location.rootPort==2 && location.route==0x21 && location.depth==2);
    for (uint32_t bad:{0x1f000000u,0x1f300000u,0x1e100000u,0x1f101000u,0x1f120003u}) {
        location={7,6,5,4};
        assert(XHCI::decodeFamilyLocation(bad,0x1f000000,2,location)==Result::Invalid);
        assert(location.bus==7 && location.rootPort==6 && location.depth==5 && location.route==4);
    }
    assert(XHCI::decodeFamilyLocation(0x1f100000,0x1f100000,2,location)==Result::Invalid);
    assert(XHCI::decodeFamilyLocation(0x1f100000,0x1f000000,0,location)==Result::Invalid);
}
void xhciHostContextsAndCommands() { contexts();commands();commandFailures();contextCommandLifecycle();familyLocations(); }
