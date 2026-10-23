// SPDX-License-Identifier: BSD-2-Clause
// Execution harness for the production adapter. The real SDK compile checks
// independently assert every XNU callback ABI offset used by this fixture.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include <mutex>
#include <cassert>
using IOReturn=int32_t;using kern_return_t=int32_t;using boolean_t=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,kIOReturnNoMemory=-3,kIOReturnExclusiveAccess=-4,kIOReturnNoResources=-5;
constexpr int KERN_SUCCESS=0,KERN_INVALID_ARGUMENT=4,KERN_RESOURCE_SHORTAGE=3;
constexpr unsigned kMillisecondScale=1000000;
inline uint64_t test_time=1;
inline bool test_timer_fail=false;
inline uint64_t mach_absolute_time() {return test_time;}
inline void clock_interval_to_absolutetime_interval(uint32_t n,unsigned,uint64_t *out) {*out=uint64_t(n)*1000;}
#define APPLE_KEXT_OVERRIDE override
#define OSDeclareDefaultStructors(T)
#define OSDefineMetaClassAndStructors(T,B)
#define OSSafeReleaseNULL(p) do {if(p) {(p)->release();(p)=nullptr;}} while(false)
class OSObject {
    unsigned refs=1;
public:
    virtual ~OSObject()=default;
    virtual bool init() {return true;}
    virtual void free() {}
    void retain() {++refs;}
    void release() {assert(refs);if(!--refs){free();delete this;}}
};
class IOWorkLoop;
class IOTimerEventSource:public OSObject {
public:
    using Action=void(*)(OSObject *,IOTimerEventSource *);
    OSObject *owner=nullptr;Action action=nullptr;IOWorkLoop *loop=nullptr;
    uint64_t deadline=0;bool pending=false;
    static IOTimerEventSource *timerEventSource(OSObject *o,Action a) {auto *t=new IOTimerEventSource;t->owner=o;o->retain();t->action=a;return t;}
    IOReturn setTimeoutMS(unsigned ms) {if(test_timer_fail)return kIOReturnNoResources;deadline=test_time+uint64_t(ms)*1000;pending=true;return 0;}
    void cancelTimeout() {pending=false;}
    void free() override {owner->release();}
};
class IOWorkLoop:public OSObject {
public:
    std::recursive_mutex gate;IOTimerEventSource *timer=nullptr;bool failAdd=false;
    using Action=IOReturn(*)(OSObject *,void *,void *,void *,void *);
    IOReturn addEventSource(IOTimerEventSource *s) {if(failAdd)return -1;assert(!timer);timer=s;s->retain();s->loop=this;return 0;}
    void removeEventSource(IOTimerEventSource *s) {assert(timer==s);timer=nullptr;s->loop=nullptr;s->release();}
    IOReturn runAction(Action a,OSObject *o,void *p1=nullptr,void *p2=nullptr,void *p3=nullptr,void *p4=nullptr) {std::lock_guard<std::recursive_mutex> lock(gate);return a(o,p1,p2,p3,p4);}
    void fire(uint64_t t) {std::lock_guard<std::recursive_mutex> lock(gate);test_time=t;auto *s=timer;if(s && s->pending && t>=s->deadline){s->pending=false;s->action(s->owner,s);}}
};
enum perfcontrol_class_t {PERFCONTROL_CLASS_IDLE=1,PERFCONTROL_CLASS_KERNEL,PERFCONTROL_CLASS_REALTIME,PERFCONTROL_CLASS_BACKGROUND,PERFCONTROL_CLASS_UTILITY,PERFCONTROL_CLASS_NONUI,PERFCONTROL_CLASS_UI,PERFCONTROL_CLASS_ABOVEUI,PERFCONTROL_CLASS_USER_INITIATED};
enum perfcontrol_event {QUANTUM_EXPIRY=1,THREAD_GROUP_UPDATE=2,PERFCONTROL_ATTR_UPDATE=3,CONTEXT_SWITCH=10,IDLE=11};
struct perfcontrol_thread_data {uint32_t energy_estimate_nj;perfcontrol_class_t perfctl_class;uint64_t thread_id,thread_group_id,scheduling_latency_at_same_basepri;void *thread_group_data,*perfctl_state;uint64_t thread_flags_mask,thread_flags;};
struct perfcontrol_cpu_counters {uint64_t instructions,cycles;};
using sched_perfcontrol_csw_t=void(*)(perfcontrol_event,uint32_t,uint64_t,uint32_t,perfcontrol_thread_data *,perfcontrol_thread_data *,perfcontrol_cpu_counters *,void *);
using sched_perfcontrol_state_update_t=void(*)(perfcontrol_event,uint32_t,uint64_t,uint32_t,perfcontrol_thread_data *,void *);
using GenericCallback=void(*)();
struct sched_perfcontrol_callbacks {
    unsigned long version;
    GenericCallback offcore,context_switch,oncore,max_runnable_latency,work_interval_notify,thread_group_init,thread_group_deinit,deadline_passed;
    sched_perfcontrol_csw_t csw;sched_perfcontrol_state_update_t state_update;
    GenericCallback thread_group_flags_update,work_interval_ctl,thread_group_blocked,thread_group_unblocked;
};
using sched_perfcontrol_callbacks_t=sched_perfcontrol_callbacks *;
constexpr unsigned SCHED_PERFCONTROL_CALLBACKS_VERSION_5=5;
extern "C" {
void sched_perfcontrol_register_callbacks(sched_perfcontrol_callbacks_t,unsigned long);
void cpu_performance_csw(perfcontrol_event,uint32_t,uint64_t,uint32_t,perfcontrol_thread_data *,perfcontrol_thread_data *,perfcontrol_cpu_counters *,void *);
void cpu_performance_state_update(perfcontrol_event,uint32_t,uint64_t,uint32_t,perfcontrol_thread_data *,void *);
}
void test_cpu(unsigned,bool,perfcontrol_class_t);
void test_native_registration(bool);
