// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace CPUPerformance {
constexpr unsigned MaxCPUs = 64, MaxClusters = 8, MaxStates = 40;
// Normalized XNU perfcontrol classes, deliberately separate from scheduler
// headers so both the accounting and policy run in offline host tests.
enum Class : uint32_t { Idle=1, Kernel, Realtime, Background, Utility, NonUI, UI, AboveUI, UserInitiated };
inline uint32_t weight(uint32_t qos) {
    switch (qos) {
    case Idle: return 0;
    case Background: return 600;
    case Utility: return 800;
    case UI: case AboveUI: return 1400;
    case UserInitiated: return 1200;
    case Realtime: return 1600;
    default: return 1000;
    }
}
inline uint32_t floor(uint32_t qos) {
    switch (qos) {
    case Realtime: return 1000;
    case AboveUI: return 900;
    case UI: return 800;
    case UserInitiated: return 650;
    default: return 0;
    }
}
struct Snapshot {
    uint64_t stamp=0, busy=0, demand=0, boostStamp=0;
    uint32_t epoch=0, qos=Idle, boostFloor=0;
};
enum ReadResult { Empty, Consistent, Contended };

// Permanent capture storage; no driver pointers, allocation, IOKit, MMIO,
// wakeups, locks which wait, or unbounded retry loops in record(). Each CPU is
// normally a single writer in XNU's interrupts-disabled csw/state_update path.
// A single try-CAS also handles an unexpected nested writer without spinning.
class Capture {
    struct alignas(64) Slot {
        uint32_t writer=0, sequence=0;
        uint64_t stamp=0, busy=0, demand=0, boostStamp=0, lossStamp=0;
        uint32_t epoch=0, qos=Idle, boostFloor=0;
    } slots[MaxCPUs];
    uint32_t activeEpoch=0;
    uint64_t boostTicks=0;
public:
    static_assert(__atomic_always_lock_free(4,nullptr) && __atomic_always_lock_free(8,nullptr), "capture requires native atomics");
    // Only called before the first callback registration. Never reset slots
    // while an old callout can still be running on another CPU.
    void configure(uint64_t hold) { __atomic_store_n(&boostTicks,hold,__ATOMIC_RELEASE); }
    void activate(uint32_t epoch) { __atomic_store_n(&activeEpoch,epoch,__ATOMIC_RELEASE); }
    void deactivate() { __atomic_store_n(&activeEpoch,0,__ATOMIC_RELEASE); }
    void record(unsigned cpu, uint64_t stamp, uint32_t qos) {
        uint32_t epoch=__atomic_load_n(&activeEpoch,__ATOMIC_ACQUIRE);
        if (!epoch || cpu>=MaxCPUs || !stamp) return;
        Slot &s=slots[cpu]; uint32_t unlocked=0;
        if (!__atomic_compare_exchange_n(&s.writer,&unlocked,1,true,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            __atomic_store_n(&s.lossStamp,stamp,__ATOMIC_RELEASE); return;
        }
        if (__atomic_load_n(&activeEpoch,__ATOMIC_ACQUIRE)!=epoch) {
            __atomic_store_n(&s.writer,0,__ATOMIC_RELEASE); return;
        }
        uint32_t oldEpoch=__atomic_load_n(&s.epoch,__ATOMIC_RELAXED);
        uint64_t before=__atomic_load_n(&s.stamp,__ATOMIC_RELAXED);
        // XNU attribute/group notifications may use mach_approximate_time,
        // which can lag the preceding dispatch timestamp. Keep the new class
        // at the current accounting boundary instead of discarding its QoS.
        if (oldEpoch==epoch && stamp<before) stamp=before;
        uint32_t seq=__atomic_load_n(&s.sequence,__ATOMIC_RELAXED);
        __atomic_store_n(&s.sequence,seq+1,__ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        uint64_t busy=0,demand=0,boostStamp=0; uint32_t boost=0;
        if (oldEpoch==epoch) {
            uint32_t oldQos=__atomic_load_n(&s.qos,__ATOMIC_RELAXED);
            uint64_t delta=stamp-before;
            busy=__atomic_load_n(&s.busy,__ATOMIC_RELAXED)+(oldQos!=Idle ? delta : 0);
            demand=__atomic_load_n(&s.demand,__ATOMIC_RELAXED)+delta*weight(oldQos);
            boostStamp=__atomic_load_n(&s.boostStamp,__ATOMIC_RELAXED);
            boost=__atomic_load_n(&s.boostFloor,__ATOMIC_RELAXED);
            if (stamp-boostStamp>__atomic_load_n(&boostTicks,__ATOMIC_ACQUIRE)) boost=0;
        }
        uint32_t nextFloor=floor(qos);
        if (nextFloor && nextFloor>=boost) { boost=nextFloor; boostStamp=stamp; }
        __atomic_store_n(&s.stamp,stamp,__ATOMIC_RELAXED);
        __atomic_store_n(&s.busy,busy,__ATOMIC_RELAXED);
        __atomic_store_n(&s.demand,demand,__ATOMIC_RELAXED);
        __atomic_store_n(&s.boostStamp,boostStamp,__ATOMIC_RELAXED);
        __atomic_store_n(&s.boostFloor,boost,__ATOMIC_RELAXED);
        __atomic_store_n(&s.qos,qos,__ATOMIC_RELAXED);
        __atomic_store_n(&s.epoch,epoch,__ATOMIC_RELAXED);
        __atomic_store_n(&s.sequence,seq+2,__ATOMIC_RELEASE);
        __atomic_store_n(&s.writer,0,__ATOMIC_RELEASE);
    }
    ReadResult read(unsigned cpu,uint32_t epoch,Snapshot &out) const {
        if (cpu>=MaxCPUs || !epoch) return Empty;
        const Slot &s=slots[cpu];
        uint32_t before=__atomic_load_n(&s.sequence,__ATOMIC_ACQUIRE);
        if (before&1) return Contended;
        Snapshot v;
        v.stamp=__atomic_load_n(&s.stamp,__ATOMIC_RELAXED);
        v.busy=__atomic_load_n(&s.busy,__ATOMIC_RELAXED);
        v.demand=__atomic_load_n(&s.demand,__ATOMIC_RELAXED);
        v.boostStamp=__atomic_load_n(&s.boostStamp,__ATOMIC_RELAXED);
        v.boostFloor=__atomic_load_n(&s.boostFloor,__ATOMIC_RELAXED);
        v.qos=__atomic_load_n(&s.qos,__ATOMIC_RELAXED);
        v.epoch=__atomic_load_n(&s.epoch,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s.sequence,__ATOMIC_ACQUIRE)!=before) return Contended;
        if (v.epoch!=epoch) return Empty;
        out=v; return Consistent;
    }
    // A loss marker coalesces nested writers. Store-only publication avoids
    // even the compiler-generated retry loop of an atomic counter increment.
    uint64_t lossStamp(unsigned cpu) const { return cpu<MaxCPUs ? __atomic_load_n(&slots[cpu].lossStamp,__ATOMIC_ACQUIRE) : 0; }
};

struct Decision { uint32_t hz=0, utilization=0, demand=0, qosFloor=0; bool valid=false; };
class Policy {
    struct CPU {
        uint64_t sampled=0,busy=0,demand=0,ignoreThrough=0;
        uint32_t epoch=0,cluster=MaxClusters;
        bool initialized=false, captured=false;
    } cpus[MaxCPUs];
    struct Cluster {
        uint32_t states[MaxStates]={},count=0,smoothed=0;
        uint64_t lowerSince=0,lastChange=0,saturatedSince=0;
    } clusters[MaxClusters];
    uint64_t lowerDelay=0,residency=0,boostHold=0,maxWindow=0;
public:
    bool configure(uint64_t delay,uint64_t minimumResidency,uint64_t hold,uint64_t window) {
        if (!delay || !minimumResidency || !hold || !window) return false;
        lowerDelay=delay;residency=minimumResidency;boostHold=hold;maxWindow=window;return true;
    }
    bool mapCPU(unsigned cpu,unsigned cluster) {
        if (cpu>=MaxCPUs || cluster>=MaxClusters || cpus[cpu].cluster!=MaxClusters) return false;
        cpus[cpu].cluster=cluster;return true;
    }
    bool addState(unsigned cluster,uint32_t hz) {
        if (cluster>=MaxClusters || !hz) return false;
        auto &c=clusters[cluster];
        if (c.count==MaxStates || (c.count && hz<=c.states[c.count-1])) return false;
        c.states[c.count++]=hz;return true;
    }
    void reset(unsigned cluster=MaxClusters) {
        for (auto &cpu:cpus) if (cluster==MaxClusters || cpu.cluster==cluster) {
            cpu.initialized=false;cpu.captured=false;
            if (cpu.sampled>cpu.ignoreThrough) cpu.ignoreThrough=cpu.sampled;
        }
        for (unsigned i=0;i<MaxClusters;++i) if (cluster==MaxClusters || i==cluster) {
            auto &c=clusters[i];c.smoothed=0;c.lowerSince=0;c.lastChange=0;c.saturatedSince=0;
        }
    }
    struct Window {
        uint32_t count=0,peak=0,demand=0,sum=0,qos=0,peakUtilization=0;
        bool reliable=true;
    } windows[MaxClusters];
    void begin() { for (auto &w:windows) w=Window{}; }
    void observe(unsigned cpu,uint64_t now,ReadResult result,const Snapshot &s,bool online,uint32_t bootstrapClass) {
        if (cpu>=MaxCPUs || cpus[cpu].cluster==MaxClusters) return;
        auto &p=cpus[cpu];auto &w=windows[p.cluster];
        if (!online) {
            // A membership change interrupts the cluster's demand history.
            if (p.initialized) w.reliable=false;
            p.initialized=false;p.captured=false;p.ignoreThrough=now;return;
        }
        ++w.count;
        if (result==Contended || (result==Consistent && s.stamp>now) || (p.initialized && now<=p.sampled)) {
            w.reliable=false;p.initialized=false;p.ignoreThrough=now;return;
        }
        // A record predating an offline/reset/lost-sample boundary cannot
        // describe the current CPU. Bootstrap from XNU until a fresh callback
        // arrives; never retain an old busy class or boost indefinitely.
        bool captured=result==Consistent && s.stamp>p.ignoreThrough;
        uint32_t qos=captured ? s.qos : bootstrapClass;
        uint64_t busy=0,demand=0;
        if (captured) {
            uint64_t tail=now-s.stamp;
            busy=s.busy+(qos!=Idle ? tail : 0);
            demand=s.demand+tail*weight(qos);
        }
        uint32_t util=qos==Idle ? 0 : 1000,raw=weight(qos);
        if (p.initialized && now>p.sampled) {
            uint64_t dt=now-p.sampled;
            if (dt>maxWindow) {w.reliable=false;p.initialized=false;p.ignoreThrough=now;return;}
            if (captured && p.captured && p.epoch==s.epoch) {
                uint64_t db=busy-p.busy,dd=demand-p.demand;
                if (db>dt || dd>dt*1600) {w.reliable=false;p.initialized=false;p.ignoreThrough=now;return;}
                util=static_cast<uint32_t>(db*1000/dt);raw=static_cast<uint32_t>(dd/dt);
            }
        }
        p.sampled=now;p.busy=busy;p.demand=demand;p.epoch=s.epoch;p.initialized=true;p.captured=captured;
        if(util>w.peakUtilization) w.peakUtilization=util;
        w.sum+=util;w.demand+=raw;if (raw>w.peak) w.peak=raw;
        uint32_t f=floor(qos);
        if (captured && s.boostFloor && s.boostStamp>p.ignoreThrough && now>=s.boostStamp &&
            now-s.boostStamp<=boostHold && s.boostFloor>f) f=s.boostFloor;
        if (f>w.qos) w.qos=f;
    }
    Decision decide(unsigned cluster,uint64_t now,uint32_t current,uint32_t ceiling) {
        Decision out;
        if (cluster>=MaxClusters || !current) return out;
        auto &c=clusters[cluster];auto &w=windows[cluster];
        if (!w.count || !w.reliable) {
            // Hysteresis requires consecutive observations. Preserve the
            // last hardware transition's residency, but discard trends
            // across gaps, contention and fully offline clusters.
            c.smoothed=0;c.lowerSince=0;c.saturatedSince=0;return out;
        }
        if (!c.count) return out;
        uint32_t max=c.states[c.count-1],raw=w.peak;
        if (w.peakUtilization>=950) {
            if (!c.saturatedSince) c.saturatedSince=now;
            if (now-c.saturatedSince>=lowerDelay && raw<750) raw=750;
        } else c.saturatedSince=0;
        // Peak per-core demand protects one busy thread on an otherwise idle
        // cluster. Background/utility still reach the top under sustained load.
        c.smoothed=(3*c.smoothed+raw)/4;
        uint32_t demand=raw>c.smoothed ? raw : c.smoothed;
        uint32_t fraction=demand>=750 ? 1000 : (demand*1000+749)/750;
        if (fraction<w.qos) fraction=w.qos;
        uint64_t target=(uint64_t(max)*fraction+999)/1000;
        uint32_t selected=0;
        for (unsigned i=0;i<c.count;++i) if (c.states[i]<=ceiling) {
            selected=c.states[i];if (selected>=target) break;
        }
        if (!selected) return out;
        out={selected,w.count ? w.sum/w.count : 0,demand,w.qos,true};
        // A newly tightened ceiling always wins over smoothing and residency.
        if (current>ceiling) return out;
        if (selected<current) {
            if (!c.lowerSince) c.lowerSince=now;
            if (now-c.lowerSince<lowerDelay || (c.lastChange && now-c.lastChange<residency)) out.hz=current;
        } else c.lowerSince=0;
        return out;
    }
    void committed(unsigned cluster,uint64_t now) { if(cluster<MaxClusters) {clusters[cluster].lastChange=now;clusters[cluster].lowerSince=0;} }
};
}
