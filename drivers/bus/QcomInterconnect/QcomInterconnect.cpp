// SPDX-License-Identifier: GPL-2.0-only
#include "QcomInterconnect.h"
#include "InterconnectResources.h"
#include "../../usb/QcomUSB/USBInterconnect.h"
#include "../../usb/QcomUSB/USBMMIO.h"
#include <ClockControllerProtocol.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSData.h>
#include <machine/machine_routines.h>
OSDefineMetaClassAndStructors(QcomInterconnect,IOService);
namespace {
QcomPower::Status result(IOReturn s) {
    using namespace QcomPower;
    if (s==kIOReturnSuccess) return OK;
    if (s==kIOReturnBadArgument) return Invalid;
    if (s==kIOReturnUnsupported) return Unsupported;
    if (s==kIOReturnNotPermitted) return Denied;
    if (s==kIOReturnBusy) return Busy;
    if (s==kIOReturnNotReady || s==kIOReturnOffline || s==kIOReturnNotAttached) return NotReady;
    if (s==kIOReturnTimeout) return TimedOut;
    return Stale; // a synchronous mutation may already have reached hardware
}
IOReturn native(QcomPower::Status s) {
    using namespace QcomPower;
    switch (s) {
    case OK:return kIOReturnSuccess;
    case Invalid:return kIOReturnBadArgument;
    case Unsupported:return kIOReturnUnsupported;
    case Denied:return kIOReturnNotPermitted;
    case Busy:return kIOReturnBusy;
    case NotReady:return kIOReturnNotReady;
    case TimedOut:return kIOReturnTimeout;
    default:return kIOReturnIOError;
    }
}
}
struct QcomInterconnect::State {
    QcomInterconnect *self;
    IOService *provider, *bus, *openOwner=nullptr;
    IOLock *lock=nullptr;
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr;
    IOMemoryMap *map=nullptr;
    USB::MMIO *io=nullptr;
    const OSSymbol *clockFunction=nullptr;
    QcomNoC::USBPath<State> path{*this};
    UInt64 serial=0,lease=0;
    bool stopping=false,timerAdded=false;
    explicit State(QcomInterconnect *service,IOService *nub):self(service),provider(nub),bus(nub->getProvider()) {
        provider->retain(); if (bus) bus->retain(); lock=IOLockAlloc();
        clockFunction=OSSymbol::withCStringNoCopy(ClockController::Function);
    }
    bool scoped(IOService *client) const {
        auto *nub=client ? client->getProvider() : nullptr;
        return nub && nub->getProvider()==bus;
    }
    QcomPower::Status request(ClockController::Controller kind,ClockController::Operation op,UInt32 id,UInt64 &value,unsigned set=0) {
        auto r=ClockController::targeted(kind,op,id,value,ClockController::USBInterconnectConsumer,set);
        const auto status=bus->callPlatformFunction(clockFunction,false,&r.request,nullptr,nullptr,nullptr);
        const bool write=op==ClockController::SetRate || op==ClockController::SetBandwidth ||
            op==ClockController::RetainClock || op==ClockController::ReleaseClock;
        if (r.request.version!=2 || r.request.size!=sizeof(r) || !ClockController::valid(r.request) ||
            r.controller!=kind || r.consumer!=ClockController::USBInterconnectConsumer || r.flags!=set ||
            r.request.operation!=op || r.request.id!=id || (write && r.request.value!=value)) return QcomPower::Stale;
        value=r.request.value; return result(status);
    }
    QcomPower::Status limit(UInt32 id,bool clock,UInt64 &value) {
        return request(ClockController::RPM,clock ? ClockController::GetMaximumRate : ClockController::GetMaximumBandwidth,id,value);
    }
    QcomPower::Status get(UInt32 id,unsigned set,bool clock,UInt64 &value) {
        return request(ClockController::RPM,clock ? ClockController::GetRate : ClockController::GetBandwidth,id,value,set);
    }
    QcomPower::Status floor(UInt32 id,bool clock,UInt64 &value) {
        return request(ClockController::RPM,clock ? ClockController::GetMinimumRate : ClockController::GetMinimumBandwidth,id,value);
    }
    QcomPower::Status vote(UInt32 id,unsigned set,bool clock,UInt64 value) {
        return request(ClockController::RPM,clock ? ClockController::SetRate : ClockController::SetBandwidth,id,value,set);
    }
    QcomPower::Status clock(UInt32 id,bool enable) {
        UInt64 value=0;
        return request(ClockController::GCC,enable ? ClockController::RetainClock : ClockController::ReleaseClock,id,value);
    }
    QcomPower::Status isolated() {
        UInt64 value=0;
        const auto status=request(ClockController::GCC,ClockController::GetResetState,18,value);
        return status!=QcomPower::OK ? status : value==1 ? QcomPower::OK : value==0 ? QcomPower::Busy : QcomPower::Stale;
    }
    UInt32 read(UInt32 offset) { return io ? io->read(offset) : UINT32_MAX; }
    void write(UInt32 offset,UInt32 value) { if (io) io->write(offset,value); }
    bool supported() {
        UInt64 caps=0;
        const auto s=request(ClockController::RPM,ClockController::GetCapabilities,0,caps);
        self->setProperty("InterconnectRPMStatus",unsigned(s),32);
        self->setProperty("InterconnectRPMCapabilities",caps,64);
        if (s!=QcomPower::OK || !(caps&ClockController::Interconnect)) return false;
        const auto status=path.supported();
        self->setProperty("InterconnectSupportStatus",unsigned(status),32);
        return status==QcomPower::OK;
    }
};
bool QcomInterconnect::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fStarted=true; fState=new State(this,provider);
    if (!fState || !fState->lock || !fState->bus || !fState->clockFunction || !provider->open(this)) goto fail;
    fProviderOpen=true;
    {
        auto &s=*fState;
        auto *owned=OSDynamicCast(OSData,provider->getProperty("qcom,owned-qos-ports"));
        auto *memory=provider->getDeviceMemoryWithIndex(0);
        if (!owned || owned->getLength()!=4 || QcomPower::le32(owned->getBytesNoCopy())!=2 ||
            !memory || memory->getPhysicalAddress()!=0x01660000 || memory->getLength()!=0x60000 ||
            provider->getDeviceMemoryCount()!=1 || provider->getResources()!=kIOReturnSuccess) goto fail;
        s.map=provider->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
        if (!s.map || !s.map->getVirtualAddress() || s.map->getLength()!=0x60000) goto fail;
        s.io=new USB::MMIO(reinterpret_cast<void *>(s.map->getVirtualAddress()),s.map->getLength());
        s.loop=IOWorkLoop::workLoop(); s.timer=IOTimerEventSource::timerEventSource(this,poll);
        if (!s.io || !s.loop || !s.timer || s.loop->addEventSource(s.timer)!=kIOReturnSuccess) goto fail;
        s.timerAdded=true;
        setProperty("SystemSuspendSupported",false);
        setProperty("InterconnectPath","USB3-A1NoC-SNOC-BIMC-EBI");
        setProperty("BandwidthObservation","acknowledged-request-not-measurement");
        registerService(); s.timer->setTimeoutMS(1); return true;
    }
fail:stop(provider); return false;
}
void QcomInterconnect::poll(OSObject *owner,IOTimerEventSource *timer) {
    auto *self=static_cast<QcomInterconnect *>(owner); auto &s=*self->fState;
    IOLockLock(s.lock);
    if (!s.stopping && !self->fPublished) {
        if (s.supported()) { self->publishResource(USBInterconnect::Resource,self); self->fPublished=true; }
        else timer->setTimeoutMS(1000);
    }
    IOLockUnlock(s.lock);
}
void QcomInterconnect::pin() {
    if (!fPinned) { retain(); fPinned=true; }
    setProperty("InterconnectFault",true);
}
IOReturn QcomInterconnect::callPlatformFunction(const OSSymbol *name,bool wait,void *a,void *b,void *c,void *d) {
    if (!name || !name->isEqualTo(USBInterconnect::Function)) return IOService::callPlatformFunction(name,wait,a,b,c,d);
    auto *r=static_cast<USBInterconnect::Request *>(a); auto *owner=static_cast<IOService *>(b);
    if (!r || !owner || c || d || !USBInterconnect::valid(*r) || !fState || ml_at_interrupt_context()) return kIOReturnBadArgument;
    auto &s=*fState; IOLockLock(s.lock);
    QcomPower::Status status=QcomPower::NotReady; bool unpin=false;
    if (!s.scoped(owner)) { status=QcomPower::Denied; goto out; }
    if ((s.stopping || fPinned) && r->action!=USBInterconnect::Action::Release) goto out;
    if (r->action==USBInterconnect::Action::Query) {
        status=s.supported() ? QcomPower::OK : QcomPower::NotReady;
        if (status==QcomPower::OK) r->flags=USBInterconnect::Supported;
        goto out;
    }
    // Track the open owner under this lock. Calling isOpen() here would
    // invert the IOService arbitration-lock -> handleOpen() lock order.
    if (owner!=s.openOwner) { status=QcomPower::Denied; goto out; }
    if (r->action==USBInterconnect::Action::Acquire) {
        if (s.lease || !s.path.empty()) { status=QcomPower::Busy; goto out; }
        if (s.serial==UINT64_MAX) { status=QcomPower::Failed; goto out; }
        s.lease=++s.serial; r->lease=s.lease;
        status=s.path.acquire(r->averageBytesPerSecond,r->peakBytesPerSecond);
        if (s.path.faulted()) pin();
    } else {
        if (!s.lease || r->lease!=s.lease) { status=QcomPower::Invalid; goto out; }
        if (r->action==USBInterconnect::Action::Release) {
            status=s.path.release();
            if (status==QcomPower::OK) {
                s.lease=0;
                if (fPinned) { fPinned=false; unpin=true; setProperty("InterconnectFault",false); }
            } else if (s.path.faulted()) pin();
        } else status=s.path.check();
    }
    if (status==QcomPower::OK) r->flags=USBInterconnect::Supported |
        (r->action==USBInterconnect::Action::Release ? 0u : USBInterconnect::Granted);
out:
    IOLockUnlock(s.lock);
    if (unpin) release();
    return native(status);
}
bool QcomInterconnect::handleOpen(IOService *client,IOOptionBits,void *) {
    if (!fState || !fState->lock) return false;
    auto &s=*fState; IOLockLock(s.lock);
    const bool allowed=!s.stopping && !fPinned && s.scoped(client) && (!s.openOwner || s.openOwner==client);
    if (allowed && !s.openOwner) { s.openOwner=client; client->retain(); }
    IOLockUnlock(s.lock); return allowed;
}
void QcomInterconnect::handleClose(IOService *client,IOOptionBits) {
    if (!fState || !fState->lock) return;
    auto &s=*fState; IOLockLock(s.lock); IOService *released=nullptr;
    if (client==s.openOwner) {
        if (s.lease || !s.path.empty()) pin();
        else { released=s.openOwner; s.openOwner=nullptr; }
    }
    IOLockUnlock(s.lock); if (released) released->release();
}
bool QcomInterconnect::handleIsOpen(const IOService *client) const {
    if (!fState || !fState->lock) return false;
    auto &s=*fState; IOLockLock(s.lock); const bool open=s.openOwner && (!client || client==s.openOwner);
    IOLockUnlock(s.lock); return open;
}
IOWorkLoop *QcomInterconnect::getWorkLoop() const { return fState ? fState->loop : nullptr; }
bool QcomInterconnect::requestTerminate(IOService *,IOOptionBits) {
    if (!fState || !fState->lock) return !fPinned;
    auto &s=*fState; IOLockLock(s.lock); const bool allowed=!s.openOwner && !s.lease && s.path.empty() && !fPinned;
    IOLockUnlock(s.lock); return allowed;
}
void QcomInterconnect::stop(IOService *provider) {
    if (fState && fState->lock) {
        auto &s=*fState; IOLockLock(s.lock); s.stopping=true;
        if (s.openOwner || s.lease || !s.path.empty()) { pin(); IOLockUnlock(s.lock); return; }
        IOLockUnlock(s.lock);
        if (s.timer) s.timer->cancelTimeout();
        if (fPublished) {
            auto *resources=getResourceService();
            if (resources->getProperty(USBInterconnect::Resource)==this) resources->removeProperty(USBInterconnect::Resource);
            fPublished=false;
        }
        if (s.timerAdded) { s.loop->removeEventSource(s.timer); s.timerAdded=false; }
    }
    if (fProviderOpen) { provider->close(this); fProviderOpen=false; }
    if (fStarted) { IOService::stop(provider); fStarted=false; }
}
void QcomInterconnect::free() {
    if (fState) {
        stop(fState->provider); if (fPinned) return;
        auto *s=fState; fState=nullptr;
        delete s->io; OSSafeReleaseNULL(s->map); OSSafeReleaseNULL(s->timer); OSSafeReleaseNULL(s->loop);
        OSSafeReleaseNULL(s->clockFunction); OSSafeReleaseNULL(s->provider); OSSafeReleaseNULL(s->bus);
        if (s->lock) IOLockFree(s->lock);
        delete s;
    }
    IOService::free();
}
