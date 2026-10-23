// SPDX-License-Identifier: BSD-2-Clause
#include <CPUPerformanceAdapter.h>
#include <KernelShim.h>
#include <XNUPerfControlCapture.h>
#include <OSMController.h>
#include <cassert>
#include <cstdio>
#include <map>
#include <thread>
#include <atomic>
struct Hardware {
    std::map<uint32_t,uint32_t> words;
    QcomPower::OSMController osm;
    unsigned writes=0;bool online=true,failSet=false;
    Hardware() {
        for(unsigned c=0;c<2;++c){unsigned b=c*0x2000;words[b+0x1004]=1;
            for(unsigned i=0;i<40;++i){unsigned row=b+0x1150+32*i;words[row]=i;words[row+4]=(1U<<26)|(4U<<16)|(i<6?20+20*i:120);words[row+8]=700+(i<6?50*i:250);}}
        assert(osm.initialize({this,read,write,wait,nullptr})==QcomPower::OK);
    }
    static uint32_t read(void *p,unsigned b,uint32_t r) {assert(!b);return static_cast<Hardware *>(p)->words[r];}
    static void write(void *p,unsigned b,uint32_t r,uint32_t v) {assert(!b);auto &h=*static_cast<Hardware *>(p);++h.writes;h.words[r]=v;}
    static void wait(void *,unsigned) {}
    static bool isOnline(void *p) {return static_cast<Hardware *>(p)->online;}
    static uint32_t count(void *p,unsigned c) {return static_cast<Hardware *>(p)->osm.stateCount(c);}
    static uint32_t frequency(void *p,unsigned c,unsigned r) {return static_cast<Hardware *>(p)->osm.state(c,r,false);}
    static uint32_t current(void *p,unsigned c) {return static_cast<Hardware *>(p)->osm.rate(c);}
    static uint32_t ceiling(void *p,unsigned c) {return static_cast<Hardware *>(p)->osm.limit(c);}
    static IOReturn set(void *p,unsigned c,uint32_t hz) {auto &h=*static_cast<Hardware *>(p);return h.failSet ? -99 : h.osm.setRate(c,hz)==QcomPower::OK ? 0 : -98;}
    CPUPerformanceAdapter::Backend backend() {return {this,isOnline,count,frequency,current,ceiling,set};}
};
static void event(unsigned cpu,uint64_t t,perfcontrol_class_t qos,perfcontrol_event kind=CONTEXT_SWITCH,uint32_t flags=0) {
    test_cpu(cpu,true,qos);perfcontrol_thread_data on{};on.perfctl_class=qos;
    if(kind==CONTEXT_SWITCH || kind==IDLE)cpu_performance_csw(kind,cpu,t,flags,nullptr,&on,nullptr,nullptr);
    else cpu_performance_state_update(kind,cpu,t,flags,&on,nullptr);
}
static CPUPerformanceAdapter *start(IOWorkLoop *loop,Hardware &h) {
    CPUPerformanceAdapter::Mapping map[]={{7,0},{3,0},{12,1},{1,1}};
    for(auto m:map)test_cpu(m.cpu,true,PERFCONTROL_CLASS_IDLE);
    auto *p=new CPUPerformanceAdapter;assert(p->init());assert(p->start(loop,h.backend(),map,4,2));return p;
}
static void release(CPUPerformanceAdapter *p,IOWorkLoop *loop) {p->stop();p->release();assert(!loop->timer);loop->release();}
int main() {
    {
        sched_perfcontrol_callbacks a{},b{};a.version=b.version=5;
        std::atomic<unsigned> wins{0};
        std::thread first([&]{if(sched_perfcontrol_register_callbacks_exclusive(&a,0)==KERN_SUCCESS)++wins;});
        std::thread second([&]{if(sched_perfcontrol_register_callbacks_exclusive(&b,0)==KERN_SUCCESS)++wins;});
        first.join();second.join();assert(wins==1);
        auto *owner=sched_perfcontrol_callbacks_owned(&a) ? &a : &b;
        assert(sched_perfcontrol_release_callbacks(owner)==KERN_SUCCESS);
    }
    {
        Hardware h;auto *loop=new IOWorkLoop;auto *p=start(loop,h);
        assert(h.writes==0 && (p->status(0)&CPUPerformanceAdapter::Automatic));
        sched_perfcontrol_callbacks foreign{};foreign.version=5;
        assert(sched_perfcontrol_register_callbacks_exclusive(&foreign,0)==KERN_RESOURCE_SHORTAGE);
        assert(sched_perfcontrol_release_callbacks(&foreign)==KERN_INVALID_ARGUMENT);
        assert(sched_perfcontrol_callbacks_owned(CPUPerformance::kernelCallbacks()));
        foreign.version=4;assert(sched_perfcontrol_register_callbacks_exclusive(&foreign,0)==KERN_INVALID_ARGUMENT);
        assert(sched_perfcontrol_register_callbacks_exclusive(nullptr,0)==KERN_INVALID_ARGUMENT);
        perfcontrol_class_t qos=PERFCONTROL_CLASS_IDLE;boolean_t present=false;
        assert(sched_perfcontrol_cpu_state(64,&qos,&present)==KERN_INVALID_ARGUMENT);
        assert(sched_perfcontrol_cpu_state(7,nullptr,&present)==KERN_INVALID_ARGUMENT);
        // This is the production callback object and timer action. The unsafe
        // wakeup flag must not cause a wakeup, allocation or register write.
        event(7,1000,PERFCONTROL_CLASS_UI,PERFCONTROL_ATTR_UPDATE,1);
        assert(h.writes==0);loop->fire(5001);assert(h.osm.rate(0)>=1920000000U && h.osm.rate(1)==384000000);
        assert(p->transitions(0)==1 && p->utilization(0)==500);
        assert(h.osm.setLimit(0,768000000)==QcomPower::OK);unsigned before=h.writes;
        loop->fire(10001);assert(h.osm.rate(0)==768000000 && h.writes==before);
        assert(h.osm.setLimit(0,0)==QcomPower::OK);loop->fire(15001);assert(h.osm.rate(0)>=1920000000U);
        // Explicit frequency requests retain manual control until auto is
        // enabled, while a tightened ceiling still clamps manual operation.
        assert(h.osm.setRate(0,1152000000)==QcomPower::OK);p->manual(0);
        loop->fire(20001);assert(h.osm.rate(0)==1152000000);
        assert(h.osm.setLimit(0,768000000)==QcomPower::OK);loop->fire(25001);assert(h.osm.rate(0)==768000000);
        assert(p->setAutomatic(0,true)==0);loop->fire(30001);assert(h.osm.rate(0)==768000000);
        h.online=false;before=h.writes;event(12,31000,PERFCONTROL_CLASS_REALTIME);loop->fire(35001);assert(h.writes==before && (p->status(0)&CPUPerformanceAdapter::Paused));
        h.online=true;loop->fire(40001);assert(h.osm.rate(1)==2304000000U);
        h.failSet=true;assert(h.osm.setLimit(0,0)==QcomPower::OK);event(7,41000,PERFCONTROL_CLASS_UI);loop->fire(45001);
        assert(p->status(0)&CPUPerformanceAdapter::Fault);before=h.writes;loop->fire(50001);assert(h.writes==before);
        h.failSet=false;assert(p->setAutomatic(0,true)==0);loop->fire(55001);assert(!(p->status(0)&CPUPerformanceAdapter::Fault));
        // A native Apple registration supersedes the adapter. Its timer must
        // notice ownership loss and stop generating OSM requests.
        test_native_registration(true);before=h.writes;loop->fire(60001);assert(p->status(0)&CPUPerformanceAdapter::OwnershipLost);
        assert(p->setAutomatic(0,true)==kIOReturnNotReady && h.writes==before);
        release(p,loop);test_native_registration(false);
        // Delayed old callbacks have only permanent capture storage to touch.
        event(7,61000,PERFCONTROL_CLASS_UI);
    }
    {
        Hardware h;auto *loop=new IOWorkLoop;test_time=70000;
        test_native_registration(true);
        auto *p=new CPUPerformanceAdapter;assert(p->init());CPUPerformanceAdapter::Mapping map[]={{0,0}};
        assert(!p->start(loop,h.backend(),map,1,2));p->stop();p->release();assert(!loop->timer && !h.writes);loop->release();
        test_native_registration(false);
    }
    {
        Hardware h;auto *loop=new IOWorkLoop;test_time=80000;auto *p=start(loop,h);
        auto *other=new CPUPerformanceAdapter;assert(other->init());CPUPerformanceAdapter::Mapping map[]={{0,0}};
        // Different workloop, same global native callback owner.
        auto *secondLoop=new IOWorkLoop;
        assert(!other->start(secondLoop,h.backend(),map,1,2));other->stop();other->release();secondLoop->release();
        event(7,81000,PERFCONTROL_CLASS_BACKGROUND);loop->fire(85000);assert(h.osm.rate(0)>384000000);
        // Offline cores do not retain stale busy demand forever.
        test_cpu(7,false,PERFCONTROL_CLASS_IDLE);test_cpu(3,true,PERFCONTROL_CLASS_IDLE);
        for(uint64_t t=90000;t<=180000;t+=5000)loop->fire(t);
        assert(h.osm.rate(0)==384000000);
        release(p,loop);
    }
    {
        Hardware h;auto *loop=new IOWorkLoop;test_time=300000;auto *p=start(loop,h);
        test_timer_fail=true;loop->fire(305000);
        assert(!loop->timer->pending && (p->status(0)&CPUPerformanceAdapter::Fault) &&
            (p->status(1)&CPUPerformanceAdapter::Fault));
        assert(p->setAutomatic(0,true)==kIOReturnNoResources);
        assert(p->status(0)&CPUPerformanceAdapter::Fault);
        test_timer_fail=false;
        // Recovery must validate online hardware and explicitly rearm the
        // failed timer, without silently clearing another cluster's fault.
        h.online=false;assert(p->setAutomatic(0,true)==kIOReturnNotReady);
        h.online=true;assert(p->setAutomatic(0,true)==kIOReturnSuccess);
        assert(loop->timer->pending && !(p->status(0)&CPUPerformanceAdapter::Fault));
        assert(p->status(1)&CPUPerformanceAdapter::Fault);
        event(7,306000,PERFCONTROL_CLASS_UI);loop->fire(310000);
        assert(h.osm.rate(0)>384000000 && h.osm.rate(1)==384000000);
        assert(p->setAutomatic(1,true)==kIOReturnSuccess);
        // Rearming while paused has the same failure/recovery semantics.
        h.online=false;test_timer_fail=true;loop->fire(315000);
        assert(p->status(0)&CPUPerformanceAdapter::Fault);
        test_timer_fail=false;h.online=true;
        assert(p->setAutomatic(0,true)==kIOReturnSuccess && loop->timer->pending);
        release(p,loop);
    }
    {
        Hardware h;auto *loop=new IOWorkLoop;test_time=400000;test_timer_fail=true;
        auto *p=new CPUPerformanceAdapter;assert(p->init());CPUPerformanceAdapter::Mapping map[]={{0,0}};
        assert(!p->start(loop,h.backend(),map,1,2) && p->startStatus()==kIOReturnNoResources);
        release(p,loop);test_timer_fail=false;
        auto *next=new IOWorkLoop;auto *valid=start(next,h);release(valid,next);
    }
    {
        Hardware h;auto *loop=new IOWorkLoop;test_time=500000;auto *p=start(loop,h);
        event(7,501000,PERFCONTROL_CLASS_REALTIME);loop->fire(505000);
        test_cpu(7,false,PERFCONTROL_CLASS_IDLE);loop->fire(510000);
        test_cpu(7,true,PERFCONTROL_CLASS_IDLE);
        for(uint64_t t=515000;t<=590000;t+=5000)loop->fire(t);
        assert(h.osm.rate(0)==384000000 && p->utilization(0)==0 && p->demand(0)==0);
        release(p,loop);
    }
    {
        Hardware h;auto *loop=new IOWorkLoop;loop->failAdd=true;
        auto *p=new CPUPerformanceAdapter;assert(p->init());CPUPerformanceAdapter::Mapping map[]={{0,0}};
        assert(!p->start(loop,h.backend(),map,1,2));p->stop();p->release();loop->release();
        auto *nextLoop=new IOWorkLoop;test_time=200000;auto *valid=start(nextLoop,h);
        // PM can disable the controller before the first sampling tick.
        h.online=false;nextLoop->fire(205000);assert(h.writes==0);
        h.online=true;event(7,206000,PERFCONTROL_CLASS_USER_INITIATED,QUANTUM_EXPIRY,1);
        nextLoop->fire(210000);assert(h.osm.rate(0)>384000000);
        release(valid,nextLoop);
    }
    puts("CPU adapter: production csw/state callbacks, gated timer/OSM, manual/auto ceilings, PM pause, failures, native ownership and lifecycle passed");
}
