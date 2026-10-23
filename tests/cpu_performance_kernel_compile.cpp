// SPDX-License-Identifier: BSD-2-Clause
#include <CPUPerformanceAdapter.h>
#include <XNUPerfControlCapture.h>
#include <stddef.h>
static_assert(sizeof(sched_perfcontrol_callbacks)==120,"native XNU table changed");
static_assert(offsetof(sched_perfcontrol_callbacks,csw)==72,"native csw offset changed");
static_assert(offsetof(sched_perfcontrol_callbacks,state_update)==80,"native state_update offset changed");
static_assert(sizeof(perfcontrol_thread_data)==64 && offsetof(perfcontrol_thread_data,perfctl_class)==4,"native thread callout ABI changed");
static_assert(PERFCONTROL_CLASS_USER_INITIATED==9 && CONTEXT_SWITCH==10 && IDLE==11,"native perfcontrol constants changed");
