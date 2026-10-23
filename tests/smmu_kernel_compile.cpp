// SPDX-License-Identifier: BSD-2-Clause
// Compile-only IOMapper ABI and SMMU template probe. Never linked or matched.
#include <IOKit/IOMapper.h>
#include <IOKit/IODMACommand.h>
#include <IOTableMemory.h>
#include <ContextTLB.h>
#include <HardwareContext.h>
#include <MappingEngine.h>
#include <ContextHandoff.h>
#include <USBMMIO.h>
static_assert((kIOMemoryMapperNone&kIOMapCacheMask)!=0,"Native buffer option fields overlap");
static_assert((IOKitMemory::bufferOptions(false)&kIOMapCacheMask)==kIOMapWriteCombineCache,
    "SMMU tables require Normal NC CPU memory with valid allocation cache bits");
static_assert(!(IOKitMemory::bufferOptions(false,true)&kIOMemoryMapperNone),
    "physical table allocation cannot smuggle MapperNone into its cache mode");
static_assert(!(MI6IOBuffer::HostPhysicalMask & (kIOMapCacheMask | kIOBufferDescriptorMemoryFlags |
    kIOMemorySharingTypeMask | kIOMemoryPhysicallyContiguous | kIOMemoryPageable |
    kIOMemoryPurgeable | kIOMemoryHostPhysicallyContiguous)),
    "the host physical mask option must not change Native cache or MD options");
class IOMapperABIProbe : public IOMapper {
    OSDeclareDefaultStructors(IOMapperABIProbe);
protected:
    bool initHardware(IOService *) APPLE_KEXT_OVERRIDE { return false; }
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE { fIsSystem=false; return IOMapper::start(provider); }
    void free() APPLE_KEXT_OVERRIDE { IOMapper::free(); }
    uint64_t getPageSize() const APPLE_KEXT_OVERRIDE { return 4096; }
    IOReturn iovmMapMemory(IOMemoryDescriptor *,uint64_t,uint64_t,uint32_t,const IODMAMapSpecification *,
        IODMACommand *,const IODMAMapPageList *,uint64_t *,uint64_t *) APPLE_KEXT_OVERRIDE { return kIOReturnNotReady; }
    IOReturn iovmUnmapMemory(IOMemoryDescriptor *,IODMACommand *,uint64_t,uint64_t) APPLE_KEXT_OVERRIDE { return kIOReturnNotReady; }
    IOReturn iovmInsert(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t) APPLE_KEXT_OVERRIDE { return kIOReturnNotReady; }
    uint64_t mapToPhysicalAddress(uint64_t) APPLE_KEXT_OVERRIDE { return 0; }
};
OSDefineMetaClassAndStructors(IOMapperABIProbe,IOMapper);
class IODMACommandABIProbe : public IODMACommand {
    OSDeclareDefaultStructors(IODMACommandABIProbe);
public:
    void free() APPLE_KEXT_OVERRIDE { IODMACommand::free(); }
    IOReturn setMemoryDescriptor(const IOMemoryDescriptor *m,bool prepare) APPLE_KEXT_OVERRIDE { return IODMACommand::setMemoryDescriptor(m,prepare); }
    IOReturn clearMemoryDescriptor(bool complete) APPLE_KEXT_OVERRIDE { return IODMACommand::clearMemoryDescriptor(complete); }
    IOReturn prepare(UInt64 o,UInt64 n,bool flush,bool sync) APPLE_KEXT_OVERRIDE { return IODMACommand::prepare(o,n,flush,sync); }
    IOReturn complete(bool invalidate,bool sync) APPLE_KEXT_OVERRIDE { return IODMACommand::complete(invalidate,sync); }
};
OSDefineMetaClassAndStructors(IODMACommandABIProbe,IODMACommand);
static_assert(sizeof(IODMACommand)==120 && sizeof(IODMACommandABIProbe)==120,"native DMA command instance ABI");
static_assert(sizeof(IOMapper)==208 && sizeof(IOMapperABIProbe)==208,"native mapper instance ABI");
class IOBufferABIProbe : public IOBufferMemoryDescriptor {
    OSDeclareDefaultStructors(IOBufferABIProbe);
public:
    IOReturn complete(IODirection direction) APPLE_KEXT_OVERRIDE {return IOBufferMemoryDescriptor::complete(direction);}
};
OSDefineMetaClassAndStructors(IOBufferABIProbe,IOBufferMemoryDescriptor);
static_assert(sizeof(IOMemoryDescriptor)==96 && sizeof(IOGeneralMemoryDescriptor)==176 &&
    sizeof(IOBufferMemoryDescriptor)==232 && sizeof(IOBufferABIProbe)==232,"native memory descriptor instances");
static_assert(sizeof(IODMAMapSpecification)==40 && __builtin_offsetof(IODMAMapSpecification,numAddressBits)==20,"DMA map specification ABI");
static_assert(sizeof(IODMAMapPageList)==16 && __builtin_offsetof(IODMAMapPageList,pageList)==8,"DMA page list ABI");
struct UnattachedContext {
    QcomPower::Status sync() { return QcomPower::NotReady; }
    QcomPower::Status rootDetached() { return QcomPower::NotReady; }
};
template class SMMU::IOTableMemory<UnattachedContext>;
template class SMMU::PageTables<SMMU::IOTableMemory<UnattachedContext>>;
struct PhysicalPages {
    QcomPower::Status page(unsigned,uint64_t &) { return QcomPower::NotReady; }
};
QcomPower::Status SMMUMapCompileProbe(SMMU::PageTables<SMMU::IOTableMemory<UnattachedContext>> &tables,PhysicalPages &source) {
    return tables.map(4096,1,source,3);
}
struct SMMURegistersProbe {
    USB::MMIO &io;
    QcomPower::Status resourceStatus=QcomPower::NotReady,resetStatus=QcomPower::NotReady;
    QcomPower::Status accessible() { return resourceStatus; }
    QcomPower::Status isolated() { return resetStatus; }
    uint64_t read64(uint32_t o) { return io.read64(o); }
    void write64(uint32_t o,uint64_t v) { io.write64(o,v); }
    uint32_t read(uint32_t o) { return io.read(o); }
    void write(uint32_t o,uint32_t v) { io.write(o,v); }
    void barrier() { __asm__ volatile("dsb osh" ::: "memory"); }
    void delay(unsigned us) { io.delay(us); }
};
template class SMMU::ContextTLB<SMMURegistersProbe>;
void NativeDMACompleteCompileProbe(IODMACommand *command) {
    (void)command->complete();
    (void)command->clearMemoryDescriptor(false);
}

template class SMMU::HardwareContext<SMMURegistersProbe>;
struct ReferencesProbe {
    void retain(uintptr_t,uintptr_t) {}
    void release(uintptr_t,uintptr_t) {}
};
using ContextProbe=SMMU::HardwareContext<SMMURegistersProbe>;
using NativeTablesProbe=SMMU::PageTables<SMMU::IOTableMemory<ContextProbe>>;
template class SMMU::IOTableMemory<ContextProbe>;
template class SMMU::PageTables<SMMU::IOTableMemory<ContextProbe>>;
template class SMMU::MappingEngine<NativeTablesProbe,ContextProbe,ReferencesProbe>;
QcomPower::Status SMMUMappingEngineCompileProbe(SMMU::MappingEngine<NativeTablesProbe,ContextProbe,ReferencesProbe> &engine,PhysicalPages &source) {
    SMMU::MappingEngine<NativeTablesProbe,ContextProbe,ReferencesProbe>::Record output;
    return engine.map(1,2,4096,0,4096,32,3,source,output);
}
