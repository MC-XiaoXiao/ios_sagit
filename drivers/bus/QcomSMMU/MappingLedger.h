// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <QcomPowerHardware.h>
namespace SMMU {
using namespace QcomPower;
// Software leases do not grant DMA. Reserved -> Mapped follows a completed
// page-table map; Retiring -> free follows a completed page-table unmap.
// Unknown operations pin the IOVA and caller tokens until actual context
// detachment, and still require caller acknowledgement before reuse.
template<unsigned Capacity=512> class MappingLedger {
    static_assert(Capacity>0,"mapping ledger capacity");
public:
    enum class Phase:uint8_t { Empty,Reserved,Mapped,Retiring,Quarantined,Retired };
    struct Record {
        uint64_t identity=0,base=0,bytes=0,address=0,length=0,requested=0;
        uintptr_t memory=0,command=0;
        Phase phase=Phase::Empty;
    };
private:
    Record records[Capacity]{};
    unsigned order[Capacity]{};
    unsigned used=0,nextFree=0;
    uint64_t low=0,high=0,serial=0;
    unsigned bits=0;
    static bool align(uint64_t value,uint64_t alignment,uint64_t &out) {
        if (value>UINT64_MAX-(alignment-1)) return false;
        out=(value+alignment-1)&~(alignment-1);return true;
    }
    Record *find(uint64_t identity) {
        if (!identity) return nullptr;
        for (unsigned i=0;i<used;++i) if (records[order[i]].identity==identity) return &records[order[i]];
        return nullptr;
    }
    void clear(Record &record) {
        for(unsigned i=0;i<used;++i)if(&records[order[i]]==&record) {
            nextFree=order[i];for(unsigned j=i+1;j<used;++j)order[j-1]=order[j];
            order[--used]=0;record={};return;
        }
    }
public:
    MappingLedger()=default;
    MappingLedger(const MappingLedger &)=delete;
    MappingLedger &operator=(const MappingLedger &)=delete;
    bool initialized() const { return bits!=0; }
    unsigned count() const {return used;}
    uint64_t payloadBytes() const {uint64_t bytes=0;for(unsigned i=0;i<used;++i)bytes+=records[order[i]].length;return bytes;}
    uint64_t spanBytes() const {uint64_t bytes=0;for(unsigned i=0;i<used;++i)bytes+=records[order[i]].bytes;return bytes;}
    Status initialize(unsigned addressBits,uint64_t first=0x10000,uint64_t end=0) {
        if (bits || count()) return Busy;
        if (addressBits<32 || addressBits>40 || first<4096 || (first&4095)) return Invalid;
        if (!end) end=1ULL<<addressBits;
        if ((end&4095) || end>(1ULL<<addressBits) || first>=end || end-first<12288) return Invalid;
        bits=addressBits;low=first;high=end;return OK;
    }
    Status reserve(uintptr_t memory,uintptr_t command,uint64_t bytes,unsigned pageOffset,
                   uint64_t alignment,unsigned addressBits,Record *&result,uint64_t fixedPage=0) {
        result=nullptr;
        if (!bits) return NotReady;
        if (!memory || !bytes || pageOffset>=4096 || bytes>4096*4096ULL-pageOffset) return Invalid;
        if (!alignment) alignment=1;
        if ((alignment&(alignment-1)) || alignment>(1ULL<<40) || (pageOffset&(alignment-1))) return Invalid;
        if (!addressBits) addressBits=bits;
        if (addressBits<32 || addressBits>bits) return Invalid;
        if (serial==UINT64_MAX) return Stale;
        if(used==Capacity)return NotReady;
        Record *slot=nullptr;unsigned slotIndex=nextFree;
        for(unsigned i=0;i<Capacity;++i) {
            if(records[slotIndex].phase==Phase::Empty){slot=&records[slotIndex];break;}
            if(++slotIndex==Capacity)slotIndex=0;
        }
        if (!slot) return NotReady;
        const uint64_t payload=((bytes+pageOffset+4095)/4096)*4096;
        const uint64_t limit=high<(1ULL<<addressBits) ? high : 1ULL<<addressBits;
        const uint64_t pageAlignment=alignment>4096 ? alignment : 4096;
        uint64_t page=0;
        if (!align(low+4096,pageAlignment,page)) return Invalid;
        if (fixedPage) {
            if ((fixedPage&(pageAlignment-1)) || fixedPage<page) return Invalid;
            page=fixedPage;
        }
        // Stable records are indexed by address. Find the first free interval
        // in one ordered pass, rather than rescanning every record after each
        // collision. Guard pages and quarantined/retired leases stay reserved.
        unsigned at=0;
        for (;at<used;++at) {
            if (page<4096 || page>=limit || payload>limit-page || limit-page-payload<4096) return NotReady;
            const uint64_t begin=page-4096,end=page+payload+4096;
            const auto &r=records[order[at]];
            if(end<=r.base)break;
            if(begin<r.base+r.bytes) {
                if (fixedPage) return Busy;
                if (!align(r.base+r.bytes+4096,pageAlignment,page)) return NotReady;
            }
        }
        if(page<4096 || page>=limit || payload>limit-page || limit-page-payload<4096)return NotReady;
        *slot={++serial,page-4096,payload+8192,page+pageOffset,payload,bytes,memory,command,Phase::Reserved};
        for(unsigned i=used;i>at;--i)order[i]=order[i-1];
        order[at]=slotIndex;++used;
        nextFree=slotIndex+1;if(nextFree==Capacity)nextFree=0;
        result=slot;return OK;
    }
    const Record *lookup(uintptr_t memory,uintptr_t command,uint64_t address,uint64_t length) const {
        for (unsigned i=0;i<used;++i) {const auto &r=records[order[i]];if (r.memory==memory && r.command==command &&
            r.address==address && r.length==length) return &r;
        }
        return nullptr;
    }
    Status mapped(uint64_t identity) {
        auto *r=find(identity);if (!r || r->phase!=Phase::Reserved) return Invalid;
        r->phase=Phase::Mapped;return OK;
    }
    Status cancel(uint64_t identity) {
        auto *r=find(identity);if (!r || r->phase!=Phase::Reserved) return Busy;
        clear(*r);return OK;
    }
    Status beginUnmap(uint64_t identity,uintptr_t memory,uintptr_t command,uint64_t address,uint64_t length) {
        auto *r=find(identity);
        if (!r || r->memory!=memory || r->command!=command || r->address!=address || r->length!=length) return Invalid;
        if (r->phase!=Phase::Mapped) return Busy;
        r->phase=Phase::Retiring;return OK;
    }
    Status finishUnmap(uint64_t identity,Status status) {
        auto *r=find(identity);if (!r || r->phase!=Phase::Retiring) return Invalid;
        if (status!=OK) { r->phase=Phase::Quarantined;return status==TimedOut || status==Failed ? status : Stale; }
        clear(*r);return OK;
    }
    Status quarantine(uint64_t identity) {
        auto *r=find(identity);if (!r || r->phase==Phase::Retired) return Invalid;
        r->phase=Phase::Quarantined;return OK;
    }
    template<class Proof> Status isolate(Proof rootDetached) {
        const auto status=rootDetached();if (status!=OK) return status;
        for(unsigned i=0;i<used;++i)records[order[i]].phase=Phase::Retired;
        return OK;
    }
    Status acknowledge(uint64_t identity,uintptr_t memory,uintptr_t command) {
        auto *r=find(identity);if (!r || r->phase!=Phase::Retired || r->memory!=memory || r->command!=command) return Invalid;
        clear(*r);return OK;
    }
};
}
