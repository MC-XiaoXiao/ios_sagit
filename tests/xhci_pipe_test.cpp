// SPDX-License-Identifier: BSD-2-Clause
#include "XHCIPipes.h"
#include "usb_memory_model.h"

namespace {
struct Order {
    void dmaWriteBarrier() {}
    void dmaReadBarrier() {}
};
struct Endpoint {
    uint8_t bLength=7,bDescriptorType=5,bEndpointAddress=0x81,bmAttributes=2;
    uint16_t wMaxPacketSize=512;
    uint8_t bInterval=0;
};
struct Companion {
    uint8_t bLength=6,bDescriptorType=48,bMaxBurst=0,bmAttributes=0;
    uint16_t wBytesPerInterval=0;
};
struct Descriptors {
    unsigned bcdUSB=0x200;
    const Endpoint *descriptor;
    const Companion *ssCompanionDescriptor=nullptr;
    const void *sspCompanionDescriptor=nullptr;
};
using Memory=XHCI::PipeMemory<MemoryAPI,Order>;
struct Machine {
    MemoryAPI api;
    Order order;
    Memory memory{api,order};
    alignas(64) uint32_t inputWords[33*16]{},outputWords[32*16]{};
    XHCI::InputContext input;
    XHCI::DeviceContext output;
    XHCI::SlotConfiguration slot;
    XHCI::PortProtocol protocol;
    XHCI::EndpointConfiguration configuration;
    unsigned stride,id=3;
    explicit Machine(unsigned size=64) : stride(size) {
        slot.rootPort=2;slot.speedID=7;slot.speed=USB::Speed::High;
        protocol.major=2;protocol.speeds[7]=slot.speed;
        assert(input.bind(inputWords,sizeof(inputWords),0x200000,stride,36,32,2)==Result::Success);
        assert(output.bind(outputWords,sizeof(outputWords),stride)==Result::Success);
        assert(memory.prepare(36)==Result::Success);
        Endpoint endpoint;Descriptors descriptors{0x200,&endpoint};
        assert(XHCI::pipeConfiguration(descriptors,slot.speed,memory.ring().startAddress(),36,configuration)==Result::Success);
        assert(XHCI::pipeInput(input,slot,protocol,2,id,&configuration)==Result::Success);
        assert(memory.publish(19,3,id,input.address(),5)==Result::Success);
        hardware(1);
    }
    XHCI::CommandRecord completion(uint64_t ticket=6) {
        XHCI::CommandRecord record;record.ticket=ticket;record.owner=0x7000;
        record.command.type=XHCI::ConfigureEndpoint;record.command.slot=3;record.command.parameter=input.address();
        record.result=Result::Success;record.phase=XHCI::CommandPhase::Completed;record.slot=3;record.completionCode=1;
        return record;
    }
    void hardware(unsigned state) {
        auto *words=outputWords+id*(stride/4);words[0]=state;words[1]=(configuration.maxPacket&2047)<<16;
        const auto dequeue=configuration.dequeue|uint64_t(configuration.cycle);
        words[2]=uint32_t(dequeue);words[3]=uint32_t(dequeue>>32);
    }
    void isolate() { memory.retireAfterIsolation();assert(memory.release()==Result::Success); }
};
void descriptorsAndInputMasks() {
    Endpoint endpoint;Companion companion;Descriptors descriptors{0x200,&endpoint};
    XHCI::EndpointConfiguration configuration;
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::High,0x100000,36,configuration)==Result::Success);
    assert(configuration.address==0x81 && configuration.type==XHCI::EndpointType::Bulk && configuration.maxPacket==512);
    endpoint.bEndpointAddress=0x10;auto old=configuration;
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::High,0x100000,36,configuration)==Result::Invalid);
    assert(configuration.address==old.address && configuration.maxPacket==old.maxPacket);
    endpoint.bEndpointAddress=0x81;endpoint.wMaxPacketSize=64;
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::Full,0x100000,36,configuration)==Result::Success);
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::Low,0x100000,36,configuration)==Result::Invalid);
    endpoint.wMaxPacketSize=1024;descriptors.ssCompanionDescriptor=&companion;
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::Super,0x100000,36,configuration)==Result::Success);
    companion.bMaxBurst=16;
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::Super,0x100000,36,configuration)==Result::Invalid);
    companion.bMaxBurst=0;descriptors.sspCompanionDescriptor=&companion;
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::Super,0x100000,36,configuration)==Result::Unsupported);
    descriptors.sspCompanionDescriptor=nullptr;companion.bDescriptorType=5;
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::Super,0x100000,36,configuration)==Result::Invalid);
    descriptors.ssCompanionDescriptor=nullptr;endpoint.bEndpointAddress=0;endpoint.bmAttributes=0;endpoint.wMaxPacketSize=8;
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::Full,0x100000,36,configuration)==Result::Success);
    assert(XHCI::endpointID(configuration.address,configuration.type)==1);
    endpoint.bEndpointAddress=1;
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::Full,0x100000,36,configuration)==Result::Unsupported);
    endpoint.bEndpointAddress=0;endpoint.bLength=6;
    assert(XHCI::pipeConfiguration(descriptors,USB::Speed::Full,0x100000,36,configuration)==Result::Invalid);
    assert(XHCI::contextEntries(2)==1 && XHCI::contextEntries(2|(1u<<31)|(1u<<8))==31);
    Machine m;
    assert(m.input.dropMask()==0 && m.input.addMask()==9);
    assert(XHCI::pipeInput(m.input,m.slot,m.protocol,10,3,&m.configuration)==Result::Invalid);
    assert(XHCI::pipeInput(m.input,m.slot,m.protocol,2,3,&m.configuration,true)==Result::Invalid);
    assert(XHCI::pipeInput(m.input,m.slot,m.protocol,2,3,nullptr)==Result::Invalid);
    m.isolate();
}
void endpointLifetime() {
    for (unsigned stride:{32u,64u}) {
        Machine m(stride);auto record=m.completion();
        assert(m.memory.release()==Result::Busy);
        assert(m.input.freeze(record.ticket)==Result::Success);
        assert(m.memory.configured(m.order,record,m.input,m.output,m.configuration)==Result::Invalid);
        assert(m.input.retire(record.ticket)==Result::Success);
        assert(m.memory.configured(m.order,record,m.input,m.output,m.configuration)==Result::Success);
        assert(m.memory.configured(m.order,record,m.input,m.output,m.configuration)==Result::Invalid);
        assert(XHCI::pipeInput(m.input,m.slot,m.protocol,10,3,&m.configuration,true)==Result::Success);
        record=m.completion(7);
        assert(m.input.dropMask()==8 && m.input.addMask()==9);
        assert(m.memory.configured(m.order,record,m.input,m.output,m.configuration,true)==Result::Success);
        assert(XHCI::pipeInput(m.input,m.slot,m.protocol,10,3,nullptr)==Result::Success);
        record=m.completion(8);
        assert(m.memory.removed(m.order,record,m.input,m.output,19)==Result::HardwareError);
        m.hardware(0);assert(m.memory.removed(m.order,record,m.input,m.output,18)==Result::NotReady);
        assert(m.memory.currentState()==DMAState::Owned);
        assert(m.memory.removed(m.order,record,m.input,m.output,19)==Result::Success);
        m.api.clearFailures=1;assert(m.memory.release()==Result::HardwareError);
        assert(!m.api.memories.empty() && m.memory.release()==Result::Success);
    }
}
void malformedProofs() {
    for (unsigned bad=0;bad<15;++bad) {
        Machine m;auto record=m.completion();
        assert(m.memory.configured(m.order,record,m.input,m.output,m.configuration)==Result::Success);
        assert(XHCI::pipeInput(m.input,m.slot,m.protocol,10,3,nullptr)==Result::Success);
        m.hardware(0);record=m.completion(7);
        switch (bad) {
        case 0:record.ticket=6;break;
        case 1:record.owner=0;break;
        case 2:record.result=Result::HardwareError;break;
        case 3:record.phase=XHCI::CommandPhase::Quarantined;break;
        case 4:record.completionCode=19;break;
        case 5:record.command.type=XHCI::EvaluateContext;break;
        case 6:record.command.slot=2;break;
        case 7:record.slot=2;break;
        case 8:record.command.option=true;break;
        case 9:record.command.endpoint=3;break;
        case 10:record.command.parameter+=64;break;
        case 11:m.inputWords[0]=1u<<4;break;
        case 12:m.inputWords[1]=0;break;
        case 13:assert(m.input.freeze(7)==Result::Success);break;
        case 14:m.outputWords[3*16]=5;break;
        }
        assert(m.memory.removed(m.order,record,m.input,m.output,19)!=Result::Success);
        assert(m.memory.currentState()==DMAState::Owned && m.memory.release()==Result::Busy);
        m.memory.quarantine();assert(m.memory.release()==Result::Busy);m.isolate();
    }
    Machine m;auto record=m.completion(6);
    record.command.type=XHCI::DisableSlot;record.command.parameter=0;
    assert(m.memory.disabledSlot(record,18)==Result::NotReady);
    assert(m.memory.disabledSlot(record,19)==Result::Success && m.memory.release()==Result::Success);
}
void allocationFailures() {
    for (unsigned fail=1;fail<=6;++fail) {
        MemoryAPI api;Order order;Memory memory(api,order);api.fail=fail;
        assert(memory.prepare(36)!=Result::Success);
        assert(memory.release()==Result::Success && api.memories.empty());
    }
    MemoryAPI api;Order order;Memory memory(api,order);
    assert(memory.prepare(41)==Result::Invalid && !api.allocationCalls);
    assert(memory.prepare(32)==Result::Success);
    assert(memory.publish(19,3,1,0x200000,5)==Result::Invalid);
    assert(memory.publish(19,3,3,0x100000000,5)==Result::Invalid);
    assert(memory.publish(19,3,3,0,5)==Result::Invalid);
    assert(memory.release()==Result::Success);
}
void adjustmentKeepsCurrentDequeue() {
    Machine m;auto record=m.completion();
    assert(m.memory.configured(m.order,record,m.input,m.output,m.configuration)==Result::Success);
    auto &ring=m.memory.ring();
    const XHCI::TRB item=XHCI::trb(0,0,XHCI::Normal<<10);
    uint64_t last=0;
    for (unsigned n=0;n<Memory::Entries;++n) {
        assert(ring.submit(&item,1,last)==Result::Success);
        assert(ring.retire(1,last));
    }
    assert(ring.nextAddress()!=ring.startAddress() && !ring.producerCycle());
    assert(XHCI::pipeInput(m.input,m.slot,m.protocol,10,3,&m.configuration,true)==Result::Success);
    record=m.completion(7);
    assert(m.memory.configured(m.order,record,m.input,m.output,m.configuration,true)==Result::Invalid);
    m.configuration.dequeue=ring.nextAddress();m.configuration.cycle=false;
    assert(XHCI::pipeInput(m.input,m.slot,m.protocol,10,3,&m.configuration,true)==Result::Success);
    m.hardware(1);
    assert(m.memory.configured(m.order,record,m.input,m.output,m.configuration,true)==Result::Success);
    m.isolate();
}
}
void xhciPipeTests() { descriptorsAndInputMasks();endpointLifetime();malformedProofs();allocationFailures();adjustmentKeepsCurrentDequeue(); }
