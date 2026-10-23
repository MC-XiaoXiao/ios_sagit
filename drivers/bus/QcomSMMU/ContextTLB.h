// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerHardware.h>
namespace SMMU {
using namespace QcomPower;
// ARM SMMUv2 CB_FSR[10:9] describes the translation format (AArch64=2),
// independently of the sticky fault bits. A quiet AArch64 bank can read 0x400.
constexpr uint32_t ContextFaultMask=0xc00001feu, ContextFormatMask=3u<<9;
inline bool contextFault(uint32_t fsr) {return (fsr&ContextFaultMask)!=0;}
// Caller has already proved exclusive ownership and the current ASID. Used
// during format transitions as well as ordinary AArch64 mapping invalidation.
template<class IO> Status synchronizeContext(IO &io,uint32_t base,unsigned asid,unsigned timeout=1000000,bool allASIDs=false) {
    // The enclosing mapper retains all table storage until this handshake
    // completes. ARM64 tables are already written through uncached pages.
    // A context format/VMID transition must also remove old global entries.
    // ARM IHI 0062D.c 16.5.30: CBn_TLBIALL covers the context's VMID;
    // TLBIASID alone only guarantees removal of non-global ASID entries.
    io.barrier(); io.write(base+(allASIDs?0x618:0x610),allASIDs?0:asid);
    // Qualcomm uses all-ones as its TLBSYNC dummy write value.
    io.write(base+0x7f0,UINT32_MAX);
    unsigned elapsed=0,delay=1;
    for (;;) {
        for (unsigned spin=0;spin<10;++spin) {
            const auto status=io.read(base+0x7f4);
            if (status==UINT32_MAX) return Failed;
            if (!(status&1)) { io.barrier(); return OK; }
        }
        if (elapsed>=timeout) return TimedOut;
        const unsigned wait=delay<timeout-elapsed ? delay : timeout-elapsed;
        io.delay(wait); elapsed+=wait;
        if (delay<32768) delay*=2;
    }
}
// Invalidate only the owned stage-1 ASID. Backend registers are the SMMU
// aperture; no global reset, scratch SMR or shared stream routing is changed.
template<class IO> class ContextTLB {
    IO &io;
    uint32_t base=0, length=0;
    unsigned asid=0;
    bool configured=false, fault=false;
public:
    explicit ContextTLB(IO &access):io(access) {}
    bool faulted() const { return fault; }
    Status configure(uint32_t aperture,unsigned bank,unsigned ownedASID) {
        if (configured) return Busy;
        if (ownedASID>65535 || aperture<0x4000 || (aperture&4095)) return Invalid;
        const auto id0=io.read(0x20),id1=io.read(0x24),id2=io.read(0x28);
        if (id0==UINT32_MAX || id1==UINT32_MAX || id2==UINT32_MAX) return Failed;
        if (!(id0&(1u<<30)) || !(id2&(1u<<12))) return Unsupported;
        const unsigned pageShift=(id1&(1u<<31)) ? 16u : 12u;
        const uint64_t globalPages=1ULL<<(((id1>>28)&7)+1);
        const uint64_t expected=2*globalPages*(1ULL<<pageShift);
        const unsigned reportedCount=id1&255, stage2=(id1>>16)&255;
        // MSM8998 LPASS advertises one inaccessible final context bank.
        const unsigned count=reportedCount==13 ? 12u : reportedCount;
        if (expected!=aperture || !count || stage2>count || count>globalPages || bank>=count || bank<stage2) return Invalid;
        const uint64_t offset=(globalPages+bank)*(1ULL<<pageShift);
        if (offset+0x7f8>aperture) return Invalid;
        base=uint32_t(offset); length=aperture; asid=ownedASID; configured=true;
        return OK;
    }
    Status sync(unsigned timeout=1000000) {
        if (!configured) return NotReady;
        if (fault) return Stale;
        const auto ttbrHigh=io.read(base+0x24);
        if (ttbrHigh==UINT32_MAX || (ttbrHigh>>16)!=asid) { fault=true; return Failed; }
        const auto status=synchronizeContext(io,base,asid,timeout);
        if (status!=OK) fault=true;
        return status;
    }
    uint32_t bankOffset() const { return configured && base<length ? base : 0; }
};
}
