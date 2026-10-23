// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOMapper.h>
#include <QcomPowerHardware.h>
#include "../../bus/QcomSMMU/ContextHandoff.h"
#include "../../bus/QcomSMMU/BypassHandoff.h"

// Private per-device mapper owned by the platform resource lease. It never
// becomes the system mapper and never matches a USB Family personality.
class QcomUSBMapper : public IOMapper {
    OSDeclareDefaultStructors(QcomUSBMapper);
    struct State;
    State *fState=nullptr;
    bool fAttached=false,fStarted=false;
    static void interruptAction(OSObject *,class IOInterruptEventSource *,int);
protected:
    bool initHardware(IOService *) APPLE_KEXT_OVERRIDE;
public:
    struct Resources {
        IOService *owner=nullptr;
        class IOWorkLoop *loop=nullptr;
        QcomPower::Status (*accessible)(IOService *)=nullptr;
        QcomPower::Status (*isolated)(IOService *)=nullptr;
        void (*fault)(IOService *)=nullptr;
    };
    // Callbacks must not reenter the platform lock. Its resource lease outlives
    // this mapper, all callbacks and every quarantined descriptor/command.
    static bool configuration(IOService *,unsigned,SMMU::ContextHandoff &);
    // Metadata-only admission for deferred capture by the actual USB resource
    // owner. create performs the register snapshot after real NoC/BCR checks.
    static bool captureConfiguration(IOService *,unsigned);
    static bool bootConfiguration(IOService *);
    static bool bypassCaptureConfiguration(IOService *,unsigned,SMMU::BypassMode * = nullptr);
    static bool bypassConfiguration(IOService *,unsigned,SMMU::BypassHandoff &);
    static IOReturn captureBypass(const Resources &,IOService *,unsigned);
    static bool firmwarePhysicalConfiguration(IOService *,unsigned);
    static IOReturn captureFirmwarePhysical(const Resources &,IOService *,unsigned,SMMU::ContextHandoff &);
    // A failed create can return an owned object: retain it until shutdown
    // proves root removal. Never hide a partial hardware takeover in nullptr.
    static IOReturn create(const Resources &,IOService *,unsigned,QcomUSBMapper *&);
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    bool healthy() const;
    bool matchesHandoff(const SMMU::ContextHandoff &) const;
    bool isolated() const;
    bool quiesce(); // controller BCR asserted; detach root, retain all tokens
    bool activate(); // all tokens acknowledged; fresh private page tables
    bool park(); // detached, drained, private tables freed; resources may go off
    bool shutdown(); // root detached and every command acknowledged
    uint64_t getPageSize() const APPLE_KEXT_OVERRIDE {return 4096;}
    IOReturn iovmMapMemory(IOMemoryDescriptor *,uint64_t,uint64_t,uint32_t,const IODMAMapSpecification *,
        IODMACommand *,const IODMAMapPageList *,uint64_t *,uint64_t *) APPLE_KEXT_OVERRIDE;
    IOReturn iovmUnmapMemory(IOMemoryDescriptor *,IODMACommand *,uint64_t,uint64_t) APPLE_KEXT_OVERRIDE;
    IOReturn iovmInsert(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t) APPLE_KEXT_OVERRIDE {return kIOReturnUnsupported;}
    uint64_t mapToPhysicalAddress(uint64_t) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
};
