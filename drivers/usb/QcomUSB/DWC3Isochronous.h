// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "DWC3Registers.h"
#include "DWC3Endpoint.h"

namespace DWC3 {
struct IsochronousCompletion {
    uint64_t cookie=0,epoch=0;
    USB::Result result=USB::Result::NotReady;
    uint32_t bytes=0,frame=0;
};

// XferNotReady supplies a 16-bit frame number, DSTS only its low 14 bits.
// A seed older than one low-counter wrap cannot prove its upper bits. The
// owner must then stop/reconfigure instead of guessing a scheduling epoch.
inline USB::Result isochronousStartFrame(const EndpointConfiguration &config,
    uint32_t eventFrame,uint32_t current,uint64_t sampledUS,uint64_t nowUS,uint32_t &target) {
    if (!config.valid() || config.type!=EndpointType::Isochronous || eventFrame>0xffff || current>0x3fff)
        return USB::Result::Invalid;
    const unsigned tickUS=config.speed==USB::Speed::Full ? 1000 : 125;
    if (nowUS<sampledUS || nowUS-sampledUS>=uint64_t(0x4000)*tickUS) return USB::Result::Timeout;
    uint32_t frame=eventFrame;
    if (config.speed!=USB::Speed::Full && config.interval<=14) {
        frame=(eventFrame&0xc000)|current;
        if (current<(eventFrame&0x3fff)) frame+=0x4000;
    }
    const auto interval=config.intervalTicks();
    const unsigned lead=config.speed==USB::Speed::Full ? 1 : 4; // at least 500 us
    const unsigned steps=(lead+interval-1)/interval;
    target=((frame+interval*steps)&~(interval-1))&0xffff;
    return USB::Result::Success;
}

// One contiguous, mapped bounce per service interval. The persistent TRB
// ring belongs to the controller and ends with an owned Link TRB. Frame data
// may retire only after its own HWO-clear completion, an acknowledged END,
// or whole-controller halt. Native requests and DMA unmapping live outside
// this class. Every method except completion reads runs under the owner gate.
template<class Controller,class Order> class IsochronousRing {
    enum class Phase { Uninitialized,Waiting,Active,Stopping,Fault } phase=Phase::Uninitialized;
    enum class Stop { Idle,Abort,Timeout,Retry } stop=Stop::Idle;
    enum class SlotPhase { Free,Queued,Complete };
    struct Slot {
        SlotPhase phase=SlotPhase::Free;
        uint64_t cookie=0,address=0;
        uint32_t requested=0,programmed=0;
        IsochronousCompletion completion{};
    } slots[63];
    Controller &controller;
    Order &order;
    volatile TRB *trbs=nullptr;
    EndpointConfiguration config{};
    uint64_t dma=0,epoch=0,readyAt=0;
    unsigned capacity=0,put=0,get=0,queued=0,resource=0,retries=0;
    uint32_t readyFrame=0;
    bool ready=false;
    USB::Result fail(USB::Result result) { phase=Phase::Fault;return result; }
    void descriptor(unsigned index) {
        const auto &slot=slots[index];
        trbs[index].addressLo=uint32_t(slot.address);trbs[index].addressHi=uint32_t(slot.address>>32);
        unsigned pcm=0;
        if (config.speed==USB::Speed::High && slot.programmed)
            pcm=(slot.programmed+config.maximumPacketSize-1)/config.maximumPacketSize-1;
        trbs[index].size=slot.programmed|(pcm<<24);
        trbs[index].control=(unsigned(TRBType::IsoFirst)<<4)|InterruptShort|InterruptComplete|
            ((config.physicalEndpoint&1) ? 0u : ContinueShort);
        order.dmaWriteBarrier();trbs[index].control|=Owned;
    }
    void link() {
        trbs[capacity].addressLo=uint32_t(dma);trbs[capacity].addressHi=uint32_t(dma>>32);
        trbs[capacity].size=0;trbs[capacity].control=unsigned(TRBType::Link)<<4;
        order.dmaWriteBarrier();trbs[capacity].control|=Owned;order.dmaWriteBarrier();
    }
    void finishAll(USB::Result reason) {
        for (unsigned n=0;n<capacity;++n) if (slots[n].phase==SlotPhase::Queued) {
            auto &slot=slots[n];
            // Caller has either never published a START, consumed END, or
            // proved a global halt. Software cannot clear live HWO here.
            trbs[n].control&=~Owned;
            slot.completion={slot.cookie,epoch,reason,0,0};slot.phase=SlotPhase::Complete;
        }
        queued=0;get=put;order.dmaWriteBarrier();
    }
    USB::Result beginStop(Stop why) {
        if (phase==Phase::Stopping) return USB::Result::Pending;
        if (phase!=Phase::Active || !resource) return USB::Result::NotReady;
        stop=why;phase=Phase::Stopping;ready=false;
        uint32_t reply=0;
        const auto result=controller.command(config.physicalEndpoint,Command::End,0,0,0,
            CommandForce|CommandInterrupt|(resource<<16),reply);
        return result==USB::Result::Pending ? result : fail(result==USB::Result::Success ? USB::Result::HardwareError : result);
    }
public:
    IsochronousRing(Controller &core,Order &barriers):controller(core),order(barriers) {}
    bool active() const { return phase==Phase::Active; }
    bool stopping() const { return phase==Phase::Stopping; }
    bool faulted() const { return phase==Phase::Fault; }
    bool ownsHardware() const { return active() || stopping() || faulted(); }
    unsigned pending() const { return queued; }
    bool available() const {
        return (phase==Phase::Waiting || phase==Phase::Active) && slots[put].phase==SlotPhase::Free;
    }
    unsigned transferResource() const { return resource; }
    USB::Result initialize(const EndpointConfiguration &endpoint,TRB *memory,uint64_t address,
        unsigned entries,uint64_t generation) {
        if (phase!=Phase::Uninitialized) return USB::Result::Busy;
        if (!endpoint.valid() || endpoint.type!=EndpointType::Isochronous ||
            endpoint.physicalEndpoint>=controller.endpointCount() || !generation || !memory ||
            (reinterpret_cast<uintptr_t>(memory)&15) || entries<4 || entries>64 ||
            reinterpret_cast<uintptr_t>(memory)>UINTPTR_MAX-entries*sizeof(TRB) ||
            !USB::dmaRange(address,entries*sizeof(TRB),controller.dmaBits(),16)) return USB::Result::Invalid;
        config=endpoint;trbs=memory;dma=address;epoch=generation;capacity=entries-1;
        for (unsigned n=0;n<entries;++n) trbs[n].addressLo=trbs[n].addressHi=trbs[n].size=trbs[n].control=0;
        link();phase=Phase::Waiting;return USB::Result::Success;
    }
    USB::Result push(uint64_t address,uint32_t length,uint32_t mappedBytes,uint64_t cookie) {
        if (phase!=Phase::Waiting && phase!=Phase::Active) return USB::Result::NotReady;
        if (!cookie || length>config.bytesPerInterval()) return USB::Result::Invalid;
        const bool in=config.physicalEndpoint&1;
        const uint32_t rounded=length ? (length+config.maximumPacketSize-1)/config.maximumPacketSize*config.maximumPacketSize : config.maximumPacketSize;
        if (mappedBytes!=(in ? length : rounded) || mappedBytes>config.bytesPerInterval() ||
            (mappedBytes && (!USB::dmaRange(address,mappedBytes,controller.dmaBits()) ||
             (address<dma+(capacity+1)*sizeof(TRB) && dma<address+mappedBytes)))) return USB::Result::Invalid;
        for (unsigned n=0;n<capacity;++n) if (slots[n].phase!=SlotPhase::Free && slots[n].cookie==cookie)
            return USB::Result::Busy;
        if (slots[put].phase!=SlotPhase::Free) return USB::Result::Busy;
        auto &slot=slots[put];slot={SlotPhase::Queued,cookie,address,length,mappedBytes,{}};
        descriptor(put);order.dmaWriteBarrier();
        if (++put==capacity) put=0;
        ++queued;
        if (phase==Phase::Active) {
            uint32_t reply=0;
            const auto result=controller.command(config.physicalEndpoint,Command::Update,0,0,0,resource<<16,reply);
            if (result!=USB::Result::Success) return fail(result);
        }
        return USB::Result::Success;
    }
    USB::Result start(uint64_t nowUS) {
        if (phase!=Phase::Waiting || !ready || !queued) return USB::Result::NotReady;
        uint32_t current=0,target=0,reply=0;
        auto result=controller.currentFrame(current);
        if (result==USB::Result::Success) result=isochronousStartFrame(config,readyFrame,current,readyAt,nowUS,target);
        if (result!=USB::Result::Success) return result;
        // From this point a timeout can have started DMA. A failed START may
        // not retire a frame merely because its command returned an error.
        phase=Phase::Active;ready=false;
        result=controller.startIsochronous(config.physicalEndpoint,dma+get*sizeof(TRB),target,reply);
        resource=(reply>>16)&127;
        if (result!=USB::Result::Success && result!=USB::Result::BusExpired) return fail(result);
        if (!resource) return fail(USB::Result::HardwareError);
        if (result==USB::Result::BusExpired) return beginStop(++retries<=5 ? Stop::Retry : Stop::Timeout);
        retries=0;return USB::Result::Success;
    }
    USB::Result idle() {
        return queued ? USB::Result::Busy : phase==Phase::Waiting ? USB::Result::Success : beginStop(Stop::Idle);
    }
    USB::Result cancel(USB::Result reason=USB::Result::Aborted) {
        if (reason!=USB::Result::Aborted && reason!=USB::Result::Timeout) return USB::Result::Invalid;
        if (phase==Phase::Waiting) { finishAll(reason);ready=false;return USB::Result::Success; }
        if (phase==Phase::Stopping) {
            stop=reason==USB::Result::Timeout ? Stop::Timeout : Stop::Abort;return USB::Result::Pending;
        }
        return beginStop(reason==USB::Result::Timeout ? Stop::Timeout : Stop::Abort);
    }
    USB::Result handle(Event event,uint64_t generation,uint64_t nowUS) {
        if (generation!=epoch || event.device() || event.endpointNumber()!=config.physicalEndpoint)
            return USB::Result::Invalid;
        if (phase==Phase::Uninitialized || phase==Phase::Fault) return USB::Result::NotReady;
        if (event.type()==7 && ((event.parameters()>>8)&15)==unsigned(Command::End)) {
            if (phase!=Phase::Stopping) return USB::Result::Invalid;
            if (event.status()) return fail(USB::Result::HardwareError);
            order.dmaReadBarrier();resource=0;phase=Phase::Waiting;ready=false;
            if (stop==Stop::Retry) {
                for (unsigned n=0;n<capacity;++n) if (slots[n].phase==SlotPhase::Queued) descriptor(n);
                order.dmaWriteBarrier(); // END proves that rearming cannot race DMA
            } else if (stop!=Stop::Idle) finishAll(stop==Stop::Timeout ? USB::Result::Timeout : USB::Result::Aborted);
            else if (queued) return fail(USB::Result::HardwareError);
            return USB::Result::Success;
        }
        if (event.type()==3) {
            if (phase!=Phase::Waiting) return USB::Result::Pending;
            readyFrame=event.parameters();readyAt=nowUS;ready=true;
            return USB::Result::Success;
        }
        if (event.type()!=1 && event.type()!=2) return USB::Result::Pending;
        if (phase==Phase::Stopping || phase==Phase::Waiting) return USB::Result::Pending;
        if (event.type()==1 || (event.status()&1)) return fail(USB::Result::HardwareError);
        order.dmaReadBarrier();
        for (unsigned n=0;n<capacity && queued;++n) {
            auto &slot=slots[get];
            if (slot.phase!=SlotPhase::Queued) return fail(USB::Result::HardwareError);
            const auto control=trbs[get].control;
            if (control&Owned) break;
            order.dmaReadBarrier();
            const auto size=trbs[get].size,remaining=size&TRBLengthMask,status=size>>28;
            const auto address=uint64_t(trbs[get].addressHi)<<32|trbs[get].addressLo;
            if (address!=slot.address || remaining>slot.programmed ||
                ((control>>4)&63)!=unsigned(TRBType::IsoFirst) ||
                (control&~((0xffffu<<14)|(63u<<4)|ContinueShort|InterruptShort|InterruptComplete)) ||
                (size&(3u<<26)) || (status!=0 && status!=1)) return fail(USB::Result::HardwareError);
            auto result=status==1 ? USB::Result::MissedService : USB::Result::Success;
            const uint32_t frame=((control>>14)&0xffff)&~(config.intervalTicks()-1);
            if ((event.status()&8) && frame==(event.parameters()&~(config.intervalTicks()-1))) result=USB::Result::MissedService;
            uint32_t actual=result==USB::Result::MissedService ? 0 : slot.programmed-remaining;
            if (actual>slot.requested) { actual=slot.requested;result=USB::Result::Overflow; }
            slot.completion={slot.cookie,epoch,result,actual,frame};slot.phase=SlotPhase::Complete;
            --queued;if (++get==capacity) get=0;
        }
        return USB::Result::Success;
    }
    USB::Result takeCompletion(uint64_t cookie,IsochronousCompletion &completion) {
        for (unsigned n=0;n<capacity;++n) if (slots[n].phase!=SlotPhase::Free && slots[n].cookie==cookie) {
            if (slots[n].phase!=SlotPhase::Complete) return USB::Result::Pending;
            completion=slots[n].completion;slots[n]={};return USB::Result::Success;
        }
        return USB::Result::Invalid;
    }
    USB::Result reclaimAfterHalt(USB::Result reason=USB::Result::HardwareError) {
        if (phase==Phase::Uninitialized || !controller.stopped()) return USB::Result::NotReady;
        order.dmaReadBarrier();finishAll(reason);phase=Phase::Waiting;resource=0;ready=false;retries=0;link();
        return USB::Result::Success;
    }
};
}
