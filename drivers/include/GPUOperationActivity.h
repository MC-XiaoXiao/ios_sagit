/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

namespace GPU { namespace OperationActivity {
constexpr uint32_t Version=4, BucketCapacity=128, SlowCapacity=1024, TopCapacity=64, ScratchOffset=128, CommandCapacity=64;
constexpr char Property[]="GPUOperationActivity";
constexpr uint64_t SlowTicks=19200; // One millisecond on the GPU's 19.2 MHz reference clock.
struct Program {
    uint64_t hash=0;
    uint32_t words=0,instructions=0,constants=0,fullRegs=0,halfRegs=0,flags=0,framebufferMask=0,stage=0;
    char name[96]{};
};
// Copy trusted compiler metadata while its owning client/program is retained.
// This diagnostic FNV identity is neither a cache key nor an authorization.
// All pointer fields and the client-local id are excluded from the hash.
template<class P> inline void describe(Program &out,const P &p) {
    out={};uint64_t hash=14695981039346656037ULL;
    auto bytes=[&](const void *memory,unsigned count) {
        const auto *data=static_cast<const uint8_t *>(memory);
        for(unsigned i=0;i<count;++i){hash^=data[i];hash*=1099511628211ULL;}
    };
    const auto *begin=reinterpret_cast<const uint8_t *>(&p.stage);
    const auto *end=reinterpret_cast<const uint8_t *>(p.constantWindows)+sizeof(p.constantWindows);
    bytes(begin,unsigned(end-begin));
    if(p.code && p.codeWords)bytes(p.code,p.codeWords*4);
    if(p.immediates && p.immediateCount)bytes(p.immediates,p.immediateCount*4);
    out.hash=hash;out.words=p.codeWords;out.instructions=p.instrlen;out.constants=p.constlen;
    out.fullRegs=p.fullRegs;out.halfRegs=p.halfRegs;out.flags=p.flags;out.framebufferMask=p.framebufferMask;out.stage=p.stage;
    if(p.name)for(unsigned i=0;i+1<sizeof(out.name) && p.name[i];++i) {
        // Keep line-oriented diagnostics parseable; no pointer/string ownership escapes.
        const auto c=static_cast<unsigned char>(p.name[i]);out.name[i]=c>32 && c<127 && c!='='?char(c):'_';
    }
}
// One synchronous encoding pass, under the GPU gate: program objects cannot
// change or disappear, and the fixed operation-record array stays in place.
// Reuse its descriptions instead of rehashing the same ISA on every draw.
// No identity or borrowed pointer survives into the next batch.
class ProgramDescriptions {
    struct Entry {const void *source=nullptr;const Program *description=nullptr;};
    Entry entries[CommandCapacity*2]{};
    unsigned count=0;
public:
    template<class P> bool describe(Program &out,const P &program) {
        for(unsigned i=0;i<count;++i)if(entries[i].source==&program) {
            out=*entries[i].description;return true;
        }
        GPU::OperationActivity::describe(out,program);
        if(count<CommandCapacity*2)entries[count++]={&program,&out};
        return false;
    }
};
struct Description {
    Program first,second;
    uint32_t opcode=0,firstID=0,secondID=0,count=0,instances=0,primitive=0,indexSize=0,width=0,height=0,scissor[4]{};
    // Uses the previous alignment padding: count in bits 8..15, actual
    // admitted LOAD_STATE window mask in bits 0..7. No resource addresses.
    uint32_t constantWindowState=0;
    // CPU-validated dispatch/layout only; never expose resource addresses.
    uint32_t groups[3]{},local[3]{},grid[3]{},blitParameters[8]{};
    uint32_t blitKind=0,blitSourceBytes=0,blitDestinationBytes=0;
    uint64_t fullSetups=0,reused=0,inplace=0,snapshots=0,flushes=0,copyBytes=0;
};
enum : uint32_t {Timed=1,Compositor=2};
struct Record {
    Description description;
    uint64_t begin=0,end=0,retired=0;
    uint32_t sequence=0,stamp=0,index=0,flags=0;
};
struct Counts {
    uint64_t samples=0,ticks=0,maximum=0,invalid=0,skipped=0,primitives=0,flushes=0,copyBytes=0;
    void add(const Record &r,bool valid) {
        if(!(r.flags&Timed)){++skipped;return;}
        if(!valid){++invalid;return;}
        const auto span=r.end-r.begin;++samples;ticks+=span;if(span>maximum)maximum=span;
        primitives+=r.description.fullSetups+r.description.reused;flushes+=r.description.flushes;copyBytes+=r.description.copyBytes;
    }
};
struct Bucket {Description first;Counts native,compositor;};
struct Snapshot {
    uint32_t version=Version,bytes=sizeof(Snapshot),enabled=0,count=0;
    uint64_t sequence=0,total=0,overflow=0,slowTotal=0,failedJobs=0,allocationSkips=0,scratchSkips=0;
    uint32_t slowCount=0,slowNext=0,topCount=0,reserved=0;
    Bucket buckets[BucketCapacity];Record slow[SlowCapacity],top[TopCapacity];
    void append(const Record &r,uint64_t jobBegin,uint64_t jobEnd) {
        ++total;
        const bool valid=jobBegin && r.begin>=jobBegin && r.end>=r.begin && r.end<=jobEnd;
        unsigned i=0;
        for(;i<count;++i)if(buckets[i].first.opcode==r.description.opcode &&
            buckets[i].first.first.hash==r.description.first.hash && buckets[i].first.second.hash==r.description.second.hash)break;
        if(i==count) {
            if(count==BucketCapacity){++overflow;return;}
            buckets[i].first=r.description;++count;
        }
        buckets[i].native.add(r,valid);if(r.flags&Compositor)buckets[i].compositor.add(r,valid);
        if(valid && (r.flags&Timed) && r.end-r.begin>=SlowTicks) {
            slow[slowNext]=r;slowNext=(slowNext+1)%SlowCapacity;++slowTotal;if(slowCount<SlowCapacity)++slowCount;
            // Retain the largest real intervals as well as a chronological
            // suffix. A burst of later small operations must not erase the
            // very long operation this diagnostic exists to identify.
            unsigned position=0;
            while(position<topCount && top[position].end-top[position].begin>=r.end-r.begin)++position;
            if(position<TopCapacity) {
                if(topCount<TopCapacity)++topCount;
                for(unsigned n=topCount-1;n>position;--n)top[n]=top[n-1];
                top[position]=r;
            }
        }
    }
    const Record &oldest(unsigned i) const {return slow[(slowNext+SlowCapacity-slowCount+i)%SlowCapacity];}
};
static_assert(ScratchOffset+CommandCapacity*16<=4096,"operation timestamp cells must fit private progress page");
static_assert(sizeof(Snapshot)<640*1024,"bounded CPU-only profiling snapshot");
} }
