// SPDX-License-Identifier: BSD-2-Clause
#include "QcomGPUBuffer.h"
#include "QcomGPUMapper.h"
#include "AdrenoSubmit.h"
#include "GPUSurfaceProtocol.h"
OSDefineMetaClassAndStructors(QcomGPUBuffer, OSObject);
QcomGPUBuffer *QcomGPUBuffer::allocate(uint64_t bytes, IOMapper *mapper)
{
    if (!bytes || bytes > GPU::MaxBufferBytes || (bytes & (GPU::PageBytes - 1))) return nullptr;
    auto *buffer = new QcomGPUBuffer;
    if (!buffer || !buffer->init()) { if (buffer) buffer->release(); return nullptr; }
    buffer->length = bytes;
    // Explicit mapper, no fallback to physical DMA. Normal Non-Cacheable RAM
    // permits byte/unaligned user copies; Device memory would alignment-fault.
    // The mapper owns SMMU table and TLB synchronization.
    auto *memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | (mapper ? kIOMapWriteCombineCache : kIOMapCopybackCache), bytes, GPU::PageBytes);
    buffer->fMemory = memory;
    buffer->fBytes = memory ? memory->getBytesNoCopy() : nullptr;
    if (!buffer->fMemory || !buffer->bytes()) { buffer->release(); return nullptr; }
    bzero(buffer->bytes(), bytes);
    if (mapper && !buffer->mapDMA(mapper)) { buffer->release(); return nullptr; }
    return buffer;
}
QcomGPUBuffer *QcomGPUBuffer::mapDescriptor(IOMemoryDescriptor *memory, IOMapper *mapper, bool shared)
{
    if (!memory || !mapper) return nullptr;
    auto bytes = memory->getLength();
    uint64_t dmaOffset=0;auto dmaBytes=memory->getDMAMapLength(&dmaOffset);
    if (!bytes || bytes > GPU::MaxBufferBytes ||
        (shared?!GPU::Surface::descriptorLayout(bytes,bytes,dmaBytes,dmaOffset,PAGE_SIZE):
            ((bytes & (GPU::PageBytes - 1)) || dmaBytes!=bytes || dmaOffset))) return nullptr;
    auto *buffer = new QcomGPUBuffer;
    if (!buffer || !buffer->init()) { if (buffer) buffer->release(); return nullptr; }
    buffer->fMemory = memory; memory->retain(); buffer->length = bytes; buffer->fShared = shared;
    if (!buffer->mapDMA(mapper)) { buffer->release(); return nullptr; }
    return buffer;
}
bool QcomGPUBuffer::mapDMA(IOMapper *mapper)
{
    if (!mapper || fMemory->prepare() != kIOReturnSuccess) return false;
    fPrepared = true;fMapper=mapper;mapper->retain();
    if(fShared) {
        uint64_t dmaOffset=0;auto dmaBytes=fMemory->getDMAMapLength(&dmaOffset);
        if(fMemory->getLength()!=length || !GPU::Surface::descriptorLayout(length,length,dmaBytes,dmaOffset,PAGE_SIZE))return false;
    }
    fDMA = IODMACommand::withSpecification(IODMACommand::OutputHost64, Adreno::AddressBits,
        length, IODMACommand::kMapped, length, GPU::PageBytes, mapper);
    if (!fDMA || fDMA->setMemoryDescriptor(fMemory,false) != kIOReturnSuccess ||
        fDMA->getIOMemoryDescriptor() != fMemory) return false;
    fDMAAttempted=true;
    if(fDMA->prepare()!=kIOReturnSuccess)return false;
    fDMAPrepared=true;
    UInt64 offset = 0; UInt32 count = 1; IODMACommand::Segment64 segment{};
    if (fDMA->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
        count != 1 || offset != length || segment.fLength != length || (segment.fIOVMAddr & 4095) ||
        !Adreno::deviceRange(segment.fIOVMAddr, length)) return false;
    address = segment.fIOVMAddr;
    if(!indexPages())return false;
    auto *gpuMapper=OSDynamicCast(QcomGPUMapper,mapper);
    return !gpuMapper || gpuMapper->validateBufferMapping(*this,fMemory,fDMA);
}
bool QcomGPUBuffer::indexPages()
{
    fPageCount = uint32_t(GPU::pageSpanBytes(length) / GPU::PageBytes);
    fPages = IONew(Page, fPageCount);
    if (!fPages) return false;
    for (uint32_t i=0;i<fPageCount;++i) {
        IOByteCount span=0; uint64_t offset=uint64_t(i)*GPU::PageBytes;
        uint64_t pa=fMemory->getPhysicalSegment(offset,&span,kIOMemoryMapperNone);
        const auto remaining=length-offset,required=remaining<GPU::PageBytes?remaining:GPU::PageBytes;
        if (!pa || (pa&(GPU::PageBytes-1)) || span<required) return false;
        fPages[i]={pa,offset};
    }
    // Sort the physical-page index once. Aliases through distinct IOSurface
    // descriptors are then compared in linear time, without trusting IDs/IOVAs.
    if(!GPU::sortBackingPages(fPages,fPageCount))return false;
    static_assert(GPU::MaxBufferBytes/GPU::PageBytes<=65536,"logical page index width");
    fLogicalPages=IONew(uint16_t,fPageCount);
    if(!fLogicalPages)return false;
    for(uint32_t i=0;i<fPageCount;++i)fLogicalPages[fPages[i].offset/GPU::PageBytes]=uint16_t(i);
    return true;
}
QcomGPUBuffer *QcomGPUBuffer::view(QcomGPUBuffer *parent,uint64_t offset,uint64_t bytes)
{
    if (!parent || parent->fParent || !parent->address || offset>=parent->length) return nullptr;
    if(!bytes)bytes=parent->length-offset;
    if(!GPU::range(parent->length,offset,bytes))return nullptr;
    auto *buffer=new QcomGPUBuffer;
    if (!buffer || !buffer->init()) { if (buffer) buffer->release(); return nullptr; }
    buffer->fParent=parent;parent->retain();buffer->fOffset=offset;
    buffer->address=parent->address+offset;buffer->length=bytes;
    return buffer;
}
bool QcomGPUBuffer::gpuWritable() const
{
    auto *memory=root()->fMemory;
    // Direction is relative to host memory: Out is read-only for the GPU.
    // Preserve the permission selected by the original task VM inspection.
    return memory && (memory->getDirection()&kIODirectionIn);
}
bool QcomGPUBuffer::synchronize() const
{
    auto *backing=root();
    // The SDM835 kernel uses noncoherent ARM I/O. Flush performs cache clean
    // and invalidation to PoC through the original, prepared descriptor. Never
    // create a second CPU alias with conflicting cache attributes.
    return !backing->fShared || (backing->fPrepared && backing->fDMAPrepared &&
        backing->fMemory->performOperation(kIOMemoryIncoherentIOFlush,0,backing->length)==kIOReturnSuccess);
}
bool QcomGPUBuffer::overlaps(const QcomGPUBuffer *other,uint64_t offset,uint64_t bytes,
    uint64_t otherOffset,uint64_t otherBytes) const
{
    if (!other || !GPU::range(length,offset,bytes) || !GPU::range(other->length,otherOffset,otherBytes)) return true;
    uint64_t begin=fOffset+offset,end=begin+bytes;
    uint64_t otherBegin=other->fOffset+otherOffset,otherEnd=otherBegin+otherBytes;
    auto *a=root();auto *b=other->root();
    if (a==b || (a->fMemory && a->fMemory==b->fMemory)) return begin<otherEnd && otherBegin<end;
    return GPU::backingOverlap(a->fPages,a->fPageCount,begin,end,b->fPages,b->fPageCount,otherBegin,otherEnd);
}
bool QcomGPUBuffer::overlapsRanges(const QcomGPUBuffer *other,GPU::StridedRange range,GPU::StridedRange otherRange) const
{
    if(!other || !GPU::validStridedRange(range,length) || !GPU::validStridedRange(otherRange,other->length))return true;
    range.begin+=fOffset;otherRange.begin+=other->fOffset;
    auto *a=root();auto *b=other->root();
    if(a==b || (a->fMemory && a->fMemory==b->fMemory))return GPU::stridedOverlap(range,otherRange);
    return GPU::backingOverlap(a->fPages,a->fPageCount,range,b->fPages,b->fPageCount,otherRange);
}
bool QcomGPUBuffer::copyBytes(uint64_t offset, void *destination, uint64_t bytes) const
{
    if (fParent) return GPU::range(length,offset,bytes) && fParent->copyBytes(fOffset+offset,destination,bytes);
    return fMemory && destination && bytes && offset <= length && bytes <= length-offset &&
        fMemory->readBytes(offset,destination,bytes) == bytes;
}
bool QcomGPUBuffer::physicalPage(unsigned index,uint64_t &physical) const
{
    physical=0;auto *buffer=root();
    if (!buffer->fMemory || !buffer->fPrepared || !buffer->fDMAPrepared ||
        !buffer->fPages || !buffer->fLogicalPages || index>=buffer->fPageCount) return false;
    // The original descriptor remains prepared and DMA-wired throughout
    // this index's lifetime. Reuse its verified physical pages instead of
    // walking VM for every page of every private job tree. mapDMA verifies
    // the entire index against the actual mapping before exposing the buffer.
    const auto &page=buffer->fPages[buffer->fLogicalPages[index]];
    if(page.offset!=uint64_t(index)*GPU::PageBytes)return false;
    physical=page.physical;
    return physical && !(physical&(GPU::PageBytes-1));
}
void QcomGPUBuffer::free()
{
    if (fParent) { OSSafeReleaseNULL(fParent); OSObject::free(); return; }
    if (fDMA) {
        auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
        if(fDMAAttempted && !fDMAPrepared && (!mapper || mapper->retainsMemory(fMemory))) {
            IOLog("QcomGPU: failed DMA preparation may remain mapped; buffer quarantined\n");return;
        }
        if(fDMAPrepared) {
            // Native complete consumes its active/map metadata even on error.
            // Never retry it or let clearMemoryDescriptor hide that error.
            auto status=fDMA->complete();fDMAPrepared=false;
            if(status!=kIOReturnSuccess) {
                IOLog("QcomGPU: explicit DMA completion failed; prepared buffer quarantined\n");return;
            }
            fDMAAttempted=false;
        }
        auto status=fDMA->clearMemoryDescriptor(false);
        if(status!=kIOReturnSuccess || (mapper && mapper->retainsMemory(fMemory))) {
            IOLog("QcomGPU: DMA unmap did not retire; prepared buffer quarantined\n");return;
        }
        OSSafeReleaseNULL(fDMA);
    }
    if (fMemory && fPrepared && fMemory->complete()!=kIOReturnSuccess) {
        IOLog("QcomGPU: memory completion failed; descriptor retained\n");return;
    }
    OSSafeReleaseNULL(fMapper);
    if (fPages) { IODelete(fPages,Page,fPageCount); fPages=nullptr; }
    if (fLogicalPages) { IODelete(fLogicalPages,uint16_t,fPageCount); fLogicalPages=nullptr; }
    OSSafeReleaseNULL(fMemory); OSObject::free();
}
