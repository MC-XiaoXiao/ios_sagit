// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCIContexts.h"
#include "USBDMA.h"

namespace XHCI {
// Descriptors has the Native StandardUSBDescriptors shape. Keep this formatter
// independent of IOKit objects so the same wire validation runs in host tests.
template<class Descriptors> USB::Result pipeConfiguration(const Descriptors &descriptors,USB::Speed speed,
    uint64_t dequeue,unsigned bits,EndpointConfiguration &out) {
    const auto *d=descriptors.descriptor;const auto *ss=descriptors.ssCompanionDescriptor;
    if (!d || d->bLength<7 || d->bDescriptorType!=5 || (d->bEndpointAddress&~0x8fu)) return USB::Result::Invalid;
    if (descriptors.sspCompanionDescriptor) return USB::Result::Unsupported;
    EndpointConfiguration value;value.address=d->bEndpointAddress;value.maxPacket=d->wMaxPacketSize;
    value.interval=d->bInterval;value.type=EndpointType(d->bmAttributes&3);value.speed=speed;value.dequeue=dequeue;
    if (value.type==EndpointType::Control) {
        if (value.address&15) return USB::Result::Unsupported;
        value.address=0; // Control endpoint zero is bidirectional.
    }
    if (ss) {
        if (ss->bLength<6 || ss->bDescriptorType!=48) return USB::Result::Invalid;
        value.companion=true;value.burst=ss->bMaxBurst;value.attributes=ss->bmAttributes;
        value.bytesPerInterval=ss->wBytesPerInterval;
    }
    uint32_t encoded[8]{};
    const auto r=encodeEndpoint(value,bits,encoded);
    if (r==USB::Result::Success) out=value;
    return r;
}
inline unsigned contextEntries(uint32_t endpoints) {
    unsigned entries=1;
    for (unsigned id=2;id<=31;++id) if (endpoints&(1u<<id)) entries=id;
    return entries;
}
inline USB::Result pipeInput(InputContext &input,SlotConfiguration slot,const PortProtocol &protocol,
    uint32_t endpoints,unsigned id,const EndpointConfiguration *configuration,bool replace=false) {
    if (id<2 || id>31 || !(endpoints&2) || (endpoints&1) ||
        (configuration && endpointID(configuration->address,configuration->type)!=id) ||
        (configuration && !replace && (endpoints&(1u<<id))) ||
        (replace && (!configuration || !(endpoints&(1u<<id)))) ||
        (!configuration && !(endpoints&(1u<<id)))) return USB::Result::Invalid;
    const auto next=configuration ? endpoints|(1u<<id) : endpoints&~(1u<<id);
    slot.entries=contextEntries(next);
    auto r=input.clear();
    if (r==USB::Result::Success) r=input.slot(slot,protocol);
    if (r==USB::Result::Success) r=configuration ? input.endpoint(*configuration,replace) : input.drop(id);
    return r;
}
// Endpoint zero belongs to SlotMemory. Every other endpoint owns a ring until
// a fresh Configure Endpoint drop completion, Disable Slot, or isolation. A
// failed command and an output Disabled byte alone never retire its memory.
template<class API,class Order> class PipeMemory {
    USB::DMARegion<API> allocation;
    ProducerRing<Order> producer;
    unsigned slot=0,id=0,addressBits=0;
    uint64_t inputAddress=0,commandFloor=0,lastCommand=0;
    bool ready=false;
    bool proof(const CommandRecord &command,Type type) const {
        return command.ticket>commandFloor && command.ticket>lastCommand && command.owner &&
            command.result==USB::Result::Success && command.phase==CommandPhase::Completed && command.completionCode==1 &&
            command.command.type==type && command.command.slot==slot && command.slot==slot &&
            !command.command.option && !command.command.endpoint &&
            command.command.parameter==(type==DisableSlot ? 0 : inputAddress);
    }
public:
    static constexpr unsigned Entries=256,Bytes=Entries*sizeof(TRB);
    explicit PipeMemory(API &api,Order &order) : allocation(api),producer(order) {}
    PipeMemory(const PipeMemory &)=delete;
    PipeMemory &operator=(const PipeMemory &)=delete;
    auto &ring() { return producer; }
    USB::DMAState currentState() const { return allocation.currentState(); }
    USB::Result prepare(unsigned bits) {
        if (ready || allocation.currentState()!=USB::DMAState::Empty) return USB::Result::Busy;
        if (bits<32 || bits>40) return USB::Result::Invalid;
        auto r=allocation.allocate(Bytes,65536);
        if (r!=USB::Result::Success) return r;
        if (!producer.initialize(static_cast<TRB *>(allocation.bytes()),allocation.busAddress(),Entries,bits))
            return USB::Result::Invalid;
        ready=true;addressBits=bits;return USB::Result::Success;
    }
    USB::Result publish(uint64_t epoch,unsigned slotID,unsigned endpoint,uint64_t input,uint64_t floor) {
        if (!ready || !slotID || slotID>255 || endpoint<2 || endpoint>31 || !input || !USB::dmaRange(input,64,addressBits,64))
            return USB::Result::Invalid;
        const auto r=allocation.publish(epoch);
        if (r==USB::Result::Success) { slot=slotID;id=endpoint;inputAddress=input;commandFloor=floor; }
        return r;
    }
    template<class Barriers> USB::Result configured(Barriers &order,const CommandRecord &command,
        const InputContext &input,const DeviceContext &output,const EndpointConfiguration &configuration,bool replace=false) {
        if (allocation.currentState()!=USB::DMAState::Owned || !proof(command,ConfigureEndpoint) || input.frozen() ||
            input.address()!=inputAddress || input.dropMask()!=(replace ? 1u<<id : 0) || input.addMask()!=(1u|(1u<<id)) ||
            endpointID(configuration.address,configuration.type)!=id || producer.pending() ||
            configuration.dequeue!=producer.nextAddress() || configuration.cycle!=bool(producer.producerCycle()))
            return USB::Result::Invalid;
        EndpointSnapshot endpoint{};const auto r=output.endpoint(order,id,endpoint);
        if (r!=USB::Result::Success) return r;
        if (endpoint.state!=1 || endpoint.maxPacket!=(configuration.maxPacket&2047) ||
            endpoint.dequeue!=(configuration.dequeue|uint64_t(configuration.cycle))) return USB::Result::HardwareError;
        lastCommand=command.ticket;return USB::Result::Success;
    }
    template<class Barriers> USB::Result removed(Barriers &order,const CommandRecord &command,
        const InputContext &input,const DeviceContext &output,uint64_t epoch) {
        if (allocation.currentState()!=USB::DMAState::Owned || !proof(command,ConfigureEndpoint) || input.frozen() ||
            input.address()!=inputAddress || input.dropMask()!=(1u<<id) || input.addMask()!=1) return USB::Result::Invalid;
        EndpointSnapshot endpoint{};const auto r=output.endpoint(order,id,endpoint);
        if (r!=USB::Result::Success) return r;
        if (endpoint.state) return USB::Result::HardwareError;
        return allocation.retire(epoch);
    }
    USB::Result disabledSlot(const CommandRecord &command,uint64_t epoch) {
        if (!proof(command,DisableSlot)) return USB::Result::Invalid;
        return allocation.retire(epoch);
    }
    void quarantine() { allocation.quarantine(); }
    void retireAfterIsolation() { allocation.retireAfterIsolation(); }
    USB::Result release() { return allocation.release(); }
};
}
