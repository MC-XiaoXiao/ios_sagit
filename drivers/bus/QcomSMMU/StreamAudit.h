// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <QcomPowerHardware.h>

namespace SMMU {
// SMR/S2CR are non-destructive configuration reads on this exclusively owned
// SMMUv2 instance. Capture every slot, then order the entire observation before
// its validation and any following normal-memory/DMA work. This does not apply
// to fault/status polling, TLB completion, root switching or register writes.
template<unsigned Capacity=128> class StreamAudit {
    struct Slot {uint32_t match=0,route=0;} slots[Capacity]{};
    unsigned count=0;
public:
    enum Reason {None,Unreadable,Match,Route,ForeignBank,Alias,InvalidContract};
    struct Result {
        QcomPower::Status status=QcomPower::OK;
        Reason reason=None;
        uint32_t offset=0,observed=0,expected=0;
    };
    template<class IO> void capture(IO &io) {
        count=io.read(0x20)&255;if(count>Capacity)count=Capacity;
        for(unsigned i=0;i<count;++i) {
            slots[i].match=io.streamConfigurationRead(0x800+4*i);
            slots[i].route=io.streamConfigurationRead(0xc00+4*i);
        }
        // Also reached for an unreadable slot: error reporting never observes
        // a partially ordered survey. No per-slot result is cached across calls.
        io.streamConfigurationReadComplete();
    }
    template<class Contract> Result validate(const Contract &contract,const uint32_t *routes) const {
        using namespace QcomPower;
        if(!routes || !contract.streamCount || contract.streamCount>8)
            return {Invalid,InvalidContract,0,0,0};
        for(unsigned slot=0;slot<count;++slot) {
            const uint32_t smr=slots[slot].match,route=slots[slot].route;
            if(smr==UINT32_MAX || route==UINT32_MAX)
                return {Failed,Unreadable,0xc00+4*slot,route,0};
            bool owned=false;
            for(unsigned i=0;i<contract.streamCount;++i)if(slot==contract.streams[i].slot) {
                if(smr!=((1u<<31)|contract.streams[i].id))
                    return {Stale,Match,0x800+4*slot,smr,(1u<<31)|contract.streams[i].id};
                if(route!=routes[i])return {Stale,Route,0xc00+4*slot,route,routes[i]};
                owned=true;
            }
            if(!owned && (smr&(1u<<31))) {
                if(!(route&0x30000) && (route&255)==contract.bank)
                    return {Denied,ForeignBank,0xc00+4*slot,route,0};
                for(unsigned i=0;i<contract.streamCount;++i)
                    if(!((smr^contract.streams[i].id)&65535&~((smr>>16)&32767)))
                        return {Denied,Alias,0x800+4*slot,smr,0};
            }
        }
        return {};
    }
};
}
