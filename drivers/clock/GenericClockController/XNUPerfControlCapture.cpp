// SPDX-License-Identifier: BSD-2-Clause
#include "XNUPerfControlCapture.h"

static_assert(CPUPerformance::Idle==static_cast<unsigned>(PERFCONTROL_CLASS_IDLE) &&
    CPUPerformance::Realtime==static_cast<unsigned>(PERFCONTROL_CLASS_REALTIME) &&
    CPUPerformance::Background==static_cast<unsigned>(PERFCONTROL_CLASS_BACKGROUND) &&
    CPUPerformance::UI==static_cast<unsigned>(PERFCONTROL_CLASS_UI) &&
    CPUPerformance::UserInitiated==static_cast<unsigned>(PERFCONTROL_CLASS_USER_INITIATED),"XNU perfcontrol class ABI changed");
static CPUPerformance::Capture capture;

// Scheduler fast path. No driver object or workloop is accessed, even for a
// PERFCONTROL_CALLOUT_WAKE_UNSAFE event. All storage remains in this resident
// Kext, so a late callback cannot access a freed controller during shutdown.
extern "C" void cpu_performance_csw(perfcontrol_event event,uint32_t cpu,uint64_t stamp,
    uint32_t,perfcontrol_thread_data *,perfcontrol_thread_data *on,perfcontrol_cpu_counters *,void *)
{
    if (event!=CONTEXT_SWITCH && event!=IDLE) return;
    if (!on) return;
    capture.record(cpu,stamp,event==IDLE ? CPUPerformance::Idle : static_cast<uint32_t>(on->perfctl_class));
}
extern "C" void cpu_performance_state_update(perfcontrol_event event,uint32_t cpu,uint64_t stamp,
    uint32_t,perfcontrol_thread_data *thread,void *)
{
    if (event!=QUANTUM_EXPIRY && event!=PERFCONTROL_ATTR_UPDATE && event!=THREAD_GROUP_UPDATE) return;
    if (thread) capture.record(cpu,stamp,static_cast<uint32_t>(thread->perfctl_class));
}
namespace CPUPerformance {
Capture &kernelCapture() {return capture;}
sched_perfcontrol_callbacks_t kernelCallbacks() {
    static sched_perfcontrol_callbacks callbacks = {
        SCHED_PERFCONTROL_CALLBACKS_VERSION_5,
        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
        cpu_performance_csw,cpu_performance_state_update,nullptr,nullptr,nullptr,nullptr
    };
    return &callbacks;
}
}
