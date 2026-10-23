// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "IORT.h"
#include "ContextTLB.h"
#include <QcomPowerHardware.h>

namespace SMMU {
constexpr const char *ContextCaptureProperty="qcom,usb-smmu-capture";
// v1 transfers an existing, exclusive stage-1 bank. v2 records a completely
// cold SMMU for native allocation while the real resource owner holds reset.
// IORT alone identifies a stream, never ownership of a bank or an ASID.
// All fields are little-endian. No desired-state or bypass flag grants DMA.
struct ContextHandoff {
    uint32_t version=1,bytes=sizeof(ContextHandoff);
    uint64_t controller=0,smmu=0;
    uint32_t aperture=0,slot=0,stream=0,bank=0,asid=0,inputBits=0,outputBits=0;
    uint32_t control=0,id0=0,id1=0,id2=0,smr=0,s2cr=0,sctlr=0,cbar=0,cba2r=0;
    uint32_t tcr=0,tcr2=0,contextID=0,mair0=0,mair1=0,reserved=0;
    uint64_t ttbr0=0,ttbr1=0;
    bool fresh() const {return version==2;}
    bool freshMetadata() const {
        return (control&(1u<<10)) && !smr && s2cr==0x200ff && !asid &&
            !sctlr && !cbar && !cba2r && !tcr && !tcr2 && !contextID && !mair0 && !mair1 && !ttbr0 && !ttbr1;
    }
    unsigned contexts() const { const unsigned n=id1&255;return n==13 ? 12u:n; }
    uint32_t contextOffset(unsigned index) const {
        return ((1u<<(((id1>>28)&7)+1))+index)<<((id1>>31) ? 16:12);
    }
    uint32_t globalOffset() const { return 1u<<((id1>>31) ? 16:12); }
    bool layoutValid(const IORTTopology &topology,uint64_t actualController,unsigned addressBits,bool selected=true) const {
        if ((version!=1 && version!=2) || (fresh() && selected && !freshMetadata()) ||
            bytes!=sizeof(*this) || reserved || !controller || controller!=actualController ||
            !smmu || smmu!=topology.base || aperture!=topology.span || stream!=topology.stream ||
            inputBits!=addressBits || inputBits<32 || inputBits>40 || inputBits>topology.addressBits ||
            (outputBits!=32 && outputBits!=36 && outputBits!=40) || asid>65535 ||
            topology.coherent || !topology.contextCount || topology.contextCount>128 ||
            (selected && !exclusiveContextInterrupt(topology,bank)) ||
            (control&0x80000009u) || id0==UINT32_MAX || id1==UINT32_MAX || id2==UINT32_MAX ||
            !(id0&(1u<<27)) || !(id0&(1u<<30)) || !(id2&(1u<<12))) return false;
        const unsigned pageShift=(id1>>31) ? 16:12,globalPages=1u<<(((id1>>28)&7)+1);
        const unsigned count=contexts(),groups=id0&255;
        const unsigned widths[]={32,36,40,42,44,48,52};
        const unsigned ias=id2&15,oas=(id2>>4)&15,vas=(id2>>8)&15;
        if (uint64_t(2)*globalPages*(1ULL<<pageShift)!=aperture || !count || count>globalPages ||
            (selected && (bank>=count || bank<((id1>>16)&255))) || !groups || groups>128 || slot>=groups || stream>=1u<<((id0>>9)&15) ||
            ias>=7 || oas>=7 || vas>=7 || outputBits>widths[ias] || outputBits>widths[oas] ||
            inputBits>widths[vas]) return false;
        return true;
    }
    bool valid(const IORTTopology &topology,uint64_t actualController,unsigned addressBits) const {
        if (!layoutValid(topology,actualController,addressBits)) return false;
        if (fresh()) return true;
        // Adopt the ASID actually used by this bank, never a guessed free ID.
        return smr==((1u<<31)|stream) && (s2cr&0x300ff)==bank &&
            (sctlr&1) && sctlr!=UINT32_MAX && ((cbar>>16)&3)==1 && cba2r==1 &&
            !(tcr&0x8040c000u) && (tcr2&16) && ttbr0!=UINT64_MAX && (ttbr0&0x0000fffffffff000ULL) &&
            !(ttbr0&4095) && (ttbr0&0x0000fffffffff000ULL)<(1ULL<<outputBits) &&
            unsigned(ttbr0>>48)==asid && !ttbr1;
    }
    template<class IO> bool otherContextsInactive(IO &io) const {
        for (unsigned n=0;n<contexts();++n) {
            if (n==bank) continue;
            const auto cb=contextOffset(n),enabled=io.read(cb);
            // HYP can return a saved guest CBAR while selecting the physical
            // VMID from its SID assignment table. Different observed VMIDs
            // therefore do not prove separate TLB namespaces. Without an
            // owner lease covering other active banks, their global entries
            // may alias this root even when the observed ASIDs differ.
            if (enabled==UINT32_MAX || (enabled&1)) return false;
        }
        return true;
    }
    template<class IO> bool registersUnchanged(IO &io) const {
        const auto cb=contextOffset(bank),gr1=globalOffset();
        return io.read(0)==control && io.read(0x20)==id0 && io.read(0x24)==id1 && io.read(0x28)==id2 &&
            io.read(0x800+4*slot)==smr && io.read(0xc00+4*slot)==s2cr && io.read(cb)==sctlr &&
            io.read(gr1+4*bank)==cbar && io.read(gr1+0x800+4*bank)==cba2r &&
            io.read(cb+0x30)==tcr && io.read(cb+0x10)==tcr2 && io.read(cb+0x34)==contextID &&
            io.read(cb+0x38)==mair0 && io.read(cb+0x3c)==mair1 &&
            io.read64(cb+0x20)==ttbr0 && io.read64(cb+0x28)==ttbr1 && otherContextsInactive(io) &&
            (!fresh() || freshStateUnchanged(io));
    }
    template<class IO> bool freshStateUnchanged(IO &io) const {
        if (!fresh() || !freshMetadata()) return false;
        const auto gr1=globalOffset();
        for (unsigned n=0;n<contexts();++n) {
            const auto cb=contextOffset(n);
            if (io.read(cb) || io.read(gr1+4*n) || io.read(gr1+0x800+4*n) ||
                io.read(cb+0x10) || io.read(cb+0x30) || io.read(cb+0x34) || io.read(cb+0x38) || io.read(cb+0x3c) ||
                io.read64(cb+0x20) || io.read64(cb+0x28) || (io.read(cb+0x58)&~ContextFormatMask)) return false;
        }
        for (unsigned n=0;n<(id0&255);++n)
            if (io.read(0x800+4*n)!=0 || io.read(0xc00+4*n)!=0x200ff) return false;
        return true;
    }
    template<class IO> bool unchanged(IO &io) const {
        return io.accessible()==QcomPower::OK && io.isolated()==QcomPower::OK && registersUnchanged(io);
    }
    static bool decode(const void *data,size_t size,ContextHandoff &out) {
        out={};if (!data || size!=sizeof(out)) return false;
        // Avoid unaligned kernel/firmware property reads and implicit padding.
        const auto *p=static_cast<const uint8_t *>(data);auto *q=reinterpret_cast<uint8_t *>(&out);
        for (unsigned n=0;n<sizeof(out);++n) q[n]=p[n];
        return (out.version==1 || out.version==2) && out.bytes==sizeof(out) && !out.reserved;
    }
};
static_assert(sizeof(ContextHandoff)==128 && offsetof(ContextHandoff,ttbr0)==112,"USB SMMU boot contract");

// Explicit cold-allocation request. Every stream must be invalid and FAULT,
// and every visible context must have a blank root/format and no fault. The
// native IRQ binding selects an actual bank; ASID0 is reserved only inside
// this inactive namespace. HardwareContext flushes it before attaching the
// private root. This snapshot itself performs no register writes or DMA grant.
template<class IO> QcomPower::Status captureFreshContext(IO &io,const IORTTopology &topology,
    uint64_t controller,unsigned addressBits,unsigned bank,ContextHandoff &out) {
    using namespace QcomPower;
    out={};
    if (!controller || (controller&4095) || !topology.base || (topology.base&4095) ||
        topology.span<0x4000 || topology.span>0x1000000 || addressBits<32 || addressBits>40) return Invalid;
    auto status=io.accessible();if (status!=OK) return status;
    status=io.isolated();if (status!=OK) return status;
    ContextHandoff h;h.version=2;h.controller=controller;h.smmu=topology.base;h.aperture=topology.span;
    h.stream=topology.stream;h.bank=bank;h.inputBits=addressBits;h.outputBits=32;h.s2cr=0x200ff;
    h.control=io.read(0);h.id0=io.read(0x20);h.id1=io.read(0x24);h.id2=io.read(0x28);
    if (!h.valid(topology,controller,addressBits)) return Unsupported;
    if (!h.freshStateUnchanged(io)) return Busy;
    if (!h.unchanged(io)) return Stale;
    status=io.accessible();if (status!=OK) return status;
    status=io.isolated();if (status!=OK) return status;
    out=h;return OK;
}

// The native USB resource owner has retired EFI software producers and holds
// real fabric/clocks plus controller reset. Observe the existing exact stream
// and exclusive stage-1 bank; never allocate IDs or create a bypass mapping.
// This reader performs no MMIO writes. The consumer independently compares
// this record before replacing the inherited root under the same leases.
template<class IO> QcomPower::Status captureInheritedContext(IO &io,const IORTTopology &topology,
    uint64_t controller,unsigned addressBits,ContextHandoff &out) {
    using namespace QcomPower;
    out={};
    if (!controller || (controller&4095) || !topology.base || (topology.base&4095) ||
        topology.span<0x4000 || topology.span>0x1000000 || addressBits<32 || addressBits>40) return Invalid;
    auto status=io.accessible();if (status!=OK) return status;
    status=io.isolated();if (status!=OK) return status;
    ContextHandoff h;h.controller=controller;h.smmu=topology.base;h.aperture=topology.span;
    h.stream=topology.stream;h.inputBits=addressBits;h.outputBits=32;
    h.control=io.read(0);h.id0=io.read(0x20);h.id1=io.read(0x24);h.id2=io.read(0x28);
    if (!h.layoutValid(topology,controller,addressBits,false)) return Unsupported;
    uint32_t smrs[128]{},routes[128]{};bool found=false;
    const unsigned groups=h.id0&255;
    for (unsigned n=0;n<groups;++n) {
        const auto smr=io.read(0x800+4*n),route=io.read(0xc00+4*n);
        if (smr==UINT32_MAX || route==UINT32_MAX) return Failed;
        smrs[n]=smr;routes[n]=route;
        if (!(smr&(1u<<31))) continue;
        const auto mask=(smr>>16)&0x7fff;
        if (((smr^h.stream)&65535&~mask)!=0) continue;
        if (found || mask || (smr&65535)!=h.stream) return Denied;
        if (route&0x30000) return Unsupported;
        found=true;h.slot=n;h.bank=route&255;h.smr=smr;h.s2cr=route;
    }
    if (!found) return NotReady;
    if (!h.layoutValid(topology,controller,addressBits)) return Unsupported;
    for (unsigned n=0;n<groups;++n)
        if (n!=h.slot && (smrs[n]&(1u<<31)) && !(routes[n]&0x30000) && (routes[n]&255)==h.bank) return Denied;
    const auto cb=h.contextOffset(h.bank),gr1=h.globalOffset();
    h.sctlr=io.read(cb);h.cbar=io.read(gr1+4*h.bank);h.cba2r=io.read(gr1+0x800+4*h.bank);
    h.tcr=io.read(cb+0x30);h.tcr2=io.read(cb+0x10);h.contextID=io.read(cb+0x34);
    h.mair0=io.read(cb+0x38);h.mair1=io.read(cb+0x3c);h.ttbr0=io.read64(cb+0x20);h.ttbr1=io.read64(cb+0x28);
    h.asid=unsigned(h.ttbr0>>48);
    const unsigned sizes[]={32,36,40};
    if ((h.tcr2&7)>=3) return Unsupported;
    h.outputBits=sizes[h.tcr2&7];
    if (!h.valid(topology,controller,addressBits)) return Unsupported;
    if (!h.unchanged(io)) return Stale;
    for (unsigned n=0;n<groups;++n)
        if (io.read(0x800+4*n)!=smrs[n] || io.read(0xc00+4*n)!=routes[n]) return Stale;
    status=io.accessible();if (status!=OK) return status;
    status=io.isolated();if (status!=OK) return status;
    out=h;return OK;
}
}
