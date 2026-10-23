// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
namespace Display {
constexpr uint32_t TimelineVersion=1,TimelineCapacity=4096;
enum class TimelineKind : uint32_t { IRQ=1, Worker, Started, CompleteBefore, CompleteAfter, Prepare, Active, VBL, ReadRetired, SurfaceRetired,
    VBLObserved, PowerRequested, PowerApplied };
struct TimelineSample {
    // ticks, request, kind:token, irq:flush, ctl:readcount,
    // writecount:source-address, phase-specific auxiliary value.
    uint64_t words[7];
    static TimelineSample make(uint64_t ticks,uint64_t request,TimelineKind kind,uint32_t token,
        uint32_t irq,uint32_t flush,uint32_t ctl,uint32_t read,uint32_t write,uint32_t address,uint64_t aux=0) {
        return {{ticks,request,(uint64_t(kind)<<32)|token,(uint64_t(irq)<<32)|flush,
                 (uint64_t(ctl)<<32)|read,(uint64_t(write)<<32)|address,aux}};
    }
};
struct TimelineHeader {
    uint32_t version=TimelineVersion,sampleBytes=sizeof(TimelineSample),capacity=TimelineCapacity,count=0;
    uint64_t total=0,dropped=0,unavailable=0,nowTicks=0,timebaseTicks=1000000,timebaseNS=0,firstSerial=0;
    // PP configuration is read only, on the native gate at snapshot time.
    uint32_t tear=0,sync=0,height=0,writeInit=0,readInit=0,threshold=0,start=0,readIRQ=0,writeIRQ=0,autoRefresh=0;
};
static_assert(sizeof(TimelineSample)==56 && sizeof(TimelineHeader)==112,"display timeline ABI");
// Both hard IRQ and gated worker may publish. No allocation, locks, spinning,
// request dereference, or waiting in append. A stalled writer cannot expose
// a partially mixed sample; a collision is reported as a missing serial.
// All fields read concurrently are atomic, including payload words.
struct TimelineRing {
    struct Slot {uint64_t serial,busy,words[7];};
    uint64_t next,dropped;
    Slot slots[TimelineCapacity];
    uint64_t total() const {return __atomic_load_n(&next,__ATOMIC_ACQUIRE);}
    uint64_t losses() const {return __atomic_load_n(&dropped,__ATOMIC_RELAXED);}
    bool append(const TimelineSample &s) {
        const auto n=__atomic_fetch_add(&next,uint64_t(1),__ATOMIC_RELAXED);
        auto &slot=slots[n%TimelineCapacity];uint64_t expected=0;
        if(!__atomic_compare_exchange_n(&slot.busy,&expected,uint64_t(1),false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            __atomic_fetch_add(&dropped,uint64_t(1),__ATOMIC_RELAXED);return false;
        }
        __atomic_store_n(&slot.serial,uint64_t(0),__ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        for(unsigned i=0;i<7;++i)__atomic_store_n(&slot.words[i],s.words[i],__ATOMIC_RELAXED);
        __atomic_store_n(&slot.serial,n+1,__ATOMIC_RELEASE);
        __atomic_store_n(&slot.busy,uint64_t(0),__ATOMIC_RELEASE);return true;
    }
    bool read(uint64_t n,TimelineSample &s) const {
        const auto &slot=slots[n%TimelineCapacity];
        if(__atomic_load_n(&slot.serial,__ATOMIC_ACQUIRE)!=n+1)return false;
        for(unsigned i=0;i<7;++i)s.words[i]=__atomic_load_n(&slot.words[i],__ATOMIC_RELAXED);
        // Order payload loads before the final generation check, also on ARM.
        __atomic_thread_fence(__ATOMIC_ACQ_REL);
        return __atomic_load_n(&slot.serial,__ATOMIC_ACQUIRE)==n+1;
    }
};
}
