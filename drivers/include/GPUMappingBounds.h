// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "GPUUserProtocol.h"
#include "GPUDiagnostics.h"

namespace GPU { namespace MappingBounds {
struct PageSpan {uint64_t first=0,end=0;};
// The plan lives in the synchronous CPU encoding workspace, not the kernel
// stack or an in-flight GPU descriptor. Overflow retains the prior whole-root
// mapping for affected resources; it never rejects otherwise valid work.
template<unsigned Capacity> struct Ranges {
    uint64_t knownMask=0,overflowMask=0;
    unsigned count=0;
    PageSpan spans[Capacity]{};
    template<class Buffer> bool recordAddress(Buffer *const *buffers,unsigned bufferCount,uint64_t address,uint64_t bytes) {
        if(!buffers || !bytes || bufferCount>64 || bytes>UINT64_MAX-address)return false;
        uint64_t matched=0;
        for(unsigned i=0;i<bufferCount;++i) {
            const auto *buffer=buffers[i];if(!buffer || buffer->length>UINT64_MAX-buffer->address)return false;
            if(address>=buffer->address && GPU::range(buffer->length,address-buffer->address,bytes))matched|=uint64_t(1)<<i;
        }
        if(!matched || address+bytes>UINT64_MAX-4095)return false;
        knownMask|=matched;
        PageSpan next{address&~uint64_t(4095),(address+bytes+4095)&~uint64_t(4095)};
        unsigned first=0;
        while(first<count && spans[first].end<next.first)++first;
        unsigned last=first;
        while(last<count && spans[last].first<=next.end) {
            if(spans[last].first<next.first)next.first=spans[last].first;
            if(spans[last].end>next.end)next.end=spans[last].end;
            ++last;
        }
        if(first==last) {
            if(count==Capacity){overflowMask|=matched;return true;}
            for(unsigned i=count;i>first;--i)spans[i]=spans[i-1];
            ++count;
        } else {
            const auto removed=last-first-1;
            for(unsigned i=last;i<count;++i)spans[i-removed]=spans[i];
            count-=removed;
        }
        spans[first]=next;return true;
    }
    bool known(unsigned i) const {return i<64 && (knownMask&(uint64_t(1)<<i));}
    bool overflow(unsigned i) const {return i<64 && (overflowMask&(uint64_t(1)<<i));}
    bool valid(unsigned bufferCount) const {
        if(bufferCount>64 || count>Capacity || (overflowMask&~knownMask))return false;
        const uint64_t mask=bufferCount==64?UINT64_MAX:(uint64_t(1)<<bufferCount)-1;
        if(knownMask&~mask)return false;
        for(unsigned i=0;i<count;++i)
            if(!spans[i].first || (spans[i].first&4095) || (spans[i].end&4095) || spans[i].first>=spans[i].end ||
                (i && spans[i-1].end>=spans[i].first))return false;
        return true;
    }
};
using Plan=Ranges<256>;
inline bool validView(uint64_t rootAddress,uint64_t rootBytes,uint64_t viewAddress,uint64_t viewBytes) {
    const auto span=GPU::pageSpanBytes(rootBytes);
    if(!rootAddress || (rootAddress&4095) || !span ||
        span>UINT64_MAX-rootAddress || viewAddress<rootAddress || viewAddress-rootAddress>=rootBytes ||
        !viewBytes || viewBytes>rootBytes-(viewAddress-rootAddress))return false;
    return true;
}
inline bool intersect(const PageSpan &span,uint64_t rootAddress,uint64_t rootBytes,PageSpan &out) {
    out={};if(!rootBytes || rootBytes>UINT64_MAX-rootAddress)return false;
    out={span.first>rootAddress?span.first:rootAddress,span.end<rootAddress+rootBytes?span.end:rootAddress+rootBytes};
    return out.first<out.end;
}
struct RootSelection {
    uint64_t address=0,bytes=0;
    bool fallback=false,overflow=false;
};
// Callers validate the complete immutable plan once before selecting roots.
// ANY unknown/overflowed view of a shared root preserves its old full mapping.
template<unsigned Capacity,class Buffer> bool selectRoot(const Ranges<Capacity> *plan,Buffer *const *buffers,
    unsigned count,unsigned index,RootSelection &out) {
    out={};if(!buffers || count>64 || index>=count || !buffers[index])return false;
    const auto *root=buffers[index]->backing();if(!root)return false;
    out={root->address,root->length,!plan,false};
    for(unsigned i=0;i<count;++i) {
        if(!buffers[i])return false;
        if(buffers[i]->backing()!=root)continue;
        if(!validView(out.address,out.bytes,buffers[i]->address,buffers[i]->length))return false;
        if(plan){out.fallback|=!plan->known(i);out.overflow|=plan->overflow(i);}
    }
    out.fallback|=out.overflow;return true;
}
template<unsigned Capacity,class Emit> bool visitSelected(const Ranges<Capacity> *plan,const RootSelection &root,Emit emit) {
    if(!validView(root.address,root.bytes,root.address,root.bytes))return false;
    const auto span=GPU::pageSpanBytes(root.bytes);
    if(root.fallback)return emit(PageSpan{root.address,root.address+span});
    if(!plan)return false;
    bool found=false;
    for(unsigned i=0;i<plan->count;++i) {
        PageSpan part;if(!intersect(plan->spans[i],root.address,span,part))continue;
        found=true;if(!emit(part))return false;
    }
    return found;
}
struct Work {
    uint64_t jobs=0,roots=0,fullPages=0,mappedPages=0,fallbackRoots=0,overflowRoots=0;
    void add(const Work &other) {
        Diagnostics::add(jobs,other.jobs);Diagnostics::add(roots,other.roots);
        Diagnostics::add(fullPages,other.fullPages);Diagnostics::add(mappedPages,other.mappedPages);
        Diagnostics::add(fallbackRoots,other.fallbackRoots);
        Diagnostics::add(overflowRoots,other.overflowRoots);
    }
};
constexpr uint32_t Version=1;
constexpr const char *Property="GPUJobMappingWork";
struct Snapshot {
    uint32_t version=Version,bytes=112;
    uint64_t sequence=0;
    Work native,compositor;
};
static_assert(sizeof(Plan)<=8192 && sizeof(Snapshot)==112,"bounded working set and optional statistics ABI");
} }
