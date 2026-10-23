// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <QcomPowerHardware.h>
namespace SMMU {
using namespace QcomPower;
struct RetainedTablePage { void *owner=nullptr;volatile uint64_t *entries=nullptr;uint64_t physical=0; };
// Adopt, never free, the boot-reserved stage-1 tree. Only vacant leaves inside
// the handed-over allocation window may change. Firmware mappings, TTBRs,
// global SMMU registers and other contexts/streams remain untouched.
template<class API,unsigned Capacity=256> class RetainedPageTables {
public:
    // A walk cursor never caches the resulting PA. Every query rereads all
    // parent descriptors and the selected actual leaf. Callers serialize the
    // tree as with translate(); destroy/re-adoption invalidates saved pointers.
    struct TranslationCursor {
        const RetainedPageTables *owner=nullptr;
        volatile uint64_t *parents[4]={},*leaf=nullptr;
        uint64_t descriptors[4]={},region=0,epoch=0;
        unsigned level=0;
    };
private:
    struct Page { RetainedTablePage memory{};volatile uint64_t *parent=nullptr;bool owned=false; } pages[Capacity]{};
    API &api;
    unsigned count=0,firstLevel=0,paBits=0,inputBits=0,mapped=0;
    unsigned walkPages[4]={};
    uint64_t first=0,end=0,epoch=1;
    bool fault=false;
    static constexpr uint64_t Address=0x0000fffffffff000ULL;
    static unsigned shift(unsigned level) {return 39-9*level;}
    bool physical(uint64_t p) const {return p && !(p&4095) && p<(1ULL<<paBits);}
    Status unhealthy(Status s) {fault=true;return s==TimedOut || s==Failed?s:Stale;}
    Status page(uint64_t pa,unsigned &index) {
        if (!physical(pa)) return Failed;
        for (index=0;index<count;++index) if (pages[index].memory.physical==pa) return OK;
        if (count==Capacity) return NotReady;
        RetainedTablePage p{};auto s=api.borrow(pa,p);if (s!=OK) return s;
        if (!p.owner || !p.entries || (reinterpret_cast<uintptr_t>(p.entries)&7) || p.physical!=pa) {
            api.release(p,false);return Failed;
        }
        index=count++;pages[index].memory=p;return OK;
    }
    Status locate(uint64_t va,bool create,volatile uint64_t *&entry,unsigned &level,TranslationCursor *cursor=nullptr) {
        unsigned current=0;entry=nullptr;uint64_t visited[4]={};
        if(cursor)*cursor={};
        for (level=firstLevel;level<=3;++level) {
            for (unsigned i=firstLevel;i<level;++i)if(visited[i]==pages[current].memory.physical)return Failed;
            visited[level]=pages[current].memory.physical;
            auto &pte=pages[current].memory.entries[(va>>shift(level))&511];auto value=pte;
            if(cursor){cursor->parents[level]=&pte;cursor->descriptors[level]=value;}
            if (level==3) {entry=&pte;return OK;}
            if (!value) {
                if (!create) return OK;
                if (count==Capacity) return NotReady;
                RetainedTablePage p{};auto s=api.allocate(p);if (s!=OK) return s;
                if (!p.owner || !p.entries || !physical(p.physical) || (reinterpret_cast<uintptr_t>(p.entries)&7)) {
                    api.release(p,true);return Failed;
                }
                for (unsigned i=0;i<count;++i) if (pages[i].memory.physical==p.physical) {pages[count++]={p,nullptr,true};return unhealthy(Failed);}
                for (unsigned i=0;i<512;++i) p.entries[i]=0;
                api.barrier();pte=p.physical|3;
                current=count;pages[count++]={p,&pte,true};
            } else if ((value&3)==3) {
                // Table permission restrictions would override our leaf AP.
                if (create && (value&(3ULL<<61))) return Unsupported;
                // Adjacent leaves usually share each parent table. Reuse its
                // retained-page index only when the descriptor just read still
                // names that physical page. Keep every PTE/permission/cycle
                // check; no translation or descriptor value is cached.
                current=walkPages[level];
                if(current>=count || pages[current].memory.physical!=(value&Address)) {
                    auto s=page(value&Address,current);if (s!=OK) return s;
                    walkPages[level]=current;
                }
            } else {entry=&pte;return (value&3)==1?OK:Failed;}
        }
        return Failed;
    }
    bool range(uint64_t address,unsigned n) const {
        return first && n && n<=4096 && !(address&4095) && address>=first && address<end && uint64_t(n)*4096<=end-address;
    }
    Status sync() {api.barrier();auto s=api.sync();return s==OK?OK:unhealthy(s);}
public:
    explicit RetainedPageTables(API &operations):api(operations) {}
    bool faulted() const {return fault;}
    unsigned mappedPages() const {return mapped;}
    unsigned tableCount() const {return count;}
private:
    template<class Contract> Status initialize(const Contract &c) {
        ++epoch;firstLevel=c.inputBits>39?0:1;paBits=c.outputBits;inputBits=c.inputBits;first=c.first;end=c.end;
        unsigned root=0;auto s=page(c.ttbr&Address,root);if (s!=OK) return s;
        // Prove the complete allocator aperture is vacant before publishing
        // the mapper, including guard pages. Do not infer this from a flag.
        for (uint64_t va=first;va<end;) {
            volatile uint64_t *entry=nullptr;unsigned level=0;s=locate(va,false,entry,level);
            if (s!=OK || (entry && *entry)) return s==OK?Busy:s;
            const uint64_t span=1ULL<<shift(level);
            va=(va&~(span-1))+span;
        }
        return OK;
    }
public:
    template<class Contract> Status adopt(const Contract &c) {
        if (first || count) return Busy;
        if (!c.valid()) return Invalid;
        auto status=api.isolated();return status==OK?initialize(c):status;
    }
    // Keep a running consumer's root and boot leaves intact. The distinct
    // proof covers retired firmware producers, reserved boot pages, an
    // exclusive context/stream and pinned resources. It is not DMA isolation.
    template<class Contract> Status adoptLive(const Contract &c) {
        if (first || count) return Busy;
        if (!c.valid()) return Invalid;
        auto status=api.liveOwnership();if(status!=OK)return status;
        status=initialize(c);return status==OK?api.liveOwnership():status;
    }
    Status translate(uint64_t va,uint64_t &pa) {
        pa=0;if (!count || fault) return NotReady;
        if(va>=(1ULL<<inputBits))return Invalid;
        volatile uint64_t *entry=nullptr;unsigned level=0;auto s=locate(va,false,entry,level);
        if (s!=OK || !entry || !*entry) return s==OK?NotReady:s;
        auto pte=*entry;
        if ((level==3 && (pte&3)!=3) || (level<3 && (pte&3)!=1)) return Failed;
        uint64_t mask=(1ULL<<shift(level))-1;
        pa=(pte&Address&~mask)|(va&mask);
        return pa<(1ULL<<paBits)?OK:Failed;
    }
    Status translate(uint64_t va,uint64_t &pa,TranslationCursor &cursor) {
        pa=0;
        if(!count || fault){cursor={};return NotReady;}
        if(va>=(1ULL<<inputBits)){cursor={};return Invalid;}
        bool valid=cursor.owner==this && cursor.epoch==epoch && cursor.leaf &&
            cursor.level>=firstLevel && cursor.level<=3 &&
            cursor.region==(va>>(cursor.level==3?21:shift(cursor.level)));
        if(valid)for(unsigned level=firstLevel;level<cursor.level;++level)
            if(!cursor.parents[level] || *cursor.parents[level]!=cursor.descriptors[level]){valid=false;break;}
        // A cached block may have become a table (or vanished). Rewalk its
        // current descriptor rather than treating a valid replacement as a
        // bad leaf or retaining an old block interpretation.
        if(valid && cursor.level<3 && (*cursor.leaf&3)!=1)valid=false;
        if(!valid) {
            volatile uint64_t *entry=nullptr;unsigned level=0;
            auto status=locate(va,false,entry,level,&cursor);
            if(status!=OK || !entry){cursor={};return status==OK?NotReady:status;}
            cursor.owner=this;cursor.epoch=epoch;cursor.level=level;
            cursor.region=va>>(level==3?21:shift(level));
            cursor.leaf=level==3?entry-((va>>12)&511):entry;
        }
        const auto pte=cursor.level==3?cursor.leaf[(va>>12)&511]:*cursor.leaf;
        if(!pte)return NotReady;
        if((cursor.level==3 && (pte&3)!=3) || (cursor.level<3 && (pte&3)!=1))return Failed;
        const uint64_t mask=(1ULL<<shift(cursor.level))-1;
        pa=(pte&Address&~mask)|(va&mask);
        return pa<(1ULL<<paBits)?OK:Failed;
    }
    Status readOnlyBootLeaf(uint64_t va,uint64_t &descriptor) {
        descriptor=0;if(!count || fault)return NotReady;
        if((va&4095) || va>=(1ULL<<inputBits))return Invalid;
        volatile uint64_t *entry=nullptr;unsigned level=0;auto s=locate(va,false,entry,level);
        if(s!=OK || !entry)return s==OK?NotReady:s;
        const uint64_t expected=3|(1ULL<<6)|(1ULL<<7)|(2ULL<<8)|(1ULL<<10)|(1ULL<<11)|(3ULL<<53);
        const uint64_t value=*entry;
        if(level!=3 || (value&~Address)!=expected || !physical(value&Address))return Denied;
        descriptor=value;return OK;
    }
    template<class Source> Status map(uint64_t address,unsigned n,Source &source,unsigned access) {
        if (fault) return Stale;
        if (!range(address,n) || !access || (access&~3u)) return Invalid;
        for (unsigned i=0;i<n;++i) {
            volatile uint64_t *entry=nullptr;unsigned level=0;auto s=locate(address+uint64_t(i)*4096,false,entry,level);
            if (s!=OK || (entry && *entry)) return s==OK?Busy:s;
        }
        unsigned written=0;Status cause=OK;
        for (;written<n;++written) {
            uint64_t pa=0;cause=source.page(written,pa);if (cause!=OK || !physical(pa)) {if(cause==OK)cause=Invalid;break;}
            volatile uint64_t *entry=nullptr;unsigned level=0;cause=locate(address+uint64_t(written)*4096,true,entry,level);
            if (cause!=OK || !entry || level!=3 || *entry) {if(cause==OK)cause=Failed;break;}
            // MAIR[0]=Normal-NC; AF/nG, outer-shareable, unprivileged, XN.
            *entry=pa|3|(1ULL<<6)|(2ULL<<8)|(1ULL<<10)|(1ULL<<11)|(3ULL<<53)|((access&2)?0:1ULL<<7);
            ++mapped;
        }
        if (written!=n) {
            for (unsigned i=0;i<written;++i) {
                volatile uint64_t *entry=nullptr;unsigned level=0;
                auto s=locate(address+uint64_t(i)*4096,false,entry,level);
                if (s!=OK || !entry || level!=3) return unhealthy(Failed);
                *entry=0;--mapped;
            }
        }
        // Even a failed allocation can have exposed new table links. Keep
        // those pages resident until shutdown; retire leaf writes by TLBSYNC.
        auto s=sync();return s==OK?cause:s;
    }
    Status unmap(uint64_t address,unsigned n) {
        if (fault) return Stale;
        if (!range(address,n)) return Invalid;
        for (unsigned i=0;i<n;++i) {
            volatile uint64_t *entry=nullptr;unsigned level=0;auto s=locate(address+uint64_t(i)*4096,false,entry,level);
            if (s!=OK || !entry || level!=3 || !*entry) return Invalid;
        }
        for (unsigned i=0;i<n;++i) {
            volatile uint64_t *entry=nullptr;unsigned level=0;auto s=locate(address+uint64_t(i)*4096,false,entry,level);
            if (s!=OK || !entry) return unhealthy(Failed);
            *entry=0;--mapped;
        }
        return sync();
    }
    Status destroy() {
        ++epoch;
        if (!count) {first=end=0;firstLevel=paBits=inputBits=0;return OK;}
        if (mapped || fault) return Busy;
        bool owned=false;for (unsigned i=0;i<count;++i)owned|=pages[i].owned;
        if (!owned) {
            for (unsigned i=count;i--;)api.release(pages[i].memory,false);
            count=firstLevel=paBits=inputBits=0;first=end=0;return OK;
        }
        auto s=api.isolated();if (s!=OK) return s;
        for (unsigned i=count;i--;) if (pages[i].owned) {
            for (unsigned j=0;j<512;++j) if (pages[i].memory.entries[j]) return unhealthy(Stale);
            if (!pages[i].parent || *pages[i].parent!=(pages[i].memory.physical|3)) return unhealthy(Stale);
            *pages[i].parent=0;
        }
        s=sync();if (s!=OK) return s;
        for (unsigned i=count;i--;) api.release(pages[i].memory,pages[i].owned);
        count=firstLevel=paBits=inputBits=mapped=0;first=end=0;return OK;
    }
};
}
