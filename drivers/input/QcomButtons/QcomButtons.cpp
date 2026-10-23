// SPDX-License-Identifier: GPL-2.0-only
#include "QcomButtons.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <pexpert/pexpert.h>
#define super IOHIDDevice
OSDefineMetaClassAndStructors(QcomButtons, super);
bool QcomButtons::handleStart(IOService *provider) {
    if (!super::handleStart(provider)) return false;
    UInt32 trace=0; PE_parse_boot_argn("input-report-trace",&trace,sizeof(trace)); fTrace=trace!=0;
    fPMIC=OSDynamicCast(QcomPMIC,provider);
    if (!fPMIC || fPMIC->buttonCount()!=3) return false;
    for (unsigned i=0;i<3;++i) {
        QcomPMICInput::Button b;
        if (!fPMIC->buttonConfig(i,b)) return false;
        fState.configure(i,b.usage,b.debounce);
    }
    {
        IOReturn status=fPMIC->prepareButtons();
        if (status!=kIOReturnSuccess) { IOLog("QcomButtons: PMIC input preparation failed (0x%x)\n",status); return false; }
    }
    fLoop=fPMIC->getWorkLoop(); if (fLoop) fLoop->retain();
    fGate=IOCommandGate::commandGate(this);
    fTimer=IOTimerEventSource::timerEventSource(this,timerAction);
    fReportBuffer=IOBufferMemoryDescriptor::withCapacity(1,kIODirectionOutIn);
    if (!fLoop || !fGate || !fTimer || !fReportBuffer || fLoop->addEventSource(fGate)!=kIOReturnSuccess) goto fail;
    fGateAdded=true;
    if (fLoop->addEventSource(fTimer)!=kIOReturnSuccess) goto fail;
    fTimerAdded=true;
    fDiagnosticEnabled=getProperty("HIDDiagnosticInputEnabled")==kOSBooleanTrue;
    if (fDiagnosticEnabled) {
        fDiagnosticTimer=IOTimerEventSource::timerEventSource(this,diagnosticRelease);
        if (!fDiagnosticTimer || fLoop->addEventSource(fDiagnosticTimer)!=kIOReturnSuccess) goto fail;
        fDiagnosticTimerAdded=true;
    }
    for (unsigned i=0;i<3;++i) {
        fInterrupts[i]=IOInterruptEventSource::interruptEventSource(this,interruptAction,provider,i);
        if (!fInterrupts[i] || fLoop->addEventSource(fInterrupts[i])!=kIOReturnSuccess) goto fail;
        fAdded[i]=true;
        // IOInterruptEventSource starts enabled. Keep the source masked until
        // IOHIDDevice has parsed the descriptor and accepts input reports.
        fInterrupts[i]->disable();
    }
    setProperty("Built-In",true);
    setProperty("HIDDefaultBehavior","");
    setProperty("HIDInputReportBytes",1,32);
    return true;
fail:
    cleanup(); return false;
}
bool QcomButtons::start(IOService *provider) {
    if (!super::start(provider)) { IOLog("QcomButtons: native HID start failed\n"); cleanup(); return false; }
    // All three IRQs and the initial read are serialized with PMIC traffic.
    if (fGate->runAction(resume)!=kIOReturnSuccess) { super::stop(provider); return false; }
    IOPMPowerState states[2] = {};
    states[0].version=states[1].version=1;
    states[0].outputPowerCharacter=states[0].inputPowerRequirement=kIOPMPowerOn;
    states[1].capabilityFlags=kIOPMDeviceUsable;
    states[1].outputPowerCharacter=states[1].inputPowerRequirement=kIOPMPowerOn;
    PMinit(); fPM=true; provider->joinPMtree(this);
    if (registerPowerDriver(this,states,2)!=kIOReturnSuccess) { super::stop(provider); return false; }
    changePowerStateTo(1);
    IOLog("QcomButtons: native HID ready (power, volume up, volume down)\n");
    registerService(); return true;
}
void QcomButtons::interruptAction(OSObject *owner,IOInterruptEventSource *,int) { static_cast<QcomButtons *>(owner)->sample(); }
void QcomButtons::timerAction(OSObject *owner,IOTimerEventSource *) { static_cast<QcomButtons *>(owner)->sample(); }
bool QcomButtons::send(UInt8 value) {
    if (fReportBuffer->writeBytes(0,&value,1)!=1 || handleReport(fReportBuffer,kIOHIDReportTypeInput,0)!=kIOReturnSuccess) return false;
    if (fTrace && value!=fReport) kprintf("QcomButtons: HID input 0x%x -> 0x%x\n",fReport,value);
    fReport=value;setProperty("ButtonInputReports",++fInputReports,64);
    if (fDiagnosticEnabled && !fDiagnosticActive && !(value&1)) setProperty("HIDDiagnosticLastReleased",true);
    return true;
}
void QcomButtons::sample() {
    if (!fOnline) {
        if (fNeutralPending) {
            fNeutralPending=!send(0);
            if (fNeutralPending) fTimer->setTimeoutMS(20);
        }
        return;
    }
    UInt32 raw=0;
    if (fPMIC->readButtons(raw)!=kIOReturnSuccess) {
        // A transient bus error is not a release. Bound recovery attempts;
        // a permanently failed input service must not leave a held HID key.
        if (++fErrors<3) { fTimer->setTimeoutMS(20); return; }
        fNeutralPending=!send(0); fState.reset(); fOnline=false;
        for (auto *irq:fInterrupts) irq->disable();
        setProperty("HIDInputOffline",true);
        if (fNeutralPending) fTimer->setTimeoutMS(20);
        return;
    }
    fErrors=0;
    UInt64 ticks,ns; clock_get_uptime(&ticks); absolutetime_to_nanoseconds(ticks,&ns);
    bool pending; UInt8 value=fState.sample(raw,ns,pending);
    if (fDiagnosticActive) value|=1; // Consumer Power; preserve physical volume keys.
    if (value!=fReport && !send(value)) pending=true;
    if (pending) fTimer->setTimeoutMS(2);
    else if (fReport) fTimer->setTimeoutMS(20); // detect a lost release or a fault while held
    else fTimer->cancelTimeout();
}
IOReturn QcomButtons::setProperties(OSObject *object) {
    auto *properties=OSDynamicCast(OSDictionary,object);
    if (!properties || !properties->getObject("HIDDiagnosticPulse")) return super::setProperties(object);
    if (properties->getCount()!=1 || properties->getObject("HIDDiagnosticPulse")!=kOSBooleanTrue) return kIOReturnBadArgument;
    if (IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess) return kIOReturnNotPrivileged;
    if (!fDiagnosticEnabled) return kIOReturnUnsupported;
    return fGateAdded ? fGate->runAction(diagnosticPulse) : kIOReturnNotReady;
}
IOReturn QcomButtons::diagnosticPulse(OSObject *o,void *,void *,void *,void *) {
    auto *s=static_cast<QcomButtons *>(o);
    if (!s->fOnline || !s->fDiagnosticTimerAdded) return kIOReturnNotReady;
    if (s->fDiagnosticActive || (s->fReport&1)) return kIOReturnBusy;
    if (!s->send(s->fReport|1)) return kIOReturnIOError;
    s->fDiagnosticActive=true;
    s->setProperty("HIDDiagnosticPulses",++s->fDiagnosticPulses,64);
    s->setProperty("HIDDiagnosticLastReleased",false);
    s->fDiagnosticTimer->setTimeoutMS(80);
    return kIOReturnSuccess;
}
void QcomButtons::diagnosticRelease(OSObject *o,IOTimerEventSource *) {
    auto *s=static_cast<QcomButtons *>(o);s->fDiagnosticActive=false;
    const bool released=!(s->fReport&1) || s->send(s->fReport&~1);
    s->setProperty("HIDDiagnosticLastReleased",released);
    if (!released) s->fTimer->setTimeoutMS(2);
    else s->sample();
}
IOReturn QcomButtons::snapshot(OSObject *owner,void *data,void *,void *,void *) {
    auto *self=static_cast<QcomButtons *>(owner);
    *static_cast<UInt8 *>(data)=self->fReport; return kIOReturnSuccess;
}
IOReturn QcomButtons::getReport(IOMemoryDescriptor *report,IOHIDReportType type,IOOptionBits options) {
    if (!report || report->getLength()<1) return kIOReturnBadArgument;
    if (type!=kIOHIDReportTypeInput || (options&255)) return kIOReturnUnsupported;
    if (!fGateAdded) return kIOReturnNotReady;
    UInt8 value; IOReturn status=fGate->runAction(snapshot,&value);
    return status!=kIOReturnSuccess ? status : report->writeBytes(0,&value,1)==1 ? kIOReturnSuccess : kIOReturnIOError;
}
IOReturn QcomButtons::newReportDescriptor(IOMemoryDescriptor **out) const {
    if (!out) return kIOReturnBadArgument;
    *out=IOBufferMemoryDescriptor::withBytes(QcomButton::ReportDescriptor,sizeof(QcomButton::ReportDescriptor),kIODirectionOutIn);
    return *out ? kIOReturnSuccess : kIOReturnNoMemory;
}
OSString *QcomButtons::newTransportString() const { return OSString::withCString("SPMI"); }
OSString *QcomButtons::newManufacturerString() const { return OSString::withCString("Qualcomm"); }
OSString *QcomButtons::newProductString() const { return OSString::withCString("Platform Buttons"); }
OSNumber *QcomButtons::newPrimaryUsageNumber() const { return OSNumber::withNumber(1,32); }
OSNumber *QcomButtons::newPrimaryUsagePageNumber() const { return OSNumber::withNumber(12,32); }
IOReturn QcomButtons::quiesce(OSObject *owner,void *,void *,void *,void *) {
    auto *self=static_cast<QcomButtons *>(owner); self->fOnline=false;
    self->fDiagnosticActive=false;
    if (self->fDiagnosticTimer) self->fDiagnosticTimer->cancelTimeout();
    if (self->fTimer) self->fTimer->cancelTimeout();
    for (auto *irq:self->fInterrupts) if (irq) irq->disable();
    self->fNeutralPending=self->fReportBuffer && self->fReport && !self->send(0);
    if (self->fNeutralPending && self->fTimerAdded) self->fTimer->setTimeoutMS(20);
    return kIOReturnSuccess;
}
IOReturn QcomButtons::resume(OSObject *owner,void *,void *,void *,void *) {
    auto *self=static_cast<QcomButtons *>(owner);
    if (self->fOnline) return kIOReturnSuccess;
    UInt32 raw; IOReturn result=self->fPMIC->readButtons(raw);
    if (result!=kIOReturnSuccess) return result;
    self->fState.reset(); self->fErrors=0; self->fOnline=true; self->fNeutralPending=false;
    for (auto *irq:self->fInterrupts) irq->enable();
    self->sample(); return kIOReturnSuccess;
}
IOReturn QcomButtons::setPowerState(unsigned long state,IOService *) {
    if (state>1) return kIOPMNoSuchState;
    if (fGateAdded) fGate->runAction(state ? resume : quiesce);
    return IOPMAckImplied;
}
void QcomButtons::stop(IOService *provider) {
    if (fGateAdded) fGate->runAction(quiesce);
    super::stop(provider);
}
void QcomButtons::handleStop(IOService *provider) { cleanup(); super::handleStop(provider); }
void QcomButtons::cleanup() {
    if (fPM) { PMstop(); fPM=false; }
    if (fGateAdded) fGate->runAction(quiesce);
    for (unsigned i=0;i<3;++i) {
        if (fAdded[i]) { fLoop->removeEventSource(fInterrupts[i]); fAdded[i]=false; }
        OSSafeReleaseNULL(fInterrupts[i]);
    }
    if (fTimerAdded) { fLoop->removeEventSource(fTimer); fTimerAdded=false; }
    if (fDiagnosticTimerAdded) { fLoop->removeEventSource(fDiagnosticTimer); fDiagnosticTimerAdded=false; }
    if (fGateAdded) { fLoop->removeEventSource(fGate); fGateAdded=false; }
    OSSafeReleaseNULL(fTimer); OSSafeReleaseNULL(fGate); OSSafeReleaseNULL(fLoop); OSSafeReleaseNULL(fReportBuffer);
    OSSafeReleaseNULL(fDiagnosticTimer);
}
void QcomButtons::free() { cleanup(); super::free(); }
