// SPDX-License-Identifier: BSD-2-Clause
#include "XHCISlots.h"
#include "usb_memory_model.h"

namespace {
struct Order {
    unsigned writes=0,reads=0;
    void dmaWriteBarrier() { ++writes; }
    void dmaReadBarrier() { ++reads; }
};
using Memory=XHCI::SlotMemory<MemoryAPI,Order>;
struct Machine {
    MemoryAPI api;
    Order order;
    Memory memory{api,order};
    XHCI::SlotConfiguration configuration;
    XHCI::PortProtocol protocol;
    unsigned stride;
    explicit Machine(unsigned contextStride,USB::Speed speed=USB::Speed::High) : stride(contextStride) {
        protocol.major=speed==USB::Speed::Super ? 3 : 2;
        protocol.speeds[7]=speed;
        configuration.rootPort=2;configuration.speedID=7;configuration.speed=speed;
        assert(!memory.outputAddress());
        assert(memory.prepare(stride,36,32,2)==Result::Success);
        assert(memory.configure(configuration,protocol)==Result::Success);
    }
    void output(unsigned address) {
        // Independent hardware writer: Native software never writes these
        // output words after publishing the DCBAA pointer.
        auto *words=reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(api.memories[0]->data)+Memory::OutputOffset);
        words[0]=7u<<20|1u<<27;words[1]=2u<<16;words[3]=address|2u<<27;
        auto *ep=words+stride/4;ep[0]=1;
        const auto dequeue=memory.ep0Ring().startAddress()|1;
        ep[2]=uint32_t(dequeue);ep[3]=uint32_t(dequeue>>32);
    }
    XHCI::CommandRecord addressed() {
        XHCI::CommandRecord record;record.ticket=9;record.command.type=XHCI::AddressDevice;
        record.owner=0x7000;record.result=Result::Success;
        record.command.parameter=memory.inputContext().address();record.command.slot=3;
        record.slot=3;record.completionCode=1;record.phase=XHCI::CommandPhase::Completed;
        return record;
    }
    XHCI::CommandRecord disabled() {
        auto record=addressed();record.command.type=XHCI::DisableSlot;record.command.parameter=0;return record;
    }
    void releaseAfterIsolation() { memory.retireAfterIsolation();assert(memory.release()==Result::Success); }
};
void actualHardwareAddress() {
    for (unsigned stride:{32u,64u}) for (USB::Speed speed:{USB::Speed::Low,USB::Speed::Full,USB::Speed::High,USB::Speed::Super}) {
        Machine m(stride,speed);
        assert(m.memory.ep0Configuration().maxPacket==(speed==USB::Speed::Super ? 512u : speed==USB::Speed::High ? 64u : 8u));
        assert(m.memory.outputAddress()==m.api.bus+Memory::OutputOffset);
        assert(m.memory.prepare(stride,36,32,2)==Result::Busy);
        assert(m.memory.publish(19)==Result::Success);
        assert(m.memory.configure(m.configuration,m.protocol)==Result::NotReady);
        assert(m.memory.release()==Result::Busy && !m.api.releases);
        m.output(73);auto command=m.addressed();XHCI::SlotSnapshot snapshot{};
        assert(m.memory.addressed(m.order,command,3,snapshot)==Result::Success);
        assert(snapshot.address==73 && snapshot.address!=command.slot && snapshot.state==2 && m.order.reads);
        // Command completion has not retired the device DMA allocation.
        assert(m.memory.currentState()==DMAState::Owned && m.memory.release()==Result::Busy);
        auto disable=m.disabled();disable.completionCode=19;
        assert(m.memory.disabled(disable,3,19)==Result::Invalid && m.memory.currentState()==DMAState::Owned);
        disable=m.disabled();assert(m.memory.disabled(disable,3,18)==Result::NotReady);
        assert(m.memory.disabled(disable,3,19)==Result::Success);
        m.api.clearFailures=1;
        assert(m.memory.release()==Result::HardwareError && m.api.memories.size()==1);
        assert(m.memory.release()==Result::Success && m.api.memories.empty());
    }
}
void badOutputAndProofs() {
    for (unsigned bad=0;bad<15;++bad) {
        Machine m(64);assert(m.memory.publish(19)==Result::Success);m.output(73);
        auto command=m.addressed();auto *words=reinterpret_cast<uint32_t *>(
            static_cast<uint8_t *>(m.api.memories[0]->data)+Memory::OutputOffset);
        switch (bad) {
        case 0:words[3]=2u<<27;break; // address zero
        case 1:words[3]=128|2u<<27;break;
        case 2:words[3]=73|1u<<27;break; // Default instead of Addressed
        case 3:words[1]=1u<<16;break;
        case 4:words[0]^=1u<<20;break;
        case 5:words[0]+=1u<<27;break;
        case 6:words[16]=2;break; // EP0 Halted
        case 7:words[18]+=16;break;
        case 8:command.completionCode=19;break;
        case 9:command.command.option=true;break;
        case 10:command.command.parameter+=64;break;
        case 11:command.slot=2;break;
        case 12:command.ticket=0;break;
        case 13:command.owner=0;break;
        case 14:command.result=Result::HardwareError;break;
        }
        XHCI::SlotSnapshot snapshot{};
        assert(m.memory.addressed(m.order,command,3,snapshot)!=Result::Success);
        assert(m.memory.release()==Result::Busy);
        m.memory.quarantine();assert(m.memory.release()==Result::Busy);
        m.releaseAfterIsolation();
    }
    Machine m(32);assert(m.memory.publish(19)==Result::Success);
    assert(m.memory.inputContext().freeze(9)==Result::Success);
    assert(m.memory.disabled(m.disabled(),3,19)==Result::Invalid);
    m.memory.quarantine();m.releaseAfterIsolation();
    assert(!m.memory.inputContext().frozen());
}
void preparationFailures() {
    for (int failure=1;failure<=6;++failure) {
        MemoryAPI api;Order order;Memory memory(api,order);api.fail=failure;
        assert(memory.prepare(64,36,32,2)!=Result::Success);
        assert(memory.release()==Result::Success && api.memories.empty() && api.commands.empty());
    }
    MemoryAPI api;Order order;Memory memory(api,order);
    assert(memory.prepare(16,36,32,2)==Result::Invalid && !api.allocationCalls);
    assert(memory.prepare(64,41,32,2)==Result::Invalid && !api.allocationCalls);
    api.fail=5;api.clearFailures=1;
    assert(memory.prepare(64,36,32,2)==Result::HardwareError && api.memories.size()==1);
    api.fail=0;assert(memory.release()==Result::Success && api.memories.empty());
}
void controlMaxPacketEvaluation() {
    for (unsigned stride:{32u,64u}) for (unsigned packet:{16u,32u,64u}) {
        Machine m(stride,USB::Speed::Full);assert(m.memory.publish(19)==Result::Success);m.output(73);
        assert(m.memory.stageControlPacket(512,10)==Result::Invalid);
        assert(m.memory.stageControlPacket(packet,10)==Result::Success);
        auto record=m.addressed();record.command.type=XHCI::EvaluateContext;record.ticket=10;
        assert(m.memory.evaluatedControl(m.order,record,3)==Result::Invalid);
        record.ticket=11;
        assert(m.memory.evaluatedControl(m.order,record,3)==Result::HardwareError);
        auto *words=reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(m.api.memories[0]->data)+Memory::OutputOffset);
        words[stride/4+1]=packet<<16;
        // Evaluate Context changes max packet size without resetting a live
        // EP0 dequeue pointer back to the start of its ring.
        words[stride/4+2]+=16;
        assert(m.memory.evaluatedControl(m.order,record,3)==Result::Success);
        assert(m.memory.ep0Configuration().maxPacket==packet);
        assert(m.memory.evaluatedControl(m.order,record,3)==Result::Invalid);
        assert(m.memory.release()==Result::Busy);m.releaseAfterIsolation();
    }
}
}
void xhciSlotTests() { actualHardwareAddress();badOutputAndProofs();preparationFailures();controlMaxPacketEvaluation(); }
