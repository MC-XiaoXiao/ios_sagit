// SPDX-License-Identifier: BSD-2-Clause
#include "CPUPerformanceAdapter.h"
#include "XNUPerfControlCapture.h"
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#define super OSObject
OSDefineMetaClassAndStructors(CPUPerformanceAdapter,super);

bool CPUPerformanceAdapter::init() {return super::init();}

static CPUPerformanceAdapter *claim;
static uint32_t generation;
static uint64_t ticksForMS(uint64_t ms) {uint64_t ticks=0;clock_interval_to_absolutetime_interval(static_cast<uint32_t>(ms),kMillisecondScale,&ticks);return ticks;}

bool CPUPerformanceAdapter::start(IOWorkLoop *loop,const Backend &backend,const Mapping *mapping,unsigned cpus,unsigned clusters)
{
    if (fLoop || !loop || !mapping || !cpus || cpus>CPUPerformance::MaxCPUs || !clusters || clusters>CPUPerformance::MaxClusters ||
        !backend.online || !backend.count || !backend.frequency || !backend.current || !backend.ceiling || !backend.set) return false;
    fLoop=loop;fLoop->retain();fBackend=backend;fClusters=clusters;
    if (!fPolicy.configure(ticksForMS(30),ticksForMS(20),ticksForMS(20),ticksForMS(100))) return false;
    for (unsigned i=0;i<cpus;++i) {
        if (mapping[i].cluster>=clusters || !fPolicy.mapCPU(mapping[i].cpu,mapping[i].cluster)) return false;
        fCPUs[fCPUCount++]=mapping[i];
        fDrops[mapping[i].cpu]=CPUPerformance::kernelCapture().lossStamp(mapping[i].cpu);
    }
    bool any=false;
    for (unsigned c=0;c<clusters;++c) {
        auto &s=fState[c];unsigned count=backend.count(backend.context,c);
        bool mapped=false;for (unsigned i=0;i<cpus;++i) mapped|=mapping[i].cluster==c;
        if (!mapped || !count || count>CPUPerformance::MaxStates || !backend.current(backend.context,c)) continue;
        for (unsigned i=0;i<count;++i) if (!fPolicy.addState(c,backend.frequency(backend.context,c,i))) return false;
        s.available=true;s.automatic=true;any=true;
    }
    if (!any) {fStartStatus=kIOReturnNotReady;return false;}
    fTimer=IOTimerEventSource::timerEventSource(this,timer);
    if (!fTimer) {fStartStatus=kIOReturnNoMemory;return false;}
    fStartStatus=fLoop->addEventSource(fTimer);
    if (fStartStatus!=kIOReturnSuccess) return false;
    fAdded=true;
    CPUPerformanceAdapter *empty=nullptr;
    if (!__atomic_compare_exchange_n(&claim,&empty,this,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {fStartStatus=kIOReturnExclusiveAccess;return false;}
    fClaimed=true;
    if (generation==UINT32_MAX) {fStartStatus=kIOReturnNoResources;return false;}
    fEpoch=++generation;
    CPUPerformance::kernelCapture().configure(ticksForMS(20));
    auto result=sched_perfcontrol_register_callbacks_exclusive(CPUPerformance::kernelCallbacks(),0);
    if (result!=KERN_SUCCESS) {fStartStatus=kIOReturnExclusiveAccess;return false;}
    fInstalled=true;
    CPUPerformance::kernelCapture().activate(fEpoch);
    fStartStatus=fTimer->setTimeoutMS(5);
    return fStartStatus==kIOReturnSuccess;
}
void CPUPerformanceAdapter::timer(OSObject *owner,IOTimerEventSource *) {static_cast<CPUPerformanceAdapter *>(owner)->tick();}
IOReturn CPUPerformanceAdapter::armTimer()
{
    IOReturn result=fTimer->setTimeoutMS(5);
    fTimerFailed=result!=kIOReturnSuccess;
    if (fTimerFailed) {
        fTimer->cancelTimeout();fPolicy.reset();
        // No timer means no future decisions. Report the stopped policy and
        // require explicit recovery instead of advertising silent success.
        for (unsigned c=0;c<fClusters;++c) if (fState[c].available) {
            fState[c].fault=true;fState[c].error=result;
        }
    }
    return result;
}
void CPUPerformanceAdapter::tick()
{
    if (!fInstalled || fLost) return;
    if (!sched_perfcontrol_callbacks_owned(CPUPerformance::kernelCallbacks())) {
        fLost=true;CPUPerformance::kernelCapture().deactivate();return;
    }
    bool online=fBackend.online(fBackend.context);
    if (!online) {fWasOnline=false;fPolicy.reset();armTimer();return;}
    if (!fWasOnline) {fPolicy.reset();fWasOnline=true;}
    fPolicy.begin();
    for (unsigned i=0;i<fCPUCount;++i) {
        unsigned cpu=fCPUs[i].cpu;
        perfcontrol_class_t current=PERFCONTROL_CLASS_IDLE;boolean_t present=false;
        CPUPerformance::Snapshot sample;
        auto read=CPUPerformance::kernelCapture().read(cpu,fEpoch,sample);
        uint64_t dropped=CPUPerformance::kernelCapture().lossStamp(cpu);
        if (dropped!=fDrops[cpu]) {
            ++fState[fCPUs[i].cluster].dropped;
            fDrops[cpu]=dropped;read=CPUPerformance::Contended;
        }
        kern_return_t result=sched_perfcontrol_cpu_state(cpu,&current,&present);
        if (result!=KERN_SUCCESS) read=CPUPerformance::Contended;
        fPolicy.observe(cpu,mach_absolute_time(),read,sample,result==KERN_SUCCESS ? present : true,static_cast<uint32_t>(current));
    }
    uint64_t now=mach_absolute_time();
    for (unsigned c=0;c<fClusters;++c) {
        auto &s=fState[c];if (!s.available || s.fault) continue;
        uint32_t current=fBackend.current(fBackend.context,c),ceiling=fBackend.ceiling(fBackend.context,c);
        if (!current || !ceiling) {s.fault=true;s.error=kIOReturnNotReady;continue;}
        auto decision=fPolicy.decide(c,now,current,ceiling);
        if (!decision.valid) continue;
        s.utilization=decision.utilization;s.demand=decision.demand;
        if (!s.automatic || decision.hz==current) continue;
        // This action runs under the same IOWorkLoop gate as user-client
        // requests. Limits and manual mode cannot race a queued old decision.
        s.error=fBackend.set(fBackend.context,c,decision.hz);
        if (s.error!=kIOReturnSuccess) {s.fault=true;continue;}
        ++s.transitions;fPolicy.committed(c,now);
    }
    armTimer();
}
IOReturn CPUPerformanceAdapter::setAutomatic(unsigned c,bool on)
{
    if (c>=fClusters) return kIOReturnBadArgument;
    auto &s=fState[c];
    if (!on) {s.automatic=false;return kIOReturnSuccess;}
    if (!s.available || !fInstalled || fLost || !sched_perfcontrol_callbacks_owned(CPUPerformance::kernelCallbacks()) ||
        !fBackend.online(fBackend.context) || !fBackend.current(fBackend.context,c) ||
        !fBackend.ceiling(fBackend.context,c)) return kIOReturnNotReady;
    if (fTimerFailed) {
        IOReturn result=armTimer();
        if (result!=kIOReturnSuccess) return result;
    }
    s.fault=false;s.error=kIOReturnSuccess;s.automatic=true;fPolicy.reset(c);return kIOReturnSuccess;
}
void CPUPerformanceAdapter::manual(unsigned c) {if(c<fClusters) fState[c].automatic=false;}
uint32_t CPUPerformanceAdapter::status(unsigned c) const
{
    if(c>=fClusters || !fState[c].available) return Unavailable;
    const auto &s=fState[c];
    return (s.automatic ? Automatic : Manual) | (s.fault ? uint32_t(Fault) : 0U) | (fLost ? uint32_t(OwnershipLost) : 0U) |
        (!fBackend.online(fBackend.context) ? uint32_t(Paused) : 0U);
}
IOReturn CPUPerformanceAdapter::shutdown(OSObject *owner,void *,void *,void *,void *)
{
    auto *self=static_cast<CPUPerformanceAdapter *>(owner);
    if (self->fClaimed) CPUPerformance::kernelCapture().deactivate();
    if (self->fTimer) self->fTimer->cancelTimeout();
    if (self->fInstalled) {
        sched_perfcontrol_release_callbacks(CPUPerformance::kernelCallbacks());self->fInstalled=false;
    }
    return kIOReturnSuccess;
}
void CPUPerformanceAdapter::stop()
{
    if (fLoop) fLoop->runAction(shutdown,this);
    if (fAdded) {fLoop->removeEventSource(fTimer);fAdded=false;}
    if (fClaimed) {CPUPerformanceAdapter *expected=this;__atomic_compare_exchange_n(&claim,&expected,nullptr,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);fClaimed=false;}
    OSSafeReleaseNULL(fTimer);OSSafeReleaseNULL(fLoop);
}
void CPUPerformanceAdapter::free() {stop();super::free();}
