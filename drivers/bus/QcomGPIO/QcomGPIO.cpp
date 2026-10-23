// SPDX-License-Identifier: GPL-2.0-only
#include "QcomGPIO.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSAtomic.h>
#define super IOInterruptController
OSDefineMetaClassAndStructors(QcomGPIO, super);

uint32_t QcomGPIO::Registers::read32(uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(base+offset);
}
void QcomGPIO::Registers::write32(uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base+offset) = value; OSSynchronizeIO();
}
bool QcomGPIO::start(IOService *provider) {
    if (!super::start(provider)) return false;
    auto *count = OSDynamicCast(OSData, provider->getProperty("#interrupt-cells"));
    auto *handle = OSDynamicCast(OSData, provider->getProperty("AAPL,phandle"));
    if (!count || count->getLength()!=4 || QcomTLMM::le32(count->getBytesNoCopy())!=2 ||
        !handle || handle->getLength()!=4 || provider->getDeviceMemoryCount()!=1) goto fail;
    fMap = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    if (!fMap || fMap->getPhysicalAddress()!=0x03400000 || fMap->getLength()<QcomTLMM::RegionSize) goto fail;
    fRegisters.base = reinterpret_cast<volatile uint8_t *>(fMap->getVirtualAddress());
    if (!fRegisters.base) goto fail;
    if (provider->getProperty("qcom,owned-gpios")) {
        auto *owned = OSDynamicCast(OSData, provider->getProperty("qcom,owned-gpios"));
        if (!owned) goto fail;
        if (owned->getLength()%4 || owned->getLength()>4*QcomTLMM::PinCount) goto fail;
        const auto *p = static_cast<const uint8_t *>(owned->getBytesNoCopy());
        for (unsigned i=0; i<owned->getLength(); i+=4) {
            unsigned pin = QcomTLMM::le32(p+i);
            if (pin>=QcomTLMM::PinCount || fOwned[pin]) goto fail;
            fOwned[pin] = true;
        }
    }
    if (auto *mux=OSDynamicCast(OSData,provider->getProperty("qcom,owned-pin-functions"))) {
        if (mux->getLength()%8) goto fail;
        const auto *p=static_cast<const uint8_t *>(mux->getBytesNoCopy());
        for (unsigned off=0;off<mux->getLength();off+=8) {
            const auto pin=QcomTLMM::le32(p+off), mask=QcomTLMM::le32(p+off+4);
            if (pin>=QcomTLMM::PinCount || !fOwned[pin] || fMuxAllowed[pin] || !mask || (mask&~0xffffU)) goto fail;
            fMuxAllowed[pin]=mask;
        }
    } else if (provider->getProperty("qcom,owned-pin-functions")) goto fail;
    fName = IODTInterruptControllerName(provider);
    vectors = IONewZero(IOInterruptVector, QcomTLMM::PinCount);
    controllerLock = IOSimpleLockAlloc();
    if (!fName || !vectors || !controllerLock) goto fail;
    for (unsigned i=0; i<QcomTLMM::PinCount; ++i) {
        vectors[i].interruptLock = IOLockAlloc();
        if (!vectors[i].interruptLock) goto fail;
    }
    fParent = provider;
    if (provider->registerInterrupt(0, this, parentInterrupt, nullptr)!=kIOReturnSuccess) goto fail;
    fParentRegistered = true;
    {
        QcomTLMM::Controller<Registers> hw(fRegisters);
        for (unsigned pin=0;pin<QcomTLMM::PinCount;++pin) if (fOwned[pin]) {
            hw.mask(pin,QcomTLMM::Low); hw.ack(pin);
        }
        // The first registered client enables the parent. An owned pin alone
        // does not imply that its driver is ready to service a boot-time IRQ.
    }
    // Controller registration retains this resident interrupt domain.
    getPlatform()->registerInterruptController(const_cast<OSSymbol *>(fName), this);
    fPublished = true;
    setProperty("GPIOPinCount", QcomTLMM::PinCount, 32);
    setProperty("GPIODeepSleepWakeSupported", false);
    IOLog("QcomGPIO: 150-pin TLMM interrupt controller ready\n");
    registerService(); return true;
fail:
    IOLog("QcomGPIO: controller initialization failed\n");
    cleanup(); super::stop(provider); return false;
}
void QcomGPIO::cleanup() {
    if (fParentRegistered) {
        fParent->disableInterrupt(0); fParent->unregisterInterrupt(0); fParentRegistered=false;
    }
    if (vectors) {
        for (unsigned i=0; i<QcomTLMM::PinCount; ++i)
            if (vectors[i].interruptLock) IOLockFree(vectors[i].interruptLock);
        IODelete(vectors, IOInterruptVector, QcomTLMM::PinCount); vectors=nullptr;
    }
    if (controllerLock) { IOSimpleLockFree(controllerLock); controllerLock=nullptr; }
    OSSafeReleaseNULL(fName); OSSafeReleaseNULL(fMap); fRegisters.base=nullptr;
}
void QcomGPIO::free() { cleanup(); super::free(); }
bool QcomGPIO::requestTerminate(IOService *, IOOptionBits) { return !fPublished; }
bool QcomGPIO::decode(IOService *nub, int source, uint32_t &pin, uint32_t &trigger) const {
    if (!nub || source<0 || source>=nub->_numInterruptSources || !nub->_interruptSources) return false;
    auto *data = nub->_interruptSources[source].vectorData;
    if (!data || data->getLength()!=8) return false;
    const auto *p=static_cast<const uint8_t *>(data->getBytesNoCopy());
    pin=QcomTLMM::le32(p); trigger=QcomTLMM::le32(p+4);
    return pin<QcomTLMM::PinCount && fOwned[pin] && QcomTLMM::validTrigger(trigger);
}
bool QcomGPIO::owns(IOService *nub, int source, uint32_t &pin) const {
    uint32_t trigger;
    return decode(nub,source,pin,trigger) && vectors[pin].interruptRegistered &&
        vectors[pin].nub==nub && vectors[pin].source==source;
}
IOReturn QcomGPIO::registerInterrupt(IOService *nub, int source, void *target, IOInterruptHandler handler, void *ref) {
    uint32_t pin,trigger;
    if (!handler || !decode(nub,source,pin,trigger)) return kIOReturnBadArgument;
    auto &v=vectors[pin]; IOLockLock(v.interruptLock);
    if (v.interruptRegistered) { IOLockUnlock(v.interruptLock); return kIOReturnNoResources; }
    auto state=IOSimpleLockLockDisableInterrupt(controllerLock);
    if (fMuxLeases.busy(pin)) {
        IOSimpleLockUnlockEnableInterrupt(controllerLock,state); IOLockUnlock(v.interruptLock);
        return kIOReturnBusy;
    }
    QcomTLMM::Controller<Registers> hw(fRegisters);
    // Do not turn a peripheral mux or an output into an interrupt input.
    uint32_t cfg=fRegisters.read32(QcomTLMM::PinBase[pin]);
    if (cfg & ((15U<<2)|(1U<<9))) {
        IOSimpleLockUnlockEnableInterrupt(controllerLock,state); IOLockUnlock(v.interruptLock);
        return kIOReturnNotReady;
    }
    hw.configureIRQ(pin,trigger); fTrigger[pin]=trigger;
    v.nub=nub; v.source=source; v.target=target; v.handler=handler; v.refCon=ref;
    v.interruptDisabledSoft=v.interruptDisabledHard=1;
    OSMemoryBarrier(); v.interruptRegistered=1;
    IOSimpleLockUnlockEnableInterrupt(controllerLock,state); IOLockUnlock(v.interruptLock);
    return kIOReturnSuccess;
}
IOReturn QcomGPIO::unregisterInterrupt(IOService *nub,int source) {
    uint32_t pin; return owns(nub,source,pin) ? super::unregisterInterrupt(nub,source) : kIOReturnBadArgument;
}
IOReturn QcomGPIO::enableInterrupt(IOService *nub,int source) {
    if (fFault) return kIOReturnOffline;
    uint32_t pin; return owns(nub,source,pin) ? super::enableInterrupt(nub,source) : kIOReturnBadArgument;
}
IOReturn QcomGPIO::disableInterrupt(IOService *nub,int source) {
    uint32_t pin;if(!owns(nub,source,pin))return kIOReturnBadArgument;
    auto status=super::disableInterrupt(nub,source);
    if(status!=kIOReturnSuccess)return status;
    // IOInterruptController only sets the software-disabled flag. A deferred
    // secure worker must also mask the pad NOW, before its query/rearm can
    // raise another edge; waiting for that edge leaves a pending GIC delivery
    // racing the client's acknowledge/unmask. Preserve RAW_STATUS for edges.
    auto irq=IOSimpleLockLockDisableInterrupt(controllerLock);
    if(vectors[pin].interruptDisabledSoft) {
        QcomTLMM::Controller<Registers>(fRegisters).mask(pin,fTrigger[pin]);
        vectors[pin].interruptDisabledHard=1;
    }
    IOSimpleLockUnlockEnableInterrupt(controllerLock,irq);return kIOReturnSuccess;
}
IOReturn QcomGPIO::causeInterrupt(IOService *,int) { return kIOReturnUnsupported; }
IOReturn QcomGPIO::getInterruptType(IOService *nub,int source,int *type) {
    uint32_t pin,trigger;
    if (!type || !decode(nub,source,pin,trigger)) return kIOReturnBadArgument;
    *type=QcomTLMM::edge(trigger) ? kIOInterruptTypeEdge : kIOInterruptTypeLevel; return kIOReturnSuccess;
}
bool QcomGPIO::vectorCanBeShared(IOInterruptVectorNumber,IOInterruptVector *) { return false; }
void QcomGPIO::disableVectorHard(IOInterruptVectorNumber pin,IOInterruptVector *) {
    auto state=IOSimpleLockLockDisableInterrupt(controllerLock);
    QcomTLMM::Controller<Registers>(fRegisters).mask(pin,fTrigger[pin]);
    IOSimpleLockUnlockEnableInterrupt(controllerLock,state);
}
void QcomGPIO::enableVector(IOInterruptVectorNumber pin,IOInterruptVector *) {
    auto state=IOSimpleLockLockDisableInterrupt(controllerLock);
    QcomTLMM::Controller<Registers>(fRegisters).unmask(pin);
    IOSimpleLockUnlockEnableInterrupt(controllerLock,state);
    if (fParent->enableInterrupt(0)!=kIOReturnSuccess) fFault=true;
}
IOInterruptAction QcomGPIO::getInterruptHandlerAddress() {
    return OSMemberFunctionCast(IOInterruptAction,this,&QcomGPIO::handleInterrupt);
}
void QcomGPIO::parentInterrupt(OSObject *target,void *,IOService *,int) {
    static_cast<QcomGPIO *>(target)->handleInterrupt(nullptr,nullptr,0);
}
IOReturn QcomGPIO::handleInterrupt(void *,IOService *,int) {
    bool handled=false;
    QcomTLMM::Controller<Registers> hw(fRegisters);
    for (unsigned pin=0; pin<QcomTLMM::PinCount; ++pin) {
        if (!fOwned[pin]) continue;
        auto &v=vectors[pin]; v.interruptActive=1; OSMemoryBarrier();
        if (!v.interruptRegistered || !hw.routedPending(pin)) { v.interruptActive=0; continue; }
        handled=true;
        if (v.interruptDisabledSoft) { v.interruptDisabledHard=1; disableVectorHard(pin,&v); }
        else {
            // Ack edges before invoking clients; a new edge must stay latched.
            if (QcomTLMM::edge(fTrigger[pin])) hw.ack(pin);
            else disableVectorHard(pin,&v);
            v.handler(v.target,v.refCon,v.nub,v.source);
            if (!QcomTLMM::edge(fTrigger[pin])) {
                hw.ack(pin);
                if (!v.interruptDisabledSoft) enableVector(pin,&v);
                else v.interruptDisabledHard=1;
            }
        }
        OSMemoryBarrier(); v.interruptActive=0;
    }
    bool foreign=false;
    if (!handled) for (unsigned pin=0;pin<QcomTLMM::PinCount;++pin)
        if ((!fOwned[pin] || !vectors[pin].interruptRegistered) && hw.routedPending(pin)) { foreign=true; break; }
    if (fSummary.fault(handled,foreign)) {
        // Reject a real unserved KPSS source immediately. Tolerate a late GIC
        // delivery after a pad was masked/acked, but bound an empty IRQ storm.
        // Never mistake a masked edge's retained RAW_STATUS for active input.
        fFault=true; fParent->disableInterrupt(0);
        IOLog("QcomGPIO: foreign source or repeated empty summary IRQ; domain disabled\n");
    }
    return kIOReturnSuccess;
}
IOReturn QcomGPIO::callPlatformFunction(const OSSymbol *name,bool wait,void *p1,void *p2,void *p3,void *p4) {
    if (!name) return kIOReturnBadArgument;
    if (name->isEqualTo("acknowledgeGPIOInterrupt")) {
        // A peripheral can discard its own latched edge after draining it.
        // Never clear another client's source or a live/unmasked interrupt.
        const auto source=reinterpret_cast<uintptr_t>(p2);
        uint32_t pin;
        if (p3 || p4 || source>INT_MAX || !owns(static_cast<IOService *>(p1),int(source),pin)) return kIOReturnBadArgument;
        const auto irq=IOSimpleLockLockDisableInterrupt(controllerLock);
        const bool masked=vectors[pin].interruptDisabledSoft && vectors[pin].interruptDisabledHard &&
            !(fRegisters.read32(QcomTLMM::PinBase[pin]+8)&1);
        if (masked) QcomTLMM::Controller<Registers>(fRegisters).ack(pin);
        IOSimpleLockUnlockEnableInterrupt(controllerLock,irq);
        return masked ? kIOReturnSuccess : kIOReturnBusy;
    }
    if (name->isEqualTo(Pinmux::Function)) {
        if (!p1 || p2 || p3 || p4) return kIOReturnBadArgument;
        return pinmux(*static_cast<Pinmux::Request *>(p1));
    }
    bool read=name->isEqualTo("getGPIOData"), set=name->isEqualTo("setGPIOData"), config=name->isEqualTo("setGPIOCFG");
    if (!read && !set && !config) return super::callPlatformFunction(name,wait,p1,p2,p3,p4);
    uintptr_t pin=reinterpret_cast<uintptr_t>(p1), value=reinterpret_cast<uintptr_t>(p2);
    if (pin>=QcomTLMM::PinCount || p3 || p4 || (!read && !fOwned[pin])) return kIOReturnBadArgument;
    if ((read && !p2) || (set && value>1) || (config && (value & ~uintptr_t(0x7c3)))) return kIOReturnBadArgument;
    auto state=IOSimpleLockLockDisableInterrupt(controllerLock);
    QcomTLMM::Controller<Registers> hw(fRegisters); IOReturn result=kIOReturnSuccess;
    if (read) { bool v=false; hw.read(pin,v); *static_cast<UInt32 *>(p2)=v; }
    else if (vectors[pin].interruptRegistered || fMuxLeases.busy(pin)) result=kIOReturnBusy;
    else if (set) {
        uint32_t cfg=fRegisters.read32(QcomTLMM::PinBase[pin]);
        if ((cfg & (15U<<2)) || !(cfg & (1U<<9))) result=kIOReturnNotReady;
        else hw.set(pin,value);
    } else hw.configure(pin,value&3,(value>>6)&7,value&(1U<<9),value&(1U<<10));
    IOSimpleLockUnlockEnableInterrupt(controllerLock,state); return result;
}
IOReturn QcomGPIO::pinmux(Pinmux::Request &r) {
    if (!Pinmux::valid(r) || !controllerLock) return kIOReturnBadArgument;
    const auto irq=IOSimpleLockLockDisableInterrupt(controllerLock);
    const auto previous=r.token;
    const auto result=fMuxLeases.apply(r,fRegisters,[&](unsigned pin,unsigned function) {
        if(!fOwned[pin] || !(fMuxAllowed[pin]&(1U<<function)))return QcomTLMM::MuxResult::Denied;
        return vectors[pin].interruptRegistered?QcomTLMM::MuxResult::Busy:QcomTLMM::MuxResult::Success;
    });
    // An uncertain MMIO write still owns a token, hence still retains its owner.
    if(!previous && r.token)r.owner->retain();
    IOSimpleLockUnlockEnableInterrupt(controllerLock,irq);
    if(previous && !r.token)r.owner->release();
    switch(result) {
    case QcomTLMM::MuxResult::Success:return kIOReturnSuccess;
    case QcomTLMM::MuxResult::Invalid:return kIOReturnBadArgument;
    case QcomTLMM::MuxResult::Denied:return kIOReturnNotPermitted;
    case QcomTLMM::MuxResult::Busy:return kIOReturnBusy;
    case QcomTLMM::MuxResult::Exhausted:return kIOReturnNoResources;
    case QcomTLMM::MuxResult::NotReady:return kIOReturnNotReady;
    default:return kIOReturnIOError;
    }
}
