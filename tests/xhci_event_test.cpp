// SPDX-License-Identifier: BSD-2-Clause
#include "XHCICore.h"
#include "XHCIEvents.h"
#include <array>
#include <cassert>
#include <vector>

namespace {
using USB::Result;
struct EventIO {
    std::array<uint32_t,0xc100/4> registers{};
    std::vector<unsigned> writes;
    bool injectAfterEHB=false;
    unsigned pendingEvents=0;
    EventIO() {
        registers[0]=0x1100040;registers[1]=32|(1u<<8)|(2u<<24);
        registers[4]=5;registers[5]=0x1000;registers[6]=0x2000;
        registers[0x44/4]=1;registers[0x48/4]=1;
    }
    uint32_t read(unsigned reg) { assert(reg<0xc100);return registers[reg/4]; }
    void signal() { registers[0x44/4]|=8;registers[0x2020/4]|=1; }
    void write(unsigned reg,uint32_t value) {
        writes.push_back(reg);
        if (reg==0x44) registers[reg/4]&=~value;
        else if (reg==0x2020) {
            auto &iman=registers[reg/4];
            iman=(iman&1u&~(value&1u))|(value&2u);
        } else {
            registers[reg/4]=value;
            if (reg==0x40) {
                if (value&1) registers[0x44/4]&=~1u;
                else registers[0x44/4]|=1;
            }
            if (reg==0x2038 && (value&8)) {
                if (pendingEvents || injectAfterEHB) signal();
                injectAfterEHB=false;
            }
        }
    }
    void delay(unsigned) {}
    void dmaReadBarrier() {}
    void dmaWriteBarrier() {}
};
struct EventMachine {
    static constexpr uint64_t Epoch=19,CommandBase=0x100000,EventBase=0x130000;
    EventIO io;
    XHCI::Core<EventIO> core{io};
    alignas(64) std::array<XHCI::TRB,256> commandMemory{},eventMemory{};
    alignas(64) XHCI::SegmentTable table{};
    XHCI::ProducerRing<EventIO> ring{io};
    XHCI::EventRing<EventIO> events{io};
    XHCI::FrameEvents frameEvents;
    bool admission=true,resetProof=false;
    unsigned retains=0,commandCalls=0,transferCalls=0,portCalls=0,produced=0,rollovers=0;
    uint32_t rawMicroFrame=0;
    Result transferResult=Result::Success;
    uint64_t ports=0;
    struct Backend {
        EventMachine &m;
        auto &ring() { return m.ring; }
        bool admitted(uint64_t epoch) { return m.admission && epoch==Epoch; }
        Result doorbell() { return m.core.doorbell(0,0); }
        void retain(uintptr_t) { ++m.retains; }
        Result isolation(uint64_t epoch) { return epoch==Epoch && m.resetProof ? Result::Success : Result::NotReady; }
    } backend{*this};
    XHCI::CommandQueue<Backend> commands{backend};
    EventMachine() {
        assert(core.probe(0xc100,40)==Result::Success && core.beginReset(0)==Result::Success);
        io.registers[0x40/4]&=~2u;assert(core.advanceReset(1)==Result::Success);
        assert(ring.initialize(commandMemory.data(),CommandBase,256,40));
        assert(events.initialize(eventMemory.data(),EventBase,256,40));
        assert(frameEvents.initialize(eventMemory.data(),256,Epoch));
        const XHCI::MemoryLayout layout{CommandBase,0x110000,0x120000,EventBase,0,&table,256,0,32};
        assert(core.configure(layout)==Result::Success && core.run()==Result::Success);
        assert(commands.initialize(Epoch,32,64,40)==Result::Success);
    }
    uint64_t submit(uintptr_t owner=0x7000) {
        uint64_t ticket=0;assert(commands.submit({},owner,10,1000,ticket)==Result::Success);return ticket;
    }
    void put(XHCI::TRB event) {
        assert(produced<256);event.control|=XHCI::Cycle;eventMemory[produced++]=event;
        ++io.pendingEvents;io.signal();
    }
    void command(const XHCI::CommandRecord &r) {
        assert(r.phase==XHCI::CommandPhase::Completed);++commandCalls;--io.pendingEvents;
    }
    Result port(unsigned port) { ports|=uint64_t(1)<<port;++portCalls;--io.pendingEvents;return Result::Success; }
    Result frameRollover(uint32_t raw) {
        const auto observed=prepareEventAcknowledgement();
        if (observed!=Result::Success) return observed;
        assert(frameEvents.observedWraps()>rollovers);
        rawMicroFrame=raw;++rollovers;--io.pendingEvents;return Result::Success;
    }
    Result prepareEventAcknowledgement() {
        return frameEvents.observe(Epoch,io,[&] { uint32_t raw=0;return core.microFrameCount(raw); });
    }
    Result transfer(const XHCI::TRB &,uint64_t epoch) {
        assert(epoch==Epoch);++transferCalls;if (transferResult==Result::Success) --io.pendingEvents;
        return transferResult;
    }
    Result drain(unsigned budget,XHCI::EventBatch &batch,uint64_t epoch=Epoch) {
        return XHCI::drainEvents(core,events,commands,*this,epoch,budget,batch);
    }
};
XHCI::TRB port(unsigned n) { return XHCI::trb(uint64_t(n)<<24,1u<<24,XHCI::PortEvent<<10); }

void dispatchAndOwnership() {
    EventMachine m;const auto ticket=m.submit();
    m.put(XHCI::trb(EventMachine::CommandBase,1u<<24,XHCI::CommandEvent<<10));
    m.put(port(2));m.put(XHCI::trb(0x1234,1u<<24,XHCI::TransferEvent<<10));
    XHCI::EventBatch b;assert(m.drain(2,b)==Result::Success);
    assert(b.count==2 && b.commands==1 && b.ports==1 && b.more && b.interruptPending);
    assert(m.commands.retained()==1 && m.commands.outstanding()==0 && m.retains==1);
    assert(m.events.dequeueAddress()==EventMachine::EventBase+32 && m.ports==(1u<<2));
    // Returning a consumed prefix reasserts the level interrupt for its tail.
    assert((m.io.registers[0x44/4]&8) && (m.io.registers[0x2020/4]&1));
    assert(m.drain(2,b)==Result::Success && b.transfers==1 && !b.more);
    XHCI::CommandRecord record;assert(m.commands.copy(ticket,record));
    uintptr_t released=0;assert(m.commands.acknowledge(ticket,record.owner,released)==Result::Success);
    assert(released==0x7000 && !m.commands.retained());
    assert(!m.commands.copyRetained(64,record));
}
void acknowledgeRace() {
    EventMachine m;XHCI::EventBatch b;
    m.io.injectAfterEHB=true; // device posts immediately after software releases EHB
    const auto start=m.io.writes.size();
    assert(m.drain(16,b)==Result::Success && !b.count);
    assert((m.io.registers[0x44/4]&8) && (m.io.registers[0x2020/4]&1));
    assert(m.io.writes[start]==0x44 && m.io.writes[start+1]==0x2020 && m.io.writes[start+2]==0x2038);
}
void malformedAndIsolation() {
    for (unsigned bad=0;bad<8;++bad) {
        EventMachine m;const auto ticket=m.submit();auto e=port(1);
        switch (bad) {
        case 0:e.lo=0;break;
        case 1:e.lo=3u<<24;break;
        case 2:e.lo|=1;break;
        case 3:e.hi=1;break;
        case 4:e.status|=1;break;
        case 5:e.control|=1u<<24;break;
        case 6:e.control=37u<<10;break; // Host Controller Event is a fault
        case 7:e=XHCI::trb(EventMachine::CommandBase+16,1u<<24,XHCI::CommandEvent<<10);break;
        }
        m.put(e);XHCI::EventBatch b;assert(m.drain(16,b)==Result::HardwareError);
        assert(!b.count && m.events.dequeueAddress()==EventMachine::EventBase);
        assert(m.commands.faulted() && m.commands.retained()==1 && !(m.io.registers[0x2020/4]&2));
        XHCI::CommandRecord r;assert(m.commands.copyRetained(0,r) && r.ticket==ticket && r.phase==XHCI::CommandPhase::Quarantined);
        uintptr_t released=0;assert(m.commands.acknowledge(ticket,r.owner,released)==Result::Busy);
        assert(m.commands.isolate()==Result::NotReady);
        m.resetProof=true;assert(m.commands.isolate()==Result::Success);
        assert(m.commands.copyRetained(0,r) && r.phase==XHCI::CommandPhase::Isolated);
        assert(m.commands.acknowledge(ticket,r.owner,released)==Result::Success && released==0x7000);
    }
}
void transferAndControllerFaults() {
    EventMachine m;m.submit();m.transferResult=Result::NotReady;
    m.put(XHCI::trb(0x1234,1u<<24,XHCI::TransferEvent<<10));
    XHCI::EventBatch b;assert(m.drain(16,b)==Result::NotReady && m.commands.faulted());
    assert(!b.count && m.commands.retained()==1 && m.transferCalls==1);
    for (uint32_t status:{UINT32_MAX,1u,4u,1u<<12}) {
        EventMachine peer;peer.submit();peer.io.registers[0x44/4]=status;
        assert(peer.drain(16,b)==Result::HardwareError && peer.commands.faulted() && !b.count);
    }
    EventMachine peer;const auto before=peer.io.writes.size();
    assert(peer.drain(0,b)==Result::Invalid && peer.drain(16,b,18)==Result::Invalid);
    assert(peer.io.writes.size()==before && !peer.commands.faulted());
}
void frameClock() {
    EventMachine m;uint32_t raw=0;
    assert(m.core.frameCounterOffset()==0x2000 && (m.io.registers[0x40/4]&(1u<<10)));
    m.io.registers[0x2000/4]=0x3fff;
    assert(m.core.microFrameCount(raw)==Result::Success && raw==0x3fff);
    assert(XHCI::frameNumber(0,raw)==2047);
    m.io.registers[0x2000/4]=0x4007; // upper bits never extend the Native raw value
    assert(m.core.microFrameCount(raw)==Result::Success && raw==7);
    assert(XHCI::frameNumber(1,raw)==2048 && XHCI::frameNumber(1,8)==2049);
    assert(XHCI::frameNumber(0x123456,0x3fff)==(uint64_t(0x123457)<<11)-1);
    const auto wrap=XHCI::trb(0,1u<<24,XHCI::MicroFrameWrapEvent<<10);
    m.put(wrap);m.put(wrap);XHCI::EventBatch b;
    assert(m.drain(1,b)==Result::Success && b.frameRollovers==1 && b.more && m.rollovers==1 && m.rawMicroFrame==7);
    assert(m.drain(1,b)==Result::Success && b.frameRollovers==1 && !b.more && m.rollovers==2);
    uint64_t stamp=0;
    assert(XHCI::rolloverNanoseconds(3000000000,7,stamp) && stamp==2999125000);
    assert(!XHCI::rolloverNanoseconds(874999,7,stamp));
    assert(!XHCI::rolloverNanoseconds(UINT64_MAX,0x4000,stamp));
    for (unsigned bad=0;bad<6;++bad) {
        EventMachine peer;auto event=wrap;
        if (bad==0) event.lo=1;
        else if (bad==1) event.hi=1;
        else if (bad==2) event.status|=1;
        else if (bad==3) event.control|=1u<<16;
        else if (bad==4) event.control|=1u<<24;
        else peer.io.registers[0x2000/4]=UINT32_MAX;
        peer.put(event);
        assert(peer.drain(1,b)==Result::HardwareError && !b.frameRollovers && !peer.rollovers);
        assert(peer.commands.faulted() && !b.count && peer.events.dequeueAddress()==EventMachine::EventBase);
    }
    assert(m.core.halt()==Result::Success && !(m.io.registers[0x40/4]&(1u<<10)));
    assert(m.core.microFrameCount(raw)==Result::NotReady);
}
void frameObserver() {
    EventMachine m;XHCI::FrameEvents observer;
    assert(!observer.initialize(nullptr,256,EventMachine::Epoch));
    assert(!observer.initialize(m.eventMemory.data(),15,EventMachine::Epoch));
    assert(!observer.initialize(m.eventMemory.data(),256,0));
    assert(observer.initialize(m.eventMemory.data(),256,EventMachine::Epoch));
    assert(!observer.initialize(m.eventMemory.data(),256,EventMachine::Epoch));
    unsigned publications=0;
    auto publish=[&] { ++publications;return Result::Success; };
    assert(observer.observe(EventMachine::Epoch+1,m.io,publish)==Result::Invalid && !publications);
    const auto wrap=XHCI::trb(0,1u<<24,XHCI::MicroFrameWrapEvent<<10);
    m.put(port(1));m.put(wrap);m.put(port(2));m.put(wrap);
    // This is the filter phase: Native time advances while the action's
    // real event consumer and ERDP still point at the start of the segment.
    assert(observer.observe(EventMachine::Epoch,m.io,publish)==Result::Success && publications==2);
    assert(observer.observedWraps()==2 && m.events.dequeueAddress()==EventMachine::EventBase);
    assert(observer.observe(EventMachine::Epoch,m.io,publish)==Result::Success && publications==2);
    XHCI::EventBatch batch;assert(m.drain(1,batch)==Result::Success && batch.more);
    assert(observer.observe(EventMachine::Epoch,m.io,publish)==Result::Success && publications==2);
    assert(m.drain(3,batch)==Result::Success && batch.frameRollovers==2 && !batch.more);
    // Complete a segment and cross its cycle toggle. Observation must not
    // replay the first cycle, or miss a wrap in the recycled slot zero.
    for (unsigned i=4;i<256;++i) m.put(port(1));
    assert(observer.observe(EventMachine::Epoch,m.io,publish)==Result::Success && publications==2);
    assert(m.drain(256,batch)==Result::Success && batch.count==252);
    m.eventMemory[0]=wrap; // new hardware cycle is zero
    assert(observer.observe(EventMachine::Epoch,m.io,publish)==Result::Success && publications==3);
    assert(observer.observe(EventMachine::Epoch,m.io,publish)==Result::Success && publications==3);
    observer.close();
    m.eventMemory[1]=wrap;
    assert(!observer.active() && observer.observe(EventMachine::Epoch,m.io,publish)==Result::NotReady);
    assert(!observer.initialize(m.eventMemory.data(),256,EventMachine::Epoch+1));
    assert(publications==3);

    EventMachine peer;XHCI::FrameEvents failed;
    assert(failed.initialize(peer.eventMemory.data(),256,EventMachine::Epoch));peer.put(wrap);
    assert(failed.observe(EventMachine::Epoch,peer.io,[] { return Result::Aborted; })==Result::Aborted);
    assert(!failed.observedWraps()); // failed/revoked sample never consumes proof
    assert(failed.observe(EventMachine::Epoch,peer.io,publish)==Result::Success && failed.observedWraps()==1);
    auto malformed=wrap;malformed.hi=1;peer.put(malformed);
    assert(failed.observe(EventMachine::Epoch,peer.io,publish)==Result::HardwareError && failed.observedWraps()==1);
    assert(peer.events.dequeueAddress()==EventMachine::EventBase);
    peer.eventMemory[1]=wrap;peer.eventMemory[1].control|=XHCI::Cycle;
    assert(failed.observe(EventMachine::Epoch,peer.io,publish)==Result::Success && failed.observedWraps()==2);

    EventMachine race;
    assert(race.prepareEventAcknowledgement()==Result::Success && !race.frameEvents.observedWraps());
    // Hardware posts after the filter scanned. The action consumes a normal
    // event; its acknowledgement must catch the unconsumed wrap as well.
    race.put(port(1));race.put(wrap);
    assert(race.drain(1,batch)==Result::Success && batch.ports==1 && !batch.frameRollovers);
    assert(race.frameEvents.observedWraps()==1 && !race.rollovers && batch.more);
    assert(race.drain(1,batch)==Result::Success && batch.frameRollovers==1 && race.rollovers==1);
}
}
void xhciEventTests() {
    dispatchAndOwnership();acknowledgeRace();malformedAndIsolation();transferAndControllerFaults();frameClock();frameObserver();
}
