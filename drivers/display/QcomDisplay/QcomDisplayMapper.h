// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOMapper.h>
#include "DisplayProtocol.h"
class QcomDisplay;
class QcomDisplayMapper : public IOMapper {
    OSDeclareDefaultStructors(QcomDisplayMapper);
    struct State;
    State *fState=nullptr;
    bool fAttached=false,fStarted=false;
    static void interruptAction(OSObject *,class IOInterruptEventSource *,int);
protected:
    bool initHardware(IOService *) APPLE_KEXT_OVERRIDE;
public:
    static QcomDisplayMapper *create(QcomDisplay *,IOService *,const Display::Mode &,uint64_t);
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    bool shutdown();
    uint64_t getPageSize() const APPLE_KEXT_OVERRIDE {return 4096;}
    IOReturn iovmMapMemory(IOMemoryDescriptor *,uint64_t,uint64_t,uint32_t,const IODMAMapSpecification *,
        IODMACommand *,const IODMAMapPageList *,uint64_t *,uint64_t *) APPLE_KEXT_OVERRIDE;
    IOReturn iovmUnmapMemory(IOMemoryDescriptor *,IODMACommand *,uint64_t,uint64_t) APPLE_KEXT_OVERRIDE;
    IOReturn iovmInsert(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t) APPLE_KEXT_OVERRIDE {return kIOReturnUnsupported;}
    uint64_t mapToPhysicalAddress(uint64_t) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
};
