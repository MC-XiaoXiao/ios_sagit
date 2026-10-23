// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
namespace Display {
constexpr uint32_t LeaseHistoryVersion=2,LeaseHistoryCapacity=256;
enum class LeasePhase : uint32_t { Prepare=1, Pending, Active, CompleteBefore, CompleteAfter, Pruned, Query, Stalled, Started };
// Started is recorded after successful CTL_START. Active precedes mapping and
// staging; neither diagnostic event proves PP_DONE or retires a surface read.
// Pointer values identify native objects only; consumers must not dereference
// them. Timestamps use mach_absolute_time, matching GPU access diagnostics.
struct LeaseSample {
    uint64_t ticks=0,request=0,active=0,queued=0;
    uint64_t fences[4]{},displayedFences[4]{};
    uint64_t effective[4]{};
    uint32_t token=0,intent=0,phase=0,changed=0,ready=0,state=0,cancelled=0;
    uint32_t front=0,staged=0,lastCompleted=0,lastDisplayed=0,flags=0;
    uint32_t surfaces[4]{},displayed[4]{},displayedTokens[4]{};
};
struct LeaseHistoryHeader {
    uint32_t version=LeaseHistoryVersion,sampleBytes=sizeof(LeaseSample),count=0,capacity=LeaseHistoryCapacity;
    uint64_t total=0,nowTicks=0,timebaseSpanTicks=1000000,timebaseSpanNanoseconds=0,owner=0;
    uint64_t frozenAtTicks=0;
};
static_assert(sizeof(LeaseSample)==224 && sizeof(LeaseHistoryHeader)==64,"display lease history ABI");
}
