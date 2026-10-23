// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "../../bus/QcomSMMU/ContextHandoff.h"
#include "../../bus/QcomSMMU/ContextTLB.h"

namespace Display { namespace MMU {
constexpr uint64_t Base=0x0cd00000,MMCCBase=0x0c8c0000,Controller=0x0c901000;
constexpr uint32_t Aperture=0x40000,MMCCBytes=0x40000,MaximumTables=256;
constexpr const char *Property="qcom,display-smmu-handoff";
constexpr const char *StatusProperty="qcom,display-smmu-status";
enum CaptureStatus:uint32_t { Disabled=0,ClocksUnavailable=1,UnsupportedProfile=2,ReservationFailed=3,Changed=4,Ready=5,
    Initialized=6,QuiesceFailed=7,InitializationFailed=8 };
// Native GIC interrupt IDs (SPI + 32), in hardware context-bank order.
constexpr uint32_t Interrupts[]={295,298,299,300,276,277,279,280,281,282,283,284,285,286,287,288,292,293,294,304};
inline SMMU::IORTTopology topology() {
    SMMU::IORTTopology out;out.base=Base;out.span=Aperture;out.stream=0;out.addressBits=40;
    out.contextCount=sizeof(Interrupts)/sizeof(Interrupts[0]);
    for(unsigned n=0;n<out.contextCount;++n)out.interrupts[n]=Interrupts[n];
    return out;
}
// Only this translated AArch64/4K stage-1 profile is admitted. A disabled
// stage-1 bank can be initialized only after a real DPU DMA halt in the bridge.
// Table-walk caching may be NC or WB; CPU table aliases remain EFI-verified WB
// and are explicitly cleaned to PoC before publication and invalidation.
inline bool format(const SMMU::ContextHandoff &c) {
    const auto route=topology();
    if(!c.valid(route,Controller,c.inputBits) || (c.sctlr&~0x1167u) || (c.sctlr&0x1027)!=0x1027 ||
        (c.mair0&255)!=0x44 || c.tcr2!=((7u<<15)|(1u<<4)|(c.outputBits==40?2:c.outputBits==36?1:0)))return false;
    const unsigned inner=(c.tcr>>8)&3,outer=(c.tcr>>10)&3,share=(c.tcr>>12)&3;
    return inner<=1 && outer<=1 && (share==2 || share==3) &&
        (c.tcr&~0x3f00u)==((1u<<23)|(64-c.inputBits));
}
inline bool overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m) {
    return n && m && a<=UINT64_MAX-n && b<=UINT64_MAX-m && a<b+m && b<a+n;
}
struct Handoff {
    enum:uint32_t { ExclusiveContext=1,BootTablesReserved=2,TablesPublished=4,LifetimeResourcesPinned=8,
        ProducerStopped=16,Required=31,WriteBackTables=1 };
    uint32_t version=1,bytes=sizeof(Handoff),flags=0,tableCount=0;
    uint64_t first=0,end=0;
    SMMU::ContextHandoff context;
    uint64_t framebuffer=0,deviceAddress=0,framebufferBytes=0;
    uint32_t cachePolicy=0,reserved=0;
    struct Table {uint64_t physical=0,digest=0;} tables[MaximumTables];
    bool valid() const {
        if(version!=1 || bytes!=sizeof(*this) || flags!=Required || !tableCount || tableCount>MaximumTables ||
            !format(context) || cachePolicy!=WriteBackTables || reserved || first<0x1000000 ||
            (first&4095) || (end&4095) || end<=first || end>0x100000000ULL || end-first<0x2000000 ||
            end-first>0x20000000 || !framebuffer || (framebuffer&4095) ||
            !framebufferBytes || (framebufferBytes&4095) || framebufferBytes>0x10000000 ||
            framebuffer>=(1ULL<<context.outputBits) || framebufferBytes>(1ULL<<context.outputBits)-framebuffer ||
            !deviceAddress || (deviceAddress&4095) || deviceAddress>=0x100000000ULL ||
            framebufferBytes>0x100000000ULL-deviceAddress || overlap(first,end-first,deviceAddress,framebufferBytes))return false;
        if(tables[0].physical!=(context.ttbr0&0x0000fffffffff000ULL))return false;
        for(unsigned n=0;n<MaximumTables;++n) {
            const auto &t=tables[n];
            if(n>=tableCount) {if(t.physical || t.digest)return false;continue;}
            if(!t.physical || (t.physical&4095) || t.physical>=(1ULL<<context.outputBits) ||
                overlap(t.physical&~0x3fffULL,16384,framebuffer,framebufferBytes))return false;
            for(unsigned j=0;j<n;++j)if(tables[j].physical==t.physical)return false;
        }
        return true;
    }
    struct Window {
        unsigned inputBits,outputBits;uint64_t ttbr,first,end;
        bool valid() const {return inputBits>=32 && inputBits<=40 && (outputBits==32 || outputBits==36 || outputBits==40) &&
            (ttbr&0x0000fffffffff000ULL) && !(ttbr&4095) && first>=0x1000000 && !(first&4095) &&
            !(end&4095) && first<end && end<=0x100000000ULL;}
    };
    Window window() const {return {context.inputBits,context.outputBits,context.ttbr0,first,end};}
};
static_assert(sizeof(Handoff)==4288 && offsetof(Handoff,context)==32 && offsetof(Handoff,tables)==192,"display retained SMMU ABI");
inline uint64_t tableDigest(volatile const uint64_t *entries) {
    uint64_t value=14695981039346656037ULL;
    for(unsigned n=0;n<512;++n) {auto word=entries[n];for(unsigned b=0;b<8;++b){value^=word&255;value*=1099511628211ULL;word>>=8;}}
    return value;
}
// Checks run before touching the SMMU aperture. These are retained clock/power
// observations, never an instruction to enable a collapsed bus in the kernel.
template<class IO> bool resourcesLive(IO &mmcc) {
    const auto power=mmcc.read(0xe020);
    if(power==UINT32_MAX || !(power&(1u<<31)))return false;
    const uint32_t offsets[]={0x5024,0xe004,0xe008};
    for(auto offset:offsets){const auto value=mmcc.read(offset);if(value==UINT32_MAX || !(value&1))return false;}
    return true;
}
template<class IO> bool streamsOwned(const SMMU::ContextHandoff &c,IO &io) {
    const auto groups=c.id0&255;
    if(!groups || groups>128)return false;
    for(unsigned n=0;n<groups;++n) {
        const auto smr=io.read(0x800+4*n),route=io.read(0xc00+4*n);
        if(smr==UINT32_MAX || route==UINT32_MAX)return false;
        if(n==c.slot) {if(smr!=c.smr || route!=c.s2cr)return false;continue;}
        if(!(smr&(1u<<31)))continue;
        if(!(route&0x30000) && (route&255)==c.bank)return false;
        if(!((smr^c.stream)&65535&~((smr>>16)&32767)))return false;
    }
    return true;
}
// Read-only capture for a live display. Unlike USB takeover, this neither
// claims reset isolation nor replaces the inherited translation format/root.
template<class IO> QcomPower::Status capture(IO &io,SMMU::ContextHandoff &out) {
    using namespace QcomPower;
    auto route=topology();out={};auto &c=out;
    c.controller=Controller;c.smmu=Base;c.aperture=Aperture;c.stream=route.stream;c.inputBits=32;c.outputBits=32;
    c.control=io.read(0);c.id0=io.read(0x20);c.id1=io.read(0x24);c.id2=io.read(0x28);
    if(!c.layoutValid(route,Controller,32,false))return Unsupported;
    bool found=false;
    for(unsigned n=0;n<(c.id0&255);++n) {
        const auto smr=io.read(0x800+4*n),s2cr=io.read(0xc00+4*n);
        if(smr==UINT32_MAX || s2cr==UINT32_MAX)return Failed;
        if(!(smr&(1u<<31)) || ((smr^c.stream)&65535&~((smr>>16)&32767)))continue;
        if(found || smr!=((1u<<31)|c.stream))return Denied;
        if(s2cr&0x30000)return Unsupported;
        c.slot=n;c.smr=smr;c.s2cr=s2cr;c.bank=s2cr&255;found=true;
    }
    if(!found || !c.layoutValid(route,Controller,32))return Unsupported;
    const auto cb=c.contextOffset(c.bank),gr1=c.globalOffset();
    c.sctlr=io.read(cb);c.cbar=io.read(gr1+4*c.bank);c.cba2r=io.read(gr1+0x800+4*c.bank);
    c.tcr=io.read(cb+0x30);c.tcr2=io.read(cb+0x10);c.contextID=io.read(cb+0x34);
    c.mair0=io.read(cb+0x38);c.mair1=io.read(cb+0x3c);c.ttbr0=io.read64(cb+0x20);c.ttbr1=io.read64(cb+0x28);
    c.asid=unsigned(c.ttbr0>>48);c.inputBits=64-(c.tcr&63);
    const unsigned sizes[]={32,36,40};if((c.tcr2&7)>=3)return Unsupported;c.outputBits=sizes[c.tcr2&7];
    if(!format(c))return Unsupported;
    return c.registersUnchanged(io) && streamsOwned(c,io) && !SMMU::contextFault(io.read(cb+0x58))?OK:Stale;
}
// Read-only eligibility for a fresh root. Preserve the exact existing SID,
// bank and VMID. This does not grant ownership or make DMA safe to restart.
template<class IO> QcomPower::Status captureDisabled(IO &io,SMMU::ContextHandoff &out) {
    using namespace QcomPower;
    auto route=topology();out={};auto &c=out;
    c.controller=Controller;c.smmu=Base;c.aperture=Aperture;c.stream=route.stream;c.inputBits=32;c.outputBits=32;
    c.control=io.read(0);c.id0=io.read(0x20);c.id1=io.read(0x24);c.id2=io.read(0x28);
    if(!c.layoutValid(route,Controller,32,false))return Unsupported;
    const unsigned widths[]={32,36,40,42,44,48,52};
    const auto pa=widths[c.id2&15]<widths[(c.id2>>4)&15]?widths[c.id2&15]:widths[(c.id2>>4)&15];
    c.outputBits=pa>=40?40:pa>=36?36:32;
    bool found=false;
    for(unsigned n=0;n<(c.id0&255);++n) {
        const auto smr=io.read(0x800+4*n),s2cr=io.read(0xc00+4*n);
        if(smr==UINT32_MAX || s2cr==UINT32_MAX)return Failed;
        if(!(smr&(1u<<31)) || ((smr^c.stream)&65535&~((smr>>16)&32767)))continue;
        if(found || smr!=((1u<<31)|c.stream) || (s2cr&0x30000))return Denied;
        c.slot=n;c.smr=smr;c.s2cr=s2cr;c.bank=s2cr&255;found=true;
    }
    if(!found || !c.layoutValid(route,Controller,32))return Unsupported;
    const auto cb=c.contextOffset(c.bank),gr1=c.globalOffset();
    c.sctlr=io.read(cb);c.cbar=io.read(gr1+4*c.bank);c.cba2r=io.read(gr1+0x800+4*c.bank);
    c.tcr=io.read(cb+0x30);c.tcr2=io.read(cb+0x10);c.contextID=io.read(cb+0x34);
    c.mair0=io.read(cb+0x38);c.mair1=io.read(cb+0x3c);c.ttbr0=io.read64(cb+0x20);c.ttbr1=io.read64(cb+0x28);
    if(c.sctlr || c.ttbr0 || c.ttbr1 || c.contextID || c.tcr || c.tcr2 || c.mair0 || c.mair1 ||
        c.cba2r!=1 || ((c.cbar>>16)&3)!=1 || SMMU::contextFault(io.read(cb+0x58)))return Unsupported;
    return c.registersUnchanged(io) && streamsOwned(c,io)?OK:Stale;
}
} }
