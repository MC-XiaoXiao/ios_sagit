// SPDX-License-Identifier: GPL-2.0-only
// Context TLB synchronization uses the common QcomSMMU implementation.
#include "QcomGPUMapper.h"
#include "QcomGPU.h"
#include "AdrenoMMU.h"
#include "AdrenoStartup.h"
#include "AdrenoAddressSpace.h"
#include "AdrenoMappedPages.h"
#include "QcomGPUBuffer.h"
#include "../../bus/QcomSMMU/ContextTLB.h"
#include "../../bus/QcomSMMU/MappingLedger.h"
#include "../../bus/QcomSMMU/StreamAudit.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOInterruptEventSource.h>
#include <libkern/c++/OSData.h>
#include <mach/vm_param.h>
#include <mach/mach_time.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
OSDefineMetaClassAndStructors(QcomGPUMapper,IOMapper);
OSDefineMetaClassAndStructors(QcomGPUAddressSpace,OSObject);
static_assert(sizeof(IOMapper)==208 && sizeof(IODMAMapSpecification)==40 && sizeof(IODMAMapPageList)==16,"iOS18 mapper callback ABI");
static_assert(__builtin_offsetof(IODMAMapSpecification,numAddressBits)==20 && __builtin_offsetof(IODMAMapPageList,pageList)==8,"native mapper argument offsets");
namespace {
IOReturn result(QcomPower::Status s) {
    using namespace QcomPower;
    switch (s) {case OK:return kIOReturnSuccess;case Invalid:return kIOReturnBadArgument;
        case Unsupported:return kIOReturnUnsupported;case Busy:return kIOReturnBusy;
        case NotReady:return kIOReturnNoResources;case TimedOut:return kIOReturnTimeout;default:return kIOReturnIOError;}
}
struct BorrowedPage {IOMemoryDescriptor *memory=nullptr;IOMemoryMap *map=nullptr;};
struct PhysicalSource {
    IOMemoryDescriptor *memory;const IODMAMapPageList *list;
    uint64_t offset,length;unsigned pageOffset;
    QcomPower::Status page(unsigned index,uint64_t &pa) {
        using namespace QcomPower;
        if (list) {
            uint64_t position=uint64_t(list->pageOffset)+(uint64_t(index)*4096)-pageOffset;
            uint64_t cpuPage=position/PAGE_SIZE;
            if (cpuPage>=list->pageListCount) return Invalid;
            if(!list->pageList[cpuPage].phys_addr)return Invalid;
            pa=(uint64_t(list->pageList[cpuPage].phys_addr)<<PAGE_SHIFT)+(position&PAGE_MASK);
            return pa && !(pa&4095)?OK:Invalid;
        }
        uint64_t relative=uint64_t(index)*4096;
        if (relative<pageOffset) {
            IOByteCount span=0;pa=memory->getPhysicalSegment(offset,&span,kIOMemoryMapperNone)&~4095ULL;
            return pa && span?OK:Invalid;
        }
        relative-=pageOffset;if (relative>=length) return Invalid;
        IOByteCount span=0;pa=memory->getPhysicalSegment(offset+relative,&span,kIOMemoryMapperNone);
        return pa && !(pa&4095) && span>=((length-relative)<4096?length-relative:4096)?OK:Invalid;
    }
};
}
struct QcomGPUMapper::State {
    using Status=QcomPower::Status;
    QcomGPU *owner=nullptr;
    IOService *provider=nullptr;
    IOMemoryMap *gpu=nullptr,*smmu=nullptr;
    IOInterruptEventSource *interrupt=nullptr;
    IOLock *lock=nullptr;
    Adreno::SMMUContract contract{};
    uint32_t bank=0,global=0,control=0,tcr=0,tcr2=0,contextID=0,mair0=0,mair1=0,cbar=0,cba2r=0,routes[8]{};
    bool ready=false,fault=false,interruptAdded=false,bound=false,faultReported=false;
    SMMU::ContextTLB<State> tlb{*this};
    Adreno::RetainedPageTables<State,GPU::Native::Memory::TablePages> tables{*this};
    // Private job tables have one cacheable CPU mapping. Retained/borrowed
    // kernel tables keep their original NC mapping and allocator unchanged.
    struct PrivateTableStorage {
        State &hardware;
        Status allocate(Adreno::MMUTablePage &out) {return hardware.allocatePage(out,kIOMapCopybackCache);}
        void release(Adreno::MMUTablePage &page,bool owned) {hardware.release(page,owned);}
        bool validate(const Adreno::MMUTablePage &page) {return hardware.validate(page);}
    } privateStorage{*this};
    Adreno::RetiredTablePageCache<PrivateTableStorage> privatePages{privateStorage};
    GPU::Diagnostics::AddressSpaces::Snapshot addressStatistics{};
    GPU::MappingBounds::Snapshot mappingWork;
    uint32_t detailOrdinal=0;
    bool cachePrivatePages=false;
    using Ledger=SMMU::MappingLedger<GPU::Native::Memory::MappingSlots>;
    Ledger ledger;
    struct FirmwareRange {uint64_t address=0,leaves[256]{};unsigned pages=0;} firmware[3];
    bool firmwarePinned=false,useTranslationCursor=true;
    SMMU::StreamAudit<> streamAudit; // State lock serializes capture/validation.
    struct PrivateRoot {QcomGPUAddressSpace *owner=nullptr;uint64_t ttbr=0;unsigned asid=0;} roots[GPU::MaxJobs];
    struct Mapping {Ledger::Record lease{};IOMemoryDescriptor *memory=nullptr;IODMACommand *command=nullptr;} mappings[GPU::Native::Memory::MappingSlots]{};
    uint32_t read(uint32_t offset) {
        auto v=*reinterpret_cast<volatile uint32_t *>(smmu->getVirtualAddress()+offset);
        // Match the loader's explicit 32-bit normalization for firmware-
        // mediated SMMU registers before composing a 64-bit TTBR or FAR.
        __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld" : "+r"(v) :: "memory");return v;
    }
    struct StreamConfigurationReader {
        const uintptr_t base; // Mapping is retained and stable under State lock.
        uint32_t streamConfigurationRead(uint32_t offset) {
            auto v=*reinterpret_cast<volatile uint32_t *>(base+offset);
            // Same explicit normalization as read(). Only the read-only stream
            // survey uses this accessor; other MMIO retains its barriers.
            __asm__ volatile("uxtw %x0,%w0" : "+r"(v));return v;
        }
        void streamConfigurationReadComplete() {__asm__ volatile("dmb oshld" ::: "memory");}
        uint32_t read(uint32_t offset) {
            const auto v=streamConfigurationRead(offset);streamConfigurationReadComplete();return v;
        }
    };
    uint64_t pair(uint32_t offset) {
        // CP can change TTBR0 while the CPU checks health. A split low/high
        // read could combine two valid roots and falsely report corruption.
        auto value=*reinterpret_cast<volatile uint64_t *>(smmu->getVirtualAddress()+offset);
        __asm__ volatile("dmb oshld" ::: "memory");return value;
    }
    void writePair(uint32_t offset,uint64_t value) {
        __asm__ volatile("dmb oshst" ::: "memory");
        *reinterpret_cast<volatile uint64_t *>(smmu->getVirtualAddress()+offset)=value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    void write(uint32_t offset,uint32_t value) {
        __asm__ volatile("dmb oshst" ::: "memory");
        *reinterpret_cast<volatile uint32_t *>(smmu->getVirtualAddress()+offset)=value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    void barrier() {__asm__ volatile("dsb osh" ::: "memory");}
    void delay(unsigned us) {IODelay(us);}
    uint32_t gpuRead(uint32_t offset) {
        auto v=*reinterpret_cast<volatile uint32_t *>(gpu->getVirtualAddress()+offset);
        __asm__ volatile("dmb oshld" ::: "memory");return v;
    }
    Status isolated() {
        if(!owner->platformReady())return QcomPower::NotReady;
        return (gpuRead(Adreno::MEControl)&Adreno::Halt)==Adreno::Halt &&
            (gpuRead(Adreno::VBIFHalted)&15)==15?QcomPower::OK:QcomPower::Busy;
    }
    bool knownRoot() {
        // Readq avoids a CPU-created low/high mixture. Firmware may itself
        // update the two register halves in separate bus writes; allow that
        // bounded transition to settle, never accept an unknown stable root.
        for(unsigned attempt=0;attempt<8;++attempt) {
            const auto current=pair(bank+0x20);
            if(current==contract.ttbr)return true;
            for(const auto &root:roots)if(root.owner && root.ttbr==current)return true;
            if(attempt!=7)delay(1);
        }
        return false;
    }
    Status failed(Status status,const char *reason,uint32_t offset=0,uint64_t observed=0,uint64_t expected=0) {
        // Keep the first cause, before a context IRQ acknowledges FSR or the
        // GPU halt changes TTBR0. This path runs only on a failed invariant.
        if(!faultReported) {
            faultReported=true;owner->setProperty("GPUMapperFaultReason",reason);
            kprintf("QcomGPU: mapper fault reason=%s result=%u register=%x observed=%llx expected=%llx\n",
                reason,unsigned(status),offset,observed,expected);
            if(smmu && owner->platformReady()) {
                const auto fsr=read(bank+0x58),syndrome=read(bank+0x68),syndrome1=read(bank+0x6c);
                const auto far=pair(bank+0x60),ttbr=pair(bank+0x20);
                owner->setProperty("GPUMapperFaultFSR",fsr,32);owner->setProperty("GPUMapperFaultFAR",far,64);
                owner->setProperty("GPUMapperFaultSyndrome",syndrome,32);
                kprintf("QcomGPU: mapper snapshot fsr=%x far=%llx fsynr0=%x fsynr1=%x ttbr=%llx context=%x sctlr=%x tcr=%x tcr2=%x\n",
                    fsr,far,syndrome,syndrome1,ttbr,read(bank+0x34),read(bank),read(bank+0x30),read(bank+0x10));
                kprintf("QcomGPU: mapper split far-lo=%x far-hi=%x ttbr-lo=%x ttbr-hi=%x sid=%x\n",
                    read(bank+0x60),read(bank+0x64),read(bank+0x20),read(bank+0x24),read(0x1400+4*contract.bank));
                for(const auto &mapping:mappings)if(mapping.memory) {
                    kprintf("QcomGPU: retained mapping va=%llx length=%llx\n",mapping.lease.address,mapping.lease.length);
                }
            }
        }
        return status;
    }
    Status check() {
        if(!owner->platformReady())return failed(QcomPower::NotReady,"platform");
        using namespace QcomPower;
        if (!smmu || fault) return Stale;
        const uint32_t offsets[]={0,bank,bank+0x34,bank+0x30,bank+0x10,bank+0x38,bank+0x3c,
            0x1000+4*contract.bank,0x1800+4*contract.bank};
        const uint32_t expected[]={global,control,contextID,tcr,tcr2,mair0,mair1,cbar,cba2r};
        for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i) {
            uint32_t value=0;
            if(i==3)Adreno::settledTranslationControl(*this,offsets[i],expected[i],value);
            else value=read(offsets[i]);
            if(value!=expected[i])return failed(Failed,"context-register",offsets[i],value,expected[i]);
        }
        if(!knownRoot())return failed(Failed,"unknown-root",bank+0x20,pair(bank+0x20),contract.ttbr);
        const auto fsr=read(bank+0x58);
        if(SMMU::contextFault(fsr))return failed(Failed,"context-fault",bank+0x58,fsr);
        StreamConfigurationReader streamRegisters{smmu->getVirtualAddress()};
        streamAudit.capture(streamRegisters);
        const auto audit=streamAudit.validate(contract,routes);
        if(audit.status!=OK) {
            using Survey=SMMU::StreamAudit<>;
            const char *reason=audit.reason==Survey::Unreadable?"stream-read":audit.reason==Survey::Match?"stream-match":
                audit.reason==Survey::Route?"stream-route":audit.reason==Survey::ForeignBank?"foreign-bank":
                audit.reason==Survey::Alias?"stream-alias":"stream-contract";
            return failed(audit.status,reason,audit.offset,audit.observed,audit.expected);
        }
        if(firmwarePinned)for(const auto &range:firmware)for(unsigned i=0;i<range.pages;++i) {
            uint64_t leaf=0;
            if(tables.readOnlyBootLeaf(range.address+uint64_t(i)*4096,leaf)!=OK || leaf!=range.leaves[i])
                return failed(Stale,"firmware-leaf",0,leaf,range.leaves[i]);
        }
        return OK;
    }
    Status sync() {
        auto status=check();
        // The bank is exclusively owned, but another registered job ASID may
        // currently be selected. Invalidate the kernel ASID explicitly.
        if(status==QcomPower::OK)status=SMMU::synchronizeContext(*this,bank,contract.asid);
        if(status!=QcomPower::OK)fault=true;return status;
    }
    Status borrow(uint64_t physical,Adreno::MMUTablePage &out) {
        auto *p=new BorrowedPage;if(!p)return QcomPower::NotReady;
        p->memory=IOMemoryDescriptor::withPhysicalAddress(physical,4096,kIODirectionInOut|kIOMemoryMapperNone);
        p->map=p->memory?p->memory->map(kIOMapWriteCombineCache):nullptr;
        if (!p->map || p->map->getLength()!=4096) {OSSafeReleaseNULL(p->map);OSSafeReleaseNULL(p->memory);delete p;return QcomPower::Failed;}
        out={p,reinterpret_cast<volatile uint64_t *>(p->map->getVirtualAddress()),physical};return QcomPower::OK;
    }
    Status allocate(Adreno::MMUTablePage &out) {return allocatePage(out,kIOMapWriteCombineCache);}
    Status allocatePage(Adreno::MMUTablePage &out,IOOptionBits cache) {
        uint64_t mask=((1ULL<<contract.outputBits)-1)&~4095ULL;
        auto *memory=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|cache,4096,mask);
        if (!memory) return QcomPower::NotReady;
        IOByteCount span=0;uint64_t pa=memory->getPhysicalSegment(0,&span,kIOMemoryMapperNone);
        if (!pa || (pa&4095) || span<4096 || !memory->getBytesNoCopy()) {memory->release();return QcomPower::Failed;}
        out={memory,static_cast<volatile uint64_t *>(memory->getBytesNoCopy()),pa};return QcomPower::OK;
    }
    void release(Adreno::MMUTablePage &p,bool owned) {
        if(!p.owner){p={};return;}
        if (owned) static_cast<IOBufferMemoryDescriptor *>(p.owner)->release();
        else {auto *v=static_cast<BorrowedPage *>(p.owner);OSSafeReleaseNULL(v->map);OSSafeReleaseNULL(v->memory);delete v;}
        p={};
    }
    bool validate(const Adreno::MMUTablePage &page) {
        if(!page.owner || !page.entries || !page.physical || (page.physical&4095) ||
            page.physical>=(1ULL<<contract.outputBits))return false;
        auto *memory=static_cast<IOBufferMemoryDescriptor *>(page.owner);
        IOByteCount span=0;const auto physical=memory->getPhysicalSegment(0,&span,kIOMemoryMapperNone);
        return physical==page.physical && span>=4096 && memory->getLength()>=4096 && memory->getBytesNoCopy()==page.entries;
    }
    bool configure() {
        using namespace QcomPower;
        if (!contract.valid() || isolated()!=OK || tlb.configure(0x10000,contract.bank,contract.asid)!=OK) return false;
        bank=tlb.bankOffset();global=read(0);control=read(bank);tcr=read(bank+0x30);tcr2=read(bank+0x10);
        contextID=read(bank+0x34);
        mair0=read(bank+0x38);mair1=read(bank+0x3c);cbar=read(0x1000);cba2r=read(0x1800);
        unsigned ips=contract.outputBits==40?2:contract.outputBits==36?1:0;
        if ((global&9) || (control&~0x1167u) || (control&0x1067)!=0x1067 || cba2r!=1 || ((cbar>>16)&3)!=1 ||
            pair(bank+0x20)!=contract.ttbr || pair(bank+0x28) || (mair0&255)!=0x44 ||
            tcr!=((1u<<23)|(2u<<12)|(64-contract.inputBits)) || tcr2!=((7u<<15)|(1u<<4)|ips)) return false;
        auto id0=read(0x20),id2=read(0x28);unsigned widths[]={32,36,40,42,44,48,52};
        if (!(id0&(1u<<27)) || (id2&15)>=7 || ((id2>>4)&15)>=7 || ((id2>>8)&15)>=7 ||
            contract.outputBits>widths[id2&15] || contract.outputBits>widths[(id2>>4)&15] || contract.inputBits>widths[(id2>>8)&15]) return false;
        unsigned groups=id0&255,sidBits=(id0>>9)&15;if(!groups || groups>128)return false;
        for (unsigned i=0;i<contract.streamCount;++i) {
            auto s=contract.streams[i];if(s.slot>=groups || s.id>=1u<<sidBits)return false;
            routes[i]=read(0xc00+4*s.slot);
            if ((routes[i]&0x300ff)!=contract.bank) return false;
        }
        return check()==OK && tables.adopt(contract)==OK && ledger.initialize(contract.inputBits,contract.first,contract.end)==OK;
    }
};
struct QcomGPUAddressSpace::State {
    QcomGPUMapper::State &hardware;
    GPU::Diagnostics::AddressSpaces::Trace trace{};
    struct PrivatePages {
        QcomGPUMapper::State &hardware;
        bool reusable=false;
        QcomPower::Status allocate(Adreno::MMUTablePage &page) {
            return hardware.privatePages.allocate(page,hardware.cachePrivatePages);
        }
        void release(Adreno::MMUTablePage &page,bool) {
            hardware.privatePages.release(page,reusable && hardware.cachePrivatePages);
        }
        QcomPower::Status publish(const Adreno::MMUTablePage &page) {
            // IOBuffer's nonpageable allocation is auto-prepared by XNU.
            // Use that original descriptor, never a conflicting CPU alias.
            if(!hardware.validate(page))return QcomPower::Stale;
            auto *memory=static_cast<IOBufferMemoryDescriptor *>(page.owner);
            return memory->performOperation(kIOMemoryIncoherentIOStore,0,4096)==kIOReturnSuccess?
                QcomPower::OK:QcomPower::Failed;
        }
        void barrier(){hardware.barrier();}
    } pages;
    Adreno::JobPageTables<PrivatePages> tables;
    unsigned asid=0;
    bool submitted=false,retired=false,badTLB=false;
    explicit State(QcomGPUMapper::State &parent):hardware(parent),pages{parent},tables(pages,&trace) {}
};
Adreno::PageTableContext QcomGPUAddressSpace::context() const {
    return fState && !fState->retired ? Adreno::PageTableContext{fState->tables.ttbr(),fState->hardware.contract.ttbr} :
        Adreno::PageTableContext{};
}
void QcomGPUAddressSpace::markSubmitted() {if(fState)fState->submitted=true;}
bool QcomGPUAddressSpace::retire(bool completed) {
    if(!fState || fState->retired)return true;
    namespace AS=GPU::Diagnostics::AddressSpaces;
    auto &privateState=*fState;auto &s=privateState.hardware;const auto wait=mach_absolute_time();IOLockLock(s.lock);
    privateState.trace.sample(AS::RetireWait,wait,mach_absolute_time());
    auto *registered=static_cast<QcomGPUMapper::State::PrivateRoot *>(nullptr);
    for(auto &root:s.roots)if(root.owner==this)registered=&root;
    bool valid=!privateState.tables.faulted() && !privateState.badTLB;
    if(valid && privateState.submitted) {
        // A timestamp follows the return-to-kernel packet. If a reported
        // completion still selects this tree, keep every backing allocation.
        {AS::Span timing(&privateState.trace,AS::RetireHealth);
            valid=!s.fault && s.check()==QcomPower::OK && s.pair(s.bank+0x20)!=privateState.tables.ttbr();}
        if(valid){AS::Span timing(&privateState.trace,AS::RetireTLB);
            valid=SMMU::synchronizeContext(s,s.bank,privateState.asid)==QcomPower::OK;}
    }
    if(valid) {
        // Error cleanup/halt-only retirement never populates this pool.
        privateState.pages.reusable=completed && privateState.submitted && registered && !s.fault;
        AS::Span timing(&privateState.trace,AS::Release);
        valid=privateState.tables.destroy(true)==QcomPower::OK;
    }
    if(valid){if(registered)*registered={};privateState.retired=true;}
    else s.fault=true;
    IOLockUnlock(s.lock);return valid;
}
void QcomGPUAddressSpace::free() {
    if(!retire()) {IOLog("QcomGPU: private page table retirement unproved; ASID and backing retained\n");return;}
    delete fState;fState=nullptr;OSSafeReleaseNULL(fMapper);OSObject::free();
}
bool QcomGPUMapper::privateAddressSpacesReady() const {
    if(!fState || !fState->lock)return false;auto &s=*fState;IOLockLock(s.lock);
    bool ready=s.ready && !s.fault && s.firmwarePinned && !s.contextID;
    IOLockUnlock(s.lock);return ready;
}
uint64_t QcomGPUMapper::apertureBytes() const {
    if(!fState || !fState->lock)return 0;
    IOLockLock(fState->lock);const auto bytes=fState->contract.end-fState->contract.first;IOLockUnlock(fState->lock);return bytes;
}
GPU::Native::Memory::MappingUsage QcomGPUMapper::memoryUsage() const {
    GPU::Native::Memory::MappingUsage usage{};
    if(!fState || !fState->lock)return usage;auto &s=*fState;IOLockLock(s.lock);
    usage={s.ledger.count(),s.ledger.payloadBytes(),s.ledger.spanBytes(),s.tables.tableCount()};
    IOLockUnlock(s.lock);return usage;
}
GPU::Diagnostics::AddressSpaces::Snapshot QcomGPUMapper::addressSpaceStatistics() const {
    GPU::Diagnostics::AddressSpaces::Snapshot snapshot{};
    if(!fState || !fState->lock)return snapshot;auto &s=*fState;IOLockLock(s.lock);
    snapshot=s.addressStatistics;snapshot.bytes=sizeof(snapshot);snapshot.enabled=s.cachePrivatePages;
    snapshot.hits=s.privatePages.hits;snapshot.allocations=s.privatePages.allocations;snapshot.cached=s.privatePages.count();
    snapshot.validationFailures=s.privatePages.validationFailures;
    IOLockUnlock(s.lock);
    absolutetime_to_nanoseconds(snapshot.timebaseSpanTicks,&snapshot.timebaseSpanNanoseconds);return snapshot;
}
GPU::MappingBounds::Snapshot QcomGPUMapper::mappingWorkStatistics() const {
    GPU::MappingBounds::Snapshot out;
    if(!fState || !fState->lock)return out;
    IOLockLock(fState->lock);out=fState->mappingWork;IOLockUnlock(fState->lock);return out;
}
QcomGPUAddressSpace *QcomGPUMapper::createAddressSpace(const QcomGPUBuffer *ring,const QcomGPUBuffer *ib,
    const QcomGPUBuffer *indices,const QcomGPUBuffer *scratch,QcomGPUBuffer *const *buffers,unsigned count,OSArray *commands,bool compositor,
    const GPU::MappingBounds::Plan *bounds) {
    using namespace QcomPower;
    namespace AS=GPU::Diagnostics::AddressSpaces;
    if(!fState || !ring || !ib || !buffers || count>GPU::Native::Memory::ResourcesPerJob ||
        (commands && (!commands->getCount() || commands->getCount()>Adreno::MaxIndirectBuffers)) ||
        (bounds && !bounds->valid(count)))return nullptr;
    auto *space=new QcomGPUAddressSpace;
    if(!space || !space->init()){if(space)space->release();return nullptr;}
    space->fMapper=this;retain();auto &s=*fState;space->fState=new QcomGPUAddressSpace::State(s);
    if(!space->fState){space->release();return nullptr;}
    const auto wait=mach_absolute_time();IOLockLock(s.lock);auto &p=*space->fState;Status status=NotReady;
    p.trace={&s.addressStatistics.native,compositor?&s.addressStatistics.compositor:nullptr,mach_absolute_time,
        s.detailOrdinal++%AS::DetailPeriod==0};
    p.trace.sample(AS::BuildWait,wait,mach_absolute_time());
    GPU::MappingBounds::Work work;work.jobs=1;
    unsigned slot=0;while(slot<GPU::MaxJobs && s.roots[slot].owner)++slot;
    if(s.ready && !s.fault && s.firmwarePinned && !s.contextID && slot<GPU::MaxJobs) {
        {AS::Span timing(&p.trace,AS::BuildHealth);status=s.check();if(status!=OK)s.fault=true;}
        // ASIDs are reserved by live job slots, exclude the retained kernel
        // ASID, and are reused only after explicit TLBSYNC at retirement.
        p.asid=slot+1;if(s.contract.asid && p.asid>=s.contract.asid)++p.asid;
        if(status==OK)status=p.tables.initialize(s.contract.inputBits,s.contract.outputBits,p.asid);
        struct FirmwarePages {
            const State::FirmwareRange &range;
            Status page(unsigned index,uint64_t &pa) {
                if(index>=range.pages)return Invalid;
                pa=range.leaves[index]&0x0000fffffffff000ULL;return OK;
            }
        };
        if(status==OK){AS::Span timing(&p.trace,AS::FirmwareMap);
            for(const auto &range:s.firmware)if(status==OK){FirmwarePages source{range};
                status=p.tables.map(range.address,range.pages,source,1);}}
        auto map=[&](const QcomGPUBuffer *buffer,unsigned access,uint64_t first=0,uint64_t bytes=0) {
            if(status!=OK || !buffer)return;
            buffer=buffer->backing();
            if(!buffer->mappedBy(this)){status=Stale;return;}
            if((access&2) && !buffer->gpuWritable()){status=Denied;return;}
            const auto span=GPU::pageSpanBytes(buffer->length);
            if(!buffer->address || (buffer->address&4095) || !span){status=Invalid;return;}
            if(!bytes)bytes=span;
            if((first&4095) || (bytes&4095) || !GPU::range(span,first,bytes)){status=Invalid;return;}
            Adreno::PinnedBufferPages<QcomGPUBuffer> source{buffer,unsigned(first/4096)};
            status=p.tables.map(buffer->address+first,unsigned(bytes/4096),source,access);
        };
        if(status==OK) {AS::Span timing(&p.trace,AS::BufferMap);
        map(ring,1); // CP ring readable in every tree; shaders cannot modify it
        map(ib,1); // immutable PM4, code and constants; no GPU writes allowed
        if(commands)for(unsigned i=0;i<commands->getCount();++i) {
            auto *command=OSDynamicCast(QcomGPUBuffer,commands->getObject(i));
            if(!command){status=Invalid;break;}
            if(command!=ib)map(command,1);
        }
        map(indices,1);
        map(scratch,3); // CP color/depth flush timestamp cells, separate from code
        for(unsigned i=0;i<count;++i)if(!buffers[i]){status=Invalid;break;}
        for(unsigned i=0;i<count && status==OK;++i) {
            // Separate IOSurface imports retain separate views of the same
            // prepared root mapping. Merge their verified working sets once;
            // unknown access falls back to the whole backing. Permissions
            // still come from the original prepared root descriptor.
            bool seen=false;
            for(unsigned j=0;j<i;++j)if(buffers[j]->backing()==buffers[i]->backing()){seen=true;break;}
            if(seen)continue;
            const auto *root=buffers[i]->backing();
            GPU::MappingBounds::RootSelection selected;
            if(!GPU::MappingBounds::selectRoot(bounds,buffers,count,i,selected)){status=Invalid;break;}
            const auto before=p.tables.mappedPages();
            const auto access=root->gpuWritable()?3:1;
            // Preserve disjoint ranges rather than filling the unused heap
            // pages between them. Every selected physical page is rechecked.
            if(!GPU::MappingBounds::visitSelected(bounds,selected,[&](const GPU::MappingBounds::PageSpan &part) {
                map(root,access,part.first-root->address,part.end-part.first);return status==OK;
            }) && status==OK)status=Invalid;
            if(status==OK) {
                ++work.roots;work.fullPages+=GPU::pageSpanBytes(root->length)/4096;
                work.mappedPages+=p.tables.mappedPages()-before;
                if(selected.fallback)++work.fallbackRoots;
                if(selected.overflow)++work.overflowRoots;
            }
        }
        }
        // The kernel fence and other tasks' allocations are deliberately not
        // mapped. The fence is written only after the kernel tree is restored.
        if(status==OK){AS::Span timing(&p.trace,AS::Publish);status=p.tables.publish();}
        if(status==OK){AS::Span timing(&p.trace,AS::BuildTLB);
            status=SMMU::synchronizeContext(s,s.bank,p.asid);p.badTLB=status!=OK;}
        if(status==OK) {
            s.roots[slot]={space,p.tables.ttbr(),p.asid};
            s.mappingWork.native.add(work);if(compositor)s.mappingWork.compositor.add(work);
        }
        else if(p.tables.published() || p.tables.faulted() || status==Failed || status==Stale || status==TimedOut)
            s.fault=true;
    }
    bool fault=s.fault;IOLockUnlock(s.lock);
    if(status!=OK){space->release();space=nullptr;}
    if(fault)s.owner->mapperFault();return space;
}
bool QcomGPUMapper::kernelContextSelected() const {
    if(!fState || !fState->lock)return false;auto &s=*fState;IOLockLock(s.lock);
    // Fault handling has closed admission under the GPU gate. Do not clear a
    // fault, change TTBR0 or create mappings to make a diagnostic possible.
    bool valid=s.smmu && s.firmwarePinned && s.owner->platformReady() &&
        s.pair(s.bank+0x20)==s.contract.ttbr && s.read(s.bank)==s.control &&
        s.read(s.bank+0x30)==s.tcr && s.read(s.bank+0x10)==s.tcr2 &&
        s.read(s.bank+0x34)==s.contextID && !(s.read(s.bank+0x58)&(1u<<30));
    IOLockUnlock(s.lock);return valid;
}
bool QcomGPUMapper::restoreKernelContext() {
    if(!fState || !fState->lock)return false;auto &s=*fState;IOLockLock(s.lock);
    bool valid=s.ready && !s.fault && s.isolated()==QcomPower::OK && s.check()==QcomPower::OK;
    if(valid && s.pair(s.bank+0x20)!=s.contract.ttbr) {
        s.writePair(s.bank+0x20,s.contract.ttbr);
        valid=s.pair(s.bank+0x20)==s.contract.ttbr &&
            SMMU::synchronizeContext(s,s.bank,s.contract.asid,1000000,true)==QcomPower::OK && s.check()==QcomPower::OK;
    }
    if(!valid)s.fault=true;
    IOLockUnlock(s.lock);return valid;
}
QcomGPUMapper *QcomGPUMapper::create(QcomGPU *owner,IOService *provider,IOMemoryMap *gpu) {
    if (!owner || !provider || !gpu || provider->getProperty("iommu-parent")) return nullptr;
    // Buffer-MD cache options overlap MapperNone. Use a ready system-mapper
    // decision and host physical contiguity, never OR conflicting cache bits.
    if (reinterpret_cast<uintptr_t>(IOMapper::gSystem)&3) return nullptr;
    auto *data=OSDynamicCast(OSData,provider->getProperty("qcom,adreno-smmu-handoff"));
    if (!data || data->getLength()!=sizeof(Adreno::SMMUContract)) return nullptr;
    auto *mapper=new QcomGPUMapper;if(!mapper || !mapper->init()){if(mapper)mapper->release();return nullptr;}
    mapper->fState=new State;
    if (!mapper->fState) {mapper->release();return nullptr;}
    auto &s=*mapper->fState;memcpy(&s.contract,data->getBytesNoCopy(),sizeof(s.contract));
    int cursor=1;PE_parse_boot_argn("gpu-cursor",&cursor,sizeof(cursor));s.useTranslationCursor=cursor!=0;
    owner->setProperty("GPUTranslationCursor",s.useTranslationCursor);
    UInt32 pipe=0;PE_parse_boot_argn("gpu-pipe",&pipe,sizeof(pipe));s.cachePrivatePages=pipe>=3 && pipe<=6;
    owner->setProperty("GPUPrivateTableCache",s.cachePrivatePages);
    owner->setProperty("GPUPrivateTableCPUCacheable",true);
    owner->setProperty("GPUPrivateTablePoCPublish",true);
    kprintf("QcomGPU: translation-cursor enabled=%u parent-leaf-recheck=1\n",s.useTranslationCursor);
    s.owner=owner;owner->retain();s.provider=provider;provider->retain();s.gpu=gpu;gpu->retain();s.lock=IOLockAlloc();
    if (!s.lock || !(mapper->fAttached=mapper->attach(provider)) || !mapper->start(provider)) {mapper->shutdown();mapper->release();return nullptr;}
    mapper->fStarted=true;
    if (!provider->setProperty("iommu-parent",mapper)) {mapper->shutdown();mapper->release();return nullptr;}
    s.bound=true;mapper->registerService();return mapper;
}
bool QcomGPUMapper::start(IOService *provider) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    static_assert(__builtin_offsetof(QcomGPUMapper,fIsSystem)==200,"native IOMapper system flag offset");
#pragma clang diagnostic pop
    fIsSystem=false;bool ok=IOMapper::start(provider);if(!ok)IOService::stop(provider);return ok;
}
bool QcomGPUMapper::initHardware(IOService *provider) {
    auto &s=*fState;auto *memory=provider->getDeviceMemoryWithIndex(1);
    if (!memory || memory->getPhysicalAddress()!=0x05040000 || memory->getLength()!=0x10000) return false;
    s.smmu=provider->mapDeviceMemoryWithIndex(1,kIOMapInhibitCache);
    int type=0;if(!s.smmu || provider->getInterruptType(1,&type)!=kIOReturnSuccess || !(type&kIOInterruptTypeLevel))return false;
    if(!s.configure())return false;
    uint64_t pa=0;auto *handoff=OSDynamicCast(OSData,provider->getProperty("qcom,gpu-handoff"));Adreno::PlatformContract cp{};
    if(!handoff || handoff->getLength()!=sizeof(cp))return false;
    memcpy(&cp,handoff->getBytesNoCopy(),sizeof(cp));
    if(!cp.valid() || s.tables.translate(cp.pm4,pa)!=QcomPower::OK || s.tables.translate(cp.pfp,pa)!=QcomPower::OK)return false;
    s.interrupt=IOInterruptEventSource::interruptEventSource(this,interruptAction,provider,1);
    if(!s.interrupt || s.owner->getWorkLoop()->addEventSource(s.interrupt)!=kIOReturnSuccess)return false;
    s.interruptAdded=true;s.interrupt->enable();s.ready=true;return true;
}
bool QcomGPUMapper::healthy() const {
    if(!fState || !fState->lock)return false;auto &s=*fState;IOLockLock(s.lock);
    bool signal=s.ready && !s.fault && s.check()!=QcomPower::OK;
    if(signal)s.fault=true;
    bool ok=s.ready && !s.fault;IOLockUnlock(s.lock);if(signal)s.owner->mapperFault();return ok;
}
bool QcomGPUMapper::retainFirmware(const Adreno::FirmwareHandoff &h) {
    if(!fState || !fState->lock)return false;auto &s=*fState;IOLockLock(s.lock);
    bool valid=s.ready && !s.fault && !s.firmwarePinned && s.isolated()==QcomPower::OK && s.check()==QcomPower::OK;
    const uint64_t addresses[]={h.pm4,h.pfp,h.gpmu};
    const uint32_t bytes[]={h.pm4Bytes,h.pfpBytes,h.gpmuWords*4};
    for(unsigned i=0;valid && i<3;++i) {
        auto &range=s.firmware[i];range.address=addresses[i];range.pages=(bytes[i]+4095)/4096;
        valid=bytes[i] && range.pages<=256 && !(range.address&4095) &&
            !(range.address<s.contract.end && s.contract.first<range.address+uint64_t(range.pages)*4096);
        for(unsigned n=0;valid && n<range.pages;++n)
            valid=s.tables.readOnlyBootLeaf(range.address+uint64_t(n)*4096,range.leaves[n])==QcomPower::OK;
    }
    s.firmwarePinned=valid;IOLockUnlock(s.lock);return valid;
}
bool QcomGPUMapper::synchronizeForRecovery() {
    if(!fState || !fState->lock)return false;auto &s=*fState;IOLockLock(s.lock);
    bool valid=s.ready && !s.fault && s.firmwarePinned && s.isolated()==QcomPower::OK && s.pair(s.bank+0x20)==s.contract.ttbr;
    if(valid)valid=s.sync()==QcomPower::OK;
    const bool signal=s.fault;IOLockUnlock(s.lock);if(signal)s.owner->mapperFault();return valid;
}
bool QcomGPUMapper::retainsMemory(IOMemoryDescriptor *memory) const {
    if(!fState || !fState->lock)return false;auto &s=*fState;IOLockLock(s.lock);bool found=false;
    for (const auto &m:s.mappings) if(m.memory==memory)found=true;
    IOLockUnlock(s.lock);return found;
}
bool QcomGPUMapper::validateBufferMapping(const QcomGPUBuffer &buffer,IOMemoryDescriptor *memory,IODMACommand *command) const {
    if(!fState || !buffer.mappedBy(this))return false;
    auto &s=*fState;IOLockLock(s.lock);bool valid=false;
    // Only this buffer's private DMA command can unmap this lease. Jobs hold
    // the buffer until completion and ASID retirement, before DMA::complete.
    if(s.ready && !s.fault)for(const auto &m:s.mappings)
        if(m.memory==memory && m.command==command && m.lease.address==buffer.address &&
            m.lease.length>=GPU::pageSpanBytes(buffer.length)) {
            Adreno::MappedBufferPages<State,QcomGPUBuffer> pages{s,&buffer};
            valid=pages.validate(unsigned(GPU::pageSpanBytes(buffer.length)/4096))==QcomPower::OK;
            break;
        }
    IOLockUnlock(s.lock);return valid;
}
IOReturn QcomGPUMapper::iovmMapMemory(IOMemoryDescriptor *memory,uint64_t offset,uint64_t length,uint32_t options,
    const IODMAMapSpecification *spec,IODMACommand *command,const IODMAMapPageList *list,uint64_t *address,uint64_t *bytes) {
    using namespace QcomPower;
    if(address)*address=0;if(bytes)*bytes=0;
    if(!fState || !address || !bytes || !memory || !command || !length || length>GPU::MaxBufferBytes ||
        offset>memory->getLength() || length>memory->getLength()-offset || !(options&3) ||
        (options&~uint32_t(3|kIODMAMapPageListFullyOccupied)) || (list && (offset || !list->pageList || !list->pageListCount || list->pageOffset>=PAGE_SIZE)))return kIOReturnUnsupported;
    if(spec){if(spec->options)return kIOReturnUnsupported;for(auto v:spec->resvA)if(v)return kIOReturnBadArgument;for(auto v:spec->resvB)if(v)return kIOReturnBadArgument;}
    IOByteCount span=0;uint64_t first=list?(uint64_t(list->pageList[0].phys_addr)<<PAGE_SHIFT)+list->pageOffset:memory->getPhysicalSegment(offset,&span,kIOMemoryMapperNone);
    if(!first)return kIOReturnBadArgument;
    unsigned pageOffset=first&4095;PhysicalSource source{memory,list,offset,length,pageOffset};auto &s=*fState;
    IOLockLock(s.lock);Status status=NotReady;State::Mapping *slot=nullptr;
    if(s.ready && !s.fault){status=s.check();if(status!=OK)s.fault=true;}
    if(s.ready && !s.fault && status==OK){
        for(auto &m:s.mappings)if(!m.memory){slot=&m;break;}
        State::Ledger::Record *record=nullptr;
        unsigned bits=spec && spec->numAddressBits && spec->numAddressBits<s.contract.inputBits?spec->numAddressBits:s.contract.inputBits;
        status=slot?s.ledger.reserve(reinterpret_cast<uintptr_t>(memory),reinterpret_cast<uintptr_t>(command),length,pageOffset,spec?spec->alignment:1,bits,record):NotReady;
        if(status==OK){
            memory->retain();command->retain();*slot={*record,memory,command};
            status=s.tables.map(record->address&~4095ULL,unsigned(record->length/4096),source,options&3);
            if(status==OK){s.ledger.mapped(record->identity);*address=record->address;*bytes=record->length;}
            else if(s.tables.faulted()){s.ledger.quarantine(record->identity);s.fault=true;}
            else {s.ledger.cancel(record->identity);*slot={};command->release();memory->release();}
        }
    }
    bool fault=s.fault;IOLockUnlock(s.lock);if(fault)s.owner->mapperFault();return result(status);
}
IOReturn QcomGPUMapper::iovmUnmapMemory(IOMemoryDescriptor *memory,IODMACommand *command,uint64_t address,uint64_t bytes) {
    using namespace QcomPower;if(!fState)return kIOReturnNotReady;auto &s=*fState;IOLockLock(s.lock);
    State::Mapping *found=nullptr;for(auto &m:s.mappings)if(m.memory==memory && m.command==command && m.lease.address==address && m.lease.length==bytes){found=&m;break;}
    Status status=Invalid;
    if(found){
        status=s.fault?Stale:s.check();if(status!=OK)s.fault=true;
        if(status==OK){status=s.ledger.beginUnmap(found->lease.identity,reinterpret_cast<uintptr_t>(memory),reinterpret_cast<uintptr_t>(command),address,bytes);
            if(status==OK){status=s.tables.unmap(address&~4095ULL,unsigned(bytes/4096));status=s.ledger.finishUnmap(found->lease.identity,status);}}
        if(status==OK){*found={};command->release();memory->release();}else{s.ledger.quarantine(found->lease.identity);s.fault=true;}
    }
    bool fault=s.fault;IOLockUnlock(s.lock);if(fault)s.owner->mapperFault();return result(status);
}
uint64_t QcomGPUMapper::mapToPhysicalAddress(uint64_t address) {
    if(!fState)return 0;auto &s=*fState;IOLockLock(s.lock);uint64_t pa=0;
    if(s.ready && !s.fault && s.check()==QcomPower::OK)s.tables.translate(address,pa);
    IOLockUnlock(s.lock);return pa;
}
void QcomGPUMapper::interruptAction(OSObject *object,IOInterruptEventSource *interrupt,int) {
    auto &s=*static_cast<QcomGPUMapper *>(object)->fState;IOLockLock(s.lock);
    if(!s.owner->platformReady()) {
        s.fault=true;s.ready=false;interrupt->disable();IOLockUnlock(s.lock);s.owner->mapperFault();return;
    }
    uint32_t fsr=s.read(s.bank+0x58);if(!SMMU::contextFault(fsr)){IOLockUnlock(s.lock);return;}
    uint64_t far=s.pair(s.bank+0x60);
    s.failed(QcomPower::Failed,"context-interrupt",s.bank+0x58,fsr);
    s.fault=true;s.ready=false;interrupt->disable();
    // Terminate an unexpected stalled fault before acknowledging its status.
    if(fsr&(1u<<30))s.write(s.bank+8,1);
    if(fsr!=UINT32_MAX)s.write(s.bank+0x58,fsr&SMMU::ContextFaultMask);
    IOLockUnlock(s.lock);IOLog("QcomGPU: SMMU context fault FSR=%x FAR=%llx; DMA resources retained\n",fsr,far);s.owner->mapperFault();
}
bool QcomGPUMapper::shutdown() {
    if(!fState)return true;auto &s=*fState;if(!s.lock)return true;
    IOLockLock(s.lock);s.ready=false;
    bool rootsIdle=true;for(const auto &root:s.roots)if(root.owner)rootsIdle=false;
    bool clean=rootsIdle && !s.ledger.count() && !s.fault && s.tables.destroy()==QcomPower::OK;
    if(clean && s.privatePages.drain(true)!=QcomPower::OK){s.fault=true;clean=false;}
    IOLockUnlock(s.lock);if(!clean)return false;
    if(s.interrupt)s.interrupt->disable();
    if(s.interruptAdded){s.owner->getWorkLoop()->removeEventSource(s.interrupt);s.interruptAdded=false;}
    OSSafeReleaseNULL(s.interrupt);
    if(s.bound){if(s.provider->getProperty("iommu-parent")==this)s.provider->removeProperty("iommu-parent");s.bound=false;}
    if(fStarted){IOService::stop(s.provider);fStarted=false;}
    if(fAttached){detach(s.provider);fAttached=false;}
    return true;
}
void QcomGPUMapper::free() {
    // Failed TLBSYNC/root ownership checks keep tables, prepared descriptors,
    // commands, MMIO and the GPU provider's pinned-resource lease alive.
    if(!shutdown()){IOLog("QcomGPU: retained SMMU mapper quarantined\n");return;}
    if(fState){auto &s=*fState;OSSafeReleaseNULL(s.smmu);OSSafeReleaseNULL(s.gpu);OSSafeReleaseNULL(s.provider);
        auto *owner=s.owner;s.owner=nullptr;if(s.lock)IOLockFree(s.lock);delete fState;fState=nullptr;if(owner)owner->release();}
    IOMapper::free();
}
