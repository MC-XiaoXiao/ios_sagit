// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "AdrenoMMU.h"
#include <GPUDiagnostics.h>
namespace Adreno {
// Only the private-job allocator uses this pool. The enclosing mapper holds
// its lock and supplies reusable=true only after real completion, deselection
// and old-ASID TLBSYNC. Retained kernel tables never enter this cache.
template<class API,unsigned Capacity=64> class RetiredTablePageCache {
    API &api;
    MMUTablePage pages[Capacity]{};
    unsigned count_=0;
    bool fault_=false;
public:
    uint64_t hits=0,allocations=0,validationFailures=0;
    explicit RetiredTablePageCache(API &operations):api(operations){}
    unsigned count() const{return count_;}
    bool faulted() const{return fault_;}
    MMUStatus allocate(MMUTablePage &out,bool enabled) {
        out={};if(fault_)return Failed;
        if(enabled && count_) {
            // Recheck the owning descriptor, physical segment and CPU view.
            // A failed entry stays owned by this faulted pool, never freed.
            if(!api.validate(pages[count_-1])) {
                fault_=true;GPU::Diagnostics::add(validationFailures);return Failed;
            }
            out=pages[--count_];pages[count_]={};GPU::Diagnostics::add(hits);return OK;
        }
        auto status=api.allocate(out);if(status==OK)GPU::Diagnostics::add(allocations);return status;
    }
    void release(MMUTablePage &page,bool reusable) {
        if(reusable && !fault_ && count_<Capacity) {pages[count_++]=page;page={};}
        else api.release(page,true);
    }
    MMUStatus drain(bool isolated) {
        if(!isolated || fault_)return Busy;
        for(unsigned i=0;i<count_;++i)if(!api.validate(pages[i])) {
            fault_=true;GPU::Diagnostics::add(validationFailures);return Failed;
        }
        while(count_)api.release(pages[--count_],true);
        return OK;
    }
};
// A private, initially empty AArch64 stage-1 tree. Only the current job's
// prepared buffers and explicitly selected global mappings are inserted.
// The tree is immutable after publication and survives until DMA retirement
// and ASID invalidation have both been proved by the enclosing mapper.
template<class API,unsigned Capacity=128> class JobPageTables {
    API &api;
    GPU::Diagnostics::AddressSpaces::Trace *trace=nullptr;
    MMUTablePage pages[Capacity]{};
    unsigned count=0,level0=0,inputBits=0,outputBits=0,asid=0,leaves=0;
    bool sealed=false,fault=false;
    static constexpr uint64_t Address=0x0000fffffffff000ULL;
    static unsigned shift(unsigned level) { return 39-9*level; }
    bool physical(uint64_t pa) const { return pa && !(pa&4095) && pa<(1ULL<<outputBits); }
    MMUStatus allocate(unsigned &index) {
        if (count==Capacity) return NotReady;
        MMUTablePage page{};MMUStatus status;
        {GPU::Diagnostics::AddressSpaces::Span timing(trace,GPU::Diagnostics::AddressSpaces::Allocate);status=api.allocate(page);}
        if (status!=OK) return status;
        if (!page.owner || !page.entries || !physical(page.physical) || (reinterpret_cast<uintptr_t>(page.entries)&7)) {
            api.release(page,true);return Failed;
        }
        for (unsigned i=0;i<count;++i) if (pages[i].physical==page.physical) {
            pages[count++]=page;fault=true;return Failed;
        }
        {GPU::Diagnostics::AddressSpaces::Span timing(trace,GPU::Diagnostics::AddressSpaces::Zero);
            for (unsigned i=0;i<512;++i) page.entries[i]=0;}
        index=count;pages[count++]=page;return OK;
    }
    MMUStatus locate(uint64_t address,bool create,volatile uint64_t *&out) {
        out=nullptr;unsigned page=0;
        for (unsigned level=level0;level<=3;++level) {
            auto &entry=pages[page].entries[(address>>shift(level))&511];
            if (level==3) { out=&entry;return OK; }
            uint64_t value=entry;
            if (!value) {
                if (!create) return NotReady;
                unsigned child=0;auto status=allocate(child);if (status!=OK) return status;
                api.barrier();entry=pages[child].physical|3;page=child;
            } else {
                if ((value&~Address)!=3) {fault=true;return Failed;}
                unsigned child=0;while (child<count && pages[child].physical!=(value&Address)) ++child;
                // Every table is privately owned; never follow an inherited
                // descriptor or accept a cycle back to an ancestor.
                if (child>=count || child<=page) {fault=true;return Failed;}
                page=child;
            }
        }
        return Failed;
    }
public:
    explicit JobPageTables(API &operations,GPU::Diagnostics::AddressSpaces::Trace *timing=nullptr):api(operations),trace(timing) {}
    bool faulted() const { return fault; }
    bool published() const { return sealed; }
    unsigned tableCount() const { return count; }
    unsigned mappedPages() const { return leaves; }
    uint64_t ttbr() const { return count ? pages[0].physical|(uint64_t(asid)<<48) : 0; }
    MMUStatus initialize(unsigned in,unsigned out,unsigned identifier) {
        if (count || inputBits) return Busy;
        if (in<32 || in>40 || (out!=32 && out!=36 && out!=40) || identifier>65535) return Invalid;
        inputBits=in;outputBits=out;asid=identifier;level0=in>39?0:1;
        unsigned root=0;return allocate(root);
    }
    template<class Source> MMUStatus map(uint64_t address,unsigned n,Source &source,unsigned access) {
        if (!count || fault) return NotReady;
        if (sealed) return Busy;
        const uint64_t end=1ULL<<inputBits;
        if (!n || n>4096 || (address&4095) || address<0x1000000 || address>=end ||
            uint64_t(n)*4096>end-address || !access || (access&~3u)) return Invalid;
        volatile uint64_t *entry=nullptr;
        for (unsigned i=0;i<n;++i) {
            uint64_t pa=0;MMUStatus status;
            {GPU::Diagnostics::AddressSpaces::Span timing(trace,GPU::Diagnostics::AddressSpaces::Source);status=source.page(i,pa);}
            if (status!=OK || !physical(pa)) return status==OK?Invalid:status;
            const uint64_t va=address+uint64_t(i)*4096;
            // This unpublished tree is private to this serialized builder.
            // Walk once per L3 table, then visit each actual leaf in order.
            // Source pages belong to the held, validated DMA mapping. Every
            // private leaf still checks its PA, access and duplicate mappings.
            if(!entry || !(va&0x1fffff)) {
                status=locate(va,true,entry);
                if(status!=OK)return status;
            } else ++entry;
            const uint64_t descriptor=pa|3|(1ULL<<6)|(2ULL<<8)|(1ULL<<10)|(1ULL<<11)|(3ULL<<53)|
                ((access&2)?0:1ULL<<7); // Normal-NC, AF/nG, OSH, unprivileged, XN
            if (*entry && *entry!=descriptor) return Denied;
            if (!*entry) { *entry=descriptor;++leaves; }
        }
        return OK;
    }
    MMUStatus publish() {
        if (!count || fault) return NotReady;
        if (sealed) return Busy;
        // CPU writes can be cached even when the SMMU table walker is not
        // coherent. Publish every owned page to PoC before exposing this root.
        // Children precede parents; no partial tree is a submitted context.
        api.barrier();
        for(unsigned i=count;i--;) {
            const auto status=api.publish(pages[i]);
            if(status!=OK){fault=true;return status;}
        }
        api.barrier();sealed=true;return OK;
    }
    MMUStatus translate(uint64_t address,uint64_t &physicalAddress) {
        physicalAddress=0;if (!count || fault || address>=(1ULL<<inputBits)) return NotReady;
        volatile uint64_t *entry=nullptr;auto status=locate(address,false,entry);
        if (status!=OK || !entry || !*entry) return status==OK?NotReady:status;
        if ((*entry&3)!=3 || !physical(*entry&Address)) return Failed;
        physicalAddress=(*entry&Address)|(address&4095);return OK;
    }
    MMUStatus destroy(bool dmaRetired) {
        if (fault || (sealed && !dmaRetired)) return Busy;
        for (unsigned i=count;i--;) api.release(pages[i],true);
        count=leaves=inputBits=outputBits=asid=level0=0;sealed=false;return OK;
    }
};
struct PageTableContext {
    uint64_t job=0,kernel=0;
    bool valid() const {
        constexpr uint64_t physicalMask=0x000000fffffff000ULL;
        return job && kernel && !(job&4095) && !(kernel&4095) &&
            !(job&~(physicalMask|0xffff000000000000ULL)) &&
            !(kernel&~(physicalMask|0xffff000000000000ULL)) &&
            (job&physicalMask) && (kernel&physicalMask) &&
            (job&physicalMask)!=(kernel&physicalMask) && (job>>48)!=(kernel>>48);
    }
};
}
