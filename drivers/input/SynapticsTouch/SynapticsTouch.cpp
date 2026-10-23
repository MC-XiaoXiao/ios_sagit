// SPDX-License-Identifier: GPL-2.0-only
#include "SynapticsTouch.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#define super IOHIDDevice
OSDefineMetaClassAndStructors(SynapticsTouch, super);

bool SynapticsTouch::Transport::select(uint16_t address) {
    uint8_t bytes[]={0xff,uint8_t(address>>8)};
    I2C::Message message{I2C::Direction::Write,bytes,2};
    return device->transfer(owner,&message,1)==kIOReturnSuccess;
}
bool SynapticsTouch::Transport::read(uint16_t address,uint8_t *bytes,size_t length) {
    if (!select(address)) return false;
    uint8_t reg=address;
    I2C::Message messages[]={{I2C::Direction::Write,&reg,1},{I2C::Direction::Read,bytes,length}};
    return device->transfer(owner,messages,2)==kIOReturnSuccess;
}
bool SynapticsTouch::Transport::write(uint16_t address,const uint8_t *bytes,size_t length) {
    if (!length || length>254 || !select(address)) return false;
    uint8_t buffer[255];buffer[0]=address;
    memcpy(buffer+1,bytes,length);
    I2C::Message message{I2C::Direction::Write,buffer,length+1};
    return device->transfer(owner,&message,1)==kIOReturnSuccess;
}
void SynapticsTouch::Transport::delayMS(unsigned ms) { IOSleep(ms); }
bool SynapticsTouch::handleStart(IOService *provider) {
    if (!super::handleStart(provider)) return false;
    fDevice=OSDynamicCast(GenericI2CDevice,provider);
    auto *handoff=OSDynamicCast(OSData,provider->getProperty("synaptics,touch-handoff"));
    uint32_t h[3];
    if (!fDevice || fDevice->address()!=0x20 || !handoff || handoff->getLength()!=sizeof(h)) return false;
    memcpy(h,handoff->getBytesNoCopy(),sizeof(h));
    if (h[0]!=1 || h[1]!=7 || h[2]>7) return false;
    fTransform.swap=h[2]&1;fTransform.invertX=h[2]&2;fTransform.invertY=h[2]&4;
    auto *rotation=OSDynamicCast(OSData,provider->getProperty("touchscreen-rotation"));
    if (rotation) {
        if (rotation->getLength()!=4) return false;
        memcpy(&fTransform.rotation,rotation->getBytesNoCopy(),4);
    } else if (provider->getProperty("touchscreen-rotation")) return false;
    if (!fTransform.valid() || !fDevice->open(this)) return false;
    fOpen=true;fTransport.device=fDevice;fTransport.owner=this;
    fGate=IOCommandGate::commandGate(this);
    fTimer=IOTimerEventSource::timerEventSource(this,timer);
    fBuffer=IOBufferMemoryDescriptor::withCapacity(TouchHID::ReportBytes,kIODirectionOutIn);
    if (!fLoop || !fGate || !fTimer || !fBuffer || fLoop->addEventSource(fGate)!=kIOReturnSuccess) goto fail;
    fGateAdded=true;
    if (fLoop->addEventSource(fTimer)!=kIOReturnSuccess) goto fail;
    fTimerAdded=true;
    fDiagnosticEnabled=getProperty("HIDDiagnosticInputEnabled")==kOSBooleanTrue;
    if (fDiagnosticEnabled) {
        fDiagnosticTimer=IOTimerEventSource::timerEventSource(this,diagnosticRelease);
        if (!fDiagnosticTimer || fLoop->addEventSource(fDiagnosticTimer)!=kIOReturnSuccess) goto fail;
        fDiagnosticTimerAdded=true;
    }
    fIRQ=IOInterruptEventSource::interruptEventSource(this,interrupt,provider,0);
    if (fIRQ) fIRQ->disable();
    if (!fIRQ || fLoop->addEventSource(fIRQ)!=kIOReturnSuccess) goto fail;
    fIRQAdded=true;fIRQ->disable();
    {
        const auto result=fSensor.initialize();
        if (fTrace) kprintf("SynapticsTouch: RMI startup result=%u product=%s\n",unsigned(result),fSensor.product);
        if (result!=RMI4::Result::Success) goto fail;
    }
    fConfigured=true;fMaxX=fSensor.maxX;fMaxY=fSensor.maxY;
    {
        RMI4::Frame neutral;
        TouchHID::report(neutral,fMaxX,fMaxY,fTransform,0,fLast);
    }
    setProperty("Built-In",true);
    // Native AppleMultitouchHIDService publishes both the generic Built-In
    // transport flag and the multitouch-specific identity used by BackBoard's
    // service wrappers.
    setProperty("MT Built-In",true);
    setProperty("HIDDefaultBehavior","");
    setProperty("DisplayIntegrated",true);
    setProperty("DigitizerCollectionDispatch",true);
    setProperty("HIDInputReportBytes",TouchHID::ReportBytes,32);
    setProperty("RMIProductID",fSensor.product);
    setProperty("RMIFunction",fSensor.function(),32);
    setProperty("RMIMaxContacts",fSensor.contactCount(),32);
    setProperty("RMIMaxX",fMaxX,32);setProperty("RMIMaxY",fMaxY,32);
    setProperty("RMIDataAddress",fSensor.reportDataAddress(),32);
    return true;
fail:
    cleanup();return false;
}
bool SynapticsTouch::start(IOService *provider) {
    UInt32 trace=0;PE_parse_boot_argn("input-report-trace",&trace,sizeof(trace));
    fTrace=trace!=0 || getProperty("TouchReportTrace")==kOSBooleanTrue;
    // Native HID caches getWorkLoop before handleStart. Publish the same loop
    // before super::start so report delivery and our gate cannot invert locks.
    if (fLoop) return false;
    fLoop=IOWorkLoop::workLoop();
    if (!fLoop) return false;
    if (!super::start(provider)) { cleanup();return false; }
    fHIDReady=true;
    IOPMPowerState states[2]{};
    states[0].version=states[1].version=1;
    // Firmware retains shared panel/touch rails in both states.
    states[0].inputPowerRequirement=states[0].outputPowerCharacter=kIOPMPowerOn;
    states[1].inputPowerRequirement=states[1].outputPowerCharacter=kIOPMPowerOn;
    states[1].capabilityFlags=kIOPMDeviceUsable;
    PMinit();fPM=true;provider->joinPMtree(this);
    if (registerPowerDriver(this,states,2)!=kIOReturnSuccess) { super::stop(provider);return false; }
    if (fGate->runAction(power,reinterpret_cast<void *>(1))!=kIOReturnSuccess) { super::stop(provider);return false; }
    changePowerStateTo(1);
    registerService();
    IOLog("SynapticsTouch: %s F%02x, %u contacts, native HID ready\n",fSensor.product,fSensor.function(),fSensor.contactCount());
    return true;
}
bool SynapticsTouch::emit(const RMI4::Frame &frame) {
    uint64_t ticks,ns;clock_get_uptime(&ticks);absolutetime_to_nanoseconds(ticks,&ns);
    uint8_t bytes[TouchHID::ReportBytes];
    if (!TouchHID::report(frame,fMaxX,fMaxY,fTransform,ns,bytes) ||
        fBuffer->writeBytes(0,bytes,sizeof(bytes))!=sizeof(bytes) ||
        handleReport(fBuffer,kIOHIDReportTypeInput,0)!=kIOReturnSuccess) return false;
    setProperty("TouchInputReports",++fInputReports,64);
    unsigned contacts=0;for(const auto &contact:frame.contacts)contacts+=contact.active;
    if(fDiagnosticActive) setProperty("TouchDiagnosticReports",++fDiagnosticReports,64);
    if(contacts && !fDiagnosticActive){
        setProperty("TouchActiveFrames",++fActiveFrames,64);
        auto *hid=OSData::withBytes(bytes,sizeof(bytes));
        auto *raw=OSData::withBytes(fSensor.lastRaw,fSensor.lastRawLength);
        if(hid){setProperty("TouchLastActiveHID",hid);hid->release();}
        if(raw){setProperty("TouchLastActiveRMI",raw);raw->release();}
    }
    if(!fDiagnosticActive && contacts && !fLastContacts)setProperty("TouchDownTransitions",++fDownTransitions,64);
    if(!fDiagnosticActive && !contacts && fLastContacts)setProperty("TouchUpTransitions",++fUpTransitions,64);
    if (fTrace && contacts!=fLastContacts) kprintf("SynapticsTouch: HID contacts=%u reports=%llu\n",contacts,fInputReports);
    fLastContacts=contacts;
    setProperty("TouchContacts",contacts,32);
    memcpy(fLast,bytes,sizeof(bytes));return true;
}
void SynapticsTouch::sample() {
    if (fStopping || !fHIDReady || fDiagnosticActive) return;
    fIRQ->disable();
    RMI4::Frame frame;
    if (fDelivery.releasing()) {
        if (!fDelivery.send(frame,[this](const RMI4::Frame &value){return emit(value);})) {
            fTimer->setTimeoutMS(20);return;
        }
        if (!fRunning) return;
    }
    if (!fRunning) return;
    if (!fConfigured) {
        if (++fRecoveries>10) { fRunning=false;setProperty("HIDInputOffline",true);return; }
        setProperty("RMIRecoveryAttempts",fRecoveries,32);
        if (fSensor.initialize()!=RMI4::Result::Success) { fTimer->setTimeoutMS(1000);return; }
        fConfigured=true;fMaxX=fSensor.maxX;fMaxY=fSensor.maxY;
    }
    const auto result=fSensor.sample(frame);
    if(fTrace){
        auto *raw=OSData::withBytes(fSensor.lastRaw,fSensor.lastRawLength);
        auto *status=OSData::withBytes(fSensor.lastStatus,sizeof(fSensor.lastStatus));
        if(raw){setProperty("RMILastRawFrame",raw);raw->release();}
        if(status){setProperty("RMILastStatus",status);status->release();}
    }
    if (result!=RMI4::Result::Success) {
        setProperty("RMILastError",static_cast<unsigned>(result),32);
        if (++fErrors<3 && result!=RMI4::Result::Reset && result!=RMI4::Result::Bootloader) {
            fTimer->setTimeoutMS(20);return;
        }
        // Release accepted fingers before any reprobe, including firmware reset.
        fDelivery.release();
        fDelivery.send(frame,[this](const RMI4::Frame &value){return emit(value);});
        if (result==RMI4::Result::Reset || result==RMI4::Result::Bootloader) fSensor.deviceReset();
        else fSensor.restore();
        fConfigured=false;setProperty("HIDInputOffline",true);
        fTimer->setTimeoutMS(1000);return;
    }
    fErrors=0;fRecoveries=0;setProperty("HIDInputOffline",false);setProperty("RMILastError",uint64_t(0),32);
    if (!fDelivery.send(frame,[this](const RMI4::Frame &value){return emit(value);})) {
        fTimer->setTimeoutMS(20);return;
    }
    // Level-low IRQ plus a watchdog: recover lost releases and device resets.
    fIRQ->enable();fTimer->setTimeoutMS(fDelivery.held() ? 20 : 1000);
}
void SynapticsTouch::interrupt(OSObject *owner,IOInterruptEventSource *,int) { static_cast<SynapticsTouch *>(owner)->sample(); }
void SynapticsTouch::timer(OSObject *owner,IOTimerEventSource *) { static_cast<SynapticsTouch *>(owner)->sample(); }
IOReturn SynapticsTouch::setProperties(OSObject *object) {
    auto *properties=OSDynamicCast(OSDictionary,object);
    if (!properties || !properties->getObject("TouchDiagnosticTap")) return super::setProperties(object);
    auto *data=OSDynamicCast(OSData,properties->getObject("TouchDiagnosticTap"));
    if (properties->getCount()!=1 || !data || data->getLength()!=7) return kIOReturnBadArgument;
    if (IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess) return kIOReturnNotPrivileged;
    if (!fDiagnosticEnabled) return kIOReturnUnsupported;
    return fGateAdded ? fGate->runAction(diagnosticTap,const_cast<void *>(data->getBytesNoCopy())) : kIOReturnNotReady;
}
IOReturn SynapticsTouch::diagnosticTap(OSObject *owner,void *request,void *,void *,void *) {
    auto *s=static_cast<SynapticsTouch *>(owner);auto *p=static_cast<uint8_t *>(request);
    if (!s->fRunning || !s->fHIDReady || !s->fConfigured || !s->fDiagnosticTimerAdded) return kIOReturnNotReady;
    if (s->fDiagnosticActive || s->fDelivery.held() || s->fDelivery.releasing()) return kIOReturnBusy;
    if (RMI4::le16(p)>32767 || RMI4::le16(p+2)>32767) return kIOReturnBadArgument;
    RMI4::Frame physical;
    if (s->fSensor.sample(physical)!=RMI4::Result::Success) return kIOReturnIOError;
    for (const auto &c:physical.contacts) if (c.active) return kIOReturnBusy;
    RMI4::Frame frame;auto &c=frame.contacts[0];c.active=true;
    c.x=uint32_t(RMI4::le16(p))*s->fMaxX/32767;c.y=uint32_t(RMI4::le16(p+2))*s->fMaxY/32767;
    c.pressure=p[4];c.width=p[5];c.height=p[6];
    s->fDiagnosticActive=true;s->fIRQ->disable();s->fTimer->cancelTimeout();
    if (!s->fDelivery.send(frame,[s](const RMI4::Frame &value){return s->emit(value);})) {
        s->fDiagnosticActive=false;s->sample();return kIOReturnIOError;
    }
    s->setProperty("TouchDiagnosticPulses",++s->fDiagnosticPulses,64);
    s->setProperty("TouchDiagnosticLastReleased",false);
    s->fDiagnosticTimer->setTimeoutMS(120);
    return kIOReturnSuccess;
}
void SynapticsTouch::diagnosticRelease(OSObject *owner,IOTimerEventSource *) {
    auto *s=static_cast<SynapticsTouch *>(owner);RMI4::Frame neutral;
    s->fDelivery.release();
    const bool released=s->fDelivery.send(neutral,[s](const RMI4::Frame &value){return s->emit(value);});
    s->setProperty("TouchDiagnosticLastReleased",released);
    if (!released) { s->fDiagnosticTimer->setTimeoutMS(20);return; }
    s->fDiagnosticActive=false;s->sample();
}
IOReturn SynapticsTouch::power(OSObject *owner,void *state,void *,void *,void *) {
    auto *self=static_cast<SynapticsTouch *>(owner);
    if (state==reinterpret_cast<void *>(2)) {
        self->fRunning=false;
        if (self->fIRQ) self->fIRQ->disable();
        if (self->fTimer) self->fTimer->cancelTimeout();
        self->fStopping=true;
        self->fHIDReady=false;
        self->fSensor.restore();
        self->fConfigured=false;
    } else if (state) {
        if (self->fRunning || self->fStopping) return kIOReturnSuccess;
        self->fRunning=true;self->fErrors=self->fRecoveries=0;self->sample();
    } else {
        if (self->fDiagnosticTimer) self->fDiagnosticTimer->cancelTimeout();
        self->fDiagnosticActive=false;
        self->fRunning=false;self->fIRQ->disable();self->fTimer->cancelTimeout();
        self->fDelivery.release();self->sample();
    }
    return kIOReturnSuccess;
}
IOReturn SynapticsTouch::setPowerState(unsigned long state,IOService *) {
    if (state>1) return kIOPMNoSuchState;
    if (fGateAdded) fGate->runAction(power,reinterpret_cast<void *>(state));
    return IOPMAckImplied;
}
IOReturn SynapticsTouch::snapshot(OSObject *owner,void *out,void *,void *,void *) {
    memcpy(out,static_cast<SynapticsTouch *>(owner)->fLast,TouchHID::ReportBytes);
    return kIOReturnSuccess;
}
IOReturn SynapticsTouch::getReport(IOMemoryDescriptor *out,IOHIDReportType type,IOOptionBits options) {
    if (!out) return kIOReturnBadArgument;
    if (type==kIOHIDReportTypeFeature && (options&255)==TouchHID::FeatureID) {
        const uint8_t feature[]={TouchHID::FeatureID,RMI4::MaxContacts};
        return out->getLength()>=sizeof(feature) && out->writeBytes(0,feature,sizeof(feature))==sizeof(feature) ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    if (type!=kIOHIDReportTypeInput || (options&255)!=TouchHID::InputID) return kIOReturnUnsupported;
    if (!fGateAdded) return kIOReturnNotReady;
    if (out->getLength()<TouchHID::ReportBytes) return kIOReturnBadArgument;
    uint8_t report[TouchHID::ReportBytes];
    IOReturn result=fGate->runAction(snapshot,report);
    return result!=kIOReturnSuccess ? result : out->writeBytes(0,report,sizeof(report))==sizeof(report) ? kIOReturnSuccess : kIOReturnIOError;
}
IOReturn SynapticsTouch::newReportDescriptor(IOMemoryDescriptor **out) const {
    if (!out) return kIOReturnBadArgument;
    uint8_t descriptor[TouchHID::DescriptorBytes];
    unsigned length=TouchHID::descriptor(descriptor,sizeof(descriptor));
    *out=length ? IOBufferMemoryDescriptor::withBytes(descriptor,length,kIODirectionOutIn) : nullptr;
    return *out ? kIOReturnSuccess : kIOReturnNoMemory;
}
IOWorkLoop *SynapticsTouch::getWorkLoop() const { return fLoop; }
OSString *SynapticsTouch::newTransportString() const { return OSString::withCString("I2C"); }
OSString *SynapticsTouch::newManufacturerString() const { return OSString::withCString("Synaptics"); }
OSString *SynapticsTouch::newProductString() const { return OSString::withCString("Mi 6 Touchscreen"); }
OSNumber *SynapticsTouch::newPrimaryUsageNumber() const { return OSNumber::withNumber(4,32); }
OSNumber *SynapticsTouch::newPrimaryUsagePageNumber() const { return OSNumber::withNumber(13,32); }
void SynapticsTouch::stop(IOService *provider) {
    if (fGateAdded) fGate->runAction(power,nullptr);
    super::stop(provider);
}
void SynapticsTouch::handleStop(IOService *provider) { cleanup();super::handleStop(provider); }
void SynapticsTouch::cleanup() {
    if (fPM) { PMstop();fPM=false; }
    // stop() releases contacts while native HID still accepts reports.
    // Teardown only cancels sources/restores registers, under the same gate.
    if (fGateAdded) fGate->runAction(power,reinterpret_cast<void *>(2));
    else { fStopping=true;fHIDReady=false; }
    if (fTimer) fTimer->cancelTimeout();
    if (fDiagnosticTimer) fDiagnosticTimer->cancelTimeout();
    if (fIRQ) fIRQ->disable();
    if (fIRQAdded) { fLoop->removeEventSource(fIRQ);fIRQAdded=false; }
    if (fTimerAdded) { fLoop->removeEventSource(fTimer);fTimerAdded=false; }
    if (fDiagnosticTimerAdded) { fLoop->removeEventSource(fDiagnosticTimer);fDiagnosticTimerAdded=false; }
    if (fGateAdded) { fLoop->removeEventSource(fGate);fGateAdded=false; }
    if (fOpen) { fSensor.restore();fConfigured=false; }
    if (fOpen) { fDevice->close(this);fOpen=false; }
    OSSafeReleaseNULL(fIRQ);OSSafeReleaseNULL(fTimer);OSSafeReleaseNULL(fGate);
    OSSafeReleaseNULL(fDiagnosticTimer);
    OSSafeReleaseNULL(fBuffer);
}
void SynapticsTouch::free() {
    cleanup();auto *loop=fLoop;super::free();if(loop)loop->release();
}
