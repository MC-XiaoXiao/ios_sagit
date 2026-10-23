// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <CPUPerformancePolicy.h>
#include <machine/machine_routines.h>
// These declarations project the documented local XNU ownership extension.
// The event types and callback-table ABI above come from the Apple kernel SDK.
extern "C" {
kern_return_t sched_perfcontrol_register_callbacks_exclusive(sched_perfcontrol_callbacks_t,unsigned long);
boolean_t sched_perfcontrol_callbacks_owned(sched_perfcontrol_callbacks_t);
kern_return_t sched_perfcontrol_release_callbacks(sched_perfcontrol_callbacks_t);
kern_return_t sched_perfcontrol_cpu_state(unsigned int,perfcontrol_class_t *,boolean_t *);
}
namespace CPUPerformance {
Capture &kernelCapture();
sched_perfcontrol_callbacks_t kernelCallbacks();
}
