// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCIContexts.h"
#include <IOKit/usb/StandardUSB.h>

namespace XHCI {
inline bool ttCleanupRequired(const SlotConfiguration &device,const EndpointConfiguration &endpoint) {
    return device.ttHub && device.ttPort &&
        (device.speed==USB::Speed::Low || device.speed==USB::Speed::Full) && endpoint.speed==device.speed &&
        (endpoint.type==EndpointType::Control || endpoint.type==EndpointType::Bulk);
}

// An endpoint can leave a TT buffer dirty without setting Native's error
// latch: cancellation, completion timeout and close all use this fence.
// Mark only after Stop/Reset has completed; wire I/O must never race a split
// transaction still executing in hardware. DMA retirement is a separate proof.
class TTBufferCleanup {
    enum class Phase { Clean,Dirty,Queued,Running } phase=Phase::Clean;
    uint64_t retryAt=0;
public:
    bool dirty() const { return phase!=Phase::Clean; }
    bool queued() const { return phase==Phase::Queued; }
    bool running() const { return phase==Phase::Running; }
    bool canRelease(bool requests,bool ownerAlive,bool isolated) const {
        return !requests && !queued() && !running() && (!dirty() || !ownerAlive || isolated);
    }
    void require() { if (phase==Phase::Clean) { phase=Phase::Dirty;retryAt=0; } }
    bool schedule(uint64_t now) {
        if (phase!=Phase::Dirty || now<retryAt) return false;
        phase=Phase::Queued;return true;
    }
    bool start() {
        if (!queued()) return false;
        phase=Phase::Running;return true;
    }
    bool finish(bool cleared,uint64_t now) {
        if (!running()) return false;
        phase=cleared ? Phase::Clean : Phase::Dirty;
        retryAt=cleared ? 0 : now>UINT64_MAX-500000 ? UINT64_MAX : now+500000;
        return true;
    }
};

struct TTClearPlan {
    StandardUSB::DeviceRequest requests[2]{};
    unsigned count=0;
};
inline USB::Result ttClearPlan(const SlotConfiguration &device,const EndpointConfiguration &endpoint,
    unsigned address,bool multiTT,TTClearPlan &out) {
    if (!device.ttHub || device.ttHub>255 || !device.ttPort || device.ttPort>15 || !address || address>127 ||
        (device.speed!=USB::Speed::Low && device.speed!=USB::Speed::Full) || endpoint.speed!=device.speed ||
        (endpoint.type!=EndpointType::Control && endpoint.type!=EndpointType::Bulk)) return USB::Result::Invalid;
    const bool control=endpoint.type==EndpointType::Control;
    if (!control && device.speed==USB::Speed::Low) return USB::Result::Invalid;
    if (control ? endpoint.address!=0 : !(endpoint.address&15) || (endpoint.address&~0x8fu)) return USB::Result::Invalid;
    TTClearPlan value;value.count=control ? 2 : 1;
    const unsigned target=(address<<4)|(endpoint.address&15)|(control ? 0 : 2u<<11);
    value.requests[0]={0x23,8,uint16_t(target|(!control && (endpoint.address&128) ? 1u<<15 : 0)),
        uint16_t(multiTT ? device.ttPort : 1),0};
    value.requests[1]=value.requests[0];value.requests[1].wValue^=0x8000;
    out=value;return USB::Result::Success;
}
inline USB::Result ttClearDefaultPlan(const SlotConfiguration &device,bool multiTT,TTClearPlan &out) {
    EndpointConfiguration control;control.type=EndpointType::Control;control.speed=device.speed;
    TTClearPlan value;
    // Address Device's SET_ADDRESS transaction was sent to USB address0.
    // Ordinary pipe recovery deliberately continues to reject address0.
    const auto result=ttClearPlan(device,control,1,multiTT,value);
    if (result!=USB::Result::Success) return result;
    for (unsigned i=0;i<value.count;++i) value.requests[i].wValue&=~uint16_t(127u<<4);
    out=value;return USB::Result::Success;
}

// Default-address recovery has no Native child/pipe. Capture its real queue
// before Address Device, then require the retained failed command from that
// exact queue. Keep the TT domain fenced across asynchronous wire retries.
class TTEnumerationRecovery {
    enum class Phase { Empty,Armed,Ready,Queued,Running,Failed,Cleared } phase=Phase::Empty;
    const void *queueIdentity=nullptr;
    uint64_t epoch=0,floor=0,input=0,retryAt=0;
    uintptr_t owner=0;
    unsigned slot=0,hub=0,index=0;
public:
    template<class Queue> USB::Result arm(Queue &queue,const SlotConfiguration &device,
        uint64_t generation,unsigned slotID,uint64_t inputAddress,uintptr_t commandOwner) {
        if (phase!=Phase::Empty) return USB::Result::Busy;
        TTClearPlan plan;
        if (!slotID || slotID>queue.maxSlots() || !generation || generation!=queue.generation() ||
            !commandOwner || !inputAddress || (inputAddress&63) ||
            ttClearDefaultPlan(device,device.multiTT,plan)!=USB::Result::Success) return USB::Result::Invalid;
        if (!queue.accepting()) return USB::Result::NotReady;
        queueIdentity=&queue;epoch=generation;floor=queue.lastTicket();input=inputAddress;
        owner=commandOwner;slot=slotID;hub=device.ttHub;index=plan.requests[0].wIndex;phase=Phase::Armed;
        return USB::Result::Success;
    }
    template<class Queue> bool failed(Queue &queue,uint64_t ticket) {
        CommandRecord record;
        if (phase!=Phase::Armed || queueIdentity!=&queue || queue.generation()!=epoch || !queue.accepting() ||
            ticket<=floor || !queue.copy(ticket,record) || record.owner!=owner ||
            record.phase!=CommandPhase::Completed || record.result!=USB::Result::HardwareError ||
            record.completionCode!=4 || record.command.type!=AddressDevice || record.command.slot!=slot ||
            record.slot!=slot || record.command.parameter!=input || record.command.option || record.command.endpoint)
            return false;
        phase=Phase::Ready;return true;
    }
    bool dirty() const {
        return phase==Phase::Ready || phase==Phase::Queued || phase==Phase::Running || phase==Phase::Failed;
    }
    bool blocks(const SlotConfiguration &candidate) const {
        return dirty() && candidate.ttHub==hub && (candidate.multiTT ? candidate.ttPort : 1)==index;
    }
    bool schedule(uint64_t now) {
        if ((phase!=Phase::Ready && phase!=Phase::Failed) || now<retryAt) return false;
        phase=Phase::Queued;return true;
    }
    bool queued() const { return phase==Phase::Queued; }
    bool start() {
        if (!queued()) return false;
        phase=Phase::Running;return true;
    }
    bool finish(bool cleared,uint64_t now) {
        if (phase!=Phase::Running) return false;
        phase=cleared ? Phase::Cleared : Phase::Failed;
        retryAt=now>UINT64_MAX-500000 ? UINT64_MAX : now+500000;return true;
    }
};
}
