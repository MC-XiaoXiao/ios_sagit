// SPDX-License-Identifier: GPL-2.0-only
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/Cpu.h>
extern "C" {
#include "bridge.h"
#include "memory_ranges.h"
void *memcpy(void *,const void *,size_t);
}
#include "../../drivers/display/QcomDisplay/DisplaySMMU.h"
#include "../../drivers/display/QcomDisplay/DisplayDMAHalt.h"
#include "../../drivers/bus/QcomSMMU/HardwareContext.h"
#include "../../drivers/bus/QcomSMMU/PageTables.h"
#include "clock_protocol.h"

inline void *operator new(size_t,void *p) noexcept {return p;}
namespace {
using namespace Display::MMU;
void delay(unsigned us) {
    uint64_t first,now,hz;
    __asm__ volatile("mrs %0,cntpct_el0":"=r"(first));
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
    do {__asm__ volatile("mrs %0,cntpct_el0":"=r"(now));}while(now-first<(hz*us+999999)/1000000);
}
struct Access {
    uint64_t base;
    uint32_t read(uint32_t offset) const {
        auto v=*reinterpret_cast<volatile uint32_t *>(base+offset);
        __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld":"+r"(v)::"memory");return v;
    }
    uint64_t read64(uint32_t offset) const {
        auto v=*reinterpret_cast<volatile uint64_t *>(base+offset);
        __asm__ volatile("dmb oshld":::"memory");return v;
    }
    void write(uint32_t offset,uint32_t value) {
        __asm__ volatile("dmb oshst":::"memory");
        *reinterpret_cast<volatile uint32_t *>(base+offset)=value;
        __asm__ volatile("dsb sy":::"memory");
    }
    void write64(uint32_t offset,uint64_t value) {
        __asm__ volatile("dmb oshst":::"memory");
        *reinterpret_cast<volatile uint64_t *>(base+offset)=value;
        __asm__ volatile("dsb sy":::"memory");
    }
};
struct State {
    Handoff handoff;
    struct Page {uint64_t physical=0,va=0,entries[512]{};unsigned level=0;} pages[MaximumTables];
    unsigned count=0;
    Access smmu{Base},mmcc{MMCCBase};
    void *tree=nullptr;EFI_BOOT_SERVICES *bs=nullptr;
    EFI_CPU_ARCH_PROTOCOL *cpu=nullptr;
    uint8_t memoryMap[65536];UINTN mapBytes=0,descriptorBytes=0;
    bool prepared=false,fresh=false;
    uint32_t width=0,height=0;
    SMMU::ContextHandoff inherited;
    uint64_t allocations[MaximumTables]{};
    struct RasterAccess:Access {
        State *owner;
        explicit RasterAccess(State *s):Access{0xc900000},owner(s) {}
        QcomPower::Status accessible() {
            if(!resourcesLive(owner->mmcc))return QcomPower::NotReady;
            auto power=owner->mmcc.read(0x2304);
            if(power==UINT32_MAX || !(power&(1u<<31)))return QcomPower::NotReady;
            const unsigned branches[]={0x2308,0x2310,0x231c,0x2328};
            for(auto off:branches){const auto v=owner->mmcc.read(off);if(v==UINT32_MAX || !(v&1))return QcomPower::NotReady;}
            return QcomPower::OK;
        }
        void delay(unsigned us) {::delay(us);}
    } raster{this};
    DMAHalt<RasterAccess> dma{raster};
    void publishFreshTables() {
        for(auto pa:allocations)if(pa)bridge_clean(pa,pa+4096);
        __asm__ volatile("dsb sy":::"memory");
    }
    struct ContextAccess:Access {
        State *owner;
        explicit ContextAccess(State *s):Access{Base},owner(s) {}
        QcomPower::Status accessible() {return owner->raster.accessible();}
        QcomPower::Status isolated() {return owner->dma.isolated();}
        void barrier() {owner->publishFreshTables();}
        void delay(unsigned us) {::delay(us);}
        void write(uint32_t offset,uint32_t value) {
            Access::write(offset,value);
            bridge_puts("[Display SMMU] write +");bridge_hex(offset);bridge_puts(" value=");bridge_hex(value);
            bridge_puts(" read=");bridge_hex(read(offset));bridge_puts("\n");
        }
        void write64(uint32_t offset,uint64_t value) {
            Access::write64(offset,value);
            bridge_puts("[Display SMMU] write64 +");bridge_hex(offset);bridge_puts(" value=");bridge_hex(value);
            bridge_puts(" read=");bridge_hex(read64(offset));bridge_puts("\n");
        }
    } contextIO{this};
    SMMU::HardwareContext<ContextAccess> newContext{contextIO};
    struct TableStorage {
        State *owner;
        QcomPower::Status allocate(SMMU::TablePage &page) {
            using namespace QcomPower;
            unsigned slot=0;while(slot<MaximumTables && owner->allocations[slot])++slot;
            if(slot==MaximumTables || owner->newContext.hasHardwareOwnership())return NotReady;
            // Allocate one whole future XNU 16 KiB alias for each 4 KiB
            // table, below the DPU's 32-bit address range. Unused alignment
            // pages are returned before any table becomes hardware visible.
            EFI_PHYSICAL_ADDRESS raw=UINT32_MAX;
            if(EFI_ERROR(owner->bs->AllocatePages(AllocateMaxAddress,EfiLoaderData,7,&raw)))return NotReady;
            const auto pa=(raw+0x3fff)&~0x3fffULL;const unsigned prefix=(pa-raw)/4096,suffix=3-prefix;
            const auto head=prefix?owner->bs->FreePages(raw,prefix):EFI_SUCCESS;
            const auto tail=suffix?owner->bs->FreePages(pa+16384,suffix):EFI_SUCCESS;
            if(EFI_ERROR(head) || EFI_ERROR(tail)) {
                bridge_puts("[Display SMMU] alignment release head=");bridge_hex(head);bridge_puts(" tail=");bridge_hex(tail);bridge_puts("\n");return Failed;
            }
            owner->allocations[slot]=pa;
            EFI_GUID guid=EFI_CPU_ARCH_PROTOCOL_GUID;
            if(!owner->cpu && (EFI_ERROR(owner->bs->LocateProtocol(&guid,nullptr,reinterpret_cast<void **>(&owner->cpu))) || !owner->cpu))return Failed;
            if(!owner->cpu->FlushDataCache || !owner->cpu->SetMemoryAttributes)return Unsupported;
            auto status=owner->cpu->FlushDataCache(owner->cpu,pa,16384,EfiCpuFlushTypeWriteBackInvalidate);
            if(!EFI_ERROR(status))status=owner->cpu->SetMemoryAttributes(owner->cpu,pa,16384,EFI_MEMORY_WB|EFI_MEMORY_XP);
            if(EFI_ERROR(status)) {
                bridge_puts("[Display SMMU] new table WB attributes=");bridge_hex(status);bridge_puts("\n");return Failed;
            }
            if(!owner->memoryMapNow())return Failed;
            for(unsigned n=0;n<4;++n)if(!owner->tableRAM(pa+4096*n) || (owner->translation(pa+4096*n,true)&1)) {
                auto *d=owner->descriptor(pa+4096*n);
                bridge_puts("[Display SMMU] table RAM rejected pa=");bridge_hex(pa+4096*n);
                bridge_puts(" type=");bridge_hex(d?d->Type:UINT32_MAX);bridge_puts(" attributes=");bridge_hex(d?d->Attribute:UINT64_MAX);
                bridge_puts(" PAR=");bridge_hex(owner->translation(pa+4096*n));bridge_puts("\n");return Failed;
            }
            bridge_puts("[Display SMMU] new WB table pa=");bridge_hex(pa);bridge_puts(" PAR=");bridge_hex(owner->translation(pa));bridge_puts("\n");
            auto *words=reinterpret_cast<volatile uint64_t *>(pa);
            for(unsigned n=0;n<2048;++n)words[n]=0;
            page={&owner->allocations[slot],words,pa};return OK;
        }
        void release(SMMU::TablePage &page) {
            if(page.owner && !owner->newContext.hasHardwareOwnership()) {
                auto *pa=static_cast<uint64_t *>(page.owner);
                if(!EFI_ERROR(owner->bs->FreePages(*pa,4)))*pa=0;
            }
            page={};
        }
        void barrier() {owner->publishFreshTables();}
        QcomPower::Status sync() {
            // This builder's root is still unreachable: the exact original
            // bank has SCTLR=0 and TTBR=0. There is no hardware TLB to retire
            // for these unpublished tables. attachPolled performs real TLBI
            // and completed TLBSYNC under the acknowledged VBIF halt later.
            return !owner->newContext.hasHardwareOwnership() && owner->disabledUnchanged()?QcomPower::OK:QcomPower::Stale;
        }
    } tableStorage{this};
    SMMU::PageTables<TableStorage,MaximumTables> freshTables{tableStorage};
    bool disabledUnchanged() {
        return resourcesLive(mmcc) && inherited.registersUnchanged(smmu) && streamsOwned(inherited,smmu) &&
            !SMMU::contextFault(smmu.read(inherited.contextOffset(inherited.bank)+0x58));
    }
    bool buildFresh() {
        const auto initialized=freshTables.initialize(32,handoff.context.outputBits);
        bridge_puts("[Display SMMU] private table allocation=");bridge_hex(initialized);bridge_puts("\n");
        if(initialized!=QcomPower::OK)return false;
        struct Physical {
            uint64_t base;
            QcomPower::Status page(unsigned n,uint64_t &pa) {pa=base+uint64_t(n)*4096;return QcomPower::OK;}
        };
        for(uint64_t offset=0;offset<handoff.framebufferBytes;) {
            const unsigned remaining=(handoff.framebufferBytes-offset)/4096,pages=remaining>4096?4096:remaining;
            Physical memory{handoff.framebuffer+offset};
            const auto mapped=freshTables.map(handoff.deviceAddress+offset,pages,memory,1);
            if(mapped!=QcomPower::OK) {
                bridge_puts("[Display SMMU] GOP table map offset=");bridge_hex(offset);bridge_puts(" status=");bridge_hex(mapped);bridge_puts("\n");return false;
            }
            offset+=uint64_t(pages)*4096;
        }
        handoff.context.ttbr0=freshTables.root();return disabledUnchanged();
    }
    bool memoryMapNow() {
        UINTN key=0;UINT32 version=0;mapBytes=sizeof(memoryMap);
        return !EFI_ERROR(bs->GetMemoryMap(&mapBytes,reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(memoryMap),&key,&descriptorBytes,&version)) &&
            descriptorBytes>=sizeof(EFI_MEMORY_DESCRIPTOR) && mapBytes%descriptorBytes==0;
    }
    const EFI_MEMORY_DESCRIPTOR *descriptor(uint64_t physical) const {
        for(UINTN n=0;n<mapBytes;n+=descriptorBytes) {
            auto *d=reinterpret_cast<const EFI_MEMORY_DESCRIPTOR *>(memoryMap+n);
            if(d->NumberOfPages>(UINT64_MAX>>12))continue;
            auto bytes=d->NumberOfPages<<12;
            if(physical>=d->PhysicalStart && physical-d->PhysicalStart<bytes && bytes-(physical-d->PhysicalStart)>=4096)return d;
        }
        return nullptr;
    }
    uint64_t translation(uint64_t address,bool writing=false) const {
        uint64_t el=0,mask=0,previous=0,result=UINT64_MAX;
        __asm__ volatile("mrs %0,CurrentEL":"=r"(el));if(el!=4 && el!=8)return result;
        __asm__ volatile("mrs %0,DAIF\n\tmsr DAIFSet,#15\n\tmrs %1,PAR_EL1":"=r"(mask),"=r"(previous)::"memory");
        if(el==4) {
            if(writing)__asm__ volatile("at s1e1w,%0"::"r"(address):"memory");
            else __asm__ volatile("at s1e1r,%0"::"r"(address):"memory");
        } else {
            if(writing)__asm__ volatile("at s1e2w,%0"::"r"(address):"memory");
            else __asm__ volatile("at s1e2r,%0"::"r"(address):"memory");
        }
        __asm__ volatile("isb\n\tmrs %0,PAR_EL1\n\tmsr PAR_EL1,%1\n\tmsr DAIF,%2":"=&r"(result):"r"(previous),"r"(mask):"memory");
        return result;
    }
    bool tableRAM(uint64_t physical) const {
        auto *d=descriptor(physical);
        if(!d || (d->Attribute&(EFI_MEMORY_RUNTIME|EFI_MEMORY_RP|EFI_MEMORY_RO)))return false;
        const auto par=translation(physical);
        // GetMemoryMap can include several supported cache policies. Verify
        // the actual CPU translation and MAIR byte instead of treating that
        // capability mask as a unique current mapping attribute.
        return !(par&1) && (par>>56)==0xff && (par&0x0000fffffffff000ULL)==(physical&0x0000fffffffff000ULL) &&
            (d->Type==EfiConventionalMemory || d->Type==EfiLoaderData || d->Type==EfiLoaderCode ||
            d->Type==EfiBootServicesData || d->Type==EfiBootServicesCode) &&
            physical<=UINT64_MAX-4096;
    }
    bool walk(uint64_t physical,unsigned level,uint64_t va) {
        if(!physical || (physical&4095) || physical>=(1ULL<<handoff.context.outputBits) || count==MaximumTables)return false;
        for(unsigned n=0;n<count;++n)if(pages[n].physical==physical)return false;
        // XNU maps physical memory in 16 KiB units. Verify the entire future
        // CPU alias has one ordinary WB attribute, not only the 4 KiB table.
        for(unsigned n=0;n<4;++n)if(!tableRAM((physical&~0x3fffULL)+4096*n))return false;
        auto &p=pages[count++];p.physical=physical;p.level=level;p.va=va;
        auto *live=reinterpret_cast<volatile uint64_t *>(physical);
        const uint64_t span=1ULL<<(39-9*level),mask=0x0000fffffffff000ULL;
        for(unsigned n=0;n<512;++n) {
            auto entry=live[n];p.entries[n]=entry;if(!entry)continue;
            const auto address=va+uint64_t(n)*span;
            if(address>=(1ULL<<handoff.context.inputBits) || !(entry&1) || (entry&3)==2)return false;
            if(level<3 && (entry&3)==3) {if(!walk(entry&mask,level+1,address))return false;}
            else if(!level || (level==3 && (entry&3)!=3) || ((entry&mask)&(span-1)) ||
                (entry&mask)>=(1ULL<<handoff.context.outputBits) || span>(1ULL<<handoff.context.outputBits)-(entry&mask))return false;
        }
        return true;
    }
    bool translate(uint64_t address,uint64_t &physical) const {
        uint64_t table=handoff.context.ttbr0&0x0000fffffffff000ULL;
        for(unsigned level=handoff.context.inputBits>39?0:1;level<=3;++level) {
            const Page *page=nullptr;for(unsigned n=0;n<count;++n)if(pages[n].physical==table){page=&pages[n];break;}
            if(!page || page->level!=level)return false;
            const uint64_t span=1ULL<<(39-9*level),entry=page->entries[(address>>(39-9*level))&511];
            if(!(entry&1))return false;
            if(level<3 && (entry&3)==3){table=entry&0x0000fffffffff000ULL;continue;}
            physical=(entry&0x0000fffffffff000ULL&~(span-1))|(address&(span-1));return true;
        }
        return false;
    }
    bool vacant(uint64_t first,uint64_t bytes) const {
        if(overlap(first,bytes,handoff.deviceAddress,handoff.framebufferBytes))return false;
        for(unsigned n=0;n<count;++n) {
            const auto &p=pages[n];const uint64_t span=1ULL<<(39-9*p.level);
            for(unsigned j=0;j<512;++j)if(p.entries[j] && (p.level==3 || (p.entries[j]&3)!=3) &&
                overlap(first,bytes,p.va+uint64_t(j)*span,span))return false;
        }
        return true;
    }
    bool unchanged() const {
        auto io=smmu,clocks=mmcc;const auto &c=handoff.context;
        const auto &expected=fresh && !newContext.hasHardwareOwnership()?inherited:c;
        if(!resourcesLive(clocks) || !expected.registersUnchanged(io) || !streamsOwned(expected,io) ||
            SMMU::contextFault(io.read(expected.contextOffset(expected.bank)+0x58)))return false;
        for(unsigned n=0;n<count;++n) {
            auto *live=reinterpret_cast<volatile const uint64_t *>(pages[n].physical);
            for(unsigned j=0;j<512;++j)if(live[j]!=pages[n].entries[j])return false;
        }
        return true;
    }
    bool reserve(uint64_t physical,uint64_t bytes,bool tables) {
        for(uint64_t n=0;n<bytes;n+=4096) {
            if(!memoryMapNow())return false;
            const auto pa=physical+n;auto *d=descriptor(pa);
            if(!d || (d->Attribute&EFI_MEMORY_RUNTIME) || (tables && !tableRAM(pa)))return false;
            if(d->Type!=EfiConventionalMemory)continue;
            EFI_PHYSICAL_ADDRESS allocation=pa;
            if(EFI_ERROR(bs->AllocatePages(AllocateAddress,EfiLoaderData,1,&allocation)) || allocation!=pa)return false;
        }
        return true;
    }
};
alignas(State) uint8_t storage[sizeof(State)];State *state=nullptr;
void status(CaptureStatus value) {
    uint32_t size=0;auto *p=dt_property(state->tree,"display-controller@c901000",StatusProperty,&size);
    if(p && size==4){const uint32_t v=value;memcpy(p,&v,4);}
    bridge_puts("[Display SMMU] status=");bridge_hex(value);bridge_puts("\n");
}
void observed() {
    uint32_t size=0;auto *p=dt_property(state->tree,"display-controller@c901000","qcom,display-smmu-observed",&size);
    const auto &c=state->handoff.context;if(p && size==sizeof(c))memcpy(p,&c,sizeof(c));
    bridge_puts("[Display SMMU] id0=");bridge_hex(c.id0);bridge_puts(" id1=");bridge_hex(c.id1);
    bridge_puts(" id2=");bridge_hex(c.id2);bridge_puts(" cr0=");bridge_hex(c.control);
    bridge_puts(" slot=");bridge_hex(c.slot);bridge_puts(" route=");bridge_hex(c.s2cr);
    bridge_puts(" bank=");bridge_hex(c.bank);bridge_puts(" sctlr=");bridge_hex(c.sctlr);
    bridge_puts(" cbar=");bridge_hex(c.cbar);bridge_puts(" cba2r=");bridge_hex(c.cba2r);
    bridge_puts(" tcr=");bridge_hex(c.tcr);bridge_puts(" tcr2=");bridge_hex(c.tcr2);
    bridge_puts(" ttbr0=");bridge_hex(c.ttbr0);bridge_puts(" mair0=");bridge_hex(c.mair0);bridge_puts("\n");
}
void observedTopology() {
    auto &s=*state;auto c=s.handoff.context;c.inputBits=c.outputBits=32;
    if(!resourcesLive(s.mmcc) || !c.layoutValid(topology(),Controller,32,false))return;
    for(unsigned n=0;n<(c.id0&255);++n) {
        const auto smr=s.smmu.read(0x800+4*n);if(!(smr&(1u<<31)))continue;
        bridge_puts("[Display SMMU] stream slot=");bridge_hex(n);bridge_puts(" smr=");bridge_hex(smr);
        bridge_puts(" s2cr=");bridge_hex(s.smmu.read(0xc00+4*n));bridge_puts("\n");
    }
    for(unsigned n=0;n<c.contexts();++n) {
        const auto control=s.smmu.read(c.contextOffset(n));if(!control && n!=c.bank)continue;
        bridge_puts("[Display SMMU] context=");bridge_hex(n);bridge_puts(" sctlr=");bridge_hex(control);
        bridge_puts(" fsr=");bridge_hex(s.smmu.read(c.contextOffset(n)+0x58));bridge_puts("\n");
    }
    if(s.raster.accessible()==QcomPower::OK) {
        bridge_puts("[Display SMMU] VBIF halt=");bridge_hex(s.raster.read(0xb0200));
        bridge_puts(" ack=");bridge_hex(s.raster.read(0xb0204));bridge_puts(" clock=");bridge_hex(s.raster.read(0x12ac));bridge_puts("\n");
    }
}
void observedRaster() {
    auto &s=*state;
    bridge_puts("[Display SMMU] raster accessible=");bridge_hex(s.raster.accessible());
    const unsigned clocks[]={0x2304,0x2308,0x2310,0x231c,0x2328};
    for(auto offset:clocks){bridge_puts(" mmcc+");bridge_hex(offset);bridge_puts("=");bridge_hex(s.mmcc.read(offset));}bridge_puts("\n");
    if(s.raster.accessible()!=QcomPower::OK)return;
    for(unsigned ctl=0;ctl<5;++ctl) {
        const auto at=0x2000+ctl*0x200;bridge_puts("[Display SMMU] raster ctl=");bridge_hex(ctl);
        const unsigned mixers[]={0,1,2,5};
        for(auto lm:mixers) {
            bridge_puts(" layer=");bridge_hex(s.raster.read(at+(lm==5?0x24:4*lm)));
            bridge_puts(" ext=");bridge_hex(s.raster.read(at+0x40+4*lm));
            bridge_puts(" ext2=");bridge_hex(s.raster.read(at+0x70+4*lm));
        }
        bridge_puts(" top=");bridge_hex(s.raster.read(at+0x14));bridge_puts(" flush=");bridge_hex(s.raster.read(at+0x18));bridge_puts("\n");
    }
    const unsigned pipes[]={0x4000,0x6000,0x8000,0xa000,0x24000,0x26000,0x28000,0x2a000};
    for(auto pipe:pipes) {
        bridge_puts("[Display SMMU] raster pipe=");bridge_hex(pipe);bridge_puts(" size=");bridge_hex(s.raster.read(0x1000+pipe));
        bridge_puts(" address=");bridge_hex(s.raster.read(0x1000+pipe+0x14));bridge_puts("\n");
    }
}
}
extern "C" int bridge_display_dma_prepare(EFI_BOOT_SERVICES *bs,void *tree) {
    state=new(storage) State;auto &s=*state;s.bs=bs;s.tree=tree;
    EFI_GUID guid={0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
    BridgeClock *clock=nullptr;
    if(EFI_ERROR(bs->LocateProtocol(&guid,nullptr,reinterpret_cast<void **>(&clock))) || !clock || clock->version<0x10005 ||
        !(s.mmcc.read(0xe020)&(1u<<31))) {status(ClocksUnavailable);return 0;}
    const char *names[]={"mmss_mnoc_ahb_clk","mmss_bimc_smmu_ahb_clk","mmss_bimc_smmu_axi_clk"};
    for(auto name:names) {
        UINTN id=0;BOOLEAN on=0;
        if(EFI_ERROR(clock->get_id(clock,reinterpret_cast<const CHAR8 *>(name),&id)) ||
            EFI_ERROR(clock->enable(clock,id)) || EFI_ERROR(clock->is_on(clock,id,&on)) || !on) {status(ClocksUnavailable);return 0;}
    }
    if(!resourcesLive(s.mmcc)){status(ClocksUnavailable);return 0;}
    const auto captured=capture(s.smmu,s.handoff.context);
    if(captured!=QcomPower::OK) {
        if(captureDisabled(s.smmu,s.handoff.context)!=QcomPower::OK){observed();observedTopology();status(UnsupportedProfile);return 0;}
        s.fresh=true;s.inherited=s.handoff.context;
    }
    observed();if(s.fresh)observedTopology();
    EFI_GUID gopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=nullptr;
    if(EFI_ERROR(bs->LocateProtocol(&gopGuid,nullptr,reinterpret_cast<void **>(&gop))) || !gop || !gop->Mode || !gop->Mode->Info) {status(UnsupportedProfile);return 0;}
    s.handoff.framebuffer=gop->Mode->FrameBufferBase;s.handoff.deviceAddress=s.handoff.framebuffer;
    s.handoff.framebufferBytes=gop->Mode->FrameBufferSize;
    s.width=gop->Mode->Info->HorizontalResolution;s.height=gop->Mode->Info->VerticalResolution;
    bridge_puts("[Display SMMU] GOP base=");bridge_hex(s.handoff.framebuffer);bridge_puts(" bytes=");bridge_hex(s.handoff.framebufferBytes);
    bridge_puts(" width=");bridge_hex(s.width);bridge_puts(" height=");bridge_hex(s.height);bridge_puts("\n");
    if(!s.handoff.framebuffer || (s.handoff.framebuffer&4095) || s.handoff.framebuffer>=0x100000000ULL ||
        !s.handoff.framebufferBytes || (s.handoff.framebufferBytes&4095) || s.handoff.framebufferBytes>0x10000000 ||
        s.handoff.framebufferBytes>0x100000000ULL-s.handoff.framebuffer || !s.memoryMapNow()) {status(UnsupportedProfile);return 0;}
    if(s.fresh) {
        const auto first=s.handoff.framebuffer&~0x3fffULL;
        const auto end=(s.handoff.framebuffer+s.handoff.framebufferBytes+0x3fff)&~0x3fffULL;
        if(!s.reserve(first,end-first,false)){status(ReservationFailed);return -1;}
        if(!s.buildFresh()){status(UnsupportedProfile);return 0;}
    }
    if(!s.memoryMapNow() || !s.walk(s.handoff.context.ttbr0&0x0000fffffffff000ULL,
        s.handoff.context.inputBits>39?0:1,0)) {status(UnsupportedProfile);return 0;}
    for(uint64_t n=0;n<s.handoff.framebufferBytes;n+=4096) {
        uint64_t pa=0;if(!s.translate(s.handoff.deviceAddress+n,pa) || pa!=s.handoff.framebuffer+n){status(UnsupportedProfile);return 0;}
    }
    for(uint64_t first=0xf0000000;first>=0x10000000;first-=0x10000000)if(s.vacant(first,0x10000000)){
        s.handoff.first=first;s.handoff.end=first+0x10000000;break;
    }
    if(!s.handoff.first || !s.unchanged()){status(UnsupportedProfile);return 0;}
    s.handoff.tableCount=s.count;s.handoff.cachePolicy=Handoff::WriteBackTables;
    for(unsigned n=0;n<s.count;++n) {
        const auto pa=s.pages[n].physical;
        s.handoff.tables[n]={pa,tableDigest(s.pages[n].entries)};
        if(overlap(pa&~0x3fffULL,16384,s.handoff.framebuffer,s.handoff.framebufferBytes) ||
            !s.reserve(pa&~0x3fffULL,16384,true)) {status(ReservationFailed);return -1;}
    }
    const auto framebufferStart=s.handoff.framebuffer&~0x3fffULL;
    const auto framebufferEnd=(s.handoff.framebuffer+s.handoff.framebufferBytes+0x3fff)&~0x3fffULL;
    if(!s.reserve(framebufferStart,framebufferEnd-framebufferStart,false) || !s.unchanged()){status(ReservationFailed);return -1;}
    s.prepared=true;
    bridge_puts("[Display SMMU] reserved tables=");bridge_hex(s.count);bridge_puts(" vacant IOVA=");bridge_hex(s.handoff.first);
    bridge_puts("..");bridge_hex(s.handoff.end);
    bridge_puts(s.fresh?"; fresh root unpublished, original bank still disabled\n":"; root and raster unchanged\n");
    return 0;
}
extern "C" uint64_t bridge_display_dma_next_arena(uint64_t start,uint64_t bytes) {
    if(!state || !state->prepared)return start;
    if(!bytes || start>UINT64_MAX-bytes)return UINT64_MAX;
    uint64_t next=start;
    for(unsigned n=0;n<state->count;++n) {
        const auto pa=state->pages[n].physical&~0x3fffULL;
        if(overlap(start,bytes,pa,16384) && pa+16384>next)next=pa+16384;
    }
    const auto &h=state->handoff;
    if(overlap(start,bytes,h.framebuffer,h.framebufferBytes) && h.framebuffer+h.framebufferBytes>next)next=h.framebuffer+h.framebufferBytes;
    return next==start?start:next>UINT64_MAX-0x1fffff?UINT64_MAX:(next+0x1fffff)&~0x1fffffULL;
}
extern "C" int bridge_display_dma_exclude(struct boot_memory_range *ranges,unsigned *count) {
    if(!state || !state->prepared)return 0;
    for(unsigned n=0;n<state->count;++n)if(boot_memory_remove(ranges,count,state->pages[n].physical&~0x3fffULL,16384))return -1;
    const auto &h=state->handoff;return boot_memory_remove(ranges,count,h.framebuffer,h.framebufferBytes);
}
extern "C" int bridge_display_dma_finish(void *tree,uint64_t framebuffer,uint64_t bytes) {
    if(!state || !state->prepared)return 0;
    // Reservation uses dt_blob before the final Apple tree is relocated into
    // the boot arena. Publish diagnostics into the same tree as the handoff.
    auto &s=*state;s.tree=tree;
    uint32_t size=0;auto *p=dt_property(tree,"display-controller@c901000",Property,&size);
    if(!p || size!=sizeof(Handoff) || framebuffer!=s.handoff.framebuffer || bytes!=s.handoff.framebufferBytes || !s.unchanged()) {status(Changed);return -1;}
    for(unsigned n=0;n<s.count;++n)bridge_clean(s.pages[n].physical,s.pages[n].physical+4096);
    if(s.fresh) {
        // ExitBootServices has retired EFI's software producer. The native
        // hardware ACK below additionally drains the only admitted DPU XIN.
        // Raster clocks are retained by display_prepare later than the early
        // table reservation. Check the DPU profile here, under those leases.
        if(!s.dma.prepare(framebuffer,s.width,s.height)) {observedRaster();status(UnsupportedProfile);return 0;}
        if(!s.dma.halt()) {
            observedRaster();observedTopology();
            status(QuiesceFailed);
            return s.dma.resume()?0:-1; // root/format have not been touched
        }
        bridge_puts("[Display SMMU] post-EFI VBIF halt acknowledged xin=");bridge_hex(s.dma.client());bridge_puts("\n");
        const auto &old=s.inherited;SMMU::Stream stream{uint16_t(old.slot),uint16_t(old.stream)};
        auto result=s.newContext.configure(Aperture,old.bank,old.asid,32,old.outputBits,&stream,1,old.cbar&255);
        bridge_puts("[Display SMMU] configure=");bridge_hex(result);bridge_puts("\n");
        if(result==QcomPower::OK)result=s.newContext.attachPolled(s.handoff.context.ttbr0);
        bridge_puts("[Display SMMU] install=");bridge_hex(result);bridge_puts("\n");
        if(result!=QcomPower::OK) {
            auto expected=s.handoff.context;capture(s.smmu,s.handoff.context);observed();observedTopology();s.handoff.context=expected;
            status(InitializationFailed);
            // Partial context mutation cannot fall back to physical bypass.
            // Keep both the halt and every table pinned until a real reset.
            if(s.newContext.hasHardwareOwnership())return -1;
            return s.dma.resume()?0:-1;
        }
        if(capture(s.smmu,s.handoff.context)!=QcomPower::OK || !s.unchanged()) {status(Changed);return -1;}
    }
    s.handoff.flags=Handoff::Required;
    if(!s.handoff.valid() || !s.unchanged()) {s.handoff.flags=0;status(Changed);return -1;}
    if(s.fresh && (!s.dma.resume() || s.newContext.check()!=QcomPower::OK || !s.unchanged())) {status(InitializationFailed);return -1;}
    memcpy(p,&s.handoff,sizeof(s.handoff));status(s.fresh?Initialized:Ready);
    if(s.fresh) {
        observed();bridge_puts("[Display SMMU] private stage-1 root installed, GOP identity mapping retained, VBIF resumed\n");
    } else bridge_puts("[Display SMMU] post-EFI live context transferred; no TTBR/stream/format writes\n");
    return 0;
}
