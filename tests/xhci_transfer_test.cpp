// SPDX-License-Identifier: BSD-2-Clause
#include "XHCITransfers.h"
#include <array>
#include <cassert>
#include <functional>
#include <map>
#include <random>
#include <vector>

namespace {
using USB::Result;using USB::Speed;
using XHCI::TransferDescriptor;using XHCI::TransferSegment;
struct Order {
    std::function<void()> barrier;
    void dmaWriteBarrier() { if (barrier) barrier(); }
    void dmaReadBarrier() {}
};
struct Commands {
    Order io;alignas(64) XHCI::TRB memory[64]{};XHCI::ProducerRing<Order> producer{io};
    std::map<uintptr_t,unsigned> refs;
    Commands() { assert(producer.initialize(memory,0x100000,64,36)); }
    auto &ring() { return producer; }
    bool admitted(uint64_t generation) { return generation==7; }
    void retain(uintptr_t owner) { ++refs[owner]; }
    Result doorbell() { return Result::Success; }
    Result isolation(uint64_t) { return Result::Busy; }
};
struct Transfers {
    Order io;alignas(64) XHCI::TRB memory[64]{};XHCI::ProducerRing<Order> producer{io};
    std::map<uintptr_t,unsigned> refs;
    std::function<void()> bell;bool admittedNow=true,reset=false;Result bellResult=Result::Success;
    Transfers() { assert(producer.initialize(memory,0x200000,64,36)); }
    auto &ring() { return producer; }
    bool admitted(uint64_t generation) { return admittedNow && generation==7; }
    void retain(uintptr_t owner) { ++refs[owner]; }
    Result doorbell(unsigned slot,unsigned id) { assert(slot==3 && id>=1 && id<=31);if (bell) bell();return bellResult; }
    Result isolation(uint64_t generation) { return reset && generation==7 ? Result::Success : Result::Busy; }
    void release(uintptr_t owner) { assert(refs.count(owner));if (!--refs[owner]) refs.erase(owner); }
};
using CommandQueue=XHCI::CommandQueue<Commands>;
using Queue=XHCI::TransferQueue<Transfers,CommandQueue,16>;
XHCI::EndpointConfiguration endpoint(XHCI::EndpointType type=XHCI::EndpointType::Bulk,bool input=true) {
    XHCI::EndpointConfiguration ep;ep.dequeue=0x200000;ep.address=input ? 0x81 : 1;
    ep.type=type;ep.speed=Speed::High;ep.maxPacket=512;
    if (type==XHCI::EndpointType::Control) { ep.address=0;ep.maxPacket=64; }
    if (type==XHCI::EndpointType::Interrupt || type==XHCI::EndpointType::Isochronous) { ep.interval=1;ep.maxPacket=1024; }
    return ep;
}
struct Machine {
    Commands c;CommandQueue commands{c};Transfers t;Queue queue{t,commands};XHCI::EndpointConfiguration ep;
    explicit Machine(XHCI::EndpointType type=XHCI::EndpointType::Bulk,bool input=true):Machine(endpoint(type,input)) {}
    explicit Machine(const XHCI::EndpointConfiguration &configuration):ep(configuration) {
        assert(commands.initialize(7,32,32,36)==Result::Success);
        assert(queue.initialize(7,3,ep,36,0xee)==Result::Success);
    }
    XHCI::TRB event(uint64_t ticket,unsigned code,uint32_t bytes,bool final=true) {
        return XHCI::trb(ticket<<1|uint64_t(final),code<<24|bytes,
            XHCI::TransferEvent<<10|XHCI::EventDataFlag|3u<<24|XHCI::endpointID(ep.address,ep.type)<<16);
    }
    uint64_t issue(XHCI::Type type,uint64_t parameter=0,unsigned endpointOverride=0,uintptr_t owner=0xee,unsigned code=1) {
        XHCI::Command command;command.type=type;command.slot=3;command.endpoint=endpointOverride ? endpointOverride : XHCI::endpointID(ep.address,ep.type);command.parameter=parameter;
        uint64_t ticket=0;assert(commands.submit(command,owner,1,1000,ticket)==Result::Success);
        XHCI::CommandRecord record{},done{};assert(commands.copy(ticket,record));
        auto completed=XHCI::trb(record.address,code<<24,XHCI::CommandEvent<<10|3u<<24);
        assert(commands.complete(completed,7,done)==Result::Success);return ticket;
    }
    void release(uint64_t ticket,uintptr_t owner) {
        uintptr_t released=0;assert(queue.acknowledge(ticket,owner,released)==Result::Success && released==owner);t.release(released);
    }
};
void encodings() {
    TransferDescriptor<> descriptor;auto ep=endpoint();
    TransferSegment sg[]={{0x30fff0,16},{0x410000,65536},{0x550000,64}};
    assert(descriptor.build(ep,sg,3,65616,36)==Result::Success);
    assert(descriptor.count()==4 && descriptor.bytes()==65616 && descriptor.in() && !descriptor.staged());
    const auto *p=descriptor.data();
    assert(p[0].type()==XHCI::Normal && p[0].parameter()==0x30fff0 && (p[0].status&0x1ffff)==16 && ((p[0].status>>17)&31)==31);
    assert(p[1].status==(65536u|1u<<17) && p[2].status==64);
    for (unsigned i=0;i<3;++i) assert((p[i].control&XHCI::Chain) && !(p[i].control&(XHCI::IOC|XHCI::InterruptShort)));
    assert(p[2].control&XHCI::EvaluateNext);assert(p[3].type()==XHCI::EventData && p[3].control==((XHCI::EventData<<10)|XHCI::IOC));
    auto original=descriptor;
    for (unsigned bad=0;bad<10;++bad) {
        auto candidate=ep;TransferSegment s{0x800000,512};unsigned count=1,len=512,bits=36;
        switch (bad) {
        case 0:s.length=0;break;case 1:s.length=511;break;case 2:s.address=1ull<<36;break;
        case 3:bits=31;break;case 4:candidate.maxPacket=64;break;case 5:candidate.dequeue|=1;break;
        case 6:len=0;break;case 7:count=0;break;case 8:len=0x1000000;break;case 9:s.address=UINT64_MAX-10;break;
        }
        assert(descriptor.build(candidate,&s,count,len,bits)==Result::Invalid);
        assert(descriptor.count()==original.count() && descriptor.bytes()==original.bytes());
        for (unsigned i=0;i<descriptor.count();++i) assert(descriptor.data()[i].control==original.data()[i].control && descriptor.data()[i].parameter()==original.data()[i].parameter());
    }
    TransferDescriptor<5> small;TransferSegment longBuffer{0x800000,4*65536};
    assert(small.build(ep,&longBuffer,1,longBuffer.length,36)==Result::Success);
    longBuffer.length+=1;assert(small.build(ep,&longBuffer,1,longBuffer.length,36)==Result::Busy && small.count()==5);
    assert(descriptor.build(ep,nullptr,0,0,36)==Result::Success && descriptor.count()==2 && !descriptor.data()[0].parameter());
    ep=endpoint(XHCI::EndpointType::Bulk,false);TransferSegment data{0x800000,1024};
    assert(descriptor.build(ep,&data,1,1024,36,nullptr,{},true)==Result::Success && descriptor.count()==4 && descriptor.staged());
    assert(descriptor.data()[1].type()==XHCI::EventData && !(descriptor.data()[1].control&XHCI::Chain));
    assert(descriptor.data()[2].type()==XHCI::Normal && !descriptor.data()[2].status);
    assert(descriptor.build(ep,&data,1,1023,36,nullptr,{},true)==Result::Invalid);
    ep.address|=128;assert(descriptor.build(ep,&data,1,1024,36,nullptr,{},true)==Result::Invalid);
    for (bool input:{false,true}) {
        ep=endpoint(XHCI::EndpointType::Control);USB::SetupPacket setup{uint8_t(input ? 0x80 : 0),6,0x1234,0x5678,1024};
        assert(descriptor.build(ep,&data,1,1024,36,&setup)==Result::Success && descriptor.count()==5 && descriptor.staged());
        p=descriptor.data();assert(p[0].parameter()==(0x0400567812340600ull|(input ? 0x80 : 0)));
        assert(p[0].status==8 && !(p[0].control&XHCI::Chain) && (p[0].control&XHCI::Immediate));
        assert(((p[0].control>>16)&3)==(input ? 3u : 2u));
        assert(p[1].type()==XHCI::Data && bool(p[1].control&(1u<<16))==input);
        assert(p[2].type()==XHCI::EventData && p[3].type()==XHCI::Status && bool(p[3].control&(1u<<16))!=input);
        assert(p[3].control&XHCI::Chain);
        setup.length=0;
        assert(descriptor.build(ep,nullptr,0,0,36,&setup)==Result::Success && descriptor.count()==3 && !descriptor.staged());
        assert(descriptor.data()[1].control&(1u<<16));
    }
    for (Speed speed:{Speed::Full,Speed::High,Speed::Super}) {
        ep=endpoint(XHCI::EndpointType::Isochronous);ep.speed=speed;
        ep.maxPacket=speed==Speed::Full ? 1023 : speed==Speed::High ? 1024|2u<<11 : 1024;
        if (speed==Speed::Super) { ep.burst=15;ep.attributes=2;ep.companion=true;ep.bytesPerInterval=49152; }
        data.length=speed==Speed::Full ? 1023 : speed==Speed::High ? 3072 : 49152;
        XHCI::IsoSchedule schedule;schedule.asap=false;schedule.frame=2047;
        assert(descriptor.build(ep,&data,1,data.length,36,nullptr,schedule)==Result::Success);
        const auto first=descriptor.data()[0];assert(first.type()==XHCI::Iso && ((first.control>>20)&2047)==2047 && !(first.control>>31));
        assert(((first.control>>16)&15)==(speed==Speed::Full ? 0u : speed==Speed::High ? 2u : 15u));
        assert(((first.control>>7)&3)==(speed==Speed::Super ? 2u : 0u));
        if (speed==Speed::Super) {
            ep.largeESIT=true;schedule.extendedTBC=true;
            assert(descriptor.build(ep,&data,1,data.length,36,nullptr,schedule)==Result::Success);
            assert(((descriptor.data()[0].status>>17)&31)==2 && !(descriptor.data()[0].control&(3u<<7)));
        }
        schedule.frame=2048;
        assert(descriptor.build(ep,&data,1,data.length,36,nullptr,schedule)==Result::Invalid);
        schedule={};assert(descriptor.build(ep,&data,1,data.length,36,nullptr,schedule)==Result::Success && descriptor.data()[0].control>>31);
        ++data.length;assert(descriptor.build(ep,&data,1,data.length,36)==Result::Invalid);
    }
}
void nativeClearFence() {
    for (auto type:{XHCI::EndpointType::Control,XHCI::EndpointType::Bulk,XHCI::EndpointType::Interrupt})
        for (bool physical:{false,true}) for (unsigned code:{2u,3u,4u,6u,36u}) {
        Machine m(type);TransferDescriptor<> td;TransferSegment data{0x300000,64};
        USB::SetupPacket setup{0x80,6,0x100,0,64};
        assert(td.build(m.ep,&data,1,64,36,type==XHCI::EndpointType::Control ? &setup : nullptr)==Result::Success);
        uint64_t ticket=0;assert(m.queue.submit(td,91,10,1000,ticket)==Result::Success);
        auto event=physical ? XHCI::trb(m.t.ring().startAddress(),code<<24,
            XHCI::TransferEvent<<10|3u<<24|XHCI::endpointID(m.ep.address,type)<<16) : m.event(ticket,code,0);
        XHCI::TransferRecord out;
        assert(m.queue.complete(event,7,out)==Result::HardwareError && m.queue.requiresNativeClear());
        assert(m.queue.finishNativeClear()==Result::NotReady && m.t.ring().pending()==td.count());
        assert(m.queue.beginNativeClear()==Result::Success);
        const auto stopped=m.issue(XHCI::ResetEndpoint);
        const auto dq=m.issue(XHCI::SetDequeue,m.t.ring().nextAddress()|m.t.ring().producerCycle());
        assert(m.queue.recover(stopped,dq)==Result::Success && !m.queue.faulted());
        assert(m.queue.finishNativeClear()==Result::NotReady); // Native request still owns its retained record.
        m.release(ticket,91);
        auto next=m.ep;next.dequeue=m.t.ring().nextAddress();next.cycle=bool(m.t.ring().producerCycle());
        assert(m.queue.reconfigure(next)==Result::Success && m.queue.requiresNativeClear());
        uint64_t rejected=99;assert(m.queue.submit(td,92,11,1000,rejected)==Result::Busy && !rejected);
        m.t.admittedNow=false;
        assert(m.queue.finishNativeClear()==Result::NotReady && m.queue.beginNativeClear()==Result::NotReady);
        m.t.admittedNow=true;
        assert(m.queue.finishNativeClear()==Result::Success && !m.queue.requiresNativeClear());
        assert(td.build(next,&data,1,64,36,type==XHCI::EndpointType::Control ? &setup : nullptr)==Result::Success);
        assert(m.queue.submit(td,92,12,1000,ticket)==Result::Success);
        if (type==XHCI::EndpointType::Control) assert(m.queue.complete(m.event(ticket,1,64,false),7,out)==Result::Pending);
        assert(m.queue.complete(m.event(ticket,1,type==XHCI::EndpointType::Control ? 0 : 64),7,out)==Result::Success);
        m.release(ticket,92);assert(m.t.refs.empty());
    }
    {
        Machine m;TransferDescriptor<> td;TransferSegment data{0x300000,64};
        assert(td.build(m.ep,&data,1,64,36)==Result::Success);
        uint64_t ticket=0;assert(m.queue.submit(td,91,10,1000,ticket)==Result::Success);
        assert(m.queue.beginNativeClear()==Result::Success && m.queue.finishNativeClear()==Result::NotReady);
        uint64_t rejected=0;assert(m.queue.submit(td,92,11,1000,rejected)==Result::Busy);
        assert(m.queue.quiesce()==Result::Aborted);
        const auto stop=m.issue(XHCI::StopEndpoint);
        const auto dq=m.issue(XHCI::SetDequeue,m.t.ring().nextAddress()|m.t.ring().producerCycle());
        assert(m.queue.recover(stop,dq)==Result::Success);
        m.release(ticket,91);assert(m.queue.finishNativeClear()==Result::Success);
    }
    {
        Machine m;assert(m.queue.beginNativeClear()==Result::Success);
        m.t.reset=true;assert(m.queue.isolate()==Result::Success && !m.queue.requiresNativeClear());
        assert(m.queue.finishNativeClear()==Result::NotReady);
    }
}
void initialization() {
    Commands c;CommandQueue commands(c);Transfers t;Queue queue(t,commands);auto ep=endpoint();
    assert(queue.initialize(7,3,ep,36,0xee)==Result::Invalid);
    assert(commands.initialize(7,32,32,36)==Result::Success);
    assert(queue.initialize(8,3,ep,36,0xee)==Result::Invalid);
    assert(queue.initialize(7,33,ep,36,0xee)==Result::Invalid);
    assert(queue.initialize(7,3,ep,40,0xee)==Result::Invalid);
    assert(queue.initialize(7,3,ep,36,0)==Result::Invalid);
    t.admittedNow=false;assert(queue.initialize(7,3,ep,36,0xee)==Result::NotReady);
    t.admittedNow=true;assert(queue.initialize(7,3,ep,36,0xee)==Result::Success);
    assert(queue.initialize(7,3,ep,36,0xee)==Result::Busy);
}
void completions() {
    for (bool control:{false,true}) for (bool input:{false,true}) {
        Machine m(control ? XHCI::EndpointType::Control : XHCI::EndpointType::Bulk,input);
        TransferDescriptor<> td;TransferSegment data{0x300000,512};USB::SetupPacket setup{uint8_t(input ? 0x80 : 0),6,0,0,512};
        assert(td.build(m.ep,&data,1,512,36,control ? &setup : nullptr)==Result::Success);
        uint64_t ticket=0;m.t.bell=[&] { XHCI::TransferRecord r{};assert(m.queue.copy(ticket,r) && m.t.refs[91] && m.t.ring().pending()==td.count()); };
        assert(m.queue.submit(td,91,10,1000,ticket)==Result::Success && ticket);
        XHCI::TransferRecord out{};const auto bytes=input ? 200u : 512u;const auto code=input ? 13u : 1u;
        assert(m.queue.complete(m.event(ticket,code,bytes,!control),8,out)==Result::Aborted && m.queue.outstanding()==1);
        assert(m.queue.complete(m.event(ticket,code,bytes,!control),7,out)==(control ? Result::Pending : Result::Success));
        if (control) {
            uintptr_t released=0;assert(m.queue.acknowledge(ticket,91,released)==Result::Busy && !released && m.t.ring().pending()==td.count());
            assert(m.queue.complete(m.event(ticket,1,0),7,out)==Result::Success);
        }
        assert(out.transferred==bytes && out.shortPacket==input && out.phase==XHCI::TransferPhase::Completed);
        assert(!m.queue.outstanding() && !m.t.ring().pending() && m.queue.retained()==1);
        assert(m.queue.complete(m.event(ticket,code,bytes),7,out)==Result::Aborted && !m.queue.faulted());
        m.release(ticket,91);assert(m.t.refs.empty() && !m.queue.retained());
    }
    {
        Machine m(XHCI::EndpointType::Bulk,false);TransferDescriptor<> td;TransferSegment s{0x300000,512};uint64_t ticket=0;
        assert(td.build(m.ep,&s,1,512,36,nullptr,{},true)==Result::Success);
        assert(m.queue.submit(td,1,0,0,ticket)==Result::Success);
        XHCI::TransferRecord out{};assert(m.queue.complete(m.event(ticket,1,512,false),7,out)==Result::Pending);
        assert(m.queue.complete(m.event(ticket,1,0),7,out)==Result::Success && out.transferred==512);m.release(ticket,1);
    }
}
void failures() {
    for (unsigned failure=0;failure<14;++failure) {
        Machine m;TransferDescriptor<> td;TransferSegment s{0x300000,1024};uint64_t ticket=0;
        assert(td.build(m.ep,&s,1,1024,36)==Result::Success);
        assert(m.queue.submit(td,8,10,100,ticket)==Result::Success);
        auto event=m.event(ticket,1,1024);XHCI::TransferRecord out{};
        switch (failure) {
        case 0:event.status=2u<<24;break;case 1:event.status=26u<<24;break;case 2:event.status=255u<<24;break;
        case 3:event.status=13u<<24|1025;break;case 4:event.status=1u<<24|1023;break;
        case 5:event.control|=1u<<22;break;case 6:event.control^=1u<<24;break;
        case 7:event.control^=1u<<16;break;case 8:event.lo=0;event.hi=0;break;
        case 9:event.lo=uint32_t((ticket+1)<<1|1);break;
        case 10:event.control&=~XHCI::EventDataFlag;event.lo=0x200000;event.hi=0;event.status=6u<<24;break;
        case 11:event.control&=~XHCI::EventDataFlag;event.lo=0x2003f0;event.hi=0;event.status=26u<<24;break;
        case 12:event.lo=uint32_t(ticket<<1);break;case 13:m.t.admittedNow=false;break;
        }
        assert(m.queue.complete(event,7,out)==(failure==13 ? Result::Aborted : Result::HardwareError));
        uintptr_t released=0;assert(m.queue.faulted() && m.queue.acknowledge(ticket,8,released)==Result::Busy && !released);
        assert(m.queue.isolate()==Result::Busy && m.t.ring().pending()==td.count() && m.t.refs[8]);
        m.t.reset=true;assert(m.queue.isolate()==Result::Success && !m.t.ring().pending());m.release(ticket,8);
        assert(m.queue.submit(td,8,11,100,ticket)==Result::NotReady);
    }
    for (bool control:{false,true}) {
        Machine m(control ? XHCI::EndpointType::Control : XHCI::EndpointType::Bulk);TransferDescriptor<> td;
        TransferSegment s{0x300000,512};USB::SetupPacket setup{0x80,6,0,0,512};uint64_t ticket=0;
        assert(td.build(m.ep,&s,1,512,36,control ? &setup : nullptr)==Result::Success);
        m.t.bellResult=Result::Timeout;assert(m.queue.submit(td,7,1,0,ticket)==Result::Timeout && ticket && m.t.refs[7]);
        m.t.reset=true;assert(m.queue.isolate()==Result::Success);m.release(ticket,7);
    }
    {
        Machine m;TransferDescriptor<> td;TransferSegment s{0x200010,512};uint64_t ticket=55;
        assert(td.build(m.ep,&s,1,512,36)==Result::Success);
        assert(m.queue.submit(td,1,1,0,ticket)==Result::Invalid && !ticket && m.t.refs.empty() && !m.t.ring().pending());
        s={0x300000,512};assert(td.build(m.ep,&s,1,512,36)==Result::Success);
        assert(m.queue.submit(td,1,10,100,ticket)==Result::Success);
        assert(m.queue.expire(9)==Result::Invalid && m.queue.expire(109)==Result::Success && m.queue.expire(110)==Result::Timeout);
    }
}
void recovery() {
    Machine m;TransferDescriptor<> td;TransferSegment s{0x300000,512};uint64_t ticket=0;
    assert(td.build(m.ep,&s,1,512,36)==Result::Success);
    auto beforeStop=m.issue(XHCI::StopEndpoint),beforeDequeue=m.issue(XHCI::SetDequeue,0x200001);
    assert(m.queue.submit(td,1,1,0,ticket)==Result::Success);
    assert(m.queue.quiesce()==Result::Aborted && m.queue.recoveryCommandFloor()==beforeDequeue);
    assert(m.queue.recover(beforeStop,beforeDequeue)==Result::Invalid && m.t.ring().pending());
    const auto next=m.t.ring().nextAddress()|m.t.ring().producerCycle();
    auto stop=m.issue(XHCI::StopEndpoint),wrong=m.issue(XHCI::SetDequeue,next+16);
    assert(m.queue.recover(stop,wrong)==Result::Invalid);
    auto deq=m.issue(XHCI::SetDequeue,next);
    assert(m.queue.recover(stop,deq)==Result::Success && !m.t.ring().pending() && !m.queue.faulted());m.release(ticket,1);
    assert(m.queue.submit(td,2,2,0,ticket)==Result::Success);
    assert(m.queue.quiesce()==Result::Aborted && m.queue.recover(stop,deq)==Result::Invalid);
    auto reset=m.issue(XHCI::ResetEndpoint);deq=m.issue(XHCI::SetDequeue,m.t.ring().nextAddress()|m.t.ring().producerCycle());
    assert(m.queue.recover(reset,deq)==Result::Success);m.release(ticket,2);
    for (unsigned invalid=0;invalid<3;++invalid) {
        Machine other;assert(other.queue.submit(td,3,1,0,ticket)==Result::Success);
        assert(other.queue.quiesce()==Result::Aborted);
        auto a=other.issue(XHCI::StopEndpoint,0,invalid==0 ? 4 : 0,invalid==1 ? 0xdd : 0xee,invalid==2 ? 19 : 1);
        auto b=other.issue(XHCI::SetDequeue,other.t.ring().nextAddress()|other.t.ring().producerCycle());
        assert(other.queue.recover(a,b)==Result::Invalid && other.t.ring().pending()==td.count());
    }
}
void pipelineAndBoundaries() {
    Machine m(XHCI::EndpointType::Isochronous);TransferDescriptor<> td;TransferSegment s{0x300000,1024};
    uint64_t tickets[16]{};
    for (unsigned i=0;i<16;++i) {
        XHCI::IsoSchedule schedule;schedule.asap=false;schedule.frame=i;
        assert(td.build(m.ep,&s,1,1024,36,nullptr,schedule)==Result::Success);
        assert(m.queue.submit(td,100+i,i,0,tickets[i])==Result::Success);
    }
    uint64_t ticket=99;assert(m.queue.submit(td,99,16,0,ticket)==Result::Busy && !ticket);
    assert(m.queue.outstanding()==16 && m.queue.retained()==16 && m.t.ring().pending()==32);
    XHCI::TransferRecord out{};
    for (unsigned i=0;i<16;++i) {
        assert(m.queue.complete(m.event(tickets[i],1,1024),7,out)==Result::Success && out.owner==100+i);
        if (i<15) assert(m.queue.retained()==16 && m.t.ring().pending()==2*(15-i));
    }
    assert(m.queue.submit(td,99,16,0,ticket)==Result::Busy && !ticket); // completions still retained
    for (unsigned i=0;i<16;++i) m.release(tickets[i],100+i);
    auto empty=XHCI::trb(0,14u<<24,XHCI::TransferEvent<<10|3u<<24|3u<<16);
    assert(m.queue.complete(empty,7,out)==Result::Pending && !m.queue.faulted());
    assert(m.queue.submit(td,99,16,0,ticket)==Result::Success);
    auto unknown=XHCI::trb(0x1230,26u<<24,XHCI::TransferEvent<<10|3u<<24|3u<<16);
    assert(m.queue.complete(unknown,7,out)==Result::HardwareError && m.t.ring().pending()==2);
    {
        Machine ring;TransferDescriptor<> tiny;assert(tiny.build(ring.ep,nullptr,0,0,36)==Result::Success);
        uint64_t previous=0;
        for (unsigned i=0;i<31;++i) {
            assert(ring.queue.submit(tiny,1,i,0,previous)==Result::Success);
            assert(ring.queue.complete(ring.event(previous,1,0),7,out)==Result::Success);ring.release(previous,1);
        }
        assert(ring.t.ring().nextAddress()==0x2003e0);
        TransferSegment sg[]={{0x30fff0,16},{0x410000,512}};
        assert(td.build(ring.ep,sg,2,528,36)==Result::Success);
        auto before=ring.t.memory[62];ticket=55;
        assert(ring.queue.submit(td,2,32,0,ticket)==Result::Unsupported && !ticket && ring.t.refs.empty());
        assert(before.control==ring.t.memory[62].control && before.parameter()==ring.t.memory[62].parameter());
        sg[0]={0x300000,512};
        assert(td.build(ring.ep,sg,2,1024,36)==Result::Success);
        assert(ring.queue.submit(td,2,32,0,ticket)==Result::Success && ring.t.memory[63].control&XHCI::Chain);
        assert(ring.queue.complete(ring.event(previous,1,0),7,out)==Result::Aborted && !ring.queue.faulted());
        assert(ring.queue.complete(ring.event(ticket,13,513),7,out)==Result::Success && out.transferred==513);
        ring.release(ticket,2);
    }
}
void endpointRecreation() {
    Machine m;TransferDescriptor<> td;TransferSegment s{0x300000,512};uint64_t old=0,current=0;
    assert(td.build(m.ep,&s,1,512,36)==Result::Success);
    assert(m.queue.submit(td,1,1,0,old)==Result::Success);
    XHCI::TransferRecord out{};assert(m.queue.complete(m.event(old,1,512),7,out)==Result::Success);m.release(old,1);
    assert(m.queue.quiesce()==Result::Aborted);
    Queue replacement(m.t,m.commands);
    assert(replacement.initialize(7,3,m.ep,36,0xef)==Result::Invalid);
    m.ep.dequeue=m.t.ring().nextAddress();m.ep.cycle=bool(m.t.ring().producerCycle());
    assert(replacement.initialize(7,3,m.ep,36,0xef)==Result::Success);
    assert(td.build(m.ep,&s,1,512,36)==Result::Success);
    assert(replacement.submit(td,2,2,0,current)==Result::Success && current>old);
    assert(replacement.complete(m.event(old,1,512),7,out)==Result::Aborted && replacement.outstanding()==1);
    assert(replacement.complete(m.event(current,1,512),7,out)==Result::Success && out.owner==2);
    uintptr_t released=0;assert(replacement.acknowledge(current,2,released)==Result::Success);m.t.release(released);
}
void publicationAndWrap() {
    {
        Machine m;TransferDescriptor<> td;TransferSegment s{0x300000,512};uint64_t ticket=0;
        assert(td.build(m.ep,&s,1,512,36)==Result::Success);bool published=false;
        m.t.io.barrier=[&] {
            if (m.t.memory[0].control&XHCI::Cycle) {
                XHCI::TransferRecord r{};assert(m.queue.copy(ticket,r) && r.phase==XHCI::TransferPhase::Pending && m.t.refs[9]);
                assert(m.queue.outstanding()==1 && m.t.ring().pending()==td.count());published=true;
            }
        };
        assert(m.queue.submit(td,9,1,0,ticket)==Result::Success && published);
    }
    {
        Commands b;CommandQueue q(b);assert(q.initialize(7,32,32,36)==Result::Success);uint64_t ticket=0;bool published=false;
        b.io.barrier=[&] { if (b.memory[0].control&XHCI::Cycle) {
            XHCI::CommandRecord r{};assert(q.copy(ticket,r) && b.refs[9] && q.outstanding()==1 && b.ring().pending()==1);published=true;
        }};
        assert(q.submit({},9,1,100,ticket)==Result::Success && published);
    }
    Machine m;std::mt19937 random(0x835515);unsigned hardwareAt=0,cycle=1;uint64_t old=0;
    for (unsigned n=0;n<6000;++n) {
        TransferDescriptor<> td;TransferSegment s{0x100000000ull+(random()%256)*65536,uint32_t(1+random()%65536)};
        assert(td.build(m.ep,&s,1,s.length,36)==Result::Success);uint64_t ticket=0;
        assert(m.queue.submit(td,101,n,0,ticket)==Result::Success && ticket>old);old=ticket;
        uint64_t cookie=0;unsigned consumed=0;uint32_t total=0;
        while (consumed<td.count()) {
            const auto trb=m.t.memory[hardwareAt];assert((trb.control&XHCI::Cycle)==cycle);
            if (trb.type()==XHCI::Link) { assert(trb.parameter()==0x200000 && trb.control&XHCI::ToggleCycle);hardwareAt=0;cycle^=1;continue; }
            if (trb.type()==XHCI::EventData) { cookie=trb.parameter();assert(trb.control&XHCI::IOC); }
            else { assert(trb.type()==XHCI::Normal && !(trb.control&(XHCI::IOC|XHCI::InterruptShort)));total+=trb.status&0x1ffff; }
            ++hardwareAt;++consumed;
        }
        assert(total==s.length && cookie==(ticket<<1|1));
        XHCI::TransferRecord out{};const bool shortened=random()%3==0;const uint32_t bytes=shortened ? random()%(s.length+1) : s.length;
        assert(m.queue.complete(m.event(ticket,shortened ? 13 : 1,bytes),7,out)==Result::Success && out.transferred==bytes);
        m.release(ticket,101);assert(!m.queue.retained() && m.t.refs.empty());
    }
}
}
void xhciHostTransfers() { encodings();initialization();completions();failures();recovery();nativeClearFence();pipelineAndBoundaries();endpointRecreation();publicationAndWrap(); }
