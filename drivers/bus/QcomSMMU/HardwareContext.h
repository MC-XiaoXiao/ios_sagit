// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "ContextTLB.h"
namespace SMMU {
struct Stream { uint16_t slot,id; };
// One-way HLOS handoff of an explicitly owned context and exact stream IDs.
// IO.accessible() checks retained fabric/clock resources; IO.isolated() reads
// a real controller reset or acknowledged fabric DMA halt. The caller keeps
// both ownership leases alive, including every producer covered by the bank.
// No global control/reset register or unowned stream/context is written.
template<class IO> class HardwareContext {
    IO &io;
    Stream streams[16]{};
    unsigned streamCount=0,groups=0,bank=0,asid=0,vmid=0,inheritedASID=0,inputBits=0,outputBits=0;
    uint32_t cb=0,gr1=0,expectedTCR=0,expectedTCR2=0,expectedControl=0;
    uint64_t root=0;
    bool configured=false,touched=false,active=false,detached=false,fault=false;
    uint32_t rejectedOffset=UINT32_MAX;
    uint64_t rejectedMask=0,rejectedExpected=0,rejectedObserved=0;
    static constexpr uint32_t FaultMask=0xc00001fe;
    int owned(unsigned slot) const {
        for (unsigned i=0;i<streamCount;++i) if (streams[i].slot==slot) return int(i);
        return -1;
    }
    Status uncertain(Status s) { fault=true; active=false; return s==TimedOut || s==Failed ? s : Stale; }
    Status mismatch(uint32_t offset,uint64_t mask,uint64_t expected,uint64_t observed) {
        if (rejectedOffset==UINT32_MAX) {
            rejectedOffset=offset;rejectedMask=mask;rejectedExpected=expected;rejectedObserved=observed;
        }
        return Failed;
    }
    Status write(uint32_t offset,uint32_t value,uint32_t mask=UINT32_MAX) {
        const auto before=io.read(offset); if (before==UINT32_MAX) return mismatch(offset,mask,value,before);
        io.write(offset,(before&~mask)|(value&mask));
        const auto after=io.read(offset);
        return after!=UINT32_MAX && (after&mask)==(value&mask) ? OK : mismatch(offset,mask,value,after);
    }
    Status write64(uint32_t offset,uint64_t value) {
        io.write64(offset,value);const auto after=io.read64(offset);
        return after==value ? OK : mismatch(offset,UINT64_MAX,value,after);
    }
    Status routes(bool exact) {
        for (unsigned slot=0;slot<groups;++slot) {
            const auto smr=io.read(0x800+4*slot),s2cr=io.read(0xc00+4*slot);
            if (smr==UINT32_MAX || s2cr==UINT32_MAX) return Failed;
            const int index=owned(slot);
            if (index>=0) {
                if (smr&(1u<<31)) {
                    if ((smr&0x7fff0000) || (smr&65535)!=streams[index].id) return Denied;
                } else if (exact) return Stale;
                continue;
            }
            if (!(smr&(1u<<31))) continue;
            if (!(s2cr&0x30000) && (s2cr&255)==bank) return Denied;
            const auto mask=(smr>>16)&0x7fff;
            for (unsigned i=0;i<streamCount;++i)
                if (((smr^streams[i].id)&65535&~mask)==0) return Denied;
        }
        return OK;
    }
    Status blockStreams() {
        for (unsigned i=0;i<streamCount;++i) {
            const auto offset=0xc00+4*streams[i].slot;
            auto before=io.read(offset); if (before==UINT32_MAX) return Failed;
            // Some Qualcomm firmware ignores FAULT writes and turns BYPASS
            // into FAULT. Probe only our own stopped stream, then verify the
            // actual FAULT type. A failed probe never permits reset release.
            io.write(offset,(before&~0x30000u)|0x20000);
            auto after=io.read(offset); if (after==UINT32_MAX) return mismatch(offset,0x30000,0x20000,after);
            if ((after&0x30000)!=0x20000) {
                before=after;io.write(offset,(before&~0x300ffu)|0x100ff);
                after=io.read(offset);
                if (after==UINT32_MAX || (after&0x30000)!=0x20000) return mismatch(offset,0x30000,0x20000,after);
            }
        }
        return OK;
    }
    Status faultsClear() {
        const auto fsr=io.read(cb+0x58); if (fsr==UINT32_MAX) return Failed;
        if (fsr&(1u<<30)) io.write(cb+8,1); // terminate an inherited stalled transaction
        if (fsr&FaultMask) io.write(cb+0x58,fsr&FaultMask);
        const auto after=io.read(cb+0x58);
        return after!=UINT32_MAX && !(after&FaultMask) ? OK : Failed;
    }
    Status flush(unsigned value,bool all=false) { io.barrier();return synchronizeContext(io,cb,value,1000000,all); }
    Status mutationsAllowed() {
        auto s=io.accessible();if (s!=OK) return s;
        s=io.isolated();if (s!=OK) return s;
        return routes(false);
    }
public:
    explicit HardwareContext(IO &access):io(access) {}
    HardwareContext(const HardwareContext &)=delete;
    HardwareContext &operator=(const HardwareContext &)=delete;
    bool faulted() const { return fault; }
    bool hasHardwareOwnership() const { return touched; }
    uint32_t bankOffset() const { return cb; }
    uint64_t rootAddress() const { return active && !fault ? root : 0; }
    bool copyWriteFailure(uint64_t (&record)[4]) const {
        if (rejectedOffset==UINT32_MAX) return false;
        record[0]=rejectedOffset;record[1]=rejectedMask;record[2]=rejectedExpected;record[3]=rejectedObserved;return true;
    }
    Status configure(uint32_t aperture,unsigned context,unsigned ownedASID,unsigned iovaBits,unsigned paBits,
                     const Stream *ownedStreams,unsigned count,unsigned ownedVMID=0) {
        if (configured) return Busy;
        if (!ownedStreams || !count || count>16 || ownedASID>65535 || ownedVMID>255 || iovaBits<32 || iovaBits>40 ||
            (paBits!=32 && paBits!=36 && paBits!=40)) return Invalid;
        auto s=io.accessible();if (s!=OK) return s;
        const auto control=io.read(0),id0=io.read(0x20),id1=io.read(0x24),id2=io.read(0x28);
        if (control==UINT32_MAX || id0==UINT32_MAX || id1==UINT32_MAX || id2==UINT32_MAX) return Failed;
        if ((control&0x80000009u) || !(id0&(1u<<27)) || !(id0&(1u<<30)) || !(id2&(1u<<12))) return Unsupported;
        const unsigned pageShift=(id1>>31) ? 16u : 12u,globalPages=1u<<(((id1>>28)&7)+1);
        const auto bytes=uint64_t(2)*globalPages*(1ULL<<pageShift);
        const unsigned reported=id1&255,contexts=reported==13 ? 12u : reported;
        if (bytes!=aperture || !contexts || contexts>globalPages || context>=contexts || context<((id1>>16)&255)) return Invalid;
        const unsigned sizes[]={32,36,40,42,44,48,52};
        const unsigned ias=id2&15,oas=(id2>>4)&15,vas=(id2>>8)&15;
        if (ias>=7 || oas>=7 || vas>=7 || paBits>sizes[ias] || paBits>sizes[oas] || iovaBits>sizes[vas]) return Unsupported;
        groups=id0&255;if (groups>128) return Unsupported;
        const unsigned sidBits=(id0>>9)&15,sidMask=(1u<<sidBits)-1;
        if (!groups) return Unsupported;
        for (unsigned i=0;i<count;++i) {
            if (ownedStreams[i].slot>=groups || (ownedStreams[i].id&~sidMask)) return Invalid;
            for (unsigned n=0;n<i;++n) if (ownedStreams[n].slot==ownedStreams[i].slot || ownedStreams[n].id==ownedStreams[i].id) return Invalid;
            streams[i]=ownedStreams[i];
        }
        streamCount=count;bank=context;asid=ownedASID;vmid=ownedVMID;inputBits=iovaBits;outputBits=paBits;
        gr1=1u<<pageShift;cb=(globalPages+bank)<<pageShift;
        s=routes(false);if (s!=OK) return s;
        configured=true;return OK;
    }
    Status attach(uint64_t table,bool faultInterruptReady) {
        if (!faultInterruptReady) return NotReady;
        return attachRoot(table,true);
    }
    // Early boot has no installed context IRQ handler. Poll faults explicitly
    // and leave CFIE off until the native kernel interrupt source is ready.
    Status attachPolled(uint64_t table) { return attachRoot(table,false); }
private:
    Status attachRoot(uint64_t table,bool interruptDelivery) {
        if (!configured) return NotReady;
        if (fault || active) return Busy;
        if (!table || (table&4095) || table>=(1ULL<<outputBits)) return Invalid;
        auto s=mutationsAllowed();if (s!=OK) return s;
        const auto oldControl=io.read(cb),oldType=io.read(gr1+4*bank),oldFormat=io.read(gr1+0x800+4*bank);
        const auto oldTCR=io.read(cb+0x30);const auto oldTTBR=io.read64(cb+0x20);
        if (oldControl==UINT32_MAX || oldType==UINT32_MAX || oldFormat==UINT32_MAX || oldTCR==UINT32_MAX || oldTTBR==UINT64_MAX) return Failed;
        if ((oldControl&1) && ((oldType>>16)&3)!=1) return Unsupported;
        const auto oldID=io.read(cb+0x34);if (oldID==UINT32_MAX) return Failed;
        const unsigned oldASID=(!(oldFormat&1) && !(oldTCR&0x80000000)) ? oldID&255 : unsigned(oldTTBR>>48);
        // A live inherited stage-1 context owns its existing ASID. A loader
        // must not assign an unrelated live ASID while handing over this bank.
        if (!touched && (oldControl&1) && (oldASID!=asid || (oldType&255)!=vmid)) return Denied;
        inheritedASID=oldASID;
        touched=true;detached=false;root=table;
        s=blockStreams();if (s!=OK) return uncertain(s);
        s=write(cb,0);if (s!=OK) return uncertain(s);
        s=faultsClear();if (s!=OK) return uncertain(s);
        // Firmware tables may contain global entries. ASID invalidation alone
        // cannot retire them when replacing this bank's root/format.
        s=flush(oldASID,true);if (s!=OK) return uncertain(s);
        s=write(gr1+0x800+4*bank,1);if (s!=OK) return uncertain(s); // AArch64, stage1
        s=write(gr1+4*bank,0x1f300|vmid);if (s!=OK) return uncertain(s); // preserve owned VMID
        expectedTCR=(1u<<23)|(2u<<12)|(64-inputBits); // TTBR1 off, NC walk, outer shareable
        expectedTCR2=(7u<<15)|(1u<<4)|(outputBits==40 ? 2u : outputBits==36 ? 1u : 0u);
        s=write(cb+0x10,expectedTCR2);if (s!=OK) return uncertain(s);
        s=write(cb+0x30,expectedTCR);if (s!=OK) return uncertain(s);
        s=write64(cb+0x20,root|(uint64_t(asid)<<48));if (s!=OK) return uncertain(s);
        s=write64(cb+0x28,0);if (s!=OK) return uncertain(s);
        s=write(cb+0x34,0);if (s!=OK) return uncertain(s);
        s=write(cb+0x38,0x44);if (s!=OK) return uncertain(s);
        s=write(cb+0x3c,0);if (s!=OK) return uncertain(s);
        s=flush(asid);if (s!=OK) return uncertain(s);
        expectedControl=(1u<<12)|(interruptDelivery ? 1u<<6:0)|(1u<<5)|7; // no stall-on-fault
        s=write(cb,expectedControl);if (s!=OK) return uncertain(s);
        for (unsigned i=0;i<streamCount;++i) {
            const auto offset=0xc00+4*streams[i].slot;
            s=write(offset,bank,0x300ff);if (s!=OK) return uncertain(s);
        }
        // Publish a new match only after its private context and translated
        // S2CR are valid. This follows the native GPU cold path and Qualcomm
        // stream programming order; firmware can ignore an early SMR write
        // while the slot still names the reset FAULT/invalid-bank state.
        for (unsigned i=0;i<streamCount;++i) {
            const auto offset=0x800+4*streams[i].slot;
            const auto current=io.read(offset);
            if (current==UINT32_MAX) return uncertain(Failed);
            if (current && current!=streams[i].id && current!=((1u<<31)|streams[i].id)) return uncertain(Denied);
            if (!(current&(1u<<31))) {
                s=write(offset,streams[i].id);if (s!=OK) return uncertain(s);
            }
            s=write(0x800+4*streams[i].slot,(1u<<31)|streams[i].id);
            if (s!=OK) return uncertain(s);
        }
        active=true;return check();
    }
public:
    Status check() {
        if (!active || fault) return Stale;
        auto s=io.accessible();if (s!=OK) return uncertain(s);
        s=routes(true);if (s!=OK) return uncertain(s);
        for (unsigned i=0;i<streamCount;++i) {
            const auto route=io.read(0xc00+4*streams[i].slot);
            if (route==UINT32_MAX || (route&0x300ff)!=bank) return uncertain(Failed);
        }
        if (io.read(cb)!=expectedControl || io.read(cb+0x10)!=expectedTCR2 || io.read(cb+0x30)!=expectedTCR ||
            io.read(cb+0x38)!=0x44 || io.read64(cb+0x20)!=(root|(uint64_t(asid)<<48)) ||
            io.read(gr1+0x800+4*bank)!=1 || io.read(gr1+4*bank)!=(0x1f300|vmid)) return uncertain(Failed);
        const auto fsr=io.read(cb+0x58);
        return fsr!=UINT32_MAX && !(fsr&FaultMask) ? OK : uncertain(Failed);
    }
    Status sync() {
        if (detached) return rootDetached();
        auto s=check();if (s!=OK) return s;
        s=flush(asid);return s==OK ? OK : uncertain(s);
    }
    Status detach() {
        if (!touched) return NotReady;
        auto s=mutationsAllowed();if (s!=OK) return s;
        active=false;detached=false;
        s=blockStreams();if (s!=OK) return uncertain(s);
        for (unsigned i=0;i<streamCount;++i) {
            s=write(0x800+4*streams[i].slot,(1u<<31)|streams[i].id);
            if (s!=OK) return uncertain(s);
        }
        s=write(cb,0);if (s!=OK) return uncertain(s);
        s=faultsClear();if (s!=OK) return uncertain(s);
        // Flush the current bank's ASID before erasing its TTBR. No saved
        // firmware translation is restored after HLOS has adopted the bank.
        const auto currentTTBR=io.read64(cb+0x20);if (currentTTBR==UINT64_MAX) return uncertain(Failed);
        s=flush(unsigned(currentTTBR>>48),true);if (s!=OK) return uncertain(s);
        if (unsigned(currentTTBR>>48)!=asid) { s=flush(asid);if (s!=OK) return uncertain(s); }
        if (inheritedASID!=asid && inheritedASID!=unsigned(currentTTBR>>48)) {
            s=flush(inheritedASID);if (s!=OK) return uncertain(s);
        }
        s=write64(cb+0x20,0);if (s!=OK) return uncertain(s);
        s=write64(cb+0x28,0);if (s!=OK) return uncertain(s);
        io.barrier();root=0;detached=true;fault=false;
        return rootDetached();
    }
    Status rootDetached() {
        if (!detached || active || root) return NotReady;
        auto s=io.accessible();if (s!=OK) return s;
        s=io.isolated();if (s!=OK) return s;
        s=routes(true);if (s!=OK) return s;
        for (unsigned i=0;i<streamCount;++i) if ((io.read(0xc00+4*streams[i].slot)&0x30000)!=0x20000) return Stale;
        return io.read(cb)==0 && io.read64(cb+0x20)==0 && io.read64(cb+0x28)==0 ? OK : Stale;
    }
};
}
