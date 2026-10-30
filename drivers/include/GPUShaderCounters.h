/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

namespace GPU { namespace ShaderCounters {
// Opt-in A540 diagnostics: one selected draw per batch, no logging while drawing.
// Selector definitions come from pinned Mesa registers/adreno/a5xx.xml.
constexpr unsigned Count=16, Capacity=64, ScratchOffset=2176, RecordFlag=4;
constexpr uint32_t SP[]={0,1,2,4,5,6,24,25}, TP[]={0,1,6,7}, UCHE[]={0,1,4,8};
// Snapshot version identifies the immutable per-boot counter layout. Keep the
// original version/bytes for shader counters; old readers reject color data.
constexpr uint32_t ShaderLayout=1, ColorLayout=2, ColorSelection=1u<<16;
constexpr uint32_t ColorSP[]={0,1,6,24}, ColorRB[]={0,1,15,16}, ColorCCU[]={0,5,7,8};
// Compact boot option: low 16 bits are the existing one-based draw index;
// bit 16 selects color diagnostics. Reject unknown bits and a layout without
// a draw. No additional boot-argument bytes are needed for a second option.
inline bool decodeSelection(uint32_t value,uint32_t limit,uint32_t &operation,uint32_t &layout) {
    operation=0;layout=ShaderLayout;
    if((value&~(0xffffu|ColorSelection)) || (value&0xffffu)>limit ||
        ((value&ColorSelection) && !(value&0xffffu)))return false;
    operation=value&0xffffu;layout=(value&ColorSelection)?ColorLayout:ShaderLayout;
    return true;
}
struct Record {
    uint64_t retired=0,hash=0,ticks=0,values[Count]{};
    uint32_t sequence=0,index=0;
};
struct Snapshot {
    uint32_t version=ShaderLayout,bytes=sizeof(Snapshot),count=0,next=0;
    Record records[Capacity];
    void append(const Record &r) {records[next]=r;next=(next+1)%Capacity;if(count<Capacity)++count;}
};
static_assert(sizeof(Snapshot)==10256,"counter wire layout unchanged");
static_assert(ScratchOffset+Count*16<=4096,"counter snapshots fit private progress page");
} }
