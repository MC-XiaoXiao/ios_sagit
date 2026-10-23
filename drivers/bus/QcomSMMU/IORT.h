// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace SMMU {
// ACPI IORT revision 0, named component -> ARM SMMUv2. This is topology,
// never a context-ownership, physical-DMA or cache-coherency authorization.
// Read only the checksummed table installed by firmware, not a raw AML file.
struct IORTTopology {
    uint64_t base=0,span=0;
    uint32_t stream=0,model=0,flags=0,addressBits=0,coherent=0;
    uint32_t contextCount=0,interrupts[128]{},interruptFlags[128]{};
};
// IORT bit0 describes edge (1), while AppleARMGIC uses level (1). A
// per-device mapper cannot acknowledge another bank's shared fault line.
inline bool exclusiveContextInterrupt(const IORTTopology &route,unsigned bank) {
    if (!route.contextCount || route.contextCount>128 || bank>=route.contextCount ||
        route.interrupts[bank]<32 || route.interrupts[bank]>=1020 || route.interruptFlags[bank]>1) return false;
    for (unsigned n=0;n<route.contextCount;++n)
        if (n!=bank && route.interrupts[n]==route.interrupts[bank]) return false;
    return true;
}
inline bool contextInterruptMatches(const IORTTopology &route,unsigned bank,uint32_t irq,uint32_t trigger) {
    return exclusiveContextInterrupt(route,bank) && irq==route.interrupts[bank] &&
        trigger==(route.interruptFlags[bank] ? 0u:1u);
}
// Backend checks controller identity, native specifier size and the actual
// provider getInterruptType result. Index0 belongs to DWC3, not the mapper.
template<class Interrupts> bool contextInterruptList(const IORTTopology &route,Interrupts &lines,unsigned &count) {
    count=0;
    if (!route.contextCount || route.contextCount>128) return false;
    const auto total=lines.count();
    if (total!=2 && total!=1+route.contextCount) return false;
    uint32_t coreIRQ=0,coreTrigger=0;
    if (!lines.read(0,coreIRQ,coreTrigger) || coreIRQ<32 || coreIRQ>=1020 || coreTrigger>1) return false;
    for (unsigned n=1;n<total;++n) {
        uint32_t irq=0,trigger=0;
        if (!lines.read(n,irq,trigger) || irq==coreIRQ) return false;
        bool known=false;
        for (unsigned bank=0;bank<route.contextCount;++bank)
            if ((total==2 || n==1+bank) && contextInterruptMatches(route,bank,irq,trigger)) known=true;
        if (!known) return false;
    }
    count=total;return true;
}
template<class Interrupts> bool contextInterruptRoute(const IORTTopology &route,Interrupts &lines,unsigned bank,unsigned &index) {
    index=0;unsigned count=0;
    if (!exclusiveContextInterrupt(route,bank) || !contextInterruptList(route,lines,count)) return false;
    const unsigned selected=count==2 ? 1:1+bank;
    uint32_t irq=0,trigger=0;
    if (!lines.read(selected,irq,trigger) || !contextInterruptMatches(route,bank,irq,trigger)) return false;
    index=selected;return true;
}
struct ContextInterruptBinding {
    unsigned bank=0,index=0;
    uint32_t irq=0,trigger=0;
    template<class Interrupts> bool capture(const IORTTopology &route,Interrupts &lines,unsigned selected) {
        *this={};unsigned source=0;
        if (!contextInterruptRoute(route,lines,selected,source)) return false;
        bank=selected;index=source;irq=route.interrupts[selected];trigger=route.interruptFlags[selected] ? 0:1;
        return true;
    }
    template<class Interrupts> bool valid(const IORTTopology &route,Interrupts &lines) const {
        unsigned source=0;
        return contextInterruptMatches(route,bank,irq,trigger) &&
            contextInterruptRoute(route,lines,bank,source) && source==index;
    }
};
enum class IORTResult { Success, Invalid, Unsupported, NotFound };
constexpr unsigned IORTMaximumBytes=65536;
namespace iort_detail {
inline uint32_t word(const uint8_t *p) {
    return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;
}
inline uint64_t pair(const uint8_t *p) { return word(p)|(uint64_t(word(p+4))<<32); }
inline bool range(unsigned offset,unsigned count,unsigned stride,unsigned bytes) {
    return offset<=bytes && stride && count<=(bytes-offset)/stride;
}
inline bool equal(const uint8_t *text,unsigned bytes,const char *name) {
    for (unsigned i=0;i<bytes;++i) {
        if (text[i]!=uint8_t(name[i])) return false;
        if (!text[i]) return true;
    }
    return false;
}
}
inline IORTResult parseIORT(const void *buffer,size_t available,const char *component,
    uint32_t inputID,IORTTopology &out) {
    out={};
    if (!buffer || !component || component[0]!='\\' || available<48) return IORTResult::Invalid;
    unsigned nameBytes=0;
    while (component[nameBytes]) {
        if (nameBytes==64 || uint8_t(component[nameBytes])<32 || uint8_t(component[nameBytes])>126)
            return IORTResult::Invalid;
        ++nameBytes;
    }
    const auto *table=static_cast<const uint8_t *>(buffer);
    using namespace iort_detail;
    const unsigned length=word(table+4),count=word(table+36),first=word(table+40);
    if (word(table)!=0x54524f49 || length<48 || length>IORTMaximumBytes || length>available ||
        !count || count>256 || first<48 || first>length || word(table+44)) return IORTResult::Invalid;
    uint8_t checksum=0;
    for (unsigned i=0;i<length;++i) checksum=uint8_t(checksum+table[i]);
    if (checksum) return IORTResult::Invalid;
    if (table[8]!=0) return IORTResult::Unsupported;
    unsigned offsets[256]{},sizes[256]{},named=256;
    unsigned cursor=first;
    for (unsigned i=0;i<count;++i) {
        if (!range(cursor,16,1,length)) return IORTResult::Invalid;
        const auto *node=table+cursor;
        const unsigned bytes=unsigned(node[1])|unsigned(node[2])<<8;
        const unsigned mappings=word(node+8),mappingOffset=word(node+12);
        if (bytes<16 || !range(cursor,bytes,1,length) ||
            (mappings && (mappingOffset<16 || !range(mappingOffset,mappings,20,bytes))))
            return IORTResult::Invalid;
        offsets[i]=cursor;sizes[i]=bytes;
        if (node[0]==1) {
            const unsigned end=mappings ? mappingOffset:bytes;
            if (end<30) return IORTResult::Invalid;
            bool terminated=false;
            for (unsigned n=29;n<end;++n) if (!node[n]) { terminated=true;break; }
            if (!terminated) return IORTResult::Invalid;
            if (equal(node+29,end-29,component)) {
                if (named!=256) return IORTResult::Invalid;
                named=i;
            }
        }
        cursor+=bytes;
    }
    if (cursor!=length) return IORTResult::Invalid;
    if (named==256) return IORTResult::NotFound;
    const auto *node=table+offsets[named];
    // No PASID, coherency override, memory hints or unknown node revision is
    // interpreted as permission to alter a translator or its global policy.
    if (node[3] || word(node+16) || word(node+20)>1 || node[24] || node[25] ||
        node[26] || node[27] || node[28]<32 || node[28]>64) return IORTResult::Unsupported;
    const unsigned mappings=word(node+8),mappingOffset=word(node+12);
    unsigned selected=256;uint32_t stream=0;
    for (unsigned i=0;i<mappings;++i) {
        const auto *mapping=node+mappingOffset+20*i;
        const uint32_t input=word(mapping),last=word(mapping+4),flags=word(mapping+16);
        if (flags&~1u || last>UINT32_MAX-input) return IORTResult::Invalid;
        if (!(flags&1) && (inputID<input || inputID-input>last)) continue;
        const uint64_t output=uint64_t(word(mapping+8))+((flags&1) ? 0:inputID-input);
        if (selected!=256 || output>65535) return IORTResult::Invalid;
        const unsigned reference=word(mapping+12);
        for (unsigned n=0;n<count;++n) if (offsets[n]==reference) selected=n;
        if (selected==256) return IORTResult::Invalid;
        stream=uint32_t(output);
    }
    if (selected==256) return IORTResult::NotFound;
    const auto *smmu=table+offsets[selected];const unsigned bytes=sizes[selected];
    if (smmu[0]!=3 || smmu[3] || bytes<60) return IORTResult::Unsupported;
    const auto base=pair(smmu+16),span=pair(smmu+24);
    const unsigned model=word(smmu+32),flags=word(smmu+36),global=word(smmu+40);
    const unsigned contexts=word(smmu+44),irqs=word(smmu+48),pmus=word(smmu+52),pmu=word(smmu+56);
    const unsigned routeCount=word(smmu+8),routeOffset=word(smmu+12);
    if (routeCount && routeOffset<60) return IORTResult::Invalid;
    // The supported terminal SMMU must not describe another translation
    // stage. Its ID mappings can describe output IDs but cannot reference a
    // further node; those graphs need an explicitly implemented walker.
    for (unsigned n=0;n<routeCount;++n)
        if (word(smmu+routeOffset+20*n+12)) return IORTResult::Unsupported;
    if ((model!=1 && model!=3 && model!=4) || (flags&~3u) || !base ||
        ((base|span)&4095) || span<0x1000 || span>0x40000 || base>UINT64_MAX-span ||
        !contexts || contexts>128 || irqs<60 || !range(irqs,contexts,8,bytes) ||
        global<60 || !range(global,2,8,bytes) || pmus>128 ||
        (pmus && (pmu<60 || !range(pmu,pmus,8,bytes)))) return IORTResult::Invalid;
    const unsigned starts[]={global,irqs,pmu,routeOffset};
    const unsigned ends[]={global+16,irqs+contexts*8,pmu+pmus*8,routeOffset+routeCount*20};
    for (unsigned i=0;i<4;++i) for (unsigned n=0;n<i;++n)
        if (starts[i]!=ends[i] && starts[n]!=ends[n] && starts[i]<ends[n] && starts[n]<ends[i])
            return IORTResult::Invalid;
    IORTTopology candidate{};
    candidate.base=base;candidate.span=span;candidate.stream=stream;
    candidate.model=model;candidate.flags=flags;candidate.addressBits=node[28];candidate.coherent=word(node+20);
    candidate.contextCount=contexts;
    for (unsigned n=0;n<contexts;++n) {
        const auto irq=word(smmu+irqs+n*8),irqFlags=word(smmu+irqs+n*8+4);
        if (irq<32 || irq>=1020 || (irqFlags&~1u)) return IORTResult::Invalid;
        candidate.interrupts[n]=irq;candidate.interruptFlags[n]=irqFlags;
    }
    out=candidate;return IORTResult::Success;
}
}
