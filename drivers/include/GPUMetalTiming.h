// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
namespace GPU {namespace MetalTiming {
constexpr uint32_t Version=1,ReportSelector=0x1005,CopySelector=0x1006,Capacity=4096;
enum Stage : uint32_t { Library=1,Function,RenderPipeline,ComputePipeline,CompilerLock,AIRInspect,AIRLower,Backend,Commit,QueueRun,ProgramRegister,Present,PresentAtTime,PresentAfterDuration,PresentOptions };
// QueueRun detail: four16-bit counts (batch attempts, index boundaries,
// other operation/dependency boundaries, capacity boundaries). argument:
// full batches[0:15], private-clear/client-index pairs[16:31], maximum batch resources[32:63].
struct Record {uint32_t version=Version,stage=0;uint64_t begin=0,end=0,identity=0,detail=0,argument=0;};
struct Sample {Record record;uint64_t received=0;uint32_t pid=0,compositor=0;};
struct History {
    uint32_t version=Version,bytes=sizeof(History),count=0,next=0;
    uint64_t total=0,now=0,timebaseTicks=1000000,timebaseNS=0;
    Sample samples[Capacity];
    void append(const Record &r,uint64_t received,uint32_t pid,bool compositor) {
        samples[next]={r,received,pid,uint32_t(compositor)};
        next=(next+1)%Capacity;if(count<Capacity)++count;++total;
    }
};
static_assert(sizeof(Record)==48 && sizeof(Sample)==64 && sizeof(History)==262192,"Metal CPU timing ABI");
}}
