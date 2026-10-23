// SPDX-License-Identifier: BSD-2-Clause
#include "XHCISlots.h"
#include "XHCITTClear.h"
#include "usb_memory_model.h"
#include <array>
#include <map>

namespace {
struct Order { void dmaReadBarrier() {} void dmaWriteBarrier() {} };
struct Backend {
    Order order;alignas(64) XHCI::TRB data[64]{};
    XHCI::ProducerRing<Order> producer{order};
    std::map<uintptr_t,unsigned> refs;
    Backend() { assert(producer.initialize(data,0x300000,64,36)); }
    auto &ring() { return producer; }
    bool admitted(uint64_t epoch) { return epoch==19; }
    void retain(uintptr_t owner) { ++refs[owner]; }
    Result doorbell() { return Result::Success; }
    Result isolation(uint64_t) { return Result::Busy; }
};
using Commands=XHCI::CommandQueue<Backend>;
using Memory=XHCI::SlotMemory<MemoryAPI,Order>;
XHCI::PortProtocol protocol(bool super=false) {
    XHCI::PortProtocol p;p.major=super ? 3 : 2;
    if (super) p.speeds[4]=USB::Speed::Super;
    else { p.speeds[1]=USB::Speed::Full;p.speeds[2]=USB::Speed::Low;p.speeds[3]=USB::Speed::High; }
    return p;
}
XHCI::FamilyLocation path(uint32_t location) {
    XHCI::FamilyLocation value;assert(XHCI::decodeFamilyLocation(location,0x1f000000,2,value)==Result::Success);return value;
}
void promote(XHCI::TopologyNode &node,XHCI::HubProfile profile) {
    XHCI::SlotConfiguration c;assert(node.hubCandidate(profile,c)==Result::Success);
    node.configuration=c;node.hubRegistered=true; // Pure topology fixture; DMA proof is exercised separately below.
}
void descriptorPolicies() {
    XHCI::HubProfile p{14,3,true};
    StandardUSB::HubDescriptor d{9,0x29,4,3u<<5,0,0,{},{}};
    assert(XHCI::hubProfile(&d,USB::Speed::High,1,p)==Result::Success && p.ports==4 && p.think==3 && p.multi);
    assert(XHCI::hubProfile(&d,USB::Speed::Full,0,p)==Result::Success && !p.think && !p.multi);
    for (unsigned bad=0;bad<7;++bad) {
        auto copy=d;auto speed=USB::Speed::High;unsigned flags=0;
        switch (bad) {
        case 0:copy.bDescriptorType=0x2a;break;
        case 1:copy.bNumberPorts=0;break;
        case 2:copy.bNumberPorts=16;break;
        case 3:copy.bLength=8;break;
        case 4:copy.bNumberPorts=8;break; // Two-byte bitmaps require eleven bytes.
        case 5:flags=2;break;
        case 6:speed=USB::Speed::Low;break;
        }
        p={14,3,true};assert(XHCI::hubProfile(&copy,speed,flags,p)==Result::Invalid && p.ports==14 && p.multi);
    }
    d.bNumberPorts=15;d.bLength=11;assert(XHCI::hubProfile(&d,USB::Speed::High,0,p)==Result::Success);
    assert(XHCI::hubProfile(&d,USB::Speed::Full,1,p)==Result::Invalid);
    StandardUSB::SuperSpeedHubDescriptor ss{12,0x2a,15,0,0,0,0,0,0};
    assert(XHCI::hubProfile(&ss,USB::Speed::Super,p)==Result::Success && p.ports==15 && !p.multi && !p.think);
    ss.bLength=11;assert(XHCI::hubProfile(&ss,USB::Speed::Super,p)==Result::Invalid);
    ss.bLength=12;assert(XHCI::hubProfile(&ss,USB::Speed::High,p)==Result::Invalid);
}
void ttWireTargets() {
    XHCI::SlotConfiguration child;child.speed=USB::Speed::Full;child.ttHub=3;child.ttPort=4;
    XHCI::EndpointConfiguration ep;ep.speed=child.speed;ep.type=XHCI::EndpointType::Control;
    XHCI::TTClearPlan plan;
    for (bool multi:{false,true}) for (auto speed:{USB::Speed::Low,USB::Speed::Full}) {
        child.speed=ep.speed=speed;
        assert(XHCI::ttClearPlan(child,ep,38,multi,plan)==Result::Success && plan.count==2);
        assert(plan.requests[0].bmRequestType==0x23 && plan.requests[0].bRequest==8 &&
            plan.requests[0].wValue==0x260 && plan.requests[1].wValue==0x8260);
        assert(plan.requests[0].wIndex==(multi ? 4 : 1) && plan.requests[1].wIndex==plan.requests[0].wIndex &&
            !plan.requests[0].wLength && !plan.requests[1].wLength);
    }
    child.speed=ep.speed=USB::Speed::Full;ep.type=XHCI::EndpointType::Bulk;
    for (unsigned endpoint:{1u,15u,0x81u,0x8fu}) {
        ep.address=endpoint;
        assert(XHCI::ttClearPlan(child,ep,38,true,plan)==Result::Success && plan.count==1);
        assert(plan.requests[0].wValue==((endpoint&128 ? 0x8000 : 0)|0x1000|0x260|(endpoint&15)) &&
            plan.requests[0].wIndex==4);
        // The wire address is child38, independent of parent slot3 and the
        // upstream hub's USB address73 used by this topology's other tests.
        assert(((plan.requests[0].wValue>>4)&127)==38);
    }
    for (unsigned bad=0;bad<13;++bad) {
        auto c=child;auto e=ep;unsigned address=38;
        switch (bad) {
        case 0:c.ttHub=0;break;case 1:c.ttHub=256;break;
        case 2:c.ttPort=0;break;case 3:c.ttPort=16;break;
        case 4:address=0;break;case 5:address=128;break;
        case 6:c.speed=e.speed=USB::Speed::High;break;
        case 7:c.speed=USB::Speed::Low;break;
        case 8:c.speed=e.speed=USB::Speed::Low;break;
        case 9:e.address=0x80;break;case 10:e.address=0x91;break;
        case 11:e.type=XHCI::EndpointType::Control;break;
        case 12:e.type=XHCI::EndpointType::Interrupt;break;
        }
        XHCI::TTClearPlan unchanged;unchanged.count=9;unchanged.requests[0].wValue=0x1234;
        assert(XHCI::ttClearPlan(c,e,address,true,unchanged)==Result::Invalid &&
            unchanged.count==9 && unchanged.requests[0].wValue==0x1234);
    }
}
void mixedSpeedTreeAndTT() {
    const auto p=protocol();
    XHCI::TopologyNode hs(0x1f200000),fs(0x1f240000),low(0x1f242000),hs2(0x1f220000),leaf(0x1f223000);
    assert(hs.initialize(path(hs.location),nullptr,USB::Speed::High,p,3)==Result::Success);hs.slot=3;
    promote(hs,{4,3,true});
    assert(fs.initialize(path(fs.location),&hs,USB::Speed::Full,p,3)==Result::Success);fs.slot=5;
    assert(fs.configuration.route==4 && fs.configuration.speedID==1 && fs.configuration.ttHub==3 &&
        fs.configuration.ttPort==4 && fs.configuration.multiTT && !fs.configuration.ttThink);
    promote(fs,{3,0,false});assert(!fs.configuration.multiTT && fs.configuration.ttHub==3);
    assert(low.initialize(path(low.location),&fs,USB::Speed::Low,p,3)==Result::Success);low.slot=6;
    assert(low.configuration.route==0x24 && low.configuration.speedID==2 && low.configuration.ttHub==3 &&
        low.configuration.ttPort==4 && low.configuration.multiTT && !low.configuration.ttThink);
    assert(hs2.initialize(path(hs2.location),&hs,USB::Speed::High,p,3)==Result::Success);hs2.slot=7;
    assert(!hs2.configuration.ttHub && !hs2.configuration.multiTT);promote(hs2,{4,1,false});
    assert(leaf.initialize(path(leaf.location),&hs2,USB::Speed::Full,p,3)==Result::Success);leaf.slot=8;
    assert(leaf.configuration.ttHub==7 && leaf.configuration.ttPort==3 && !leaf.configuration.multiTT);
    assert(hs.children==2 && fs.children==1 && hs2.children==1 && low.descendsFrom(hs) && !hs.descendsFrom(low));
    assert(low.ttOwner()==&hs && low.ttDomainAlive() && leaf.ttOwner()==&hs2 && leaf.ttDomainAlive());
    fs.retiring=true;fs.hubRegistered=false;
    assert(!low.ancestorsReady() && low.ttOwner()==&hs && low.ttDomainAlive());
    fs.retiring=false;fs.hubRegistered=true;
    assert(hs.detach()==Result::Busy && fs.detach()==Result::Busy);
    XHCI::SlotConfiguration same;assert(hs.hubCandidate({4,3,true},same)==Result::Success);
    assert(hs.hubCandidate({4,3,false},same)==Result::Busy);
    hs.unregisterHub(true);
    assert(!leaf.ancestorsReady() && !low.ancestorsReady() && !low.ttDomainAlive() && !leaf.ttDomainAlive());
    assert(leaf.detach()==Result::Success && hs2.detach()==Result::Success);
    assert(low.detach()==Result::Success && fs.detach()==Result::Success && !hs.children);
    assert(hs.detach()==Result::Success);
    assert(hs.hubCandidate({4,3,true},same)==Result::NotReady); // Retiring TT descendants requires a new hub lifetime.
}
void fullSpeedRootAndSuperSpeed() {
    for (bool super:{false,true}) {
        const auto p=protocol(super);const auto speed=super ? USB::Speed::Super : USB::Speed::Full;
        XHCI::TopologyNode root(0x1f200000),child(0x1f220000),wrong(0x1f230000);
        assert(root.initialize(path(root.location),nullptr,speed,p,super ? 4 : 1)==Result::Success);root.slot=3;
        promote(root,{4,0,false});
        assert(child.initialize(path(child.location),&root,super ? speed : USB::Speed::Low,p,super ? 4 : 1)==Result::Success);
        assert(!child.configuration.ttHub && !child.configuration.ttPort && !child.configuration.multiTT);
        assert(wrong.initialize(path(wrong.location),&root,super ? USB::Speed::Full : USB::Speed::High,p,super ? 4 : 1)==Result::Invalid);
        assert(child.detach()==Result::Success);
        root.unregisterHub(false);XHCI::SlotConfiguration same;
        assert(root.hubCandidate({4,0,false},same)==Result::Success);
    }
}
void topologyRejectionsAndDepth() {
    const auto p=protocol();XHCI::TopologyNode root(0x1f200000);
    assert(root.initialize(path(root.location),nullptr,USB::Speed::High,p,3)==Result::Success);root.slot=3;
    XHCI::TopologyNode child(0x1f220000);
    assert(child.initialize(path(child.location),&root,USB::Speed::Full,p,3)==Result::NotReady);
    promote(root,{4,0,false});
    assert(child.initialize(path(child.location),&root,USB::Speed::Full,p,1)==Result::NotReady);
    root.retiring=true;assert(child.initialize(path(child.location),&root,USB::Speed::Full,p,3)==Result::NotReady);root.retiring=false;
    XHCI::TopologyNode outOfRange(0x1f250000);assert(outOfRange.initialize(path(outOfRange.location),&root,USB::Speed::Full,p,3)==Result::Invalid);
    auto forged=path(child.location);forged.route=3;
    assert(child.initialize(forged,&root,USB::Speed::Full,p,3)==Result::Invalid && !root.children && !child.parent);
    auto ambiguous=p;ambiguous.speeds[7]=USB::Speed::Full;
    assert(child.initialize(path(child.location),&root,USB::Speed::Full,ambiguous,3)==Result::Unsupported && !root.children);
    std::array<uint32_t,5> locations{0x1f210000,0x1f211000,0x1f211100,0x1f211110,0x1f211111};
    XHCI::TopologyNode a(locations[0]),b(locations[1]),c(locations[2]),d(locations[3]),e(locations[4]);
    XHCI::TopologyNode *nodes[]={&root,&a,&b,&c,&d,&e};
    for (unsigned i=1;i<6;++i) {
        assert(nodes[i]->initialize(path(locations[i-1]),nodes[i-1],USB::Speed::High,p,3)==Result::Success);nodes[i]->slot=3+i;
        if (i<5) promote(*nodes[i],{1,0,false});
    }
    XHCI::SlotConfiguration candidate;assert(e.hubCandidate({1,0,false},candidate)==Result::Invalid);
    assert(e.root()==&root && e.descendsFrom(root));
    for (unsigned i=5;i>0;--i) assert(nodes[i]->detach()==Result::Success);
    assert(!root.children);
}

struct Machine {
    MemoryAPI api;Order order;Backend backend;Commands commands{backend};
    Memory memory{api,order};XHCI::TopologyNode node{0x1f200000};
    XHCI::PortProtocol ports=protocol();unsigned stride;
    MemoryAPI::Memory *allocation=nullptr;
    uint64_t now=1;
    explicit Machine(unsigned contextStride=64):stride(contextStride) {
        api.bus=0x100000;
        assert(commands.initialize(19,32,stride,36)==Result::Success);
        assert(node.initialize(path(node.location),nullptr,USB::Speed::High,ports,3)==Result::Success);node.slot=3;
        assert(memory.prepare(stride,36,32,2)==Result::Success);allocation=api.memories.back();
        assert(memory.configure(node.configuration,ports)==Result::Success && memory.publish(19)==Result::Success);
        writeSlot(node.configuration,73,2);auto *ep=output()+stride/4;ep[0]=1;ep[1]=64u<<16;
        const auto address=memory.ep0Ring().startAddress()|1;ep[2]=uint32_t(address);ep[3]=uint32_t(address>>32);
        auto &input=memory.inputContext();uint64_t ticket=0;
        assert(input.submit(commands,XHCI::AddressDevice,3,false,0xee,now++,1000,ticket)==Result::Success);
        complete(ticket);assert(input.retire(ticket)==Result::Success);
        XHCI::CommandRecord record;assert(commands.copy(ticket,record));XHCI::SlotSnapshot out;
        assert(memory.addressed(order,record,3,out)==Result::Success && out.address==73);ack(ticket);
    }
    uint32_t *output() { return reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(allocation->data)+Memory::OutputOffset); }
    void writeSlot(const XHCI::SlotConfiguration &c,unsigned address,unsigned state) {
        auto *out=output();out[0]=c.route|c.speedID<<20|uint32_t(c.multiTT)<<25|uint32_t(c.hub)<<26|c.entries<<27;
        out[1]=c.rootPort<<16|c.ports<<24;out[2]=c.ttHub|c.ttPort<<8|c.ttThink<<16;out[3]=address|state<<27;
    }
    void complete(uint64_t ticket,unsigned code=1,unsigned slot=3) {
        XHCI::CommandRecord record,done;assert(commands.copy(ticket,record));
        assert(commands.complete(XHCI::trb(record.address,code<<24,XHCI::CommandEvent<<10|slot<<24),19,done)==Result::Success);
    }
    void ack(uint64_t ticket) {
        XHCI::CommandRecord r;assert(commands.copy(ticket,r));uintptr_t owner=0;
        assert(commands.acknowledge(ticket,r.owner,owner)==Result::Success && backend.refs[owner]);
        if (!--backend.refs[owner]) backend.refs.erase(owner);
    }
    uint64_t configure(bool evaluate=false,uintptr_t owner=0xee) {
        uint64_t ticket=0;
        assert(memory.inputContext().submit(commands,evaluate ? XHCI::EvaluateContext : XHCI::ConfigureEndpoint,
            3,false,owner,now++,1000,ticket)==Result::Success);return ticket;
    }
    void finish() {
        if (memory.inputContext().frozen()) {
            const auto t=memory.inputContext().commandTicket();XHCI::CommandRecord r;assert(commands.copy(t,r));
            if (r.phase==XHCI::CommandPhase::Pending) complete(t);
            assert(memory.inputContext().retire(t)==Result::Success);
        }
        for (unsigned i=0;i<64;++i) { XHCI::CommandRecord r;if (commands.copyRetained(i,r)) ack(r.ticket); }
        XHCI::Command disable;disable.type=XHCI::DisableSlot;disable.slot=3;uint64_t ticket=0;
        assert(commands.submit(disable,0xee,now++,1000,ticket)==Result::Success);complete(ticket);
        XHCI::CommandRecord record;assert(commands.copy(ticket,record));
        assert(memory.disabled(record,3,19)==Result::Success);ack(ticket);
        assert(memory.release()==Result::Success && backend.refs.empty() && api.memories.empty());
    }
};
void actualHubConfigureAndUnchangedEP0() {
    for (unsigned stride:{32u,64u}) {
        Machine m(stride);m.node.configuration.entries=4;XHCI::SlotConfiguration c;
        assert(m.node.hubCandidate({4,3,true},c)==Result::Success);
        std::array<uint32_t,8> ep;std::memcpy(ep.data(),m.output()+stride/4,32);
        assert(m.memory.stageHub(c,m.ports,m.commands)==Result::Success);
        assert(m.memory.inputContext().addMask()==1 && !m.memory.inputContext().dropMask());
        auto *in=static_cast<uint32_t *>(m.allocation->data)+stride/4;
        assert(in[0]==(3u<<20|1u<<25|1u<<26|4u<<27) && in[1]==(2u<<16|4u<<24) && in[2]==3u<<16);
        const auto ticket=m.configure();XHCI::SlotSnapshot out;
        assert(m.memory.hubConfigured(m.order,m.commands,ticket,0xee,3,73,out)==Result::Invalid);
        m.complete(ticket);m.writeSlot(c,73,3);
        assert(m.memory.hubConfigured(m.order,m.commands,ticket,0xee,3,73,out)==Result::Invalid); // Still frozen input.
        assert(m.memory.inputContext().retire(ticket)==Result::Success);
        assert(m.memory.hubConfigured(m.order,m.commands,ticket,0xee,3,73,out)==Result::Success);m.ack(ticket);
        assert(out.hub && out.multiTT && out.ports==4 && out.ttThink==3 && out.entries==4);
        assert(!std::memcmp(ep.data(),m.output()+stride/4,32));
        assert(m.memory.currentState()==DMAState::Owned && m.memory.release()==Result::Busy);m.finish();
    }
}
void failedEnumerationDefaultTT() {
    for (unsigned stride:{32u,64u}) for (bool multi:{false,true}) for (auto speed:{USB::Speed::Low,USB::Speed::Full}) {
        Machine m(stride);XHCI::SlotConfiguration hub;
        assert(m.node.hubCandidate({4,2,multi},hub)==Result::Success);
        assert(m.memory.stageHub(hub,m.ports,m.commands)==Result::Success);
        auto ticket=m.configure();m.complete(ticket);assert(m.memory.inputContext().retire(ticket)==Result::Success);
        m.writeSlot(hub,73,2);XHCI::SlotSnapshot snapshot;
        assert(m.memory.hubConfigured(m.order,m.commands,ticket,0xee,3,73,snapshot)==Result::Success);m.ack(ticket);
        m.node.configuration=hub;m.node.hubRegistered=true;
        XHCI::TopologyNode childNode(0x1f220000);
        assert(childNode.initialize(path(childNode.location),&m.node,speed,m.ports,3)==Result::Success);
        XHCI::Command enable;enable.type=XHCI::EnableSlot;
        assert(m.commands.submit(enable,0xee,m.now++,1000,ticket)==Result::Success);m.complete(ticket,1,5);m.ack(ticket);
        childNode.slot=5;m.api.bus=0x200000;Memory child(m.api,m.order);
        assert(child.prepare(stride,36,32,2)==Result::Success);
        assert(child.configure(childNode.configuration,m.ports)==Result::Success && child.publish(19)==Result::Success);
        auto &input=child.inputContext();XHCI::TTEnumerationRecovery recovery;
        assert(recovery.arm(m.commands,childNode.configuration,19,5,input.address(),0xee)==Result::Success);
        assert(input.submit(m.commands,XHCI::AddressDevice,5,false,0xee,m.now++,1000,ticket)==Result::Success);
        assert(!recovery.failed(m.commands,ticket)); // Pending command is not an error proof.
        m.complete(ticket,4,5);assert(input.retire(ticket)==Result::Success);
        assert(recovery.failed(m.commands,ticket) && !recovery.failed(m.commands,ticket));m.ack(ticket);
        assert(recovery.dirty() && recovery.blocks(childNode.configuration) && m.node.children==1);
        auto another=childNode.configuration;another.ttPort=3;
        assert(recovery.blocks(another)==!multi); // Single-TT fences every downstream branch.
        another.ttHub=7;assert(!recovery.blocks(another));
        XHCI::TTClearPlan plan;
        assert(XHCI::ttClearDefaultPlan(childNode.configuration,multi,plan)==Result::Success && plan.count==2);
        assert(plan.requests[0].wValue==0 && plan.requests[1].wValue==0x8000 &&
            plan.requests[0].wIndex==(multi ? 2 : 1) && !plan.requests[0].wLength);
        // No Native child or pipe was created, and no new Address Device is
        // admitted in this TT domain while the default-address buffer is dirty.
        assert(child.release()==Result::Busy && m.node.detach()==Result::Busy);
        assert(!recovery.finish(true,10) && recovery.schedule(10) && !recovery.schedule(10));
        assert(recovery.start() && !recovery.start() && recovery.finish(false,10));
        assert(recovery.blocks(childNode.configuration) && !recovery.schedule(500009));
        assert(recovery.schedule(500010) && recovery.start() && recovery.finish(true,500010));
        assert(!recovery.dirty() && !recovery.blocks(childNode.configuration));
        assert(child.release()==Result::Busy && m.node.children==1); // Clearing TT does not release slot DMA.
        XHCI::Command disable;disable.type=XHCI::DisableSlot;disable.slot=5;
        assert(m.commands.submit(disable,0xee,m.now++,1000,ticket)==Result::Success);m.complete(ticket,1,5);
        XHCI::CommandRecord record;assert(m.commands.copy(ticket,record));
        assert(child.disabled(record,5,19)==Result::Success);m.ack(ticket);
        m.api.clearFailures=1;assert(child.release()==Result::HardwareError && m.node.children==1);
        assert(child.release()==Result::Success && childNode.detach()==Result::Success && !m.node.children);
        m.finish();
    }
}
void enumerationFailureProofs() {
    for (unsigned bad=0;bad<8;++bad) {
        Machine m;XHCI::SlotConfiguration c;c.speed=USB::Speed::Full;c.ttHub=3;c.ttPort=2;
        XHCI::TTEnumerationRecovery recovery;auto &input=m.memory.inputContext();
        assert(recovery.arm(m.commands,c,19,5,input.address(),0xee)==Result::Success);
        XHCI::Command command;command.type=XHCI::AddressDevice;command.slot=5;command.parameter=input.address();
        uintptr_t owner=0xee;unsigned code=4;
        switch (bad) {
        case 0:command.slot=6;break;case 1:owner=0xab;break;
        case 2:command.parameter+=64;break;case 3:command.option=true;break;
        case 4:command.type=XHCI::ConfigureEndpoint;break;
        case 5:code=1;break;case 6:code=3;break;
        case 7:break; // Acknowledged command no longer proves this new recovery.
        }
        uint64_t ticket=0;assert(m.commands.submit(command,owner,m.now++,1000,ticket)==Result::Success);
        m.complete(ticket,code,command.slot);
        if (bad==7) m.ack(ticket);
        assert(!recovery.failed(m.commands,ticket) && !recovery.dirty());m.finish();
    }
    Machine m;XHCI::SlotConfiguration c;c.speed=USB::Speed::Full;c.ttHub=3;c.ttPort=2;
    auto &input=m.memory.inputContext();XHCI::TTEnumerationRecovery recovery;
    assert(recovery.arm(m.commands,c,19,5,input.address(),0xee)==Result::Success);
    Backend otherBackend;Commands other(otherBackend);assert(other.initialize(19,32,64,36)==Result::Success);
    XHCI::Command command;command.type=XHCI::AddressDevice;command.slot=5;command.parameter=input.address();
    uint64_t ticket=0;assert(other.submit(command,0xee,1,1000,ticket)==Result::Success);
    XHCI::CommandRecord record,done;assert(other.copy(ticket,record));
    assert(other.complete(XHCI::trb(record.address,4u<<24,XHCI::CommandEvent<<10|5u<<24),19,done)==Result::Success);
    assert(!recovery.failed(other,ticket)); // Same owner/slot/input/epoch on a foreign queue is insufficient.
    uintptr_t owner=0;assert(other.acknowledge(ticket,0xee,owner)==Result::Success);
    assert(owner==0xee && otherBackend.refs[owner]==1);otherBackend.refs.erase(owner);
    assert(m.commands.submit(command,0xee,m.now++,1000,ticket)==Result::Success);m.complete(ticket,4,5);
    XHCI::TTEnumerationRecovery stale;
    assert(stale.arm(m.commands,c,19,5,input.address(),0xee)==Result::Success);
    assert(!stale.failed(m.commands,ticket)); // Arm floor excludes earlier completed commands.
    assert(recovery.failed(m.commands,ticket));m.ack(ticket);m.finish();
}
void hubCommandAndOutputProofs() {
    for (unsigned bad=0;bad<15;++bad) {
        Machine m;XHCI::SlotConfiguration c;assert(m.node.hubCandidate({4,3,true},c)==Result::Success);
        assert(m.memory.stageHub(c,m.ports,m.commands)==Result::Success);
        const auto ticket=m.configure(bad==0,bad==1 ? 0xab : 0xee);
        m.complete(ticket,bad==2 ? 19 : 1);m.writeSlot(c,73,3);
        assert(m.memory.inputContext().retire(ticket)==Result::Success);
        auto *out=m.output();
        switch (bad) {
        case 3:out[0]^=1u<<26;break;
        case 4:out[0]^=1u<<25;break;
        case 5:out[1]^=1u<<24;break;
        case 6:out[2]^=1u<<16;break;
        case 7:out[3]=72|3u<<27;break;
        case 8:out[0]^=1;break;
        case 9:out[0]^=1u<<20;break;
        case 10:out[2]^=1;break;
        case 11:out[2]^=1u<<8;break;
        case 12:out[0]^=1u<<27;break;
        case 13:m.ack(ticket);break; // A copied completion is not a retained queue record.
        case 14:out[3]=73|1u<<27;break;
        }
        XHCI::SlotSnapshot snapshot;
        assert(m.memory.hubConfigured(m.order,m.commands,ticket,0xee,3,73,snapshot)!=Result::Success);
        assert(m.memory.release()==Result::Busy && m.memory.currentState()==DMAState::Owned);m.finish();
    }
    Machine m;Backend peer;Commands other{peer};assert(other.initialize(19,32,64,36)==Result::Success);
    XHCI::SlotConfiguration c;assert(m.node.hubCandidate({4,0,false},c)==Result::Success);
    assert(m.memory.stageHub(c,m.ports,m.commands)==Result::Success);
    const auto t=m.configure();m.complete(t);m.writeSlot(c,73,3);assert(m.memory.inputContext().retire(t)==Result::Success);
    XHCI::SlotSnapshot out;assert(m.memory.hubConfigured(m.order,other,t,0xee,3,73,out)==Result::Invalid);m.finish();
}
void parentLeaseSurvivesChildUnmapFailure() {
    Machine m;XHCI::SlotConfiguration hub;
    assert(m.node.hubCandidate({4,0,true},hub)==Result::Success);
    assert(m.memory.stageHub(hub,m.ports,m.commands)==Result::Success);
    const auto registered=m.configure();m.complete(registered);m.writeSlot(hub,73,3);
    assert(m.memory.inputContext().retire(registered)==Result::Success);XHCI::SlotSnapshot snapshot;
    assert(m.memory.hubConfigured(m.order,m.commands,registered,0xee,3,73,snapshot)==Result::Success);m.ack(registered);
    m.node.configuration=hub;m.node.hubRegistered=true;
    XHCI::TopologyNode node(0x1f220000);
    assert(node.initialize(path(node.location),&m.node,USB::Speed::Full,m.ports,3)==Result::Success);
    assert(m.node.children==1 && node.configuration.ttHub==3 && node.configuration.ttHub!=73);
    Memory child(m.api,m.order);m.api.bus=0x500000;
    assert(child.prepare(64,36,32,2)==Result::Success);
    auto *out=reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(m.api.memories.back()->data)+Memory::OutputOffset);
    assert(child.configure(node.configuration,m.ports)==Result::Success && child.publish(19)==Result::Success);
    // Independent child output: FS speedID1, route2, parent TT SLOT3/port2,
    // hardware USB address38 (neither the child's slot5 nor parent address73).
    out[0]=2|1u<<20|1u<<25|1u<<27;out[1]=2u<<16;out[2]=3|2u<<8;out[3]=38|2u<<27;
    out[16]=1;const auto dq=child.ep0Ring().startAddress()|1;out[18]=uint32_t(dq);out[19]=uint32_t(dq>>32);
    uint64_t ticket=0;auto &input=child.inputContext();
    assert(input.submit(m.commands,XHCI::AddressDevice,5,false,0xee,m.now++,1000,ticket)==Result::Success);
    m.complete(ticket,1,5);assert(input.retire(ticket)==Result::Success);
    XHCI::CommandRecord record;assert(m.commands.copy(ticket,record));
    assert(child.addressed(m.order,record,5,snapshot)==Result::Success && snapshot.address==38);m.ack(ticket);node.slot=5;
    m.node.retiring=true;m.node.hubRegistered=false;
    assert(!node.ancestorsReady() && m.node.detach()==Result::Busy);
    XHCI::Command disable;disable.type=XHCI::DisableSlot;disable.slot=5;
    assert(m.commands.submit(disable,0xee,m.now++,1000,ticket)==Result::Success);m.complete(ticket,1,5);
    assert(m.commands.copy(ticket,record) && child.disabled(record,5,19)==Result::Success);m.ack(ticket);
    m.api.clearFailures=1;
    assert(child.release()==Result::HardwareError && m.node.children==1 && m.node.detach()==Result::Busy);
    assert(m.memory.currentState()==DMAState::Owned && m.memory.release()==Result::Busy);
    assert(child.release()==Result::Success && node.detach()==Result::Success && !m.node.children);
    m.finish();
}
}
void xhciHubTests() {
    descriptorPolicies();ttWireTargets();mixedSpeedTreeAndTT();fullSpeedRootAndSuperSpeed();topologyRejectionsAndDepth();
    actualHubConfigureAndUnchangedEP0();hubCommandAndOutputProofs();parentLeaseSurvivesChildUnmapFailure();
    failedEnumerationDefaultTT();enumerationFailureProofs();
}
