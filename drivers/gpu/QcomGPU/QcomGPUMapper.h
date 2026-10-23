// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOMapper.h>
#include <GPUNativeMemory.h>
#include <GPUDiagnostics.h>
#include <GPUMappingBounds.h>
class QcomGPU;
class IOMemoryMap;
namespace Adreno {struct FirmwareHandoff;}
namespace Adreno {struct PageTableContext;}
class QcomGPUBuffer;
class QcomGPUAddressSpace;
class OSArray;
class QcomGPUMapper : public IOMapper {
    OSDeclareDefaultStructors(QcomGPUMapper);
    struct State;
    State *fState=nullptr;
    friend class QcomGPUAddressSpace;
    bool fAttached=false,fStarted=false;
    static void interruptAction(OSObject *,class IOInterruptEventSource *,int);
protected:
    bool initHardware(IOService *) APPLE_KEXT_OVERRIDE;
public:
    static QcomGPUMapper *create(QcomGPU *,IOService *,IOMemoryMap *);
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    bool shutdown();
    bool healthy() const;
    bool retainFirmware(const Adreno::FirmwareHandoff &);
    bool synchronizeForRecovery(); // CP/AXI stopped; configuration and pinned firmware unchanged
    bool privateAddressSpacesReady() const;
    uint64_t apertureBytes() const;
    GPU::Native::Memory::MappingUsage memoryUsage() const;
    GPU::Diagnostics::AddressSpaces::Snapshot addressSpaceStatistics() const;
    GPU::MappingBounds::Snapshot mappingWorkStatistics() const;
    QcomGPUAddressSpace *createAddressSpace(const QcomGPUBuffer *,const QcomGPUBuffer *,
        const QcomGPUBuffer *,const QcomGPUBuffer *,QcomGPUBuffer *const *,unsigned,OSArray *commands=nullptr,bool compositor=false,
        const GPU::MappingBounds::Plan *bounds=nullptr);
    bool restoreKernelContext(); // real ME/PFP halt and AXI drain required
    bool kernelContextSelected() const; // read-only check; permits a recorded non-stalled fault
    bool retainsMemory(IOMemoryDescriptor *) const;
    bool validateBufferMapping(const QcomGPUBuffer &,IOMemoryDescriptor *,IODMACommand *) const;
    uint64_t getPageSize() const APPLE_KEXT_OVERRIDE {return 4096;}
    IOReturn iovmMapMemory(IOMemoryDescriptor *,uint64_t,uint64_t,uint32_t,const IODMAMapSpecification *,
        IODMACommand *,const IODMAMapPageList *,uint64_t *,uint64_t *) APPLE_KEXT_OVERRIDE;
    IOReturn iovmUnmapMemory(IOMemoryDescriptor *,IODMACommand *,uint64_t,uint64_t) APPLE_KEXT_OVERRIDE;
    IOReturn iovmInsert(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t) APPLE_KEXT_OVERRIDE {return kIOReturnUnsupported;}
    uint64_t mapToPhysicalAddress(uint64_t) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
};
class QcomGPUAddressSpace : public OSObject {
    OSDeclareDefaultStructors(QcomGPUAddressSpace);
    friend class QcomGPUMapper;
    struct State;
    State *fState=nullptr;
    QcomGPUMapper *fMapper=nullptr;
public:
    Adreno::PageTableContext context() const;
    void markSubmitted();
    bool retire(bool completed=false);
    void free() APPLE_KEXT_OVERRIDE;
};
