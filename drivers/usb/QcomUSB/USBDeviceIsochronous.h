// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBDeviceRequests.h"
#include "DWC3Isochronous.h"

namespace USB {
// USB3 BOS device capability. These are the DWC3 default exit bounds used by
// the reference IP driver (10us U1, 511us U2), with LTM and SSP absent. The
// Native Family owns the BOS header, response truncation and capability list.
constexpr uint8_t DeviceSuperSpeedCapability[]={10,16,3,0,14,0,1,10,255,1};
inline Result deviceConnectionSpeed(Speed maximum,uint32_t status,Speed &out) {
    if (maximum!=Speed::High && maximum!=Speed::Super) return Result::Invalid;
    if (status==UINT32_MAX) return Result::HardwareError;
    Speed speed=Speed::Unknown;
    switch (status&7) {
    case 0:speed=Speed::High;break;
    case 1:case 3:speed=Speed::Full;break;
    case 4:if (maximum!=Speed::Super) return Result::Unsupported;speed=Speed::Super;break;
    default:return Result::Unsupported; // no LS device or DWC_usb31/32 mode
    }
    out=speed;return Result::Success;
}
// The pinned Family builds SS companions itself: bulk has bMaxBurst15,
// periodic endpoints have burst/Mult0 and wBytesPerInterval equal to MPS.
// Its ISO table still permits the USB2 high-bandwidth value3072 at SS; that
// would produce reserved wMaxPacketSize bits and an inconsistent companion.
// Constrain creation before the Family allocates or publishes a pipe. The
// caller passes the advertised controller capability, not the current link.
inline Result devicePipeParameters(Speed maximum,int type,int direction,int &packet,
    int interval,unsigned synchronization) {
    if ((maximum!=Speed::High && maximum!=Speed::Super) || type<0 || type>3 ||
        direction<0 || direction>1 || synchronization>3 || (type!=1 && synchronization) ||
        packet==0 || packet< -1 || interval<0 || ((type==1 || type==3) && interval>16) ||
        (type==1 && !interval)) return Result::Invalid;
    // Native getInterval(FS) is also written, truncated, to bInterval. Inputs
    // above8 produce an invalid FS ISO descriptor (>16 or wrapped to zero).
    // Keep the pipe out of the Family before either speed can publish it.
    if (type==1 && interval>8) return Result::Unsupported;
    const bool super=maximum==Speed::Super;
    if (packet== -1) {
        if (super && type==1) packet=1024;
        return Result::Success;
    }
    const unsigned limits[]={super ? 512u:64u,super ? 1024u:3072u,super ? 1024u:512u,1024u};
    if (unsigned(packet)>limits[type]) return super && type==1 ? Result::Unsupported:Result::Invalid;
    if ((type==0 || type==2) && unsigned(packet)!=limits[type]) return Result::Invalid;
    if (type==1 && !super) {
        const unsigned transactions=packet>2048 ? 3:packet>1024 ? 2:1;
        if (unsigned(packet)%transactions) return Result::Invalid;
    }
    return Result::Success;
}

// Native activateEndpoint's packet argument is a service-interval byte
// budget for HS ISO (up to3072), not encoded wMaxPacketSize. Its interval is
// also the exact bInterval byte emitted by IOUSBDevicePipe::copyDescriptor,
// including the Family's FS conversion. Program that wire exponent directly;
// interpreting the FS value as a duration would disagree with the descriptor.
inline Result deviceEndpointConfiguration(unsigned endpoint,int type,Speed speed,unsigned packet,
    unsigned interval,DWC3::EndpointConfiguration &configuration,unsigned pipeProperty=0) {
    if (type<0 || type>3) return Result::Invalid;
    // The pinned Family permits values1/2 only while the pipe is inactive;
    // this is a state check, not an SS-speed check. USB2 audio clients set
    // them too. The getter is forwarded only to the hardware activation
    // hook; it does not change the Family's wire descriptors. Accept the
    // same opaque values with ordinary descriptor-defined transactions.
    // No burst, stream or synchronization meaning is assigned to a hint.
    if (pipeProperty>2) return Result::Unsupported;
    DWC3::EndpointConfiguration value{endpoint,DWC3::EndpointType(type),speed,packet,interval,0};
    // Match the actual companion emitted by IOUSBDevicePipe. Streams remain
    // absent (bmAttributes0); the runtime pipe property is not a burst count.
    if (speed==Speed::Super && value.type==DWC3::EndpointType::Bulk) value.burst=15;
    if (value.type==DWC3::EndpointType::Isochronous) {
        if (speed==Speed::High) {
            const unsigned transactions=packet>2048 ? 3:packet>1024 ? 2:1;
            if (packet%transactions) return Result::Invalid;
            value.maximumPacketSize=packet/transactions;value.mult=transactions-1;
        }
    }
    if (!value.valid()) return Result::Invalid;
    configuration=value;return Result::Success;
}

// Aggregate lifetime above the per-frame DMA queue. The caller owns request
// and frame storage and retains the client buffer until take() detaches the
// entire request. DMA tickets contain only their independent memory retain;
// they never contain a native request or frame-list pointer.
template<class API,unsigned Slots=64,unsigned RequestSlots=64> class DeviceIsochronous {
public:
    static constexpr unsigned MaximumFrames=1024;
    struct Request;
    struct Frame {
        uint32_t requested=0,actual=0;
        uint64_t offset=0,cookie=0,timestampNS=0;
        Result result=Result::Pending;
        Request *owner=nullptr;
        bool published=false,retired=false,complete=false;
    };
    struct Request {
        Frame *frames=nullptr;
        typename API::Memory *buffer=nullptr;
        void *context=nullptr;
        uint64_t serial=0,scope=0,deadline=0;
        unsigned endpoint=0,count=0,prepare=0,publish=0,completed=0,packet=0;
        Result cancellation=Result::Pending;
        bool announced=false;
    };
private:
    API &api;
    DeviceRequests<API,Slots> &dma;
    Request *requests[RequestSlots]{};
    Frame *mapped[Slots]{};
    uint64_t serial=0;
    Request *find(uint64_t value) {
        for (auto *r:requests) if (r && r->serial==value) return r;
        return nullptr;
    }
    bool first(const Request &request) const {
        for (auto *r:requests) if (r && r->endpoint==request.endpoint && r->serial<request.serial) return false;
        return true;
    }
    static void finish(Frame &frame,Result result,uint32_t bytes=0) {
        if (frame.complete) return;
        frame.result=result;frame.actual=bytes;frame.complete=true;++frame.owner->completed;
        if (result!=Result::Success && result!=Result::Overflow) frame.timestampNS=0;
    }
public:
    DeviceIsochronous(API &memory,DeviceRequests<API,Slots> &queue):api(memory),dma(queue) {}
    DeviceIsochronous(const DeviceIsochronous &)=delete;
    DeviceIsochronous &operator=(const DeviceIsochronous &)=delete;
    Result admit(Request &request,Frame *frames,unsigned count,typename API::Memory *buffer,
        unsigned endpoint,unsigned packet,unsigned budget,uint64_t scope,uint64_t deadline,void *context) {
        if (request.serial || !frames || !context || !count || count>MaximumFrames || !buffer ||
            endpoint<2 || endpoint>=32 || !packet || packet>1024 || budget<packet || !scope ||
            serial>=(UINT64_MAX-32)/MaximumFrames) return Result::Invalid;
        unsigned free=RequestSlots;
        for (unsigned n=0;n<RequestSlots;++n) if (!requests[n]) { free=n;break; }
        if (free==RequestSlots) return Result::Busy;
        uint64_t total=0;
        for (unsigned n=0;n<count;++n) {
            if (frames[n].requested>budget) return Result::Invalid;
            total+=frames[n].requested;
            if (total>DMARegion<API>::MaximumBytes) return Result::Invalid;
        }
        if (total>api.length(buffer)) return Result::Invalid;
        request={frames,buffer,context,++serial,scope,deadline,endpoint,count,0,0,0,packet};
        total=0;
        for (unsigned n=0;n<count;++n) {
            const auto length=frames[n].requested;
            frames[n]={length,0,total,0,0,Result::Pending,&request};total+=length;
        }
        requests[free]=&request;return Result::Success;
    }
    // Prefetch is bounded by the shared frame pool, including across native
    // requests. Each frame has its own mapping lane, while publish() enforces
    // endpoint FIFO order before any HWO/START/UPDATE reaches hardware.
    Result prefetch() {
        for (;;) {
            Request *chosen=nullptr;
            for (auto *r:requests) if (r && r->cancellation==Result::Pending && r->prepare<r->count &&
                (!chosen || r->serial<chosen->serial)) chosen=r;
            if (!chosen) return Result::Success;
            auto &frame=chosen->frames[chosen->prepare];uint64_t cookie=0;
            auto result=dma.admit(chosen->endpoint,chosen->buffer,frame.requested,chosen->packet,
                chosen->scope,0,&frame,cookie,frame.offset,
                32+(chosen->serial-1)*MaximumFrames+chosen->prepare+1);
            if (result==Result::Busy) return Result::Success;
            if (result!=Result::Success) return result;
            auto &entry=mapped[(cookie-1)%Slots];
            if (entry) return Result::HardwareError;
            frame.cookie=cookie;entry=&frame;++chosen->prepare;
        }
    }
    template<class Ring> Result publish(unsigned ep,Ring &ring,uint64_t epoch) {
        while (ring.available()) {
            Request *chosen=nullptr;
            for (auto *r:requests) if (r && r->endpoint==ep && r->publish<r->count &&
                (!chosen || r->serial<chosen->serial)) chosen=r;
            if (!chosen || chosen->cancellation!=Result::Pending) return Result::Success;
            auto &frame=chosen->frames[chosen->publish];
            if (frame.complete) { ++chosen->publish;continue; }
            typename DeviceRequests<API,Slots>::View view;
            if (!frame.cookie || !dma.preparedRequest(frame.cookie,view)) return Result::Success;
            if (view.scope!=chosen->scope) return Result::HardwareError;
            auto result=dma.publish(frame.cookie,epoch);
            if (result!=Result::Success) return result;
            frame.published=true;++chosen->publish;
            result=ring.push(view.bus,view.length,view.capacity,frame.cookie);
            if (result!=Result::Success) return result; // active DMA remains pinned until stop/halt
        }
        return Result::Success;
    }
    template<class Ring> Result collect(unsigned ep,Ring &ring,uint64_t epoch,
        uint64_t stampNS,uint32_t currentFrame,unsigned tickNS,bool clockValid,bool isolating=false) {
        for (auto *frame:mapped) if (frame && frame->owner->endpoint==ep && frame->published && !frame->retired) {
            DWC3::IsochronousCompletion completion;
            auto result=ring.takeCompletion(frame->cookie,completion);
            if (result==Result::Pending || (isolating && result==Result::Invalid)) continue;
            if (result!=Result::Success || completion.epoch!=epoch) return Result::HardwareError;
            // DSTS and completed TRBs share the low 14-bit SOF counter. Do
            // not guess a timestamp beyond half a counter wrap. This sample
            // is captured before the potentially slow OUT copy/unmap worker.
            const auto lag=(currentFrame-(completion.frame&0x3fff))&0x3fff;
            const auto elapsed=uint64_t(lag)*tickNS;
            if (clockValid && lag<0x2000 && elapsed<=stampNS &&
                (completion.result==Result::Success || completion.result==Result::Overflow))
                frame->timestampNS=stampNS-elapsed;
            result=dma.completed(frame->cookie,epoch,completion.result,completion.bytes,true);
            if (result!=Result::Success) return result;
            frame->retired=true;
        }
        return Result::Success;
    }
    Result reap() {
        for (auto &frame:mapped) if (frame) {
            typename DeviceRequests<API,Slots>::Completion completion;
            auto result=dma.takeCompletion(frame->cookie,completion);
            if (result==Result::Pending) continue;
            if (result!=Result::Success || completion.context!=frame) return Result::HardwareError;
            finish(*frame,completion.result,completion.bytes);frame=nullptr;
        }
        return Result::Success;
    }
    void cancel(unsigned ep,Result reason=Result::Aborted) {
        for (auto *r:requests) if (r && r->endpoint==ep && r->cancellation==Result::Pending) {
            bool affected=false;
            for (unsigned n=0;n<r->count;++n) {
                auto &frame=r->frames[n];
                if (frame.complete) continue;
                if (frame.cookie) affected=dma.cancel(frame.cookie,reason) || affected;
                else { finish(frame,reason);affected=true; }
            }
            if (affected) r->cancellation=reason;
            r->prepare=r->count;
        }
    }
    void expire(uint64_t nowUS) {
        for (auto *r:requests) if (r && r->cancellation==Result::Pending && r->deadline && nowUS>=r->deadline) {
            // A fully hardware-completed request may still be copying OUT
            // data. Its old deadline must not cancel a newer native request.
            bool pending=false;
            for (unsigned n=0;n<r->count && !pending;++n) {
                auto &frame=r->frames[n];
                pending=!frame.complete && (!frame.cookie || dma.cancellable(frame.cookie));
            }
            if (pending) cancel(r->endpoint,Result::Timeout);
        }
    }
    bool cancelled(unsigned ep,Result &reason) const {
        for (auto *r:requests) if (r && r->endpoint==ep && r->cancellation!=Result::Pending) {
            reason=r->cancellation;return true;
        }
        return false;
    }
    uint64_t notice() const {
        Request *chosen=nullptr;
        for (auto *r:requests) if (r && !r->announced && r->completed==r->count && first(*r) &&
            (!chosen || r->serial<chosen->serial)) chosen=r;
        return chosen ? chosen->serial : 0;
    }
    void announce(uint64_t value) { if (auto *r=find(value)) r->announced=true; }
    uint64_t pending(unsigned ep) const {
        Request *chosen=nullptr;
        for (auto *r:requests) if (r && r->endpoint==ep && (!chosen || r->serial<chosen->serial)) chosen=r;
        return chosen ? chosen->serial : 0;
    }
    bool contains(uint64_t value) { return find(value); }
    Request *take(uint64_t value,Result &result) {
        auto *r=find(value);
        if (!r || r->completed!=r->count || !first(*r)) return nullptr;
        for (auto &entry:requests) if (entry==r) { entry=nullptr;break; }
        result=r->cancellation==Result::Pending ? Result::Success : r->cancellation;
        if (result==Result::Success) for (unsigned n=0;n<r->count;++n)
            if (r->frames[n].result!=Result::Success) { result=r->frames[n].result;break; }
        return r; // detached before the caller performs a reentrant native callback
    }
    bool empty() const {
        for (auto *r:requests) if (r) return false;
        return dma.empty();
    }
};
}
