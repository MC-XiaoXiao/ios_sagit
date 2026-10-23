// SPDX-License-Identifier: GPL-2.0-only
#include "QcomPMIC.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <machine/machine_routines.h>
#define super IOInterruptController
OSDefineMetaClassAndStructors(QcomPMIC, super);
namespace {
uint32_t cell(const uint8_t *p) { return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
bool same(const QcomPMICInput::IRQ &a,const QcomPMICInput::IRQ &b) {
    return a.sid==b.sid && a.peripheral==b.peripheral && a.bit==b.bit;
}
}
bool QcomPMIC::Bus::read(unsigned sid,unsigned address,uint8_t *data,unsigned count) {
    error=spmi->transfer(sid,address,data,count,false); return error==kIOReturnSuccess;
}
bool QcomPMIC::Bus::write(unsigned sid,unsigned address,const uint8_t *data,unsigned count) {
    if (count>8) { error=kIOReturnBadArgument; return false; }
    uint8_t copy[8]; bcopy(data,copy,count);
    error=spmi->transfer(sid,address,copy,count,true); return error==kIOReturnSuccess;
}
bool QcomPMIC::start(IOService *provider) {
    if (!super::start(provider)) return false;
    fSPMI=OSDynamicCast(QcomSPMI,provider);
    if (!fSPMI) goto fail;
    fBus.spmi=fSPMI;
    {
        auto *node=fSPMI->getProvider();
        auto *data=OSDynamicCast(OSData,node->getProperty("qcom,owned-irqs"));
        auto *count=OSDynamicCast(OSData,node->getProperty("#interrupt-cells"));
        auto *handle=OSDynamicCast(OSData,node->getProperty("AAPL,phandle"));
        if (!data || data->getLength()%16 || !data->getLength() ||
            data->getLength()>16*QcomPMICInput::MaxIRQs || !count || count->getLength()!=4 ||
            cell(static_cast<const uint8_t *>(count->getBytesNoCopy()))!=4 || !handle || handle->getLength()!=4) goto fail;
        fCount=data->getLength()/16;
        const auto *p=static_cast<const uint8_t *>(data->getBytesNoCopy());
        for (unsigned i=0;i<fCount;++i,p+=16) {
            auto &r=fEntries[i]; r.irq={cell(p),cell(p+4),cell(p+8),cell(p+12)};
            if (!QcomPMICInput::valid(r.irq) || !fSPMI->irqChannel(r.irq.sid,r.irq.peripheral,r.apid)) goto fail;
            for (unsigned j=0;j<i;++j) if (same(r.irq,fEntries[j].irq)) goto fail;
        }
        if (auto *buttons=OSDynamicCast(OSData,node->getProperty("qcom,button-map"))) {
            if (buttons->getLength()!=sizeof(fButtons)) goto fail;
            p=static_cast<const uint8_t *>(buttons->getBytesNoCopy());
            fButtonCount=3;
            for (unsigned i=0;i<3;++i,p+=20) {
                auto &b=fButtons[i]; b={cell(p),cell(p+4),cell(p+8),cell(p+12),cell(p+16)};
                if (!QcomPMICInput::valid(b)) goto fail;
                bool found=false; auto irq=QcomPMICInput::interrupt(b);
                for (unsigned j=0;j<fCount;++j) found|=same(irq,fEntries[j].irq) && fEntries[j].irq.trigger==3;
                if (!found) goto fail;
                for (unsigned j=0;j<i;++j) if (b.usage==fButtons[j].usage || same(irq,QcomPMICInput::interrupt(fButtons[j]))) goto fail;
            }
        }
        if (auto *usb=OSDynamicCast(OSData,node->getProperty("qcom,usb-connector"))) {
            if (usb->getLength()!=12 && usb->getLength()!=16) goto fail;
            p=static_cast<const uint8_t *>(usb->getBytesNoCopy());
            fUSBConfig={cell(p),cell(p+4),cell(p+8)};
            if (usb->getLength()==16) fUSBConfig.vconn=cell(p+12);
            if (!fUSBConfig.valid()) goto fail;
            const unsigned peripherals[6]={0x13,0x13,0x13,0x11,0x11,0x11};
            const unsigned bits[6]={7,4,3,0,1,2};
            for (unsigned k=0;k<6;++k) {
                bool found=false;
                for (unsigned j=0;j<fCount;++j) {
                    const auto &irq=fEntries[j].irq;
                    found|=irq.sid==fUSBConfig.sid && irq.peripheral==peripherals[k] &&
                        irq.bit==bits[k] && (irq.trigger&3);
                }
                if (!found) goto fail;
            }
        } else if (node->getProperty("qcom,usb-connector")) goto fail;
        fName=IODTInterruptControllerName(node);
    }
    vectors=IONewZero(IOInterruptVector,fCount);
    fLoop=fSPMI->getWorkLoop(); if (fLoop) fLoop->retain();
    fGate=IOCommandGate::commandGate(this,dispatch);
    if (!fName || !vectors || !fLoop || !fGate || fLoop->addEventSource(fGate)!=kIOReturnSuccess) goto fail;
    fGateAdded=true;
    fInterrupt=IOInterruptEventSource::interruptEventSource(this,interruptAction,fSPMI->getProvider(),0);
    if (!fInterrupt || fLoop->addEventSource(fInterrupt)!=kIOReturnSuccess) goto fail;
    fIRQAdded=true;
    fInterrupt->disable();
    // No SPMI transactions run in hard interrupt context. The level parent is
    // masked by IOInterruptEventSource until this shared workloop drains it.
    if (!publishButtonSources()) goto fail;
    {
        IOPMPowerState states[2] = {};
        states[0].version=states[1].version=1;
        // Keep SPMI usable to program masks, but only the active state exports
        // child power. A live connector child must hold this interrupt domain
        // active rather than having its IRQs silently parked underneath it.
        states[0].inputPowerRequirement=kIOPMPowerOn;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        states[1].outputPowerCharacter=states[1].inputPowerRequirement=kIOPMPowerOn;
        PMinit(); fPM=true; provider->joinPMtree(this);
        if (registerPowerDriver(this,states,2)!=kIOReturnSuccess) goto fail;
        changePowerStateTo(1);
    }
    getPlatform()->registerInterruptController(const_cast<OSSymbol *>(fName),this);
    fPublished=true;
    setProperty("PMICButtonCount",fButtonCount,32);
    setProperty("PMICUSBConnectorAvailable",fUSBConfig.valid());
    setProperty("PMICDeepSleepWakeSupported",false);
    IOLog("QcomPMIC: interrupt domain ready (%u sources, %u buttons)\n",fCount,fButtonCount);
    registerService(); return true;
fail:
    IOLog("QcomPMIC: interrupt domain initialization failed\n");
    cleanup(); super::stop(provider); return false;
}
bool QcomPMIC::publishButtonSources() {
    if (!fButtonCount) return true;
    auto *controllers=OSArray::withCapacity(3), *specifiers=OSArray::withCapacity(3);
    bool ok=controllers && specifiers;
    for (unsigned i=0;ok && i<3;++i) {
        auto irq=QcomPMICInput::interrupt(fButtons[i]);
        auto *data=OSData::withBytes(&irq,sizeof(irq));
        ok=data && controllers->setObject(fName) && specifiers->setObject(data);
        OSSafeReleaseNULL(data);
    }
    if (ok) ok=setProperty(gIOInterruptControllersKey,controllers) && setProperty(gIOInterruptSpecifiersKey,specifiers);
    OSSafeReleaseNULL(controllers); OSSafeReleaseNULL(specifiers); return ok;
}
void QcomPMIC::cleanup() {
    if (fPM) { PMstop(); fPM=false; }
    if (fIRQAdded) { fInterrupt->disable(); fLoop->removeEventSource(fInterrupt); fIRQAdded=false; }
    if (fGateAdded) { fLoop->removeEventSource(fGate); fGateAdded=false; }
    OSSafeReleaseNULL(fInterrupt); OSSafeReleaseNULL(fGate); OSSafeReleaseNULL(fLoop); OSSafeReleaseNULL(fName);
    if (vectors) { IODelete(vectors,IOInterruptVector,fCount); vectors=nullptr; }
}
void QcomPMIC::free() { cleanup(); super::free(); }
bool QcomPMIC::requestTerminate(IOService *,IOOptionBits) { return !fPublished; }
IOWorkLoop *QcomPMIC::getWorkLoop() const { return fLoop; }
int QcomPMIC::decode(IOService *nub,int source) const {
    if (!nub || source<0 || source>=nub->_numInterruptSources || !nub->_interruptSources) return -1;
    auto *data=nub->_interruptSources[source].vectorData;
    if (!data || data->getLength()!=16) return -1;
    const auto *p=static_cast<const uint8_t *>(data->getBytesNoCopy());
    QcomPMICInput::IRQ irq{cell(p),cell(p+4),cell(p+8),cell(p+12)};
    for (unsigned i=0;i<fCount;++i) if (same(irq,fEntries[i].irq) && irq.trigger==fEntries[i].irq.trigger) return i;
    return -1;
}
IOReturn QcomPMIC::perform(Request &r) {
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    return fGateAdded ? fGate->runAction(dispatch,&r) : kIOReturnNotReady;
}
IOReturn QcomPMIC::dispatch(OSObject *owner,void *arg,void *,void *,void *) {
    auto *self=static_cast<QcomPMIC *>(owner); auto &r=*static_cast<Request *>(arg);
    if (self->fFault && r.op!=Unregister && r.op!=Disable && r.op!=Suspend && r.op!=ReleaseUSB) return kIOReturnOffline;
    QcomPMICInput::Hardware<Bus> hw(self->fBus); self->fBus.error=kIOReturnSuccess;
    if (r.op==Suspend || r.op==Resume) {
        self->fSuspended=r.op==Suspend;
        if (self->fSuspended) self->fInterrupt->disable();
        for (unsigned i=0;i<self->fCount;++i) if (self->vectors[i].interruptRegistered && self->fEntries[i].enabled) {
            bool ok=self->fSuspended ? hw.mask(self->fEntries[i].irq) : hw.unmask(self->fEntries[i].irq);
            if (!ok) { self->fault(); return self->fBus.error; }
        }
        self->updateParent();
        return kIOReturnSuccess;
    }
    if (self->fSuspended && r.op!=Unregister && r.op!=Disable && r.op!=ReleaseUSB) return kIOReturnOffline;
    if (r.op==ClaimUSB || r.op==ReleaseUSB) {
        if (!r.nub || r.nub->getProvider()!=self || !self->fUSBConfig.valid()) return kIOReturnBadArgument;
        if (r.op==ReleaseUSB) {
            if (self->fUSBClient!=r.nub) return kIOReturnNotOpen;
            for (unsigned i=0;i<self->fCount;++i)
                if (self->vectors[i].interruptRegistered && self->vectors[i].nub==r.nub) return kIOReturnBusy;
            self->fUSBClient=nullptr; r.nub->release(); return kIOReturnSuccess;
        }
        if (self->fUSBClient) return kIOReturnExclusiveAccess;
        for (unsigned i=0;i<self->fCount;++i) {
            const auto &irq=self->fEntries[i].irq;
            if (irq.sid==self->fUSBConfig.sid && self->vectors[i].interruptRegistered &&
                (irq.peripheral==0x11 || irq.peripheral==0x13 || irq.peripheral==0x14 || irq.peripheral==0x16 ||
                 irq.peripheral==(self->fUSBConfig.adcBase>>8))) return kIOReturnExclusiveAccess;
        }
        auto *controllers=OSArray::withCapacity(6), *specifiers=OSArray::withCapacity(6);
        bool ok=controllers && specifiers;
        const unsigned peripherals[6]={0x13,0x13,0x13,0x11,0x11,0x11};
        const unsigned bits[6]={7,4,3,0,1,2};
        for (unsigned k=0;ok && k<6;++k) for (unsigned i=0;i<self->fCount;++i) {
            const auto &irq=self->fEntries[i].irq;
            if (irq.sid!=self->fUSBConfig.sid || irq.peripheral!=peripherals[k] || irq.bit!=bits[k]) continue;
            if (self->vectors[i].interruptRegistered) { ok=false; break; }
            auto *data=OSData::withBytes(&irq,sizeof(irq));
            ok=data && controllers->setObject(self->fName) && specifiers->setObject(data);
            OSSafeReleaseNULL(data); break;
        }
        if (ok) ok=r.nub->setProperty(gIOInterruptControllersKey,controllers) &&
            r.nub->setProperty(gIOInterruptSpecifiersKey,specifiers);
        OSSafeReleaseNULL(controllers); OSSafeReleaseNULL(specifiers);
        if (!ok) return kIOReturnNoResources;
        self->fUSBClient=r.nub; r.nub->retain(); return kIOReturnSuccess;
    }
    if (r.op==TransferUSB) {
        if (!r.nub || self->fUSBClient!=r.nub || !r.data) return kIOReturnNotOpen;
        auto &t=*static_cast<USBTransfer *>(r.data);
        const unsigned peripheral=t.address>>8;
        if (!t.bytes || !t.count || t.count>8 || (t.address&255)+t.count>256 ||
            (peripheral!=1 && peripheral!=0x11 && peripheral!=0x13 && peripheral!=0x14 && peripheral!=0x16 &&
             peripheral!=(self->fUSBConfig.adcBase>>8)) || (peripheral==1 && t.write)) return kIOReturnBadArgument;
        // Charger MISC contains unrelated watchdog/thermal controls. USB only
        // needs tCCDebounce, the vendor PBS selector and its secure-access key.
        if (peripheral==0x16 && (t.count!=1 || ((t.address&255)!=0x52 &&
            (t.address&255)!=0xe9 && (t.address&255)!=0xd0))) return kIOReturnBadArgument;
        return self->fSPMI->transfer(self->fUSBConfig.sid,t.address,t.bytes,t.count,t.write);
    }
    if (r.op==PrepareButtons || r.op==ReadButtons) {
        UInt32 mask=0;
        for (unsigned i=0;i<self->fButtonCount;++i) {
            bool down=false;
            bool ok=r.op==PrepareButtons ? hw.prepareButton(self->fButtons[i]) : hw.readButton(self->fButtons[i],down);
            if (!ok) return self->fBus.error==kIOReturnSuccess ? kIOReturnUnsupported : self->fBus.error;
            if (down) mask|=1U<<i;
        }
        if (r.data) *static_cast<UInt32 *>(r.data)=mask;
        return kIOReturnSuccess;
    }
    if (r.op==GPIORead || r.op==GPIOConfigure) {
        auto &g=*static_cast<QcomPMICInput::GPIORequest *>(r.data);
        bool allowed=false;
        for (unsigned i=0;i<self->fCount;++i) {
            auto &e=self->fEntries[i];
            if (e.irq.sid==g.sid && e.irq.peripheral==0xc0+g.pin-1 && e.irq.bit==0) {
                allowed=true;
                if (r.op==GPIOConfigure && self->vectors[i].interruptRegistered) return kIOReturnBusy;
            }
        }
        if (!allowed || g.sid>15 || g.pin<1 || g.pin>26 || g.pull>5 || g.output>1 || g.value>1) return kIOReturnBadArgument;
        bool value=false;
        bool ok=r.op==GPIORead ? hw.readGPIO(g.sid,g.pin,value) : hw.configureGPIO(g.sid,g.pin,g.output,g.value,g.pull);
        if (ok && r.op==GPIORead) g.value=value;
        return ok ? kIOReturnSuccess : (self->fBus.error==kIOReturnSuccess ? kIOReturnUnsupported : self->fBus.error);
    }
    int index=self->decode(r.nub,r.source);
    if (index<0) return kIOReturnBadArgument;
    auto &v=self->vectors[index]; auto &entry=self->fEntries[index]; bool ok=true;
    if (r.op==Register) {
        if (self->fUSBClient && entry.irq.sid==self->fUSBConfig.sid && r.nub!=self->fUSBClient &&
            (entry.irq.peripheral==0x11 || entry.irq.peripheral==0x13 || entry.irq.peripheral==0x14 || entry.irq.peripheral==0x16 ||
             entry.irq.peripheral==(self->fUSBConfig.adcBase>>8))) return kIOReturnExclusiveAccess;
        if (!r.handler) return kIOReturnBadArgument;
        if (v.interruptRegistered) return kIOReturnNoResources;
        if (!hw.exclusive(entry.irq,self->ownedMask(index)))
            return self->fBus.error==kIOReturnSuccess ? kIOReturnExclusiveAccess : self->fBus.error;
        if (!hw.configureIRQ(entry.irq)) return self->fBus.error;
        self->fSPMI->irqClear(entry.apid,1U<<entry.irq.bit);
        v.nub=r.nub; v.source=r.source; v.target=r.target; v.handler=r.handler; v.refCon=r.ref;
        v.interruptDisabledSoft=v.interruptDisabledHard=1; v.interruptRegistered=1;
        return kIOReturnSuccess;
    }
    if (!v.interruptRegistered || v.nub!=r.nub || v.source!=r.source) return kIOReturnBadArgument;
    if (r.op==Enable) {
        self->fSPMI->irqAccess(entry.apid,true);
        ok=hw.unmask(entry.irq);
        if (ok) { entry.enabled=true; v.interruptDisabledSoft=v.interruptDisabledHard=0; }
    } else {
        entry.enabled=false; v.interruptDisabledSoft=v.interruptDisabledHard=1;
        ok=hw.mask(entry.irq);
        if (r.op==Unregister) { ok=hw.ack(entry.irq) && ok; self->fSPMI->irqClear(entry.apid,1U<<entry.irq.bit); v.interruptRegistered=0; }
    }
    if (!ok) { self->fault(); return self->fBus.error; }
    self->updateParent();
    return kIOReturnSuccess;
}
unsigned QcomPMIC::ownedMask(unsigned index) const {
    unsigned mask=0;
    for (unsigned i=0;i<fCount;++i) if (fEntries[i].apid==fEntries[index].apid) mask|=1U<<fEntries[i].irq.bit;
    return mask;
}
void QcomPMIC::updateParent() {
    bool any=false;
    for (unsigned i=0;i<fCount;++i) any|=fEntries[i].enabled;
    if (any && !fFault && !fSuspended) fInterrupt->enable();
    else fInterrupt->disable();
}
IOReturn QcomPMIC::registerInterrupt(IOService *nub,int source,void *target,IOInterruptHandler handler,void *ref) {
    Request r{Register,nub,source,target,handler,ref,nullptr}; return perform(r);
}
IOReturn QcomPMIC::unregisterInterrupt(IOService *nub,int source) { Request r{Unregister,nub,source}; return perform(r); }
IOReturn QcomPMIC::enableInterrupt(IOService *nub,int source) { Request r{Enable,nub,source}; return perform(r); }
IOReturn QcomPMIC::disableInterrupt(IOService *nub,int source) { Request r{Disable,nub,source}; return perform(r); }
IOReturn QcomPMIC::causeInterrupt(IOService *,int) { return kIOReturnUnsupported; }
IOReturn QcomPMIC::getInterruptType(IOService *nub,int source,int *type) {
    int i=decode(nub,source); if (i<0 || !type) return kIOReturnBadArgument;
    *type=fEntries[i].irq.trigger&3 ? kIOInterruptTypeEdge : kIOInterruptTypeLevel; return kIOReturnSuccess;
}
void QcomPMIC::interruptAction(OSObject *owner,IOInterruptEventSource *,int) { static_cast<QcomPMIC *>(owner)->drain(); }
void QcomPMIC::fault() {
    fFault=true; if (fInterrupt) fInterrupt->disable();
    setProperty("PMICInterruptFault",true);
    IOLog("QcomPMIC: interrupt domain disabled after SPMI/IRQ failure\n");
}
void QcomPMIC::drain() {
    if (fFault || fSuspended) return;
    QcomPMICInput::Hardware<Bus> hw(fBus);
    bool handled=false;
    // The arbiter IRQ_STATUS fallback also handles the documented empty
    // OWNER_ACC_STATUS condition. Only explicitly owned APIDs are touched.
    for (unsigned i=0;i<fCount;++i) {
        auto &e=fEntries[i]; auto &v=vectors[i]; unsigned bit=1U<<e.irq.bit;
        unsigned status=fSPMI->irqStatus(e.apid);
        if (status&~ownedMask(i)) { fault(); return; }
        if (!(status&bit)) continue;
        handled=true;
        bool level=!(e.irq.trigger&3);
        if ((!v.interruptRegistered || !e.enabled || level) && !hw.mask(e.irq)) { fault(); return; }
        fSPMI->irqClear(e.apid,bit);
        if (!hw.ack(e.irq)) { fault(); return; }
        if (v.interruptRegistered && e.enabled) {
            v.interruptActive=1;
            v.handler(v.target,v.refCon,v.nub,v.source);
            v.interruptActive=0;
        }
        if (level && v.interruptRegistered && e.enabled && !hw.unmask(e.irq)) { fault(); return; }
    }
    // A firmware APID outside our ownership can also assert the shared parent.
    // Preserve that APID, tolerate late delivery, and bound an unserved storm.
    if (fSummary.fault(handled,false)) fault();
}
bool QcomPMIC::buttonConfig(unsigned i,QcomPMICInput::Button &b) const {
    if (i>=fButtonCount) return false; b=fButtons[i]; return true;
}
IOReturn QcomPMIC::prepareButtons() { Request r{PrepareButtons}; return perform(r); }
bool QcomPMIC::usbConfiguration(PMICUSB::Config &c) const { c=fUSBConfig; return c.valid(); }
IOReturn QcomPMIC::claimUSB(IOService *client,bool acquire) {
    Request r{acquire ? ClaimUSB : ReleaseUSB,client}; return perform(r);
}
IOReturn QcomPMIC::usbTransfer(IOService *client,UInt16 address,UInt8 *bytes,unsigned count,bool write) {
    USBTransfer t{address,bytes,count,write}; Request r{TransferUSB,client,0,nullptr,nullptr,nullptr,&t}; return perform(r);
}
IOReturn QcomPMIC::setPowerState(unsigned long state,IOService *) {
    if (state>1) return kIOPMNoSuchState;
    Request r{state ? Resume : Suspend};
    if (fGateAdded) perform(r);
    return IOPMAckImplied;
}
IOReturn QcomPMIC::readButtons(UInt32 &mask) { Request r{ReadButtons,nullptr,0,nullptr,nullptr,nullptr,&mask}; return perform(r); }
IOReturn QcomPMIC::callPlatformFunction(const OSSymbol *name,bool wait,void *p1,void *p2,void *p3,void *p4) {
    if (!name) return kIOReturnBadArgument;
    bool read=name->isEqualTo("getPMICGPIO"), config=name->isEqualTo("configurePMICGPIO");
    if (!read && !config) return super::callPlatformFunction(name,wait,p1,p2,p3,p4);
    if (!p1 || p2 || p3 || p4) return kIOReturnBadArgument;
    Request r{read ? GPIORead : GPIOConfigure,nullptr,0,nullptr,nullptr,nullptr,p1}; return perform(r);
}
