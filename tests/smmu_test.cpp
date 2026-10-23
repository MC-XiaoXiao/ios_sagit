// SPDX-License-Identifier: GPL-2.0-only
#include <PageTables.h>
#include <ContextTLB.h>
#include <DMACommandState.h>
#include <PhysicalPages.h>
#include <StreamAudit.h>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <vector>
using namespace QcomPower;
struct PhysicalReader {
    bool listed=true,shortSpan=false;
    unsigned shift=14;
    uint64_t skip=0;
    std::vector<uint64_t> frames{0x20000,0x40000,0x60000};
    bool hasPageList() const {return listed;}
    uint64_t pageCount() const {return frames.size();}
    uint64_t pageOffset() const {return skip;}
    uint64_t pageFrame(unsigned index) const {assert(index<frames.size());return frames[index];}
    uint64_t segment(uint64_t offset,uint64_t &span) const {
        const uint64_t index=offset>>shift,within=offset&((1ULL<<shift)-1);
        if (index>=frames.size()) {span=0;return 0;}
        span=shortSpan ? 1:(1ULL<<shift)-within;
        return (frames[index]<<shift)+within;
    }
};
struct Tables {
    std::map<uint64_t,void *> storage;
    uint64_t next=0x10000000;
    unsigned allocs=0,failAllocation=0,releases=0,syncs=0,barriers=0,failSync=0;
    bool detached=true;
    Status syncFailure=TimedOut;
    Status allocate(SMMU::TablePage &p) {
        if (++allocs==failAllocation) return NotReady;
        void *bytes=std::aligned_alloc(4096,4096); assert(bytes);
        p={bytes,static_cast<uint64_t *>(bytes),next}; storage[next]=bytes; next+=4096;
        return OK;
    }
    void release(SMMU::TablePage &p) {
        assert(p.owner && storage.at(p.physical)==p.owner);
        storage.erase(p.physical); std::free(p.owner); p={}; ++releases;
    }
    void barrier() { ++barriers; }
    Status sync() { assert(barriers); return ++syncs==failSync ? syncFailure : OK; }
    Status isolated() { return detached ? OK : Busy; }
    uint64_t walk(uint64_t root,unsigned bits,uint64_t iova) {
        auto *page=static_cast<uint64_t *>(storage.at(root));
        for (unsigned level=bits>39?0:1;level<3;++level) {
            const uint64_t pte=page[(iova>>(39-9*level))&511];
            if (!pte) return 0;
            assert((pte&3)==3); page=static_cast<uint64_t *>(storage.at(pte&0x0000fffffffff000ULL));
        }
        return page[(iova>>12)&511];
    }
    ~Tables() { for (auto &p:storage) std::free(p.second); }
};
struct Source {
    uint64_t first=0x90000000;
    unsigned failAt=UINT32_MAX;
    Status page(unsigned i,uint64_t &p) { if (i==failAt) return NotReady; p=first+uint64_t(i)*8192; return OK; }
};
struct SMMURegisters {
    std::map<uint32_t,uint32_t> words{{0x20,1u<<30},{0x24,2u<<28|6u},{0x28,1u<<12},{0x8024,5u<<16}};
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    unsigned delays=0,barriers=0,busyReads=0;
    bool hang=false,bad=false;
    uint32_t read(uint32_t o) {
        if ((o&0xfff)==0x7f4) { if(bad)return UINT32_MAX; if(hang || busyReads) { if(busyReads)--busyReads; return 1; } }
        return words[o];
    }
    void write(uint32_t o,uint32_t v) { words[o]=v; writes.push_back({o,v}); }
    void barrier() { ++barriers; }
    void delay(unsigned us) { delays+=us; }
};
static void streamAuditChecks() {
    using Audit=SMMU::StreamAudit<>;using Result=Audit::Result;
    struct Contract {unsigned bank=0,streamCount=2;struct {uint32_t slot,id;} streams[8]{{0,0x10},{127,0x20}};} c;
    uint32_t routes[8]{};
    struct IO {
        uint32_t groups=128,match[128]{},route[128]{};
        unsigned reads=0,barriers=0;bool open=false;
        uint32_t read(uint32_t offset) {assert(offset==0x20 && !open);open=true;return groups;}
        uint32_t streamConfigurationRead(uint32_t offset) {
            assert(open);++reads;
            if(offset>=0xc00)return route[(offset-0xc00)/4];
            return match[(offset-0x800)/4];
        }
        void streamConfigurationReadComplete(){assert(open);open=false;++barriers;}
    } io;
    auto reference=[&]() -> Result {
        unsigned count=io.groups&255;if(count>128)count=128;
        for(unsigned slot=0;slot<count;++slot) {
            const auto smr=io.match[slot],route=io.route[slot];
            if(smr==UINT32_MAX || route==UINT32_MAX)return {Failed,Audit::Unreadable,0xc00+4*slot,route,0};
            bool owned=false;
            for(unsigned i=0;i<c.streamCount;++i)if(slot==c.streams[i].slot) {
                if(smr!=((1u<<31)|c.streams[i].id))return {Stale,Audit::Match,0x800+4*slot,smr,(1u<<31)|c.streams[i].id};
                if(route!=routes[i])return {Stale,Audit::Route,0xc00+4*slot,route,routes[i]};
                owned=true;
            }
            if(!owned && (smr&(1u<<31))) {
                if(!(route&0x30000) && (route&255)==c.bank)return {Denied,Audit::ForeignBank,0xc00+4*slot,route,0};
                for(unsigned i=0;i<c.streamCount;++i)if(!((smr^c.streams[i].id)&65535&~((smr>>16)&32767)))
                    return {Denied,Audit::Alias,0x800+4*slot,smr,0};
            }
        }
        return {};
    };
    Audit audit;unsigned cases=0;
    auto check=[&]() {
        const auto expected=reference();const auto reads=io.reads,barriers=io.barriers;
        audit.capture(io);assert(!io.open);
        const auto got=audit.validate(c,routes);
        assert(got.status==expected.status && got.reason==expected.reason && got.offset==expected.offset &&
            got.observed==expected.observed && got.expected==expected.expected);
        assert(io.reads-reads==2*std::min(io.groups&255,128u) && io.barriers==barriers+1);++cases;
    };
    auto reset=[&]() {for(unsigned i=0;i<128;++i){io.match[i]=0;io.route[i]=0;}io.groups=128;io.match[0]=(1u<<31)|0x10;io.match[127]=(1u<<31)|0x20;};
    reset();check();assert(audit.validate(c,routes).status==OK);
    for(unsigned slot=0;slot<128;++slot) {
        reset();io.match[slot]=UINT32_MAX;check();
        reset();io.route[slot]=UINT32_MAX;check();
        if(slot && slot!=127) {
            reset();io.match[slot]=(1u<<31)|0x40;check();assert(audit.validate(c,routes).reason==Audit::ForeignBank);
            io.route[slot]=1;check();assert(audit.validate(c,routes).status==OK);
            io.match[slot]=(1u<<31)|(0x3fu<<16)|0x40;check();
            io.match[slot]=(1u<<31)|0x10;check();assert(audit.validate(c,routes).reason==Audit::Alias);
        }
    }
    for(unsigned n=0;n<2;++n) {
        reset();io.match[c.streams[n].slot]^=1;check();assert(audit.validate(c,routes).reason==Audit::Match);
        reset();io.route[c.streams[n].slot]=1;check();assert(audit.validate(c,routes).reason==Audit::Route);
    }
    uint32_t random=0x4498;
    for(unsigned trial=0;trial<10000;++trial) {
        reset();
        for(unsigned k=0;k<4;++k) {
            random=random*1664525u+1013904223u;unsigned slot=(random>>16)&127;
            random=random*1664525u+1013904223u;io.match[slot]=random;
            random=random*1664525u+1013904223u;io.route[slot]=random;
        }
        check();
    }
    for(unsigned count:{0u,1u,63u,127u,128u,255u}){reset();io.groups=count;check();}
    reset();audit.capture(io);assert(audit.validate(c,nullptr).status==Invalid);
    printf("SMMU stream audit: %u legacy-result differential cases; every slot freshly read, one completion barrier, no MMIO writes\n",cases);
}
int main() {
    streamAuditChecks();
    for (unsigned shift:{12u,14u,16u}) for (unsigned skip:{0u,1u,4095u,12289u}) {
        if (skip>=(1u<<shift)) continue;
        PhysicalReader reader;reader.shift=shift;reader.skip=skip;
        const uint64_t bytes=uint64_t(3)*(1u<<shift)-skip-3;
        SMMU::PhysicalPages<PhysicalReader> upl(reader);
        assert(upl.initialize(0,bytes,shift)==OK && upl.pageOffset()==(skip&4095));
        const unsigned count=unsigned((bytes+upl.pageOffset()+4095)/4096);
        std::vector<uint64_t> addresses;
        for (unsigned n=0;n<count;++n) {
            uint64_t pa=0;const auto pos=uint64_t(skip)+uint64_t(n)*4096-upl.pageOffset();
            assert(upl.page(n,pa)==OK && pa==(reader.frames[pos>>shift]<<shift)+(pos&((1u<<shift)-1)));
            addresses.push_back(pa);
        }
        uint64_t pa=0;assert(upl.page(count,pa)==Invalid && !pa);
        reader.listed=false;SMMU::PhysicalPages<PhysicalReader> md(reader);
        assert(md.initialize(skip,bytes,shift)==OK && md.pageOffset()==upl.pageOffset());
        for (unsigned n=0;n<count;++n) assert(md.page(n,pa)==OK && pa==addresses[n]);
    }
    {
        PhysicalReader reader;SMMU::PhysicalPages<PhysicalReader> source(reader);
        uint64_t pa=0;assert(source.page(0,pa)==Invalid && source.initialize(1,4096,14)==Invalid);
        reader.skip=16384;assert(source.initialize(0,4096,14)==Invalid);reader.skip=16383;
        assert(source.initialize(0,49152,14)==Invalid);reader.skip=0;
        assert(source.initialize(0,4096,11)==Invalid && source.initialize(0,4096,17)==Invalid);
        reader.frames[0]=0;assert(source.initialize(0,4096,14)==Invalid);
        reader.frames[0]=UINT64_MAX;assert(source.initialize(0,4096,14)==Invalid);
        reader.frames[0]=0x20000;assert(source.initialize(0,16385,14)==OK);
        reader.frames[1]=0;assert(source.page(4,pa)==Invalid);
        assert(source.initialize(0,4096,14)==Invalid); // one initialization per prepared source
        reader.listed=false;reader.shortSpan=true;SMMU::PhysicalPages<PhysicalReader> shortMD(reader);
        assert(shortMD.initialize(1,4096,14)==Invalid);
    }
    {
        using namespace USB;
        DMACommandState state; unsigned completed=0,cleared=0;
        state.prepared(Result::Success,true);
        assert(state.clear([&]{++completed;return Result::Timeout;},[&]{++cleared;return Result::Success;})==Result::Timeout);
        assert(state.faulted() && completed==1 && !cleared);
        assert(state.clear([&]{++completed;return Result::NotReady;},[&]{++cleared;return Result::Success;})==Result::HardwareError);
        assert(completed==1 && !cleared); // consumed native completion must not be retried
        assert(state.recover([]{return Result::Busy;})==Result::Busy && state.faulted());
        assert(state.recover([]{return Result::Success;})==Result::Success && !state.faulted());
        assert(state.clear([&]{++completed;return Result::HardwareError;},[&]{++cleared;return Result::Success;})==Result::Success);
        assert(completed==1 && cleared==1); // detached-root proof never repeats complete()
        assert(state.recover([]{assert(false);return Result::Success;})==Result::Invalid);
        DMACommandState normal; normal.prepared(Result::Success,true);
        assert(normal.clear([&]{++completed;return Result::Success;},[&]{++cleared;return Result::Busy;})==Result::Busy);
        const auto n=completed;
        assert(normal.clear([&]{++completed;return Result::NotReady;},[&]{++cleared;return Result::Success;})==Result::Success && completed==n);
        DMACommandState early; early.prepared(Result::Invalid,false);
        assert(early.clear([]{return Result::NotReady;},[]{return Result::Success;})==Result::Success);
        DMACommandState unknown; unknown.prepared(Result::HardwareError,true);
        assert(unknown.clear([]{assert(false);return Result::Success;},[]{assert(false);return Result::Success;})==Result::HardwareError);
        DMACommandState stolen; stolen.prepared(Result::Success,true);
        assert(stolen.clear([]{return Result::NotReady;},[]{assert(false);return Result::Success;})==Result::NotReady && stolen.faulted());
    }
    for (unsigned bits:{32u,36u,39u,40u}) {
        Tables api; SMMU::PageTables<Tables> pt(api); Source source;
        assert(pt.initialize(bits,40)==OK && pt.initialize(bits,40)==Busy);
        const uint64_t iova=bits==40 ? (1ULL<<39)-4096 : (1ULL<<30)-4096;
        assert(pt.map(iova,1024,source,1)==OK && pt.mappedPages()==1024);
        for (unsigned i=0;i<1024;++i) {
            uint64_t pa=0;unsigned access=0;
            assert(pt.translate(iova+uint64_t(i)*4096+17,pa,access)==OK && pa==source.first+uint64_t(i)*8192+17 && access==1);
            const auto pte=api.walk(pt.root(),bits,iova+uint64_t(i)*4096);
            assert((pte&3)==3 && (pte&(3ULL<<53))==(3ULL<<53) && (pte&(1ULL<<10)) && (pte&(1ULL<<7)));
        }
        assert(pt.map(iova,1,source,3)==Busy);
        assert(pt.unmap(iova-4096,1025)==Invalid && pt.mappedPages()==1024);
        api.detached=false; assert(pt.destroy()==Busy);
        assert(pt.unmap(iova,1024)==OK && pt.mappedPages()==0 && pt.allocatedTables()==1);
        assert(pt.map(0x4000,1,source,2)==OK);
        uint64_t pa=0;unsigned access=0;assert(pt.translate(0x4000,pa,access)==OK && access==3);
        assert(pt.unmap(0x4000,1)==OK);api.detached=true;
        assert(pt.destroy()==OK && api.storage.empty());
    }
    for (unsigned failed=1;failed<=4;++failed) {
        Tables api;api.failAllocation=failed;SMMU::PageTables<Tables> pt(api);Source source;
        if(failed==1) { assert(pt.initialize(40,40)==NotReady && api.storage.empty());continue; }
        assert(pt.initialize(40,40)==OK);
        assert(pt.map(0x10000,2,source,3)==NotReady && !pt.faulted() && !pt.mappedPages() && pt.allocatedTables()==1);
        api.failAllocation=0;assert(pt.map(0x10000,2,source,3)==OK && pt.unmap(0x10000,2)==OK && pt.destroy()==OK);
    }
    for (unsigned failure:{0u,1u,255u,256u,511u,512u,1023u}) {
        Tables api;SMMU::PageTables<Tables> pt(api);Source source;source.failAt=failure;
        assert(pt.initialize(40,40)==OK && pt.map(0x1ff000,1024,source,3)==NotReady && !pt.mappedPages() && pt.allocatedTables()==1);
        assert(pt.destroy()==OK && api.storage.empty());
    }
    for (bool unmap:{false,true}) {
        Tables api;SMMU::PageTables<Tables> pt(api);Source source;
        assert(pt.initialize(40,40)==OK);
        if(unmap) assert(pt.map(0x10000,2,source,3)==OK);
        api.failSync=api.syncs+1;
        assert((unmap?pt.unmap(0x10000,2):pt.map(0x10000,2,source,3))==TimedOut && pt.faulted());
        const auto held=api.storage.size();assert(held>1 && pt.map(0x10000,1,source,3)==Stale && pt.unmap(0x10000,1)==Stale);
        api.detached=false;assert(pt.destroy()==Busy && api.storage.size()==held);
        api.detached=true;assert(pt.destroy()==OK && api.storage.empty());
    }
    for (auto status:{NotReady,Busy,Denied,Invalid}) {
        Tables api;SMMU::PageTables<Tables> pt(api);Source source;
        assert(pt.initialize(40,40)==OK);api.failSync=1;api.syncFailure=status;
        assert(pt.map(4096,1,source,3)==Stale && pt.faulted() && pt.allocatedTables()==4);
        assert(pt.destroy()==OK && api.storage.empty());
    }
    {
        Tables api;SMMU::PageTables<Tables,4> pt(api);Source source;
        assert(pt.initialize(40,40)==OK && pt.map(0x1000,1,source,3)==OK);
        assert(pt.map(0x200000,1,source,3)==NotReady && pt.mappedPages()==1 && pt.allocatedTables()==4);
        uint64_t pa=0;unsigned access=0;assert(pt.translate(0x1000,pa,access)==OK && pa==source.first);
        assert(pt.unmap(0x1000,1)==OK && pt.destroy()==OK);
    }
    {
        Tables api;SMMU::PageTables<Tables> pt(api);Source source;
        assert(pt.initialize(31,40)==Invalid && pt.initialize(40,41)==Invalid && !api.allocs);
        assert(pt.initialize(32,32)==OK);
        for (uint64_t iova:{0ULL,4097ULL,1ULL<<32,static_cast<unsigned long long>(UINT64_MAX)})assert(pt.map(iova,1,source,3)==Invalid);
        assert(pt.map(4096,0,source,3)==Invalid && pt.map(4096,4097,source,3)==Invalid && pt.map(4096,1,source,0)==Invalid);
        source.first=1ULL<<32;assert(pt.map(4096,1,source,3)==Invalid && pt.allocatedTables()==1);
        assert(pt.destroy()==OK);
    }
    {
        Tables api;SMMU::PageTables<Tables> pt(api);Source source;assert(pt.initialize(40,40)==OK);
        uint32_t random=1234567;
        for (unsigned n=0;n<2000;++n) {
            random=random*1664525u+1013904223u;
            const uint64_t iova=((uint64_t(random)<<4)&((1ULL<<40)-1)&~4095ULL)+4096;
            const unsigned count=1+(random%32);
            assert(pt.map(iova,count,source,3)==OK && pt.unmap(iova,count)==OK && pt.allocatedTables()==1);
        }
        assert(pt.destroy()==OK && api.storage.empty());
    }
    {
        SMMURegisters io;SMMU::ContextTLB<SMMURegisters> tlb(io);
        assert(tlb.configure(0x10000,0,5)==OK && tlb.bankOffset()==0x8000);
        io.busyReads=25;assert(tlb.sync(100)==OK && io.delays==3);
        assert((io.writes==std::vector<std::pair<uint32_t,uint32_t>>{{0x8610,5},{0x87f0,UINT32_MAX}}));
        io.hang=true;assert(tlb.sync(50)==TimedOut && io.delays==53 && tlb.faulted());
        const auto writes=io.writes.size();assert(tlb.sync()==Stale && io.writes.size()==writes);
        SMMURegisters stale;stale.words[0x8024]=6u<<16;SMMU::ContextTLB<SMMURegisters> wrong(stale);
        assert(wrong.configure(0x10000,0,5)==OK && wrong.sync()==Failed && stale.writes.empty());
        SMMURegisters bad;bad.bad=true;SMMU::ContextTLB<SMMURegisters> invalid(bad);
        assert(invalid.configure(0x10000,0,5)==OK && invalid.sync()==Failed);
        SMMURegisters geometry;SMMU::ContextTLB<SMMURegisters> unsupported(geometry);
        assert(unsupported.configure(0x10000,6,5)==Invalid && unsupported.configure(0x20000,0,5)==Invalid);
        geometry.words[0x28]=0;assert(unsupported.configure(0x10000,0,5)==Unsupported && geometry.writes.empty());
    }
    puts("SMMU: LPAE ranges/permissions, rollback/pruning, 2000 sparse reuse cycles, ASID sync faults and native DMA completion quarantine passed");
}
