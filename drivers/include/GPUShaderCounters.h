/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

namespace GPU { namespace ShaderCounters {
// Opt-in A540 diagnostics: one selected draw per batch, no logging while drawing.
// Selector definitions come from pinned Mesa registers/adreno/a5xx.xml.
constexpr unsigned Count=16, Capacity=64, ScratchOffset=1152, RecordFlag=4;
constexpr uint32_t SP[]={0,1,2,4,5,6,24,25}, TP[]={0,1,6,7}, UCHE[]={0,1,4,8};
struct Record {
    uint64_t retired=0,hash=0,ticks=0,values[Count]{};
    uint32_t sequence=0,index=0;
};
struct Snapshot {
    uint32_t version=1,bytes=sizeof(Snapshot),count=0,next=0;
    Record records[Capacity];
    void append(const Record &r) {records[next]=r;next=(next+1)%Capacity;if(count<Capacity)++count;}
};
static_assert(ScratchOffset+Count*16<=4096,"counter snapshots fit private progress page");
} }
