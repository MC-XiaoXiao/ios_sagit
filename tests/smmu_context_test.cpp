// SPDX-License-Identifier: GPL-2.0-only
#include <HardwareContext.h>
#include <ContextHandoff.h>
#include <BypassHandoff.h>
#include "../drivers/usb/QcomUSB/USBDirectDMA.h"
#include <MappingLedger.h>
#include <MappingEngine.h>
#include <PageTables.h>
#include <cstdlib>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
using namespace QcomPower;
struct Hardware {
    static constexpr uint32_t Bank=0x9000;
    struct Write { uint32_t offset;uint64_t value;unsigned bytes; };
    std::map<uint32_t,uint32_t> words;
    std::vector<Write> writes;
    bool reset=true,accessibleFlag=true,quirk=false,hang=false;
    unsigned failWrite=0,failRead=0,elapsed=0,barriers=0;
    Hardware() {
        words[0x20]=(1u<<30)|(1u<<27)|(15u<<9)|8;
        words[0x24]=(2u<<28)|6;words[0x28]=(1u<<12)|(2u<<8)|(2u<<4)|2;
        words[0x808]=(1u<<31)|0x1400;words[0xc08]=0x2000001;
        words[0x1004]=0x1f300;words[0x1804]=1;
        words[Bank]=0x1067;words[Bank+0x10]=(7u<<15)|18;words[Bank+0x30]=(1u<<23)|(2u<<12)|24;
        words[Bank+0x20]=0x11000000;words[Bank+0x24]=5u<<16;
    }
    Status accessible() { return accessibleFlag ? OK : NotReady; }
    Status isolated() { return reset ? OK : Busy; }
    uint32_t read(uint32_t o) {
        if (failRead) {--failRead;return UINT32_MAX;}
        if (o==Bank+0x7f4 && hang) return 1;
        return words[o];
    }
    uint64_t read64(uint32_t o) {
        if (failRead) {--failRead;return UINT64_MAX;}
        return uint64_t(words[o])|(uint64_t(words[o+4])<<32);
    }
    void write(uint32_t o,uint32_t v) {
        writes.push_back({o,v,4});
        if (o==0xc08 && quirk) {
            const auto type=v&0x30000;
            if (type==0x20000) { if(writes.size()==failWrite)failRead=1;return; }
            if (type==0x10000) v=(v&~0x30000u)|0x20000;
        }
        if (o==Bank+0x58) words[o]&=~v;
        else words[o]=v;
        if (writes.size()==failWrite) failRead=1;
    }
    void write64(uint32_t o,uint64_t v) {
        writes.push_back({o,v,8});words[o]=uint32_t(v);words[o+4]=uint32_t(v>>32);
        if (writes.size()==failWrite) failRead=1;
    }
    void barrier() {++barriers;}
    void delay(unsigned us) {elapsed+=us;}
    void scopedWrites() const {
        for (const auto &w:writes) assert(w.offset==0x808 || w.offset==0xc08 || w.offset==0x1004 || w.offset==0x1804 ||
            (w.offset>=Bank && w.offset<Bank+0x1000));
    }
};
struct TableStorage {
    SMMU::HardwareContext<Hardware> &context;
    std::map<uint64_t,void *> pages;
    uint64_t next=0x12000000;
    explicit TableStorage(SMMU::HardwareContext<Hardware> &c):context(c) {}
    Status allocate(SMMU::TablePage &page) {
        void *bytes=std::aligned_alloc(4096,4096);assert(bytes);
        page={bytes,static_cast<volatile uint64_t *>(bytes),next};pages[next]=bytes;next+=4096;return OK;
    }
    void release(SMMU::TablePage &page) {assert(pages.erase(page.physical));std::free(page.owner);page={};}
    void barrier() {}
    Status sync() {return context.sync();}
    Status isolated() {return context.rootDetached();}
    ~TableStorage() {assert(pages.empty());}
};
struct References {
    std::map<uintptr_t,unsigned> held;
    void retain(uintptr_t memory,uintptr_t command) {++held[memory];if(command)++held[command];}
    void release(uintptr_t memory,uintptr_t command) {assert(held[memory]);--held[memory];if(command){assert(held[command]);--held[command];}}
};
struct DataPages {
    unsigned failAt=UINT32_MAX;
    Status page(unsigned i,uint64_t &physical) {if(i==failAt)return NotReady;physical=0x90000000+uint64_t(i)*8192;return OK;}
};
static const SMMU::Stream owned[]{ {2,0x1400} };
static Status configure(SMMU::HardwareContext<Hardware> &c) {return c.configure(0x10000,1,5,40,40,owned,1);}
static void contextInterruptModels() {
    struct Lines {
        struct Line {uint32_t irq,trigger,nativeTrigger;bool controller=true;};
        std::vector<Line> values;
        unsigned fail=0,reads=0,change=0;
        unsigned count() const {return unsigned(values.size());}
        bool read(unsigned index,uint32_t &irq,uint32_t &trigger) {
            ++reads;if (index>=values.size() || fail==reads) return false;
            auto &v=values[index];irq=v.irq;trigger=v.trigger^(reads==change ? 1:0);
            return v.controller && trigger<=1 && trigger==v.nativeTrigger;
        }
    };
    for (unsigned banks:{1u,6u,128u}) for (unsigned mode=0;mode<3;++mode) {
        SMMU::IORTTopology route{};route.contextCount=banks;
        Lines lines;lines.values.push_back({163,1,1});
        for (unsigned bank=0;bank<banks;++bank) {
            const unsigned edge=mode==2 ? bank%2:mode;route.interrupts[bank]=396+bank;route.interruptFlags[bank]=edge;
            lines.values.push_back({396+bank,1-edge,1-edge});
        }
        unsigned count=0;assert(SMMU::contextInterruptList(route,lines,count) && count==1+banks);
        lines.reads=0;
        for (unsigned bank=0;bank<banks;++bank) {
            unsigned index=0;Lines full=lines;
            assert(SMMU::contextInterruptRoute(route,full,bank,index) && index==1+bank);
            SMMU::ContextInterruptBinding binding;
            assert(!binding.valid(route,full) && binding.capture(route,full,bank) &&
                binding.index==1+bank && binding.valid(route,full));
            auto rewired=route;rewired.interruptFlags[bank]^=1;Lines updated=lines;
            updated.values[1+bank].trigger^=1;updated.values[1+bank].nativeTrigger^=1;
            assert(SMMU::contextInterruptRoute(rewired,updated,bank,index) && !binding.valid(rewired,updated));
            rewired=route;rewired.interrupts[bank]+=256;updated=lines;updated.values[1+bank].irq+=256;
            assert(SMMU::contextInterruptRoute(rewired,updated,bank,index) && !binding.valid(rewired,updated));
            Lines compact;compact.values.push_back(lines.values[0]);compact.values.push_back(lines.values[1+bank]);
            assert(SMMU::contextInterruptRoute(route,compact,bank,index) && index==1);
            assert(binding.valid(route,compact)==(bank==0));
            compact.values[1].trigger^=1;compact.values[1].nativeTrigger^=1;
            assert(!SMMU::contextInterruptRoute(route,compact,bank,index) && !index);
        }
        // Every provider-read failure, stale native type and changed selected
        // specifier must reject the exact same helper called by Native code.
        Lines baseline=lines;unsigned index=0;
        assert(SMMU::contextInterruptRoute(route,baseline,0,index));
        for (unsigned n=1;n<=baseline.reads;++n) {
            Lines broken=lines;broken.fail=n;index=99;
            assert(!SMMU::contextInterruptRoute(route,broken,0,index) && !index);
        }
        for (unsigned bank=0;bank<banks;++bank) {
            Lines broken=lines;broken.values[1+bank].nativeTrigger^=1;
            assert(!SMMU::contextInterruptRoute(route,broken,0,index));
            broken=lines;broken.values[1+bank].controller=false;
            assert(!SMMU::contextInterruptRoute(route,broken,0,index));
        }
        Lines changed=lines;changed.change=baseline.reads;
        assert(!SMMU::contextInterruptRoute(route,changed,0,index));
        auto invalid=route;invalid.interruptFlags[0]=2;assert(!SMMU::contextInterruptRoute(invalid,lines,0,index));
        invalid=route;invalid.interrupts[0]=31;assert(!SMMU::contextInterruptRoute(invalid,lines,0,index));
        invalid=route;invalid.interrupts[0]=1020;assert(!SMMU::contextInterruptRoute(invalid,lines,0,index));
        invalid=route;invalid.interrupts[0]=163;Lines alias=lines;alias.values[1].irq=163;
        assert(!SMMU::contextInterruptRoute(invalid,alias,0,index));
        invalid=route;invalid.contextCount=129;assert(!SMMU::contextInterruptRoute(invalid,lines,0,index));
        assert(!SMMU::contextInterruptRoute(route,lines,banks,index));
        Lines empty;assert(!SMMU::contextInterruptList(route,empty,count) && !count);
        if (banks>1) {
            invalid=route;invalid.interrupts[1]=invalid.interrupts[0];
            assert(!SMMU::contextInterruptRoute(invalid,lines,0,index));
            Lines compact;compact.values.push_back(lines.values[0]);compact.values.push_back(lines.values[1]);
            assert(!SMMU::contextInterruptRoute(invalid,compact,0,index));
            compact=lines;compact.values[1]=compact.values[2];
            assert(!SMMU::contextInterruptRoute(route,compact,0,index));
        }
    }
    puts("SMMU IRQ: actual IORT edge/level to Apple trigger matching, full/selected bank routes, shared-line rejection and every provider read fault passed");
}
static void captureModels() {
    struct Capture:Hardware {
        unsigned reads=0,failAt=0,checks=0,loseAt=0,changeAt=0;
        Status accessible() {return ++checks==loseAt ? NotReady:Hardware::accessible();}
        uint32_t read(uint32_t offset) {
            assert(offset<=0x10000-4);++reads;
            if(reads==changeAt)words[0x81c]=(1u<<31)|0x1500;
            return reads==failAt ? UINT32_MAX:Hardware::read(offset);
        }
        uint64_t read64(uint32_t offset) {
            assert(offset<=0x10000-8);++reads;
            if(reads==changeAt)words[0x81c]=(1u<<31)|0x1500;
            return reads==failAt ? UINT64_MAX:Hardware::read64(offset);
        }
    };
    SMMU::IORTTopology route{};route.base=0x1680000;route.span=0x10000;
    route.stream=0x1400;route.addressBits=40;route.contextCount=8;route.interrupts[1]=397;
    auto capture=[&](Capture &io,SMMU::ContextHandoff &h) {return SMMU::captureInheritedContext(io,route,0xa800000,40,h);};
    Capture original;original.words[0x1004]|=0x37;SMMU::ContextHandoff h;
    assert(capture(original,h)==OK && h.valid(route,0xa800000,40) && h.bank==1 && h.slot==2 && h.asid==5 &&
        (h.cbar&255)==0x37 && h.ttbr0==0x5000011000000ULL && h.unchanged(original) && original.writes.empty());
    Capture baseline;assert(capture(baseline,h)==OK);const auto total=baseline.reads;
    // HYP can retain these guest VMIDs while using one SID-assigned physical
    // namespace. Neither distinct VMIDs nor distinct ASIDs proves isolation.
    for (unsigned selectedVMID:{0u,55u,255u}) for (unsigned otherVMID:{0u,1u,55u,56u,255u})
        for (unsigned otherASID:{5u,6u,261u}) for (unsigned type:{0u,1u,2u,3u}) for (unsigned format:{0u,1u}) {
            Capture io;io.words[0x1004]=0x1f300|selectedVMID;
            io.words[0xa000]=1;io.words[0x1008]=(type<<16)|otherVMID;io.words[0x1808]=format;
            io.words[0xa010]=16;io.words[0xa024]=otherASID<<16;
            assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
            io.words[0xa000]=0;
            assert(capture(io,h)==OK && h.otherContextsInactive(io) && h.unchanged(io) && io.writes.empty());
        }
    for (unsigned n=1;n<=total;++n) {
        Capture io;io.failAt=n;h.controller=1;
        assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
    }
    for (unsigned n=1;n<=baseline.checks;++n) {
        Capture io;io.loseAt=n;assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
    }
    for (unsigned f=0;f<26;++f) {
        Capture io;
        switch(f) {
        case 0:io.reset=false;break;
        case 1:io.accessibleFlag=false;break;
        case 2:io.words[0x20]=(io.words[0x20]&~255u)|129;break;
        case 3:io.words[0x24]=(2u<<28)|32;break;
        case 4:io.words[0x800]=(1u<<31)|(1u<<16)|0x1400;break;
        case 5:io.words[0x810]=(1u<<31)|0x1400;break;
        case 6:io.words[0x810]=(1u<<31)|0x1401;io.words[0xc10]=1;break;
        case 7:io.words[0xc08]=0x10000;break;
        case 8:io.words[0xc08]=0x20000;break;
        case 9:io.words[0xc08]=8;break;
        case 10:io.words[Hardware::Bank]=0;break;
        case 11:io.words[0x1804]=0x10001;break;
        case 12:io.words[Hardware::Bank+0x10]&=~16u;break;
        case 13:io.words[Hardware::Bank+0x10]|=3;break;
        case 14:io.words[Hardware::Bank+0x20]|=1;break;
        case 15:io.words[Hardware::Bank+0x28]=4096;break;
        case 16:io.words[0xa000]=1;io.words[0x1008]=0x1f301;io.words[0x1808]=1;
            io.words[0xa010]=16;io.words[0xa024]=5u<<16;break;
        case 17:io.words[0xa000]=1;io.words[0x1008]=0x1f300;break;
        case 18:io.words[0x20]&=~(1u<<27);break;
        case 19:io.words[0x24]|=2u<<16;break;
        case 20:io.words[0x808]=0;break;
        case 21:io.words[0x1004]=0;break;
        case 22:io.words[0x1804]=0;break;
        case 23:io.words[Hardware::Bank+0x20]=0;break;
        case 24:io.words[Hardware::Bank+0x30]|=1u<<22;break;
        case 25:io.words[0]=1;break;
        }
        assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
        if(f<2)assert(!io.reads);
        if(f==2 || f==3)assert(io.reads==4); // reject layouts before scanning registers
    }
    // A changed, unrelated route also invalidates the snapshot. Capture must
    // not publish a claim assembled across two hardware configurations.
    Capture changed;changed.changeAt=total-1;assert(capture(changed,h)==Stale && !h.controller && changed.writes.empty());
    route.interruptFlags[1]=1;Capture edge;
    assert(capture(edge,h)==OK && h.valid(route,0xa800000,40) && edge.writes.empty());
    route.interrupts[2]=route.interrupts[1];Capture shared;
    assert(capture(shared,h)!=OK && !h.controller && shared.writes.empty());
    route.interrupts[2]=0;route.interruptFlags[1]=2;Capture reserved;
    assert(capture(reserved,h)!=OK && !h.controller && reserved.writes.empty());
    route.interruptFlags[1]=0;
    for(unsigned bits:{32u,36u,40u}) {
        Capture io;io.words[Hardware::Bank+0x10]=(7u<<15)|16|(bits==40 ? 2u:bits==36 ? 1u:0u);
        assert(SMMU::captureInheritedContext(io,route,0xa800000,bits,h)==OK && h.outputBits==bits && io.writes.empty());
    }
    Capture io;route.span=4096;assert(capture(io,h)==Invalid && !io.reads);
    puts("SMMU capture: stopped resources, sole active visible bank, guest-VMID alias rejection, bounded snapshot and revocation passed");
}
static void freshContextModels() {
    struct Cold:Hardware {
        unsigned reads=0,failAt=0,checks=0,loseAt=0,changeAt=0;
        bool enforcePublication=false;
        Cold() {
            words.clear();words[0]=0x200626;words[0x20]=0x48017a10;words[0x24]=0x20008006;words[0x28]=0x7111;
            for (unsigned n=0;n<16;++n) words[0xc00+4*n]=0x200ff;
        }
        Status accessible() {return ++checks==loseAt ? NotReady:Hardware::accessible();}
        uint32_t read(uint32_t offset) {
            assert(offset<=0x10000-4);++reads;
            if (reads==changeAt) words[0x83c]=(1u<<31)|0x1500;
            return reads==failAt ? UINT32_MAX:Hardware::read(offset);
        }
        uint64_t read64(uint32_t offset) {
            assert(offset<=0x10000-8);++reads;
            if (reads==changeAt) words[0x83c]=(1u<<31)|0x1500;
            return reads==failAt ? UINT64_MAX:Hardware::read64(offset);
        }
        void write(uint32_t offset,uint32_t value) {
            if (enforcePublication && offset==0x800 && (value&(1u<<31)) &&
                ((words[0xc00]&0x30000) || !(words[0x8000]&1))) {
                writes.push_back({offset,value,4});return;
            }
            Hardware::write(offset,value);
        }
        void scoped() const {
            for (const auto &w:writes)
                assert(w.offset==0x800 || w.offset==0xc00 || w.offset==0x1000 || w.offset==0x1800 ||
                       (w.offset>=0x8000 && w.offset<0x9000));
        }
    };
    SMMU::IORTTopology route{};route.base=0x1680000;route.span=0x10000;route.stream=0x1400;
    route.addressBits=36;route.contextCount=8;
    for (unsigned n=0;n<8;++n) route.interrupts[n]=396+n;
    auto capture=[&](Cold &io,SMMU::ContextHandoff &h) {return SMMU::captureFreshContext(io,route,0xa800000,32,0,h);};
    Cold original;SMMU::ContextHandoff h;
    assert(capture(original,h)==OK && h.fresh() && h.valid(route,0xa800000,32) && h.bank==0 && h.slot==0 && !h.asid && original.writes.empty());
    const auto total=original.reads,checks=original.checks;
    SMMU::ContextHandoff decoded;
    assert(SMMU::ContextHandoff::decode(&h,sizeof(h),decoded) && decoded.valid(route,0xa800000,32));
    for (unsigned n=1;n<=total;++n) {
        Cold io;io.failAt=n;
        assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
    }
    for (unsigned n=1;n<=checks;++n) {
        Cold io;io.loseAt=n;
        assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
    }
    for (unsigned failure=0;failure<18;++failure) {
        Cold io;
        switch(failure) {
        case 0:io.reset=false;break;
        case 1:io.accessibleFlag=false;break;
        case 2:io.words[0]&=~(1u<<10);break;
        case 3:io.words[0x800]=1u<<31;break;
        case 4:io.words[0x83c]=0x1234;break;
        case 5:io.words[0xc3c]=0x20000;break;
        case 6:io.words[0x9000]=1;break;
        case 7:io.words[0x1004]=0x10000;break;
        case 8:io.words[0x1804]=1;break;
        case 9:io.words[0x9010]=16;break;
        case 10:io.words[0x9030]=24;break;
        case 11:io.words[0x9034]=1;break;
        case 12:io.words[0x9038]=0x44;break;
        case 13:io.words[0x903c]=1;break;
        case 14:io.words[0x9020]=0x12000000;break;
        case 15:io.words[0x9028]=0x12000000;break;
        case 16:io.words[0x9058]=2;break;
        case 17:io.words[0x20]&=~(1u<<30);break;
        }
        assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
    }
    Cold raced;raced.changeAt=total-1;
    assert(capture(raced,h)!=OK && !h.controller && raced.writes.empty());
    Cold io;io.enforcePublication=true;assert(capture(io,h)==OK);
    SMMU::HardwareContext<Cold> context(io);const SMMU::Stream stream{uint16_t(h.slot),uint16_t(h.stream)};
    assert(context.configure(h.aperture,h.bank,h.asid,h.inputBits,h.outputBits,&stream,1)==OK);
    assert(context.attach(0x12000000,true)==OK && context.check()==OK);
    assert(io.words[0x800]==0x80001400 && (io.words[0xc00]&0x300ff)==0);
    assert(context.detach()==OK && context.rootDetached()==OK);io.scoped();
    const auto writeCount=io.writes.size();
    for (unsigned n=1;n<=writeCount;++n) {
        Cold broken;assert(capture(broken,h)==OK);
        SMMU::HardwareContext<Cold> partial(broken);
        assert(partial.configure(h.aperture,h.bank,h.asid,h.inputBits,h.outputBits,&stream,1)==OK);
        broken.failWrite=n;
        const auto status=partial.attach(0x12000000,true);
        if (status==OK) partial.detach();
        else assert(partial.hasHardwareOwnership());
        broken.failWrite=0;
        assert(partial.detach()==OK && partial.rootDetached()==OK);broken.scoped();
    }
    puts("SMMU cold allocation: actual unused stream/context/IRQ, repeated blank-state proof, every capture fault and scoped attach/detach write failure passed");
}

static void bypassModels() {
    struct Bypass:Hardware {
        unsigned reads=0,failAt=0,checks=0,loseAt=0,changeAt=0;
        Bypass() {words[0xc08]=0x20100ff;}
        Status accessible() {return ++checks==loseAt ? NotReady:Hardware::accessible();}
        uint32_t read(uint32_t offset) {
            assert(offset<0x1000);++reads; // no context/ASID/TTBR access is permitted
            if(reads==changeAt)words[0xc1c]^=1;
            return reads==failAt ? UINT32_MAX:Hardware::read(offset);
        }
    };
    SMMU::IORTTopology route{};route.base=0x1680000;route.span=0x10000;
    route.stream=0x1400;route.addressBits=40;route.contextCount=8;
    auto capture=[&](Bypass &io,SMMU::BypassHandoff &h) {return SMMU::captureBypass(io,route,0xa800000,36,h);};
    Bypass baseline;SMMU::BypassHandoff h;
    assert(capture(baseline,h)==OK && h.valid(route,0xa800000,36) && h.slot==2 && h.s2cr==0x20100ff && baseline.writes.empty());
    const auto total=baseline.reads,checks=baseline.checks;
    baseline.reset=false;assert(h.unchanged(baseline)); // live bypass lease does not require reset
    baseline.words[0xc08]^=1;assert(!h.unchanged(baseline));baseline.words[0xc08]^=1;
    baseline.words[0x810]=(1u<<31)|(1u<<16)|0x1401;assert(!h.unchanged(baseline));
    for(unsigned n=1;n<=total;++n) {
        Bypass io;io.failAt=n;h.controller=1;assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
    }
    for(unsigned n=1;n<=checks;++n) {
        Bypass io;io.loseAt=n;assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
    }
    for(unsigned f=0;f<19;++f) {
        Bypass io;
        switch(f) {
        case 0:io.reset=false;break;
        case 1:io.accessibleFlag=false;break;
        case 2:io.words[0]=1;break;
        case 3:io.words[0]=8;break;
        case 4:io.words[0]=0x80000000;break;
        case 5:io.words[0x20]=(io.words[0x20]&~255u)|129;break;
        case 6:io.words[0x20]&=~(1u<<27);break;
        case 7:io.words[0x20]=(io.words[0x20]&~255u)|2;break;
        case 8:io.words[0x24]=0;break;
        case 9:io.words[0x28]=0;break;
        case 10:io.words[0x808]=0;break;
        case 11:io.words[0x800]=(1u<<31)|(1u<<16)|0x1400;break;
        case 12:io.words[0x810]=(1u<<31)|0x1400;break;
        case 13:io.words[0xc08]=0x200ff;break; // Qualcomm rewritten BYPASS is actually FAULT
        case 14:io.words[0xc08]=1;io.words[Hardware::Bank]=0;break; // disabled context is not this proof
        case 15:io.words[0xc08]=0x30000;break;
        case 16:io.words[0xc08]=UINT32_MAX;break;
        case 17:io.words[0x20]=(io.words[0x20]&~0x1e00u)|(12u<<9);break;
        case 18:io.words[0x808]|=1u<<16;break;
        }
        assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
        if(f<2)assert(!io.reads);
        if(f==5 || f==8)assert(io.reads==4);
    }
    Bypass changed;changed.changeAt=total;assert(capture(changed,h)==Stale && !h.controller && changed.writes.empty());
    Bypass io;route.coherent=true;assert(capture(io,h)==Invalid && !io.reads);route.coherent=false;
    for(unsigned bits=32;bits<=40;++bits) {
        Bypass current;assert(SMMU::captureBypass(current,route,0xa800000,bits,h)==OK && h.addressBits==bits);
        for(unsigned n=0;n<sizeof(h)/4;++n) {
            auto bad=h;auto *p=reinterpret_cast<unsigned char *>(&bad);
            for(unsigned b=0;b<4;++b)p[4*n+b]=255;
            assert(!bad.valid(route,0xa800000,bits) || !bad.unchanged(current));
        }
        SMMU::BypassHandoff copy;
        assert(SMMU::BypassHandoff::decode(&h,h.bytes,copy) && h.same(copy));
        assert(!SMMU::BypassHandoff::decode(&h,h.bytes-1,copy));
        assert(!SMMU::BypassHandoff::decode(&h,sizeof(h),copy)); // v1 wire is still64 bytes
    }
    struct Direct:Bypass {
        SMMU::BypassHandoff proof;
        USB::DirectDMAWindow window{1,32,0,0,uint64_t(1)<<36};
        USB::Result readDMAWindow(USB::DirectDMAWindow &out) {out=window;return USB::Result::Success;}
        USB::Result dmaAccessible() {return accessibleFlag ? USB::Result::Success:USB::Result::Timeout;}
        USB::Result dmaIsolated() {return reset ? USB::Result::Success:USB::Result::Busy;}
        USB::Result dmaRoute() {return proof.unchanged(*this) ? USB::Result::Success:USB::Result::HardwareError;}
    } direct;
    assert(capture(direct,direct.proof)==OK);
    USB::DirectDMA<Direct> lease(direct);
    assert(lease.acquire(36)==USB::Result::Success && lease.owned());
    direct.reset=false;assert(lease.check()==USB::Result::Success);
    direct.accessibleFlag=false;const auto readCount=direct.reads;
    assert(lease.check()==USB::Result::Timeout && lease.faulted() && direct.reads==readCount);
    assert(lease.release()==USB::Result::Timeout && direct.reads==readCount && lease.owned());
    direct.accessibleFlag=true;direct.reset=true;
    assert(lease.release()==USB::Result::Success && !lease.owned());
    assert(lease.acquire(36)==USB::Result::Success);direct.words[0xc08]=0x200ff;
    assert(lease.check()==USB::Result::HardwareError && lease.faulted());
    assert(lease.release()==USB::Result::HardwareError && lease.owned());
    direct.words[0xc08]=direct.proof.s2cr;
    assert(lease.release()==USB::Result::Success && direct.writes.empty());
    puts("SMMU bypass: exact live BYPASS route, bounded capture, no context access or writes, and retained route verification passed");
}
static void disabledBypassModels() {
    struct Disabled:Hardware {
        unsigned reads=0,checks=0,resetChecks=0,failAt=0,loseAt=0,resetLostAt=0;
        unsigned changeAt=0;uint32_t changeOffset=0;
        unsigned bank=1,pageShift=12;
        uint32_t cb() const {return (8u+bank)<<pageShift;}
        uint32_t gr1() const {return 1u<<pageShift;}
        Disabled(unsigned index=1,unsigned shift=12):bank(index),pageShift(shift) {
            words[0x24]=(2u<<28)|6|(shift==16 ? 1u<<31:0);
            words[0xc08]=0x2000000|bank;
            // A quiet AArch64 FSR includes FORMAT=2 independently of faults.
            words[cb()]=0;words[cb()+0x58]=0x400;
            words[gr1()+4*bank]=0x10000;words[gr1()+0x800+4*bank]=1;
        }
        Status accessible() {return ++checks==loseAt ? NotReady:Hardware::accessible();}
        Status isolated() {return ++resetChecks==resetLostAt ? Busy:Hardware::isolated();}
        uint32_t read(uint32_t offset) {
            // Observe only the selected bank's control/fault and GR1 fields;
            // no root, ASID, TCR, other bank or namespace access is allowed.
            assert(offset<0x1000 || offset==cb() || offset==cb()+0x58 ||
                offset==gr1()+4*bank || offset==gr1()+0x800+4*bank);
            ++reads;if(reads==changeAt)words[changeOffset]^=1;
            return reads==failAt ? UINT32_MAX:Hardware::read(offset);
        }
    };
    SMMU::IORTTopology route{};route.base=0x1680000;route.span=0x10000;
    route.stream=0x1400;route.addressBits=40;route.contextCount=6;
    auto capture=[&](Disabled &io,SMMU::BypassHandoff &h) {
        return SMMU::captureBypass(io,route,0xa800000,36,h,SMMU::BypassMode::DisabledContext);
    };
    Disabled baseline;SMMU::BypassHandoff h;
    assert(capture(baseline,h)==OK && h.valid(route,0xa800000,36) && h.version==2 && h.bytes==80 &&
        h.bank==1 && h.sctlr==0 && h.cbar==0x10000 && h.cba2r==1 && baseline.writes.empty());
    const auto total=baseline.reads,checks=baseline.checks,resetChecks=baseline.resetChecks;
    baseline.reset=false;assert(h.unchanged(baseline));
    for(unsigned n=1;n<=total;++n) {
        Disabled io;io.failAt=n;assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
    }
    for(unsigned n=1;n<=checks;++n) {
        Disabled io;io.loseAt=n;assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
        if(n==1)assert(!io.reads);
    }
    for(unsigned n=1;n<=resetChecks;++n) {
        Disabled io;io.resetLostAt=n;assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
        if(n==1)assert(!io.reads);
    }
    for(unsigned f=0;f<25;++f) {
        Disabled io;const auto original=route;
        switch(f) {
        case 0:io.words[io.cb()]=1;break; // active translation
        case 1:io.words[io.cb()]=2;break; // M=0 alone is insufficient
        case 2:io.words[io.gr1()+4]=0;break; // stage2 translation
        case 3:io.words[io.gr1()+4]=0x30000;break; // combined translation
        case 4:io.words[io.gr1()+4]|=1;break; // unknown VMID/attributes
        case 5:io.words[io.gr1()+0x804]=2;break;
        case 6:io.words[io.cb()+0x58]=2;break;
        case 7:io.words[io.cb()+0x58]=0x40000000;break;
        case 8:io.words[0x20]&=~(1u<<30);break;
        case 9:io.words[0x24]|=2u<<16;break; // stage2-only bank
        case 10:io.words[0xc08]=6;break; // bank outside actual ID1 count
        case 11:route.contextCount=1;break;
        case 12:route.contextCount=129;break;
        case 13:io.words[0x24]=(2u<<28)|9;break; // context span exceeds aperture
        case 14:io.words[0x80c]=(1u<<31)|0x1500;io.words[0xc0c]=1;break; // shared bank
        case 15:io.words[0xc08]=0x10001;break; // architected mode needs request1
        case 16:io.words[0xc08]=0x20001;break;
        case 17:io.words[0x808]=0;break;
        case 18:io.words[0x80c]=(1u<<31)|(0xffu<<16)|0x1400;break;
        case 19:io.words[0]=1;break;
        case 20:io.words[0]=1u<<31;break;
        case 21:io.words[0x24]=(2u<<28);break;
        case 22:io.words[io.gr1()+4]=UINT32_MAX;break;
        case 23:io.reset=false;break;
        case 24:io.accessibleFlag=false;break;
        }
        assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
        if(f>=23)assert(!io.reads);
        route=original;
    }
    for(unsigned bank:{0u,1u,5u})for(unsigned shift:{12u,16u})for(unsigned format:{0u,1u}) {
        Disabled io(bank,shift);route.span=16u<<shift;
        io.words[io.gr1()+0x800+4*bank]=format;
        io.words[io.cb()+0x58]=format ? 0x400:0x200;
        assert(capture(io,h)==OK && h.contextOffset()==io.cb() && h.cba2r==format && io.writes.empty());
        for(unsigned n=0;n<sizeof(h)/4;++n) {
            auto bad=h;auto *p=reinterpret_cast<unsigned char *>(&bad);
            for(unsigned b=0;b<4;++b)p[4*n+b]=255;
            assert(!bad.valid(route,0xa800000,36) || !bad.unchanged(io));
        }
        SMMU::BypassHandoff copy;
        unsigned char unaligned[81]{};const auto *bytes=reinterpret_cast<const unsigned char *>(&h);
        for(unsigned n=0;n<sizeof(h);++n)unaligned[n+1]=bytes[n];
        assert(SMMU::BypassHandoff::decode(unaligned+1,80,copy) && h.same(copy));
        assert(!SMMU::BypassHandoff::decode(&h,64,copy));
        for(unsigned formatBits:{0u,0x200u,0x400u,0x600u}) {
            io.words[io.cb()+0x58]=formatBits;assert(h.unchanged(io));
        }
        io.words[io.cb()+0x58]=0x400;
        for(uint32_t offset:{io.cb(),io.gr1()+4*bank,io.gr1()+0x800+4*bank,io.cb()+0x58}) {
            io.words[offset]^=2;assert(!h.unchanged(io));io.words[offset]^=2;
        }
        io.words[0x80c]=(1u<<31)|0x1500;io.words[0xc0c]=bank;assert(!h.unchanged(io));
    }
    route.span=0x10000;
    Disabled wrongMode;
    assert(SMMU::captureBypass(wrongMode,route,0xa800000,36,h)!=OK && !h.controller);
    Disabled invalidMode;
    assert(SMMU::captureBypass(invalidMode,route,0xa800000,36,h,SMMU::BypassMode(4))==Invalid && !invalidMode.reads);
    Disabled changed;changed.changeAt=total;changed.changeOffset=changed.cb()+0x58;
    assert(capture(changed,h)==Stale && !h.controller && changed.writes.empty());
    struct Direct:Disabled {
        SMMU::BypassHandoff proof;
        USB::DirectDMAWindow window{1,32,0,0,uint64_t(1)<<36};
        USB::Result readDMAWindow(USB::DirectDMAWindow &out) {out=window;return USB::Result::Success;}
        USB::Result dmaAccessible() {return accessibleFlag ? USB::Result::Success:USB::Result::Timeout;}
        USB::Result dmaIsolated() {return reset ? USB::Result::Success:USB::Result::Busy;}
        USB::Result dmaRoute() {return proof.unchanged(*this) ? USB::Result::Success:USB::Result::HardwareError;}
    } direct;
    assert(capture(direct,direct.proof)==OK);
    USB::DirectDMA<Direct> lease(direct);assert(lease.acquire(36)==USB::Result::Success);
    direct.reset=false;assert(lease.check()==USB::Result::Success);
    direct.accessibleFlag=false;const auto reads=direct.reads;
    assert(lease.check()==USB::Result::Timeout && direct.reads==reads && lease.faulted() && lease.owned());
    direct.accessibleFlag=true;direct.words[direct.cb()]=1;direct.reset=true;
    assert(lease.release()==USB::Result::HardwareError && lease.owned());
    direct.words[direct.cb()]=0;assert(lease.release()==USB::Result::Success && !lease.owned() && direct.writes.empty());
    puts("SMMU disabled bypass: exclusive exact route, stage1/S2-bypass, zero SCTLR, FSR format independent of fault bits, read-only bank observation and retained DMA lifecycle passed");
}
static void unmatchedBypassModels() {
    struct Unmatched:Hardware {
        unsigned reads=0,checks=0,resetChecks=0,failAt=0,loseAt=0,resetLostAt=0,changeAt=0;
        uint32_t changeOffset=0,changeValue=0;
        Unmatched() {words[0x808]=0;}
        Status accessible() {return ++checks==loseAt ? NotReady:Hardware::accessible();}
        Status isolated() {return ++resetChecks==resetLostAt ? Busy:Hardware::isolated();}
        uint32_t read(uint32_t offset) {
            assert(offset<0x1000); // No CBAR, context, ASID or translation-table reads.
            ++reads;if(reads==changeAt)words[changeOffset]=changeValue;
            return reads==failAt ? UINT32_MAX:Hardware::read(offset);
        }
    };
    SMMU::IORTTopology route{};route.base=0x1680000;route.span=0x10000;
    route.stream=0x1400;route.addressBits=36;
    auto capture=[&](Unmatched &io,SMMU::BypassHandoff &h) {
        return SMMU::captureBypass(io,route,0xa800000,36,h,SMMU::BypassMode::UnmatchedStream);
    };
    Unmatched baseline;SMMU::BypassHandoff h;
    assert(capture(baseline,h)==OK && h.valid(route,0xa800000,36) && h.version==3 && h.bytes==64 &&
        h.slot==UINT32_MAX && !h.smr && !h.s2cr && !h.bank && !h.cbar && baseline.writes.empty());
    const auto reads=baseline.reads,checks=baseline.checks,resetChecks=baseline.resetChecks;
    for(unsigned n=1;n<=reads;++n) {
        Unmatched io;io.failAt=n;assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
    }
    for(unsigned n=1;n<=checks;++n) {
        Unmatched io;io.loseAt=n;assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
        if(n==1)assert(!io.reads);
    }
    for(unsigned n=1;n<=resetChecks;++n) {
        Unmatched io;io.resetLostAt=n;assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
        if(n==1)assert(!io.reads);
    }
    for(unsigned count:{1u,8u,128u})for(unsigned shift:{12u,16u})for(unsigned bits:{32u,36u,40u}) {
        Unmatched io;io.words[0x20]=(io.words[0x20]&~255u)|count;
        io.words[0x24]=(2u<<28)|(shift==16 ? 1u<<31:0); // No bank is needed.
        io.words[0x28]=(2u<<4)|2;route.span=16u<<shift;route.addressBits=bits;
        // Unrelated exact/wildcard routes, disabled entries and arbitrary
        // context state cannot be turned into this stream's mapping.
        for(unsigned n=0;n<count;++n) {io.words[0x800+4*n]=(1u<<31)|0x1500;io.words[0xc00+4*n]=0x300ff;}
        assert(SMMU::captureBypass(io,route,0xa800000,bits,h,SMMU::BypassMode::UnmatchedStream)==OK &&
            h.valid(route,0xa800000,bits) && io.writes.empty());
        io.reset=false;assert(h.unchanged(io));
        for(unsigned n=0;n<sizeof(h)/4;++n) {
            auto bad=h;auto *p=reinterpret_cast<unsigned char *>(&bad);p[4*n]^=1;
            assert(!bad.valid(route,0xa800000,bits) || !bad.unchanged(io));
        }
        unsigned char bytes[65]{};const auto *p=reinterpret_cast<const unsigned char *>(&h);
        for(unsigned n=0;n<64;++n)bytes[n+1]=p[n];
        SMMU::BypassHandoff copy;
        assert(SMMU::BypassHandoff::decode(bytes+1,64,copy) && copy.same(h));
        assert(!SMMU::BypassHandoff::decode(&h,80,copy));
        io.words[0x800]=(1u<<31)|0x1400;assert(!h.unchanged(io));
        io.words[0x800]=(1u<<31)|0x1500;io.words[0]|=1u<<10;assert(!h.unchanged(io));
    }
    route.span=0x10000;route.addressBits=36;
    for(unsigned fault=0;fault<18;++fault) {
        Unmatched io;auto topology=route;
        switch(fault) {
        case 0:io.words[0]=1u<<10;break; // Architected no-match fault policy.
        case 1:io.words[0]=1;break;
        case 2:io.words[0]=8;break;
        case 3:io.words[0]=1u<<31;break;
        case 4:io.words[0x808]=(1u<<31)|0x1400;io.words[0xc08]=0x10000;break;
        case 5:io.words[0x808]=(1u<<31)|0x1400;io.words[0xc08]=0x20000;break;
        case 6:io.words[0x808]=(1u<<31)|0x1400;io.words[0xc08]=1;break;
        case 7:io.words[0x808]=(1u<<31)|(255u<<16)|0x1400;break;
        case 8:io.words[0x808]=(1u<<31)|(255u<<16)|0x1480;break;
        case 9:io.words[0x20]&=~(1u<<27);break;
        case 10:io.words[0x20]&=~255u;break;
        case 11:io.words[0x20]=(io.words[0x20]&~255u)|129;break;
        case 12:io.words[0x28]=0;break;
        case 13:io.words[0x800]=UINT32_MAX;break;
        case 14:io.words[0xc00]=UINT32_MAX;break;
        case 15:io.reset=false;break;
        case 16:io.accessibleFlag=false;break;
        case 17:topology.coherent=true;break;
        }
        assert(SMMU::captureBypass(io,topology,0xa800000,36,h,SMMU::BypassMode::UnmatchedStream)!=OK &&
            !h.controller && io.writes.empty());
    }
    for(auto change:std::vector<std::pair<uint32_t,uint32_t>>{{0,1u<<10},{0x800,(1u<<31)|0x1400}}) {
        Unmatched io;io.changeAt=4+2*(io.words[0x20]&255);io.changeOffset=change.first;io.changeValue=change.second;
        assert(capture(io,h)!=OK && !h.controller && io.writes.empty());
    }
    // Selection is explicit: neither existing exact-route mode guesses that
    // absence of a match is an identity mapping.
    Unmatched wrong;assert(SMMU::captureBypass(wrong,route,0xa800000,36,h)==NotReady);
    struct Direct:Unmatched {
        SMMU::BypassHandoff proof;
        USB::DirectDMAWindow window{1,32,0,0,uint64_t(1)<<36};
        USB::Result readDMAWindow(USB::DirectDMAWindow &out) {out=window;return USB::Result::Success;}
        USB::Result dmaAccessible() {return accessibleFlag ? USB::Result::Success:USB::Result::Timeout;}
        USB::Result dmaIsolated() {return reset ? USB::Result::Success:USB::Result::Busy;}
        USB::Result dmaRoute() {return proof.unchanged(*this) ? USB::Result::Success:USB::Result::HardwareError;}
    } direct;
    assert(capture(direct,direct.proof)==OK);
    USB::DirectDMA<Direct> lease(direct);assert(lease.acquire(36)==USB::Result::Success);
    direct.reset=false;assert(lease.check()==USB::Result::Success);
    direct.words[0x808]=(1u<<31)|0x1400;
    assert(lease.check()==USB::Result::HardwareError && lease.faulted() && lease.owned());
    direct.reset=true;assert(lease.release()==USB::Result::HardwareError && lease.owned());
    direct.words[0x808]=0;assert(lease.release()==USB::Result::Success && !lease.owned() && direct.writes.empty());
    puts("SMMU unmatched bypass: USFCFG0, full no-match proof, explicit v3 capture, all read/lease failures and retained DMA lifecycle passed");
}
int main() {
    freshContextModels();
    contextInterruptModels();
    captureModels();
    bypassModels();
    disabledBypassModels();
    unmatchedBypassModels();
    {
        Hardware io;SMMU::ContextHandoff handoff{};
        handoff.controller=0x0a800000;handoff.smmu=0x1680000;handoff.aperture=0x10000;
        handoff.slot=2;handoff.stream=0x1400;handoff.bank=1;handoff.asid=5;handoff.inputBits=40;handoff.outputBits=40;
        handoff.control=io.read(0);handoff.id0=io.read(0x20);handoff.id1=io.read(0x24);handoff.id2=io.read(0x28);
        handoff.smr=io.read(0x808);handoff.s2cr=io.read(0xc08);handoff.sctlr=io.read(Hardware::Bank);
        handoff.cbar=io.read(0x1004);handoff.cba2r=io.read(0x1804);
        handoff.tcr=io.read(Hardware::Bank+0x30);handoff.tcr2=io.read(Hardware::Bank+0x10);
        handoff.contextID=io.read(Hardware::Bank+0x34);handoff.mair0=io.read(Hardware::Bank+0x38);handoff.mair1=io.read(Hardware::Bank+0x3c);
        handoff.ttbr0=io.read64(Hardware::Bank+0x20);handoff.ttbr1=io.read64(Hardware::Bank+0x28);
        SMMU::IORTTopology route{};route.base=handoff.smmu;route.span=handoff.aperture;
        route.stream=0x1400;route.addressBits=40;route.contextCount=8;route.interrupts[1]=397;
        assert(handoff.valid(route,0x0a800000,40) && handoff.unchanged(io) && io.writes.empty());
        assert(!handoff.valid(route,0x0a600000,40) && !handoff.valid(route,0x0a800000,36));
        for (auto offset:{0u,0x20u,0x24u,0x28u,0x808u,0xc08u,0x1004u,0x1804u,Hardware::Bank,
            Hardware::Bank+0x30,Hardware::Bank+0x10,Hardware::Bank+0x34,Hardware::Bank+0x38,
            Hardware::Bank+0x3c,Hardware::Bank+0x20,Hardware::Bank+0x24,Hardware::Bank+0x28}) {
            io.words[offset]^=1;assert(!handoff.unchanged(io));io.words[offset]^=1;
        }
        io.reset=false;assert(!handoff.unchanged(io));io.reset=true;
        io.accessibleFlag=false;assert(!handoff.unchanged(io));io.accessibleFlag=true;
        // MMIO can expose guest CBAR values. Distinct visible VMIDs/ASIDs
        // cannot authorize another active bank in the same physical namespace.
        for (unsigned type:{0u,1u,2u,3u}) for (unsigned vmid:{0u,1u,55u,56u,255u})
            for (unsigned asid:{5u,6u,261u}) for (unsigned format:{0u,1u}) {
                io.words[0xa000]=1;io.words[0x1008]=(type<<16)|vmid;io.words[0x1808]=format;
                io.words[0xa010]=16;io.words[0xa024]=asid<<16;
                assert(!handoff.unchanged(io));
                io.words[0xa000]=0;assert(handoff.unchanged(io));
            }
        for (unsigned n=0;n<sizeof(handoff)/4;++n) {
            auto changed=handoff;auto *bytes=reinterpret_cast<unsigned char *>(&changed);
            for (unsigned b=0;b<4;++b) bytes[n*4+b]=255;
            assert(!changed.valid(route,0x0a800000,40) || !changed.unchanged(io));
        }
        SMMU::ContextHandoff decoded{};
        assert(SMMU::ContextHandoff::decode(&handoff,sizeof(handoff),decoded) && decoded.unchanged(io));
        assert(!SMMU::ContextHandoff::decode(&handoff,sizeof(handoff)-1,decoded));
        auto invalid=handoff;invalid.reserved=1;assert(!SMMU::ContextHandoff::decode(&invalid,sizeof(invalid),decoded));
        assert(io.writes.empty());
    }
    unsigned attachWrites=0,detachWrites=0;
    {
        Hardware io;SMMU::HardwareContext<Hardware> c(io);assert(configure(c)==OK);
        io.reset=false;assert(c.attachPolled(0x12000000)==Busy && io.writes.empty());
        io.reset=true;assert(c.attachPolled(0x12000000)==OK && c.check()==OK);
        assert(io.words[Hardware::Bank]==0x1027 && c.sync()==OK && c.detach()==OK);
    }
    {
        Hardware io;io.words[0x1004]|=0x37;SMMU::HardwareContext<Hardware> context(io);
        assert(context.configure(0x10000,1,5,40,40,owned,1,0x37)==OK);
        assert(context.attach(0x12000000,true)==OK && io.words[0x1004]==0x1f337 && context.check()==OK);
        assert(context.detach()==OK && io.words[0x1004]==0x1f337);io.scopedWrites();
        Hardware mismatch;SMMU::HardwareContext<Hardware> wrong(mismatch);
        assert(wrong.configure(0x10000,1,5,40,40,owned,1,0x37)==OK);
        assert(wrong.attach(0x12000000,true)==Denied && mismatch.writes.empty());
        Hardware wide;wide.words[0]=1u<<31;SMMU::HardwareContext<Hardware> unsupported(wide);
        assert(configure(unsupported)==Unsupported && wide.writes.empty());
    }
    for (bool quirk:{false,true}) {
        Hardware io;io.quirk=quirk;SMMU::HardwareContext<Hardware> c(io);
        assert(configure(c)==OK && io.writes.empty());
        io.reset=false;assert(c.attach(0x12000000,true)==Busy && io.writes.empty());
        io.reset=true;assert(c.attach(0x12000000,false)==NotReady && io.writes.empty());
        assert(c.attach(0x12000000,true)==OK && c.check()==OK && c.hasHardwareOwnership());
        bool globalRetired=false;for (const auto &w:io.writes) if (w.offset==Hardware::Bank+0x618) globalRetired=true;
        assert(globalRetired); // firmware may have used global, ASID-independent PTEs
        assert((io.words[0xc08]&0x300ff)==1 && io.words[Hardware::Bank+0x38]==0x44);
        assert(io.read64(Hardware::Bank+0x20)==(0x12000000ULL|(5ULL<<48)));
        if (!quirk) attachWrites=io.writes.size();
        io.reset=false;assert(c.check()==OK && c.sync()==OK);
        const auto before=io.writes.size();assert(c.detach()==Busy && io.writes.size()==before);
        io.reset=true;assert(c.detach()==OK && c.rootDetached()==OK && c.sync()==OK);
        assert(io.words[Hardware::Bank]==0 && io.read64(Hardware::Bank+0x20)==0 && (io.words[0xc08]&0x30000)==0x20000);
        if (!quirk) detachWrites=io.writes.size()-before;
        assert(c.attach(0x13000000,true)==OK && c.detach()==OK);
        io.scopedWrites();
    }
    for (unsigned stage=1;stage<=attachWrites;++stage) {
        Hardware io;SMMU::HardwareContext<Hardware> c(io);assert(configure(c)==OK);io.failWrite=stage;
        assert(c.attach(0x12000000,true)!=OK && c.faulted());
        io.failWrite=0;io.failRead=0;
        assert(c.detach()==OK && c.rootDetached()==OK && !c.faulted());io.scopedWrites();
    }
    for (unsigned stage=1;stage<=detachWrites;++stage) {
        Hardware io;SMMU::HardwareContext<Hardware> c(io);assert(configure(c)==OK && c.attach(0x12000000,true)==OK);
        io.failWrite=io.writes.size()+stage;
        assert(c.detach()!=OK);io.failWrite=0;io.failRead=0;
        assert(c.detach()==OK && c.rootDetached()==OK);io.scopedWrites();
    }
    {
        Hardware io;io.words[0x80c]=(1u<<31)|0x1480;io.words[0xc0c]=1;
        SMMU::HardwareContext<Hardware> c(io);assert(configure(c)==Denied && io.writes.empty());
        io.words[0xc0c]=2;io.words[0x80c]=(1u<<31)|(0xffu<<16)|0x1400;
        assert(configure(c)==Denied && io.writes.empty());
        io.words[0x80c]=0;assert(configure(c)==OK);
        io.words[0x80c]=(1u<<31)|0x1480;io.words[0xc0c]=1;
        assert(c.attach(0x12000000,true)==Denied && io.writes.empty());
    }
    {
        Hardware io;SMMU::HardwareContext<Hardware> c(io);io.words[0x808]=0;
        assert(configure(c)==OK && c.attach(0x12000000,true)==OK && c.detach()==OK);
        io.scopedWrites();
    }
    {
        Hardware io;SMMU::HardwareContext<Hardware> c(io);assert(configure(c)==OK && c.attach(0x12000000,true)==OK);
        io.words[Hardware::Bank+0x58]=1u<<1;assert(c.check()==Failed && c.faulted());
        assert(c.detach()==OK && !io.words[Hardware::Bank+0x58]);
        assert(c.attach(0x13000000,true)==OK);io.accessibleFlag=false;
        const auto before=io.writes.size();assert(c.sync()==Stale && io.writes.size()==before);
        io.accessibleFlag=true;assert(c.detach()==OK);
    }
    {
        Hardware io;SMMU::HardwareContext<Hardware> c(io);assert(configure(c)==OK);io.hang=true;
        assert(c.attach(0x12000000,true)==TimedOut && c.faulted());
        io.hang=false;assert(c.detach()==OK && c.rootDetached()==OK);
    }
    for (unsigned asid:{0u,0x1234u}) {
        Hardware io;io.words[Hardware::Bank+0x24]=asid<<16;
        SMMU::HardwareContext<Hardware> c(io);
        assert(c.configure(0x10000,1,asid,40,40,owned,1)==OK);
        assert(c.attach(0x812000000ULL,true)==OK && c.sync()==OK && c.detach()==OK);
        io.scopedWrites();
    }
    {
        Hardware io;io.quirk=true;io.words[0xc10]=0x100ff;
        const SMMU::Stream two[]{{2,0x1400},{4,0x1401}};
        SMMU::HardwareContext<Hardware> c(io);
        assert(c.configure(0x10000,1,5,40,40,two,2)==OK && c.attach(0x12000000,true)==OK && c.check()==OK);
        assert(io.words[0x810]==((1u<<31)|0x1401) && (io.words[0xc10]&0x300ff)==1);
        assert(c.detach()==OK && (io.words[0xc10]&0x30000)==0x20000 && c.rootDetached()==OK);
    }
    {
        Hardware io;SMMU::HardwareContext<Hardware> c(io);
        assert(c.configure(0x10000,1,6,40,40,owned,1)==OK);
        assert(c.attach(0x12000000,true)==Denied && io.writes.empty());
        Hardware stalled;stalled.words[Hardware::Bank+0x58]=0x40000002;
        SMMU::HardwareContext<Hardware> recovered(stalled);
        assert(configure(recovered)==OK && recovered.attach(0x12000000,true)==OK && !stalled.words[Hardware::Bank+0x58]);
        bool resumed=false;for(const auto &w:stalled.writes)resumed|=w.offset==Hardware::Bank+8 && w.value==1;
        assert(resumed && recovered.detach()==OK);
    }
    {
        using Ledger=SMMU::MappingLedger<4>;Ledger ledger;
        assert(ledger.initialize(32,0x10000,0x40000)==OK);
        Ledger::Record *r=nullptr;
        assert(ledger.reserve(1,2,9000,64,64,32,r)==OK && (r->address&63)==0 && r->length==12288);
        const auto id=r->identity,address=r->address,length=r->length,base=r->base;
        assert(ledger.mapped(id)==OK && ledger.cancel(id)==Busy);
        assert(ledger.beginUnmap(id,1,3,address,length)==Invalid);
        assert(ledger.beginUnmap(id,1,2,address,length)==OK && ledger.finishUnmap(id,TimedOut)==TimedOut);
        assert(ledger.lookup(1,2,address,length)->phase==Ledger::Phase::Quarantined);
        Ledger::Record *next=nullptr;
        assert(ledger.reserve(3,4,9000,64,64,32,next)==OK && next->base>=base+12288+8192);
        assert(ledger.isolate([]{return Busy;})==Busy && ledger.count()==2);
        assert(ledger.isolate([]{return OK;})==OK);
        assert(ledger.acknowledge(id,1,3)==Invalid && ledger.acknowledge(id,1,2)==OK);
        const auto second=next->identity;assert(ledger.acknowledge(second,3,4)==OK && !ledger.count());
        assert(ledger.reserve(1,2,9000,64,64,32,r)==OK && r->base==base && r->identity!=id);
        assert(ledger.cancel(r->identity)==OK);
        assert(ledger.reserve(1,2,1,1,64,32,r)==Invalid);
        assert(ledger.reserve(1,2,1,0,0,32,r,0x20000)==OK);
        assert(ledger.reserve(3,4,1,0,0,32,next,0x20000)==Busy);
        assert(ledger.mapped(r->identity)==OK && ledger.beginUnmap(r->identity,1,2,r->address,r->length)==OK);
        assert(ledger.finishUnmap(r->identity,OK)==OK && !ledger.count());
    }
    {
        SMMU::MappingLedger<2> ledger;assert(ledger.initialize(40)==OK);
        SMMU::MappingLedger<2>::Record *a=nullptr,*b=nullptr,*c=nullptr;
        assert(ledger.reserve(1,2,4096,0,65536,32,a)==OK && a->address<(1ULL<<32) && !(a->address&65535));
        assert(ledger.reserve(1,2,4096,0,4096,40,b)==OK && ledger.reserve(1,2,1,0,1,32,c)==NotReady);
        assert(ledger.cancel(a->identity)==OK && ledger.cancel(b->identity)==OK);
    }
    {
        SMMU::MappingLedger<4096> ledger;assert(ledger.initialize(40,0x10000000,0x20000000)==OK);
        decltype(ledger)::Record *records[1024]{};uint64_t addresses[1024]{};
        for(unsigned i=0;i<1024;++i) {
            assert(ledger.reserve(i+1,i+2,16384,0,4096,40,records[i])==OK);
            addresses[i]=records[i]->address;assert(ledger.mapped(records[i]->identity)==OK);
            if(i)assert(records[i]->base>=records[i-1]->base+records[i-1]->bytes);
        }
        assert(ledger.count()==1024 && ledger.payloadBytes()==1024*16384 && ledger.spanBytes()==1024*(16384+8192));
        for(unsigned i=0;i<1024;i+=2) {
            auto id=records[i]->identity;
            assert(ledger.beginUnmap(id,i+1,i+2,addresses[i],16384)==OK && ledger.finishUnmap(id,OK)==OK);
        }
        for(unsigned i=0;i<1024;i+=2) {
            decltype(ledger)::Record *replacement=nullptr;
            assert(ledger.reserve(i+3000,i+4000,16384,0,4096,40,replacement)==OK && replacement->address==addresses[i]);
        }
        for(unsigned i=1;i<1024;i+=2)assert(records[i]->address==addresses[i] && records[i]->phase==decltype(ledger)::Phase::Mapped);
        assert(ledger.count()==1024);
    }
    {
        Hardware io;SMMU::HardwareContext<Hardware> context(io);assert(configure(context)==OK);
        TableStorage storage(context);SMMU::PageTables<TableStorage> tables(storage);References references;
        SMMU::MappingEngine<decltype(tables),decltype(context),References> engine(tables,context,references);
        assert(engine.initialize(40)==OK && tables.initialize(40,40)==OK && context.attach(tables.root(),true)==OK && engine.begin()==OK);
        DataPages data;decltype(engine)::Record a,b;
        io.reset=false;
        assert(engine.map(1,2,8192,0,4096,32,1,data,a)==OK && references.held[1]==1 && references.held[2]==1);
        uint64_t physical=0;unsigned access=0;
        assert(tables.translate(a.address,physical,access)==OK && physical==0x90000000 && access==1);
        assert(engine.unmap(1,3,a.address,a.length)==Invalid && engine.count()==1);
        assert(engine.unmap(1,2,a.address,a.length)==OK && !engine.count() && !references.held[1] && !references.held[2]);
        data.failAt=1;
        assert(engine.map(1,2,8192,0,4096,32,3,data,a)==NotReady && !engine.count() && !references.held[1] && !engine.faulted());
        data.failAt=UINT32_MAX;
        assert(engine.map(1,2,8192,0,4096,32,3,data,a)==OK);
        io.hang=true;
        assert(engine.unmap(1,2,a.address,a.length)==TimedOut && engine.faulted() && engine.count()==1 && references.held[1]==1);
        assert(engine.map(3,4,4096,0,4096,32,3,data,b)==Stale && !references.held[3]);
        assert(engine.isolate()==NotReady && !engine.retired(1,2,a.address,a.length));
        io.hang=false;io.reset=true;
        assert(context.detach()==OK && engine.isolate()==OK && engine.retired(1,2,a.address,a.length));
        assert(references.held[1]==1 && engine.acknowledge(a.identity,1,3)==Invalid);
        assert(engine.acknowledge(a.identity,1,2)==OK && !references.held[1] && !engine.count());
        assert(tables.destroy()==OK && storage.pages.empty());
        assert(tables.initialize(40,40)==OK && context.attach(tables.root(),true)==OK && engine.begin()==OK);
        assert(engine.map(1,2,4096,0,4096,32,3,data,b)==OK && a.identity!=b.identity);
        assert(engine.unmap(1,2,b.address,b.length)==OK && context.detach()==OK && engine.isolate()==OK && tables.destroy()==OK);
    }
    puts("SMMU context: exact stream ownership, Qualcomm FAULT quirk, scoped handoff/teardown faults, and guarded IOVA leases passed");
}
