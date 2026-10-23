// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "IORT.h"
#include "ContextTLB.h"
#include <QcomPowerHardware.h>

namespace SMMU {
constexpr const char *BypassHandoffProperty="qcom,usb-smmu-bypass";
enum class BypassMode : uint32_t { Architected=1, DisabledContext=2, UnmatchedStream=3 };
// v1 observes an exact architected S2CR BYPASS route. v2 observes an exact
// translated route into an exclusive, disabled stage-1/S2-bypass bank. It
// v3 observes no matching stream and sCR0.USFCFG=0 (ARM IHI0062D.c2.3.6).
// It never creates a route or changes global policy. The owner retains the
// fabric and observation; a reference SID or missing iommus is not proof.
struct BypassHandoff {
    uint32_t version=1,bytes=64;
    uint64_t controller=0,smmu=0;
    uint32_t aperture=0,stream=0,slot=0,addressBits=0;
    uint32_t control=0,id0=0,id1=0,id2=0,smr=0,s2cr=0;
    uint32_t bank=0,sctlr=0,cbar=0,cba2r=0;
    bool disabledContext() const {return version==2;}
    bool unmatchedStream() const {return version==3;}
    unsigned contexts() const {const unsigned n=id1&255;return n==13 ? 12u:n;}
    uint32_t globalOffset() const {return 1u<<((id1>>31) ? 16:12);}
    uint32_t contextOffset() const {
        return ((1u<<(((id1>>28)&7)+1))+bank)<<((id1>>31) ? 16:12);
    }
    bool same(const BypassHandoff &other) const {
        const auto *a=reinterpret_cast<const uint8_t *>(this),*b=reinterpret_cast<const uint8_t *>(&other);
        for(unsigned n=0;n<sizeof(*this);++n)if(a[n]!=b[n])return false;
        return true;
    }
    bool layoutValid() const {
        if((version!=1 && version!=2 && version!=3) || bytes!=(disabledContext() ? 80u:64u) ||
            (!disabledContext() && (bank || sctlr || cbar || cba2r)) ||
            !controller || (controller&4095) || !smmu || (smmu&4095) ||
            aperture<0x4000 || aperture>0x1000000 || addressBits<32 || addressBits>40 ||
            control==UINT32_MAX || (control&0x80000009u) || id0==UINT32_MAX || id1==UINT32_MAX || id2==UINT32_MAX ||
            !(id0&(1u<<27)) || !(id0&255) || (id0&255)>128 ||
            (unmatchedStream() ? slot!=UINT32_MAX:slot>=(id0&255)) ||
            stream>=1u<<((id0>>9)&15))return false;
        const unsigned pageShift=(id1>>31) ? 16:12,globalPages=1u<<(((id1>>28)&7)+1);
        const unsigned widths[]={32,36,40,42,44,48,52},ias=id2&15,oas=(id2>>4)&15;
        if(disabledContext() && (!(id0&(1u<<30)) || !contexts() || contexts()>globalPages ||
            bank>=contexts() || bank<((id1>>16)&255)))return false;
        return uint64_t(2)*globalPages*(1ULL<<pageShift)==aperture && ias<7 && oas<7 &&
            addressBits<=widths[ias] && addressBits<=widths[oas];
    }
    bool routeValid() const {
        if(unmatchedStream())return !(control&(1u<<10)) && !smr && !s2cr;
        if(smr!=((1u<<31)|stream) || s2cr==UINT32_MAX)return false;
        if(!disabledContext())return (s2cr&0x30000)==0x10000;
        // Match the architected disabled stage-1/S2-bypass configuration.
        // M=0 alone cannot exclude stage-2 translation, a stalled fault or
        // an inaccessible bank. No CB index, format or namespace is guessed.
        return (s2cr&0x300ff)==bank && !sctlr && cbar==0x10000 && cba2r<=1;
    }
    bool valid(const IORTTopology &topology,uint64_t actualController,unsigned bits) const {
        return layoutValid() && controller==actualController && smmu==topology.base && aperture==topology.span &&
            stream==topology.stream && addressBits==bits && bits<=topology.addressBits && !topology.coherent &&
            (!disabledContext() || (topology.contextCount<=128 && bank<topology.contextCount)) && routeValid();
    }
    template<class IO> bool bankUnchanged(IO &io) const {
        if(!disabledContext())return true;
        const auto cb=contextOffset(),gr1=globalOffset();
        // FSR.FORMAT[10:9] is independent of the fault state and may be
        // UNKNOWN for a disabled bank. ARM IHI0062D.c FaultSetIn masks only
        // that field; every remaining nonzero bit still refuses takeover.
        return io.read(cb)==sctlr && io.read(gr1+4*bank)==cbar &&
            io.read(gr1+0x800+4*bank)==cba2r && !(io.read(cb+0x58)&~ContextFormatMask);
    }
    template<class IO> bool unchanged(IO &io) const {
        if(!layoutValid() || !routeValid() ||
            io.accessible()!=QcomPower::OK || io.read(0)!=control || io.read(0x20)!=id0 ||
            io.read(0x24)!=id1 || io.read(0x28)!=id2)return false;
        for(unsigned n=0;n<(id0&255);++n) {
            const auto match=io.read(0x800+4*n),route=io.read(0xc00+4*n);
            if(match==UINT32_MAX || route==UINT32_MAX)return false;
            if(n==slot) {if(match!=smr || route!=s2cr)return false;continue;}
            if((match&(1u<<31)) && !((match^stream)&65535&~((match>>16)&0x7fff)))return false;
            if(disabledContext() && (match&(1u<<31)) && (route&0x300ff)==bank)return false;
        }
        return bankUnchanged(io) && io.accessible()==QcomPower::OK;
    }
    static bool decode(const void *data,size_t size,BypassHandoff &out) {
        out={};if(!data || (size!=64 && size!=80))return false;
        const auto *p=static_cast<const uint8_t *>(data);auto *q=reinterpret_cast<uint8_t *>(&out);
        for(unsigned n=0;n<size;++n)q[n]=p[n];
        return out.bytes==size && (((out.version==1 || out.version==3) && size==64) || (out.version==2 && size==80));
    }
};
static_assert(sizeof(BypassHandoff)==80 && offsetof(BypassHandoff,bank)==64,"USB observed SMMU bypass ABI");

template<class IO> QcomPower::Status captureBypass(IO &io,const IORTTopology &topology,
    uint64_t controller,unsigned bits,BypassHandoff &out,BypassMode mode=BypassMode::Architected) {
    using namespace QcomPower;
    out={};
    if((mode!=BypassMode::Architected && mode!=BypassMode::DisabledContext && mode!=BypassMode::UnmatchedStream) ||
        !controller || (controller&4095) || !topology.base || (topology.base&4095) ||
        topology.span<0x4000 || topology.span>0x1000000 || bits<32 || bits>40 || bits>topology.addressBits || topology.coherent)return Invalid;
    auto status=io.accessible();if(status!=OK)return status;
    status=io.isolated();if(status!=OK)return status;
    BypassHandoff h;h.controller=controller;h.smmu=topology.base;h.aperture=topology.span;
    if(mode==BypassMode::UnmatchedStream) {h.version=3;h.slot=UINT32_MAX;}
    h.stream=topology.stream;h.addressBits=bits;h.control=io.read(0);h.id0=io.read(0x20);h.id1=io.read(0x24);h.id2=io.read(0x28);
    if(!h.layoutValid())return Unsupported;
    uint32_t matches[128]{},routes[128]{};bool found=false;
    for(unsigned n=0;n<(h.id0&255);++n) {
        const auto match=io.read(0x800+4*n),route=io.read(0xc00+4*n);
        if(match==UINT32_MAX || route==UINT32_MAX)return Failed;
        matches[n]=match;routes[n]=route;
        if(!(match&(1u<<31)) || ((match^h.stream)&65535&~((match>>16)&0x7fff)))continue;
        // A matching entry, including FAULT or wildcard/BYPASS, is outside
        // this explicitly requested no-match contract. Do not fall back.
        if(mode==BypassMode::UnmatchedStream)return Denied;
        if(found || match!=((1u<<31)|h.stream))return Denied;
        if((route&0x30000)!=(mode==BypassMode::Architected ? 0x10000u:0u))return Unsupported;
        found=true;h.slot=n;h.smr=match;h.s2cr=route;
    }
    if(!found && mode!=BypassMode::UnmatchedStream)return NotReady;
    if(mode==BypassMode::UnmatchedStream && !h.routeValid())return Unsupported;
    if(mode==BypassMode::DisabledContext) {
        h.version=2;h.bytes=80;h.bank=h.s2cr&255;
        if(!h.layoutValid() || topology.contextCount>128 || h.bank>=topology.contextCount)return Unsupported;
        for(unsigned n=0;n<(h.id0&255);++n)
            if(n!=h.slot && (matches[n]&(1u<<31)) && (routes[n]&0x300ff)==h.bank)return Denied;
        const auto cb=h.contextOffset(),gr1=h.globalOffset();
        h.sctlr=io.read(cb);h.cbar=io.read(gr1+4*h.bank);h.cba2r=io.read(gr1+0x800+4*h.bank);
        if(io.read(cb+0x58)&~ContextFormatMask)return Unsupported;
    }
    if(!h.valid(topology,controller,bits) || !h.unchanged(io))return Stale;
    for(unsigned n=0;n<(h.id0&255);++n)
        if(io.read(0x800+4*n)!=matches[n] || io.read(0xc00+4*n)!=routes[n])return Stale;
    if(!h.bankUnchanged(io))return Stale;
    status=io.accessible();if(status!=OK)return status;
    status=io.isolated();if(status!=OK)return status;
    out=h;return OK;
}
}
