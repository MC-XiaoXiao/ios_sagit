// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
namespace Display {
struct MappingCacheStatistics {
    uint32_t version=1,bytes=sizeof(MappingCacheStatistics);
    uint64_t nowTicks=0,displayed=0,hits=0,misses=0,evicted=0,cached=0;
};
static_assert(sizeof(MappingCacheStatistics)==56,"mapping cache diagnostic ABI");

constexpr uint32_t SwapStatisticsVersion=1,SwapSampleCapacity=2048;
struct SwapSample {uint64_t ticks;uint32_t token,flags;};
struct SwapStatisticsHeader {
    uint32_t version=SwapStatisticsVersion,sampleBytes=sizeof(SwapSample),count=0,reserved=0;
    uint64_t displayed=0,failed=0,timebaseSpanTicks=1000000,timebaseSpanNanoseconds=0,nowTicks=0;
};
static_assert(sizeof(SwapSample)==16 && sizeof(SwapStatisticsHeader)==56,"displayed swap statistics ABI");
struct PrepareStatistics {
    uint32_t version=1,bytes=sizeof(PrepareStatistics);
    uint64_t nowTicks=0,displayed=0,hardwareVBL=0;
    uint64_t attempts=0,hits=0,discarded=0,prepareTicks=0,hitLeadTicks=0,lateCalls=0,lateTicks=0;
};
}
