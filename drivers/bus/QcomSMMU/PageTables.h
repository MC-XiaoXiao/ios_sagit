// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerHardware.h>
namespace SMMU {
using QcomPower::Status;
using namespace QcomPower;
struct TablePage {
    void *owner=nullptr;
    volatile uint64_t *entries=nullptr;
    uint64_t physical=0;
};
// ARM64 stage-1, 4 KiB granule. The API supplies exclusive uncached table
// storage, a store barrier and a context-ASID invalidation with completed
// TLBSYNC. It must serialize callers and keep context/clock ownership alive.
// A failed sync pins table pages and every associated client mapping. No
// destructor frees DMA-visible storage; destroy requires the root detached.
template<class API,unsigned Capacity=256> class PageTables {
    struct Page { TablePage memory{}; bool used=false,retired=false; } pages[Capacity];
    API &api;
    unsigned inputBits=0, outputBits=0, firstLevel=0, mapped=0;
    bool fault=false;
    static constexpr uint64_t Address=0x0000fffffffff000ULL;
    static unsigned shift(unsigned level) { return 39-9*level; }
    static unsigned entryIndex(uint64_t iova,unsigned level) { return unsigned(iova>>shift(level))&511; }
    bool range(uint64_t iova,unsigned count) const {
        const uint64_t bytes=uint64_t(count)*4096;
        return inputBits && count && count<=4096 && iova>=4096 && !(iova&4095) &&
            iova<(1ULL<<inputBits) && bytes<=(1ULL<<inputBits)-iova;
    }
    bool physical(uint64_t address) const {
        return !(address&4095) && address<(1ULL<<outputBits) && 4096<=(1ULL<<outputBits)-address;
    }
    int find(uint64_t address) const {
        for (unsigned i=0;i<Capacity;++i) if (pages[i].used && !pages[i].retired && pages[i].memory.physical==address) return int(i);
        return -1;
    }
    Status allocate(unsigned &index) {
        for (index=0;index<Capacity;++index) if (!pages[index].used) break;
        if (index==Capacity) return NotReady;
        TablePage p{}; auto s=api.allocate(p);
        if (s!=OK) return s;
        if (!p.owner || !p.entries || (reinterpret_cast<uintptr_t>(p.entries)&7) || !p.physical || !physical(p.physical)) {
            api.release(p); return Invalid;
        }
        if (find(p.physical)>=0) { api.release(p); fault=true; return Failed; }
        for (unsigned n=0;n<512;++n) p.entries[n]=0;
        pages[index]={p,true,false}; return OK;
    }
    Status leaf(uint64_t iova,bool create,volatile uint64_t *&entry) {
        unsigned page=0;
        for (unsigned level=firstLevel;level<3;++level) {
            auto &pte=pages[page].memory.entries[entryIndex(iova,level)];
            const auto value=pte;
            if (!value) {
                if (!create) { entry=nullptr; return OK; }
                unsigned child=0; auto s=allocate(child); if (s!=OK) return s;
                api.barrier(); // initialize a table before publishing its link
                pte=pages[child].memory.physical|3; page=child;
            } else {
                if ((value&~Address)!=3) { fault=true; return Failed; }
                const int child=find(value&Address);
                if (child<0) { fault=true; return Failed; }
                page=unsigned(child);
            }
        }
        entry=&pages[page].memory.entries[entryIndex(iova,3)]; return OK;
    }
    bool prune(unsigned page,unsigned level) {
        bool empty=true;
        for (unsigned n=0;n<512;++n) {
            auto &entry=pages[page].memory.entries[n]; const auto value=entry;
            if (value && level<3) {
                const int child=find(value&Address);
                if ((value&~Address)!=3 || child<0) { fault=true; return false; }
                if (prune(unsigned(child),level+1)) {
                    entry=0; pages[child].retired=true;
                }
                if (fault) return false;
            }
            if (entry) empty=false;
        }
        return empty;
    }
    Status synchronize() {
        if (fault) return Stale;
        api.barrier(); const auto status=api.sync();
        if (status!=OK) {
            fault=true;
            // PTEs are already visible. Busy/NotReady/Denied here cannot be
            // reported as a clean mapping rejection to the IOKit caller.
            return status==TimedOut || status==Failed ? status : Stale;
        }
        for (unsigned i=1;i<Capacity;++i) if (pages[i].used && pages[i].retired) {
            api.release(pages[i].memory); pages[i]={};
        }
        return OK;
    }
    Status rollback(uint64_t iova,unsigned count,Status cause) {
        for (unsigned i=0;i<count;++i) {
            volatile uint64_t *entry=nullptr;
            auto s=leaf(iova+uint64_t(i)*4096,false,entry);
            if (s!=OK || !entry || !*entry) { fault=true; return Stale; }
            *entry=0; --mapped;
        }
        prune(0,firstLevel);
        const auto s=synchronize(); return s==OK ? cause : s;
    }
public:
    explicit PageTables(API &operations):api(operations) { static_assert(Capacity>=4,"at least four translation levels"); }
    PageTables(const PageTables &)=delete;
    PageTables &operator=(const PageTables &)=delete;
    bool faulted() const { return fault; }
    unsigned mappedPages() const { return mapped; }
    unsigned allocatedTables() const {
        unsigned count=0; for (const auto &p:pages) count+=p.used; return count;
    }
    uint64_t root() const { return inputBits && pages[0].used ? pages[0].memory.physical : 0; }
    Status initialize(unsigned iovaBits,unsigned paBits) {
        if (inputBits) return Busy;
        if (iovaBits<32 || iovaBits>40 || paBits<32 || paBits>40) return Invalid;
        inputBits=iovaBits; outputBits=paBits; firstLevel=iovaBits>39 ? 0 : 1;
        unsigned index=0; const auto status=allocate(index);
        if (status!=OK) { inputBits=outputBits=0; return status; }
        api.barrier(); return OK;
    }
    template<class Source> Status map(uint64_t iova,unsigned count,Source &source,unsigned access) {
        if (fault) return Stale;
        if (!range(iova,count) || !access || (access&~3u)) return Invalid;
        for (unsigned i=0;i<count;++i) {
            volatile uint64_t *entry=nullptr;
            const auto s=leaf(iova+uint64_t(i)*4096,false,entry); if (s!=OK) return s;
            if (entry && *entry) return Busy; // no break-before-make overwrite
        }
        // nG, AF, outer-shareable Normal-NC (MAIR index0), unprivileged,
        // execute-never. S1 cannot express write-only: that request uses RW,
        // as in ARM's stage-1 IOMMU model; read-only remains protected.
        const uint64_t attributes=3|(1ULL<<6)|(2ULL<<8)|(1ULL<<10)|(1ULL<<11)|(3ULL<<53)|
            ((access&2) ? 0 : 1ULL<<7);
        for (unsigned i=0;i<count;++i) {
            uint64_t pa=0; auto s=source.page(i,pa);
            if (s!=OK || !physical(pa)) return rollback(iova,i,s==OK ? Invalid : s);
            volatile uint64_t *entry=nullptr;
            s=leaf(iova+uint64_t(i)*4096,true,entry);
            if (s!=OK) return fault ? Stale : rollback(iova,i,s);
            *entry=pa|attributes; ++mapped;
        }
        return synchronize(); // also invalidate cached faults/walk state
    }
    Status unmap(uint64_t iova,unsigned count) {
        if (fault) return Stale;
        if (!range(iova,count)) return Invalid;
        for (unsigned i=0;i<count;++i) {
            volatile uint64_t *entry=nullptr;
            const auto s=leaf(iova+uint64_t(i)*4096,false,entry); if (s!=OK) return s;
            if (!entry || !*entry) return Invalid; // validate the whole range first
        }
        for (unsigned i=0;i<count;++i) {
            volatile uint64_t *entry=nullptr;
            const auto s=leaf(iova+uint64_t(i)*4096,false,entry); if (s!=OK) return s;
            *entry=0; --mapped;
        }
        prune(0,firstLevel); return synchronize();
    }
    Status translate(uint64_t iova,uint64_t &pa,unsigned &access) {
        if (fault) return Stale;
        if (!inputBits || iova>=(1ULL<<inputBits)) return Invalid;
        volatile uint64_t *entry=nullptr;
        const auto s=leaf(iova,false,entry); if (s!=OK) return s;
        if (!entry || !*entry) return NotReady;
        const auto pte=*entry;
        if ((pte&3)!=3) { fault=true; return Failed; }
        pa=(pte&Address)|(iova&4095); access=(pte&(1ULL<<7)) ? 1u : 3u;
        return OK;
    }
    Status destroy() {
        if (!inputBits) return OK;
        // isolated means this root is uninstalled and no hardware walker can
        // resume it, not merely that one DMA request has completed.
        auto s=api.isolated(); if (s!=OK) return s;
        api.barrier(); s=api.sync(); if (s!=OK) { fault=true; return s==TimedOut || s==Failed ? s : Stale; }
        for (auto &p:pages) if (p.used) { api.release(p.memory); p={}; }
        inputBits=outputBits=firstLevel=mapped=0; fault=false; return OK;
    }
};
}
