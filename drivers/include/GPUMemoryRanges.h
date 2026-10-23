// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUUserProtocol.h>
namespace GPU {
struct BackingPage { uint64_t physical, offset; };
// Shared CPU/native byte-range arithmetic. Texture views use one span per
// slice and retain the original stride, excluding unexposed mips between them.
struct StridedRange { uint64_t begin=0,bytes=0,stride=0,count=1; };
inline bool validStridedRange(const StridedRange &r,uint64_t length) {
    return r.count && r.bytes && range(length,r.begin,r.bytes) &&
        (r.count==1 || (r.stride>=r.bytes && r.stride<=(length-r.begin-r.bytes)/(r.count-1)));
}
inline bool stridedOverlap(const StridedRange &a,const StridedRange &b) {
    uint64_t i=0,j=0;
    while(i<a.count && j<b.count) {
        uint64_t x=a.begin+i*a.stride,y=b.begin+j*b.stride;
        if(x<y+b.bytes && y<x+a.bytes)return true;
        if(x+a.bytes<=y)++i;else ++j;
    }
    return false;
}
inline bool stridedPageOverlap(const StridedRange &a,uint64_t pa,const StridedRange &b,uint64_t pb) {
    uint64_t i=a.count>1 && pa>=a.begin?(pa-a.begin)/a.stride:0;
    uint64_t j=b.count>1 && pb>=b.begin?(pb-b.begin)/b.stride:0;
    while(i<a.count && j<b.count) {
        uint64_t x=a.begin+i*a.stride,y=b.begin+j*b.stride;
        if(x>=pa+PageBytes || y>=pb+PageBytes)break;
        uint64_t lo=x>pa?x-pa:0,hi=x+a.bytes>pa?x+a.bytes-pa:0;
        uint64_t qlo=y>pb?y-pb:0,qhi=y+b.bytes>pb?y+b.bytes-pb:0;
        if(hi>PageBytes)hi=PageBytes;
        if(qhi>PageBytes)qhi=PageBytes;
        if(lo<hi && qlo<qhi && lo<qhi && qlo<hi)return true;
        if(hi<=qlo || lo>=hi)++i;else ++j;
    }
    return false;
}
// Skip runs of lower physical addresses without scanning every pinned page.
// Exponential search keeps interleaved page sets linear and sparse matches
// logarithmic. Byte/slice overlap is still checked at every matching page.
inline uint32_t seekBackingPage(const BackingPage *pages,uint32_t count,uint32_t current,uint64_t physical) {
    uint32_t first=current+1,last=first,step=1;
    while(last<count && pages[last].physical<physical) {
        first=last+1;
        if(step>=count-last){last=count;break;}
        last+=step;step*=2;
    }
    while(first<last) {
        const uint32_t middle=first+(last-first)/2;
        if(pages[middle].physical<physical)first=middle+1;else last=middle;
    }
    return first;
}
inline bool backingOverlap(const BackingPage *a,uint32_t na,const StridedRange &range,
    const BackingPage *b,uint32_t nb,const StridedRange &other) {
    if(!a || !b || !na || !nb)return true;
    if(a[na-1].physical<b[0].physical || b[nb-1].physical<a[0].physical)return false;
    uint32_t i=0,j=0;
    while(i<na && j<nb) {
        if(a[i].physical<b[j].physical){i=seekBackingPage(a,na,i,b[j].physical);continue;}
        if(a[i].physical>b[j].physical){j=seekBackingPage(b,nb,j,a[i].physical);continue;}
        if(stridedPageOverlap(range,a[i].offset,other,b[j].offset))return true;
        ++i;++j;
    }
    return false;
}
inline bool sortBackingPages(BackingPage *pages,uint32_t count) {
    if (!pages || !count || count>MaxBufferBytes/PageBytes) return false;
    auto sift = [&](uint32_t start,uint32_t size) {
        for (uint32_t i=start;i<size/2;) {
            uint32_t child=i*2+1;
            if (child+1<size && pages[child].physical<pages[child+1].physical) ++child;
            if (pages[i].physical>=pages[child].physical) break;
            auto value=pages[i];pages[i]=pages[child];pages[child]=value;i=child;
        }
    };
    for (uint32_t i=count/2;i;--i) sift(i-1,count);
    for (uint32_t n=count;n>1;--n) {
        auto value=pages[0];pages[0]=pages[n-1];pages[n-1]=value;sift(0,n-1);
    }
    for (uint32_t i=0;i<count;++i) if (!pages[i].physical ||
        (pages[i].physical&(PageBytes-1)) || (pages[i].offset&(PageBytes-1)) ||
        pages[i].offset>=uint64_t(count)*PageBytes || (i && pages[i-1].physical==pages[i].physical)) return false;
    return true;
}
// Inputs are sorted, pinned physical-page indexes; byte ranges have already
// been checked against their descriptors. No assumptions about physical order
// in a descriptor, resource IDs, or different GPU virtual mappings are made.
inline bool backingOverlap(const BackingPage *a,uint32_t na,uint64_t begin,uint64_t end,
    const BackingPage *b,uint32_t nb,uint64_t otherBegin,uint64_t otherEnd) {
    if (!a || !b || !na || !nb || begin>=end || otherBegin>=otherEnd) return true;
    if(a[na-1].physical<b[0].physical || b[nb-1].physical<a[0].physical)return false;
    uint32_t i=0,j=0;
    while (i<na && j<nb) {
        const auto &p=a[i],&q=b[j];
        if (p.physical<q.physical) {i=seekBackingPage(a,na,i,q.physical);continue;}
        if (p.physical>q.physical) {j=seekBackingPage(b,nb,j,p.physical);continue;}
        uint64_t lo=begin>p.offset?begin-p.offset:0,hi=end>p.offset?end-p.offset:0;
        uint64_t qlo=otherBegin>q.offset?otherBegin-q.offset:0,qhi=otherEnd>q.offset?otherEnd-q.offset:0;
        if (hi>PageBytes) hi=PageBytes;
        if (qhi>PageBytes) qhi=PageBytes;
        if (lo<hi && qlo<qhi && lo<qhi && qlo<hi) return true;
        ++i;++j;
    }
    return false;
}
}
