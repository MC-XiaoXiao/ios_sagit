// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IODMACommand.h>

// Current iOS18 Host Family requires this subclass, including its extended
// descriptor setter. It can retain an original MD plus a separate copy MD.
class AppleUSBHostDMACommand : public IODMACommand {
    OSDeclareDefaultStructors(AppleUSBHostDMACommand);
    UInt8 nativeState[24];
public:
    void free() APPLE_KEXT_OVERRIDE;
    bool initWithSpecification(SegmentFunction,UInt8,UInt64,MappingOptions,UInt64,UInt32,IOMapper *,void *) APPLE_KEXT_OVERRIDE;
    IOReturn setMemoryDescriptor(const IOMemoryDescriptor *,bool) APPLE_KEXT_OVERRIDE;
    IOReturn clearMemoryDescriptor(bool) APPLE_KEXT_OVERRIDE;
    const IOMemoryDescriptor *getMemoryDescriptor() const APPLE_KEXT_OVERRIDE;
    IOReturn prepare(UInt64,UInt64,bool,bool) APPLE_KEXT_OVERRIDE;
    IOReturn complete(bool,bool) APPLE_KEXT_OVERRIDE;
    // Audited consumed arguments. Native callers can also pass an unused x4.
    virtual IOReturn nativeSlot31(const IOMemoryDescriptor *,bool,UInt32);
    virtual uintptr_t nativeSlot32(); // opaque forwarding entry; never called directly
};
static_assert(sizeof(AppleUSBHostDMACommand)==144,"iOS18 Host DMA instance ABI");
