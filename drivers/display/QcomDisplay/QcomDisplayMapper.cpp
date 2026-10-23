// SPDX-License-Identifier: GPL-2.0-only
// Hardware invalidation uses the shared ARM SMMUv2 context implementation.
#include "QcomDisplayMapper.h"
#include "QcomDisplay.h"
#include "DisplaySMMU.h"
#include "../../bus/QcomSMMU/RetainedPageTables.h"
#include "../../bus/QcomSMMU/MappingLedger.h"
#include "../../bus/QcomSMMU/PhysicalPages.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <mach/vm_param.h>
#include <kern/task.h>

OSDefineMetaClassAndStructors(QcomDisplayMapper,IOMapper);
static_assert(sizeof(IOMapper)==208 && sizeof(IODMAMapSpecification)==40 && sizeof(IODMAMapPageList)==16,"iOS18 mapper callback ABI");
static_assert(offsetof(IODMAMapSpecification,numAddressBits)==20 && offsetof(IODMAMapPageList,pageList)==8,"native mapper argument offsets");
namespace {
using namespace QcomPower;
IOReturn native(Status s) {
    switch(s) {case OK:return kIOReturnSuccess;case Invalid:return kIOReturnBadArgument;case Busy:return kIOReturnBusy;
        case Unsupported:return kIOReturnUnsupported;case NotReady:return kIOReturnNoResources;
        case TimedOut:return kIOReturnTimeout;default:return kIOReturnIOError;}
}
struct MMIORegisters {
    IOMemoryMap *map=nullptr;
    uint32_t read(uint32_t offset) const {
        auto value=*reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()+offset);
        __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld":"+r"(value)::"memory");return value;
    }
};
struct PhysicalSource {
    IOMemoryDescriptor *memory;const IODMAMapPageList *list;
    bool hasPageList() const {return list!=nullptr;}
    uint64_t pageOffset() const {return list->pageOffset;}
    uint64_t pageCount() const {return list->pageListCount;}
    uint64_t pageFrame(unsigned n) const {return list->pageList[n].phys_addr;}
    uint64_t segment(uint64_t offset,uint64_t &span) {
        IOByteCount bytes=0;auto pa=memory->getPhysicalSegment(offset,&bytes,kIOMemoryMapperNone);span=bytes;return pa;
    }
};
}
struct QcomDisplayMapper::State {
    using Status=QcomPower::Status;
    QcomDisplay *owner=nullptr;IOService *provider=nullptr;
    Display::MMU::Handoff handoff;
    IOLock *lock=nullptr;MMIORegisters registers,mmcc;
    IOInterruptEventSource *interrupt=nullptr;
    IOWorkLoop *loop=nullptr;
    bool ready=false,fault=false,cacheFault=false,bound=false,interruptAdded=false;
    unsigned bank=0;
    struct Page {
        IOMemoryDescriptor *memory=nullptr;IOMemoryMap *map=nullptr;
        volatile uint64_t *entries=nullptr;uint64_t physical=0;bool prepared=false;
    } pages[512];
    SMMU::RetainedPageTables<State,512> tables{*this};
    using Ledger=SMMU::MappingLedger<64>;Ledger ledger;
    struct Mapping {Ledger::Record lease;IOMemoryDescriptor *memory=nullptr;IODMACommand *command=nullptr;} mappings[64];
    uint32_t read(uint32_t offset) {return registers.read(offset);}
    uint64_t read64(uint32_t offset) {
        auto v=*reinterpret_cast<volatile uint64_t *>(registers.map->getVirtualAddress()+offset);
        __asm__ volatile("dmb oshld":::"memory");return v;
    }
    void write(uint32_t offset,uint32_t value) {
        __asm__ volatile("dmb oshst":::"memory");
        *reinterpret_cast<volatile uint32_t *>(registers.map->getVirtualAddress()+offset)=value;
        __asm__ volatile("dsb oshst":::"memory");
    }
    void delay(unsigned us) {IODelay(us);}
    Status isolated() {return QcomPower::Busy;} // a running raster is never a DMA-stop proof
    Status liveOwnership() {
        using namespace QcomPower;
        if(fault || cacheFault || !registers.map || !mmcc.map)return Stale;
        if(!Display::MMU::resourcesLive(mmcc))return NotReady;
        const auto &c=handoff.context;
        return c.registersUnchanged(*this) && Display::MMU::streamsOwned(c,*this) &&
            !SMMU::contextFault(read(bank+0x58)) ? OK:Stale;
    }
    void barrier() {
        // Firmware tables were WB in EFI. Never create an uncached alias of
        // them. New WB pages use the same explicit cache-publication protocol.
        for(auto &p:pages)if(p.memory && p.prepared &&
            p.memory->performOperation(kIOMemoryIncoherentIOStore,0,4096)!=kIOReturnSuccess)cacheFault=true;
        __asm__ volatile("dsb osh":::"memory");
    }
    Status sync() {
        auto status=liveOwnership();
        // No other active context is admitted. Include global boot entries
        // and walk caches, without changing TTBR, SMR/S2CR or global control.
        if(status==QcomPower::OK)status=SMMU::synchronizeContext(*this,bank,handoff.context.asid,1000000,true);
        if(status!=QcomPower::OK)fault=true;return status;
    }
    Page *emptyPage() {for(auto &p:pages)if(!p.memory)return &p;return nullptr;}
    void discard(Page &p) {
        OSSafeReleaseNULL(p.map);
        if(p.prepared && p.memory->complete(kIODirectionInOut)!=kIOReturnSuccess) {
            cacheFault=true;return;
        }
        OSSafeReleaseNULL(p.memory);p={};
    }
    Status borrow(uint64_t physical,SMMU::RetainedTablePage &out) {
        using namespace QcomPower;
        const Display::MMU::Handoff::Table *boot=nullptr;
        for(unsigned n=0;n<handoff.tableCount;++n)if(handoff.tables[n].physical==physical){boot=&handoff.tables[n];break;}
        auto *p=emptyPage();if(!boot || !p)return NotReady;
        p->memory=IOMemoryDescriptor::withPhysicalAddress(physical,4096,kIODirectionInOut|kIOMemoryMapperNone);
        if(!p->memory)return NotReady;
        if(p->memory->prepare(kIODirectionInOut)!=kIOReturnSuccess){discard(*p);return Failed;}
        p->prepared=true;p->map=p->memory->map(kIOMapCopybackCache);
        if(!p->map || p->map->getLength()!=4096){discard(*p);return Failed;}
        p->entries=reinterpret_cast<volatile uint64_t *>(p->map->getVirtualAddress());p->physical=physical;
        if(Display::MMU::tableDigest(p->entries)!=boot->digest){discard(*p);return Stale;}
        out={p,p->entries,physical};return OK;
    }
    Status allocate(SMMU::RetainedTablePage &out) {
        using namespace QcomPower;
        auto *p=emptyPage();if(!p)return NotReady;
        const auto mask=((1ULL<<handoff.context.outputBits)-1)&~4095ULL;
        auto *buffer=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|kIOMapCopybackCache,4096,mask);
        if(!buffer)return NotReady;
        p->memory=buffer;IOByteCount span=0;p->physical=buffer->getPhysicalSegment(0,&span,kIOMemoryMapperNone);
        p->entries=static_cast<volatile uint64_t *>(buffer->getBytesNoCopy());
        if(!p->physical || (p->physical&4095) || p->physical>mask || span<4096 || !p->entries ||
            p->memory->prepare(kIODirectionInOut)!=kIOReturnSuccess){discard(*p);return Failed;}
        p->prepared=true;out={p,p->entries,p->physical};return OK;
    }
    void release(SMMU::RetainedTablePage &p,bool) {if(p.owner)discard(*static_cast<Page *>(p.owner));p={};}
    bool configure() {
        using namespace QcomPower;
        if(!handoff.valid())return false;
        bank=handoff.context.contextOffset(handoff.context.bank);
        if(liveOwnership()!=OK)return false;
        // Revalidate every reserved table before touching even a vacant PTE.
        for(unsigned n=0;n<handoff.tableCount;++n) {
            SMMU::RetainedTablePage page;
            if(borrow(handoff.tables[n].physical,page)!=OK)return false;
            release(page,false);if(cacheFault)return false;
        }
        if(tables.adoptLive(handoff.window())!=OK)return false;
        for(uint64_t n=0;n<handoff.framebufferBytes;n+=4096) {
            uint64_t physical=0;
            if(tables.translate(handoff.deviceAddress+n,physical)!=OK || physical!=handoff.framebuffer+n)return false;
        }
        return liveOwnership()==OK && ledger.initialize(32,handoff.first,handoff.end)==OK;
    }
};

QcomDisplayMapper *QcomDisplayMapper::create(QcomDisplay *owner,IOService *provider,const Display::Mode &mode,uint64_t address) {
    if(!owner || !provider || provider->getProperty("iommu-parent") || (reinterpret_cast<uintptr_t>(IOMapper::gSystem)&3))return nullptr;
    auto *data=OSDynamicCast(OSData,provider->getProperty(Display::MMU::Property));
    if(!data || data->getLength()!=sizeof(Display::MMU::Handoff))return nullptr;
    auto *mapper=new QcomDisplayMapper;
    if(!mapper || !mapper->init()){if(mapper)mapper->release();return nullptr;}
    mapper->fState=new State;if(!mapper->fState){mapper->release();return nullptr;}
    auto &s=*mapper->fState;memcpy(&s.handoff,data->getBytesNoCopy(),sizeof(s.handoff));
    if(!s.handoff.valid() || s.handoff.framebuffer!=mode.framebufferAddress ||
        s.handoff.framebufferBytes!=mode.framebufferSize || s.handoff.deviceAddress!=address){mapper->release();return nullptr;}
    s.owner=owner;owner->retain();s.provider=provider;provider->retain();s.lock=IOLockAlloc();
    if(!s.lock || !(mapper->fAttached=mapper->attach(provider)) || !mapper->start(provider)) {mapper->shutdown();mapper->release();return nullptr;}
    mapper->fStarted=true;
    if(!provider->setProperty("iommu-parent",mapper)){mapper->shutdown();mapper->release();return nullptr;}
    s.bound=true;mapper->registerService();return mapper;
}
bool QcomDisplayMapper::start(IOService *provider) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    static_assert(__builtin_offsetof(QcomDisplayMapper,fIsSystem)==200,"native IOMapper system flag offset");
#pragma clang diagnostic pop
    fIsSystem=false;bool ok=IOMapper::start(provider);if(!ok)IOService::stop(provider);return ok;
}
bool QcomDisplayMapper::initHardware(IOService *provider) {
    auto &s=*fState;
    const uint64_t bases[]={Display::MMU::Base,Display::MMU::MMCCBase};
    const uint64_t spans[]={Display::MMU::Aperture,Display::MMU::MMCCBytes};
    if(provider->getDeviceMemoryCount()!=3)return false;
    for(unsigned n=0;n<2;++n) {
        auto *memory=provider->getDeviceMemoryWithIndex(1+n);
        if(!memory || memory->getPhysicalAddress()!=bases[n] || memory->getLength()!=spans[n])return false;
    }
    auto *specs=OSDynamicCast(OSArray,provider->getProperty("IOInterruptSpecifiers"));
    auto *controllers=OSDynamicCast(OSArray,provider->getProperty("IOInterruptControllers"));
    const unsigned bank=s.handoff.context.bank,index=bank+1;
    if(!specs || !controllers || specs->getCount()!=21 || controllers->getCount()!=21)return false;
    for(unsigned n=0;n<21;++n) {
        auto *spec=OSDynamicCast(OSData,specs->getObject(n));int type=0;
        if(!spec || spec->getLength()!=8 || !controllers->getObject(n) || !controllers->getObject(0) ||
            !controllers->getObject(n)->isEqualTo(controllers->getObject(0)) ||
            provider->getInterruptType(n,&type)!=kIOReturnSuccess || type!=kIOInterruptTypeLevel)return false;
        const auto *words=static_cast<const uint32_t *>(spec->getBytesNoCopy());
        if(words[1]!=1 || words[0]!=(n?Display::MMU::Interrupts[n-1]:115u))return false;
    }
    s.mmcc.map=provider->mapDeviceMemoryWithIndex(2,kIOMapInhibitCache);
    if(!s.mmcc.map || !Display::MMU::resourcesLive(s.mmcc))return false;
    s.registers.map=provider->mapDeviceMemoryWithIndex(1,kIOMapInhibitCache);
    if(!s.registers.map || !s.configure())return false;
    s.interrupt=IOInterruptEventSource::interruptEventSource(this,interruptAction,provider,index);
    s.loop=s.owner->getWorkLoop();if(s.loop)s.loop->retain();
    if(!s.interrupt || !s.loop || s.loop->addEventSource(s.interrupt)!=kIOReturnSuccess)return false;
    s.interruptAdded=true;IOLockLock(s.lock);s.interrupt->enable();
    // A bridge-created context polls faults with CFIE disabled. Enable its
    // fault delivery only after the real native context IRQ is installed.
    if(!(s.handoff.context.sctlr&(1u<<6))) {
        if(s.liveOwnership()!=QcomPower::OK){IOLockUnlock(s.lock);return false;}
        const auto control=s.handoff.context.sctlr|(1u<<6);
        s.write(s.bank,control);
        if(s.read(s.bank)!=control){s.fault=true;IOLockUnlock(s.lock);return false;}
        s.handoff.context.sctlr=control;
        if(s.liveOwnership()!=QcomPower::OK){s.fault=true;IOLockUnlock(s.lock);return false;}
    }
    s.ready=true;IOLockUnlock(s.lock);
    s.owner->setProperty("DisplayMapperContext",bank,32);
    s.owner->setProperty("DisplayMapperIOVAFirst",s.handoff.first,64);
    s.owner->setProperty("DisplayMapperIOVAEnd",s.handoff.end,64);
    s.owner->setProperty("DisplayMapperBootTables",s.handoff.tableCount,32);
    return true;
}
IOReturn QcomDisplayMapper::iovmMapMemory(IOMemoryDescriptor *memory,uint64_t offset,uint64_t length,uint32_t options,
    const IODMAMapSpecification *spec,IODMACommand *command,const IODMAMapPageList *list,uint64_t *address,uint64_t *bytes) {
    using namespace QcomPower;
    if(address)*address=0;if(bytes)*bytes=0;
    if(!fState || !memory || !command || !address || !bytes || offset || !length || (length&4095) ||
        length>16*1024*1024 || length!=memory->getLength() || !(options&kIODMAMapReadAccess) ||
        (options&~uint32_t(3|kIODMAMapPageListFullyOccupied)) || (list && (!list->pageList || !list->pageListCount)))return kIOReturnUnsupported;
    if(spec) {if(spec->options)return kIOReturnUnsupported;for(auto v:spec->resvA)if(v)return kIOReturnBadArgument;for(auto v:spec->resvB)if(v)return kIOReturnBadArgument;}
    PhysicalSource source{memory,list};SMMU::PhysicalPages<PhysicalSource> physical(source);
    auto status=physical.initialize(0,length,PAGE_SHIFT);
    if(status!=OK || physical.pageOffset())return kIOReturnBadArgument;
    auto &s=*fState;IOLockLock(s.lock);State::Mapping *slot=nullptr;
    status=s.ready && !s.fault?s.liveOwnership():NotReady;
    if(status==OK) {
        for(auto &m:s.mappings)if(!m.memory){slot=&m;break;}
        State::Ledger::Record *record=nullptr;
        const unsigned bits=spec && spec->numAddressBits && spec->numAddressBits<32?spec->numAddressBits:32;
        status=slot?s.ledger.reserve(reinterpret_cast<uintptr_t>(memory),reinterpret_cast<uintptr_t>(command),length,0,spec?spec->alignment:1,bits,record):NotReady;
        if(status==OK) {
            memory->retain();command->retain();*slot={*record,memory,command};
            // The DPU is a reader even when IOSurface's original descriptor
            // has a general InOut direction. This context never grants writes.
            status=s.tables.map(record->address,unsigned(record->length/4096),physical,1);
            if(status==OK){status=s.ledger.mapped(record->identity);if(status==OK){*address=record->address;*bytes=record->length;}else s.fault=true;}
            if(status!=OK) {
                if(s.tables.faulted() || s.fault || s.cacheFault){s.ledger.quarantine(record->identity);s.fault=true;}
                else {s.ledger.cancel(record->identity);*slot={};command->release();memory->release();}
            }
        }
    } else if(s.ready)s.fault=true;
    const bool fault=s.fault;IOLockUnlock(s.lock);
    if(fault)s.owner->displayMapperFault();return native(status);
}
IOReturn QcomDisplayMapper::iovmUnmapMemory(IOMemoryDescriptor *memory,IODMACommand *command,uint64_t address,uint64_t bytes) {
    using namespace QcomPower;
    if(!fState)return kIOReturnNotReady;auto &s=*fState;IOLockLock(s.lock);State::Mapping *found=nullptr;
    for(auto &m:s.mappings)if(m.memory==memory && m.command==command && m.lease.address==address && m.lease.length==bytes){found=&m;break;}
    Status status=Invalid;
    if(found) {
        status=s.liveOwnership();
        if(status==OK)status=s.ledger.beginUnmap(found->lease.identity,reinterpret_cast<uintptr_t>(memory),reinterpret_cast<uintptr_t>(command),address,bytes);
        if(status==OK){status=s.tables.unmap(address,unsigned(bytes/4096));status=s.ledger.finishUnmap(found->lease.identity,status);}
        if(status==OK){*found={};command->release();memory->release();}
        else {s.ledger.quarantine(found->lease.identity);s.fault=true;}
    }
    const bool fault=s.fault;IOLockUnlock(s.lock);if(fault)s.owner->displayMapperFault();return native(status);
}
uint64_t QcomDisplayMapper::mapToPhysicalAddress(uint64_t address) {
    if(!fState)return 0;auto &s=*fState;IOLockLock(s.lock);uint64_t pa=0;
    if(s.ready && s.liveOwnership()==QcomPower::OK && address>=s.handoff.first && address<s.handoff.end)s.tables.translate(address,pa);
    IOLockUnlock(s.lock);return pa;
}
void QcomDisplayMapper::interruptAction(OSObject *object,IOInterruptEventSource *source,int) {
    auto &s=*static_cast<QcomDisplayMapper *>(object)->fState;IOLockLock(s.lock);
    if(!Display::MMU::resourcesLive(s.mmcc)){s.fault=true;s.ready=false;source->disable();IOLockUnlock(s.lock);s.owner->displayMapperFault();return;}
    const auto fsr=s.read(s.bank+0x58);
    if(!SMMU::contextFault(fsr)){IOLockUnlock(s.lock);return;}
    const auto far=s.read64(s.bank+0x60);s.fault=true;s.ready=false;source->disable();
    const bool owned=s.handoff.context.registersUnchanged(s) && Display::MMU::streamsOwned(s.handoff.context,s);
    if(owned && fsr!=UINT32_MAX) {if(fsr&(1u<<30))s.write(s.bank+8,1);s.write(s.bank+0x58,fsr&SMMU::ContextFaultMask);}
    IOLockUnlock(s.lock);
    s.owner->setProperty("DisplayMapperFaultFSR",fsr,32);s.owner->setProperty("DisplayMapperFaultFAR",far,64);
    IOLog("QcomDisplay: MMSS context fault fsr=%x far=%llx; DMA resources retained\n",fsr,far);s.owner->displayMapperFault();
}
bool QcomDisplayMapper::shutdown() {
    if(!fState)return true;auto &s=*fState;if(!s.lock)return true;
    IOLockLock(s.lock);s.ready=false;
    const bool clean=!s.ledger.count() && !s.fault && !s.cacheFault && s.tables.destroy()==QcomPower::OK && !s.cacheFault;
    IOLockUnlock(s.lock);if(!clean)return false;
    if(s.interrupt)s.interrupt->disable();
    if(s.interruptAdded){s.loop->removeEventSource(s.interrupt);s.interruptAdded=false;}
    OSSafeReleaseNULL(s.interrupt);
    if(s.bound){if(s.provider->getProperty("iommu-parent")==this)s.provider->removeProperty("iommu-parent");s.bound=false;}
    if(fStarted){IOService::stop(s.provider);fStarted=false;}
    if(fAttached){detach(s.provider);fAttached=false;}return true;
}
void QcomDisplayMapper::free() {
    if(!shutdown()){IOLog("QcomDisplay: retained MMSS mapper quarantined until a verified DPU stop/reset\n");return;}
    if(fState) {
        auto &s=*fState;for(auto &p:s.pages)if(p.memory)s.discard(p);
        if(s.cacheFault){IOLog("QcomDisplay: MMSS table descriptor cleanup failed; resources retained\n");return;}
        OSSafeReleaseNULL(s.loop);OSSafeReleaseNULL(s.registers.map);OSSafeReleaseNULL(s.mmcc.map);OSSafeReleaseNULL(s.provider);
        auto *owner=s.owner;s.owner=nullptr;if(s.lock)IOLockFree(s.lock);delete fState;fState=nullptr;if(owner)owner->release();
    }
    IOMapper::free();
}
