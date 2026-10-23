// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCIContexts.h"
#include "XHCIHubs.h"
#include "USBDMA.h"

namespace XHCI {
// One HCD-owned allocation per device, held until Disable Slot completes or
// the parent proves whole-controller isolation. Completing Address Device
// permits editing its input context; it does not relinquish the output context
// or EP0 ring. The parent owns this object before publishing any DCBAA entry.
template<class API,class Order> class SlotMemory {
    USB::DMARegion<API> allocation;
    InputContext input;
    DeviceContext output;
    ProducerRing<Order> controlRing;
    SlotConfiguration configuration{};
    EndpointConfiguration control{};
    unsigned maxSlots=0,addressBits=0;
    unsigned stagedPacket=0;
    uint64_t controlCommandFloor=0;
    SlotConfiguration stagedHub{};
    uint64_t hubCommandFloor=0;
    const void *hubQueue=nullptr;
    uint64_t hubEpoch=0;
    bool hubStaged=false;
    bool ready=false;
public:
    static constexpr unsigned Bytes=16384,InputOffset=0,OutputOffset=4096,ControlOffset=8192,ControlEntries=256;
    explicit SlotMemory(API &api,Order &order) : allocation(api),controlRing(order) {}
    SlotMemory(const SlotMemory &)=delete;
    SlotMemory &operator=(const SlotMemory &)=delete;
    USB::DMAState currentState() const { return allocation.currentState(); }
    uint64_t outputAddress() const { return ready ? allocation.busAddress()+OutputOffset : 0; }
    InputContext &inputContext() { return input; }
    const DeviceContext &deviceContext() const { return output; }
    auto &ep0Ring() { return controlRing; }
    const EndpointConfiguration &ep0Configuration() const { return control; }
    EndpointConfiguration currentControlConfiguration() const {
        auto value=control;value.dequeue=controlRing.nextAddress();value.cycle=bool(controlRing.producerCycle());return value;
    }
    USB::Result prepare(unsigned stride,unsigned bits,unsigned slots,unsigned ports) {
        if (ready || allocation.currentState()!=USB::DMAState::Empty) return USB::Result::Busy;
        if ((stride!=32 && stride!=64) || bits<32 || bits>40 || !slots || slots>255 || !ports || ports>127)
            return USB::Result::Invalid;
        auto r=allocation.allocate(Bytes,65536);
        if (r!=USB::Result::Success) return r;
        auto *cpu=static_cast<uint8_t *>(allocation.bytes());const auto address=allocation.busAddress();
        r=input.bind(cpu+InputOffset,33*stride,address+InputOffset,stride,bits,slots,ports);
        if (r==USB::Result::Success) r=output.bind(cpu+OutputOffset,32*stride,stride);
        if (r==USB::Result::Success && !controlRing.initialize(reinterpret_cast<TRB *>(cpu+ControlOffset),
            address+ControlOffset,ControlEntries,bits)) r=USB::Result::Invalid;
        if (r==USB::Result::Success) { ready=true;maxSlots=slots;addressBits=bits; }
        return r;
    }
    USB::Result configure(const SlotConfiguration &slot,const PortProtocol &protocol) {
        if (!ready || allocation.currentState()!=USB::DMAState::Ready) return USB::Result::NotReady;
        auto r=input.clear();
        if (r!=USB::Result::Success) return r;
        r=input.slot(slot,protocol);
        EndpointConfiguration endpoint;
        endpoint.speed=slot.speed;endpoint.type=EndpointType::Control;endpoint.dequeue=controlRing.startAddress();
        endpoint.maxPacket=slot.speed==USB::Speed::Super ? 512 : slot.speed==USB::Speed::High ? 64 : 8;
        if (r==USB::Result::Success) r=input.endpoint(endpoint);
        if (r==USB::Result::Success) { configuration=slot;control=endpoint; }
        return r;
    }
    USB::Result publish(uint64_t epoch) {
        if (!ready || !control.maxPacket) return USB::Result::NotReady;
        return allocation.publish(epoch);
    }
    USB::Result stageControlPacket(unsigned packet,uint64_t floor) {
        if (!ready || allocation.currentState()!=USB::DMAState::Owned) return USB::Result::NotReady;
        auto candidate=control;candidate.maxPacket=packet;
        uint32_t encoded[8]{};
        auto r=encodeEndpoint(candidate,addressBits,encoded);
        if (r!=USB::Result::Success) return r;
        r=input.clear();
        if (r==USB::Result::Success) r=input.endpoint(candidate);
        if (r==USB::Result::Success) { stagedPacket=packet;controlCommandFloor=floor; }
        return r;
    }
    template<class Barriers> USB::Result evaluatedControl(Barriers &order,const CommandRecord &command,unsigned slot) {
        if (!stagedPacket || allocation.currentState()!=USB::DMAState::Owned || !slot || slot>maxSlots ||
            command.ticket<=controlCommandFloor || !command.owner || command.result!=USB::Result::Success ||
            command.phase!=CommandPhase::Completed || command.completionCode!=1 || command.command.type!=EvaluateContext ||
            command.command.slot!=slot || command.slot!=slot || command.command.option || command.command.endpoint ||
            command.command.parameter!=input.address() || input.frozen() || input.dropMask() || input.addMask()!=2)
            return USB::Result::Invalid;
        EndpointSnapshot endpoint{};
        const auto r=output.endpoint(order,1,endpoint);
        if (r!=USB::Result::Success) return r;
        if (endpoint.state!=1 || endpoint.maxPacket!=stagedPacket) return USB::Result::HardwareError;
        control.maxPacket=stagedPacket;stagedPacket=0;return USB::Result::Success;
    }
    template<class Barriers> USB::Result addressed(Barriers &order,const CommandRecord &command,
        unsigned slot,SlotSnapshot &snapshot) const {
        if (allocation.currentState()!=USB::DMAState::Owned || !slot || slot>maxSlots || !command.ticket || !command.owner ||
            command.result!=USB::Result::Success || command.phase!=CommandPhase::Completed ||
            command.completionCode!=1 || command.command.type!=AddressDevice || command.command.slot!=slot || command.slot!=slot ||
            command.command.option || command.command.parameter!=input.address()) return USB::Result::Invalid;
        auto r=output.slot(order,snapshot);
        if (r!=USB::Result::Success) return r;
        if (!snapshot.address || snapshot.state!=2 || !sameSlotConfiguration(snapshot,configuration))
            return USB::Result::HardwareError;
        unsigned state=0;uint64_t dequeue=0;
        r=output.endpoint(order,1,state,dequeue);
        if (r!=USB::Result::Success) return r;
        return state==1 && dequeue==(controlRing.startAddress()|1) ? USB::Result::Success : USB::Result::HardwareError;
    }
    template<class Queue> USB::Result stageHub(const SlotConfiguration &candidate,const PortProtocol &protocol,Queue &queue) {
        if (!ready || allocation.currentState()!=USB::DMAState::Owned) return USB::Result::NotReady;
        if (hubStaged || input.frozen()) return USB::Result::Busy;
        if (!queue.generation() || !queue.accepting()) return USB::Result::NotReady;
        if (!candidate.hub || !candidate.ports || candidate.route!=configuration.route ||
            candidate.rootPort!=configuration.rootPort || candidate.speedID!=configuration.speedID ||
            candidate.speed!=configuration.speed || candidate.ttHub!=configuration.ttHub || candidate.ttPort!=configuration.ttPort ||
            (configuration.hub && !sameHubProfile(configuration,candidate))) return USB::Result::Invalid;
        auto result=input.clear();
        if (result==USB::Result::Success) result=input.slot(candidate,protocol);
        if (result==USB::Result::Success) {
            stagedHub=candidate;hubCommandFloor=queue.lastTicket();hubQueue=&queue;hubEpoch=queue.generation();hubStaged=true;
        }
        return result;
    }
    template<class Barriers,class Queue> USB::Result hubConfigured(Barriers &order,Queue &queue,uint64_t ticket,
        uintptr_t owner,unsigned slot,unsigned address,SlotSnapshot &snapshot) {
        CommandRecord command{};
        if (!hubStaged || hubQueue!=&queue || hubEpoch!=queue.generation() || !queue.accepting() ||
            allocation.currentState()!=USB::DMAState::Owned || !owner || !slot || slot>maxSlots || !address || address>127 ||
            ticket<=hubCommandFloor || !queue.copy(ticket,command) || command.owner!=owner ||
            command.phase!=CommandPhase::Completed || command.result!=USB::Result::Success || command.completionCode!=1 ||
            command.command.type!=ConfigureEndpoint || command.command.slot!=slot || command.slot!=slot ||
            command.command.parameter!=input.address() || command.command.option || command.command.endpoint ||
            input.frozen() || input.dropMask() || input.addMask()!=1) return USB::Result::Invalid;
        const auto result=output.slot(order,snapshot);
        if (result!=USB::Result::Success) return result;
        if (snapshot.address!=address || snapshot.state<2 || !sameSlotConfiguration(snapshot,stagedHub))
            return USB::Result::HardwareError;
        configuration=stagedHub;hubStaged=false;return USB::Result::Success;
    }
    USB::Result disabled(const CommandRecord &command,unsigned slot,uint64_t epoch) {
        if (!slot || slot>maxSlots || !command.ticket || !command.owner || command.result!=USB::Result::Success ||
            command.phase!=CommandPhase::Completed || command.completionCode!=1 || command.command.type!=DisableSlot ||
            command.command.slot!=slot || command.slot!=slot || command.command.parameter || command.command.endpoint ||
            command.command.option || input.frozen())
            return USB::Result::Invalid;
        // Parent clears DCBAA[slot] after this matching completion, before
        // allowing unmap/free. A timeout or an output state byte is no proof.
        return allocation.retire(epoch);
    }
    void quarantine() { allocation.quarantine(); }
    void retireAfterIsolation() {
        allocation.retireAfterIsolation();
        if (input.frozen()) input.retire(input.commandTicket());
    }
    USB::Result release() { return allocation.release(); }
};
}
