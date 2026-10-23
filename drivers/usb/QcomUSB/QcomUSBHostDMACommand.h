// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/usb/AppleUSBHostDMACommand.h>

// Client descriptors are prepared for CPU copies only. The HCD must use its
// own IOMemoryAPI/DMARegion buffers for every hardware TRB. Keeping these two
// roles explicit avoids native Host copies, implicit IOMMU unmaps, and cache
// invalidation discarding data that the HCD has just copied into a client MD.
class QcomUSBHostDMACommand : public AppleUSBHostDMACommand {
    OSDeclareDefaultStructors(QcomUSBHostDMACommand);
public:
    static QcomUSBHostDMACommand *withClientSpecification(UInt64 maxSegment,UInt64 maxTransfer=0,void *refCon=nullptr);
    IODMACommand *cloneCommand(void *) APPLE_KEXT_OVERRIDE;
    bool initWithSpecification(SegmentFunction,UInt8,UInt64,MappingOptions,UInt64,UInt32,IOMapper *,void *) APPLE_KEXT_OVERRIDE;
    bool initWithSpecification(SegmentFunction,const SegmentOptions *,UInt32,IOMapper *,void *) APPLE_KEXT_OVERRIDE;
    IOReturn setMemoryDescriptor(const IOMemoryDescriptor *,bool) APPLE_KEXT_OVERRIDE;
    IOReturn clearMemoryDescriptor(bool) APPLE_KEXT_OVERRIDE;
    const IOMemoryDescriptor *getMemoryDescriptor() const APPLE_KEXT_OVERRIDE;
    IOReturn nativeSlot31(const IOMemoryDescriptor *,bool,UInt32) APPLE_KEXT_OVERRIDE;
    IOReturn prepare(UInt64=0,UInt64=0,bool=true,bool=true) APPLE_KEXT_OVERRIDE;
    IOReturn complete(bool=true,bool=true) APPLE_KEXT_OVERRIDE;
    IOReturn synchronize(IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn prepareWithSpecification(SegmentFunction,UInt8,UInt64,MappingOptions,UInt64,UInt32,IOMapper *,UInt64,UInt64,bool,bool) APPLE_KEXT_OVERRIDE;
    IOReturn prepareWithSpecification(SegmentFunction,const SegmentOptions *,UInt32,IOMapper *,UInt64,UInt64,bool,bool) APPLE_KEXT_OVERRIDE;
};
static_assert(sizeof(QcomUSBHostDMACommand)==144,"Host client DMA policy adds no instance fields");
static_assert(sizeof(IODMACommand::SegmentOptions)==40,"native DMA specification layout");
