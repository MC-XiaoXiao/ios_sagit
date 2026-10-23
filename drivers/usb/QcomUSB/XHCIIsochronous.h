// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCITransfers.h"
#include "XHCIFrames.h"
#include <IOKit/usb/IOUSBHostIsochronous.h>

namespace XHCI {
// The Native legacy adapter advances one frame on every update() call. Keep
// preparation, hardware admission and CPU publication separate; publication
// is ordered even when later DMA payloads have already completed.
class IsochronousProgress {
    uint32_t total=0,prepared=0,submitted=0,published=0;
    uint64_t actual=0;
public:
    explicit IsochronousProgress(uint32_t count=0) : total(count) {}
    uint32_t count() const { return total; }
    uint32_t nextPreparation() const { return prepared; }
    uint32_t nextSubmission() const { return submitted; }
    uint32_t nextPublication() const { return published; }
    uint32_t buffered() const { return prepared-published; }
    uint64_t transferred() const { return actual; }
    bool done() const { return published==total; }
    bool prepare(uint32_t index) {
        if (index!=prepared || prepared==total) return false;
        ++prepared;return true;
    }
    bool submit(uint32_t index) {
        if (index!=submitted || submitted==prepared) return false;
        ++submitted;return true;
    }
    bool publish(uint32_t index,uint32_t bytes) {
        if (index!=published || published==submitted) return false;
        ++published;actual+=bytes;return true;
    }
};

inline USB::Result isochronousInterval(const EndpointConfiguration &endpoint,uint32_t &microframes) {
    if (endpoint.type!=EndpointType::Isochronous || !endpoint.interval || endpoint.interval>16)
        return USB::Result::Invalid;
    if (endpoint.speed!=USB::Speed::Full && endpoint.speed!=USB::Speed::High && endpoint.speed!=USB::Speed::Super)
        return USB::Result::Unsupported;
    microframes=(endpoint.speed==USB::Speed::Full ? 8u : 1u)<<(endpoint.interval-1);
    return USB::Result::Success;
}

inline USB::Result isochronousTarget(const EndpointConfiguration &endpoint,uint64_t firstFrame,uint64_t index,
    uint64_t &target) {
    uint32_t interval=0;const auto result=isochronousInterval(endpoint,interval);
    if (result!=USB::Result::Success) return result;
    if (firstFrame>UINT64_MAX/8) return USB::Result::Invalid;
    const auto start=firstFrame*8;
    if (start%interval || index>(UINT64_MAX-start)/interval) return USB::Result::Invalid;
    target=start+index*interval;return USB::Result::Success;
}

// Native startFrame is a 64-bit millisecond frame number; a transaction is
// one endpoint service interval, which can be shorter or longer than 1 ms.
// Return Pending for a future transaction outside the hardware Frame ID
// window. The caller must recheck immediately before TRB publication.
inline USB::Result isochronousSchedule(const EndpointConfiguration &endpoint,uint64_t firstFrame,uint64_t index,
    uint64_t currentFrame,uint32_t raw,unsigned threshold,IsoSchedule &out) {
    uint32_t interval=0;const auto result=isochronousInterval(endpoint,interval);
    if (result!=USB::Result::Success) return result;
    if (threshold>56 || raw>MicroFrameMask) return USB::Result::Invalid;
    if (!firstFrame) { out={};return USB::Result::Success; }
    if ((currentFrame&2047)!=(raw>>3) || currentFrame>UINT64_MAX/8 || firstFrame>UINT64_MAX/8)
        return USB::Result::Invalid;
    uint64_t target=0;
    if (isochronousTarget(endpoint,firstFrame,index,target)!=USB::Result::Success) return USB::Result::Invalid;
    // xHCI 4.11.2.5: round IST itself to frames, then add one frame.
    // For ESIT>=1ms the first Frame ID must also be on an ESIT boundary.
    const auto earliest=currentFrame+(threshold+7)/8+1,latest=currentFrame+895;
    if ((target>>3)<earliest) return USB::Result::Timeout;
    if ((target>>3)>latest) return USB::Result::Pending;
    out={unsigned((target>>3)&2047),false,false};return USB::Result::Success;
}

// CFC describes checking contiguous TDs, not support for the first Frame ID.
// A software-empty queue is not proof that a running ISO flow has ended.
// Only a real Ring Underrun/Overrun or a fresh endpoint context resets this
// state. A non-CFC gap drains the old flow before starting another Frame ID.
class IsochronousFlow {
    uint64_t next=0;
    bool running=false,anchored=false,draining=false;
public:
    bool active() const { return running; }
    bool waitingForEmpty() const { return draining; }
    bool hasAnchor() const { return anchored; }
    uint64_t nextTarget() const { return next; }
    void ended() { next=0;running=anchored=draining=false; }
    void drain() { if (running) draining=true; }
    USB::Result plan(const EndpointConfiguration &endpoint,bool contiguousFrames,uint64_t firstFrame,uint64_t index,
        uint64_t currentFrame,uint32_t raw,unsigned threshold,IsoSchedule &out) {
        uint32_t interval=0;auto r=isochronousInterval(endpoint,interval);
        if (r!=USB::Result::Success) return r;
        if (threshold>56 || raw>MicroFrameMask || currentFrame>UINT64_MAX/8 || (currentFrame&2047)!=(raw>>3))
            return USB::Result::Invalid;
        if (!firstFrame) {
            if (draining) return USB::Result::Pending;
            out={};return USB::Result::Success;
        }
        uint64_t target=0;r=isochronousTarget(endpoint,firstFrame,index,target);
        if (r!=USB::Result::Success) return r;
        if (running && anchored && target<next) return USB::Result::Timeout;
        const bool continuous=running && anchored && target==next;
        if (running && !contiguousFrames && !continuous) draining=true;
        if ((target>>3)<currentFrame+(threshold+7)/8+1) return USB::Result::Timeout;
        if (draining) return USB::Result::Pending;
        if ((target&7) && !continuous) return USB::Result::Timeout;
        // SIA continuations have no 895ms Frame ID limit. This also permits
        // long-ESIT streams when consecutive intervals exceed that window.
        if (continuous && (!contiguousFrames || interval>895u*8)) {
            out={};return USB::Result::Success;
        }
        return isochronousSchedule(endpoint,firstFrame,index,currentFrame,raw,threshold,out);
    }
    bool submitted(const EndpointConfiguration &endpoint,uint64_t firstFrame,uint64_t index) {
        uint32_t interval=0;if (draining || isochronousInterval(endpoint,interval)!=USB::Result::Success) return false;
        uint64_t target=next;
        if (firstFrame) {
            if (isochronousTarget(endpoint,firstFrame,index,target)!=USB::Result::Success) return false;
            anchored=true;
        }
        if (anchored && target<=UINT64_MAX-interval) next=target+interval;
        else { anchored=false;next=0; }
        running=true;return true;
    }
};
}
