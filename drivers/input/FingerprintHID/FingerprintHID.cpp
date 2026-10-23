// SPDX-License-Identifier: GPL-2.0-only
#include "FingerprintHID.h"
#include <ClockControllerProtocol.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
using namespace QcomHomeProtocol;
#define super IOHIDDevice
#define HOME_LOG(...) do { if (fTrace) kprintf(__VA_ARGS__); else IOLog(__VA_ARGS__); } while (false)
#define HOME_WORK_LOG(s,...) do { if ((s)->fTrace) kprintf(__VA_ARGS__); else IOLog(__VA_ARGS__); } while (false)
OSDefineMetaClassAndAbstractStructors(FingerprintHID,super);

namespace {
bool gpioProperty(IOService *service,IOService *controller,const char *key,uint32_t flags,uint32_t &value) {
    auto *d=OSDynamicCast(OSData,service->getProperty(key));
    auto *handle=OSDynamicCast(OSData,controller->getProvider()->getProperty("AAPL,phandle"));
    if (!d || d->getLength()!=12 || !handle || handle->getLength()!=4) return false;
    const auto *p=static_cast<const uint8_t *>(d->getBytesNoCopy());
    if (read32(p)!=read32(handle->getBytesNoCopy()) || read32(p+8)!=flags) return false;
    value=read32(p+4); return true;
}
IOService *copyService(const char *name) {
    auto *match=IOService::serviceMatching(name);
    if (!match) return nullptr;
    IOService *service=IOService::waitForMatchingService(match,5000000000ULL);
    match->release(); return service;
}
}
IOReturn FingerprintHID::gpio(const char *operation,uint32_t pin,uintptr_t value) {
    return fGPIO ? fGPIO->callPlatformFunction(operation,false,reinterpret_cast<void *>(uintptr_t(pin)),
        reinterpret_cast<void *>(value),nullptr,nullptr) : kIOReturnNotReady;
}
bool FingerprintHID::clock(unsigned operation,uint32_t id,uint64_t value) {
    auto r=ClockController::targeted(ClockController::GCC,ClockController::Operation(operation),id,value,ClockController::FingerprintConsumer);
    return fClock && fClock->callPlatformFunction(ClockController::Function,true,&r.request,nullptr,nullptr,nullptr)==kIOReturnSuccess;
}
bool FingerprintHID::prepareResources(IOService *p) {
    fGPIO=copyService("QcomGPIO"); fClock=copyService("QcomGCC");
    if (!fGPIO || !fClock) return false;
    if (!gpioProperty(p,fGPIO,"power-gpios",0,fPowerPin) || !gpioProperty(p,fGPIO,"reset-gpios",1,fResetPin) ||
        !gpioProperty(p,fGPIO,"irq-gpios",0,fIRQPin) || fPowerPin>=150 || fResetPin>=150 || fIRQPin>=150 ||
        fPowerPin==fResetPin || fResetPin==fIRQPin || fPowerPin==fIRQPin) return false;
    // The SPI controller and its pinmux belong to the trusted application.
    // Retain its normal-world interface clocks through the common GCC owner.
    if (!clock(ClockController::RetainClock,54)) return false;
    fBusVote=true;
    auto rate=ClockController::targeted(ClockController::GCC,ClockController::GetRate,66);
    if (fClock->callPlatformFunction(ClockController::Function,true,&rate.request,nullptr,nullptr,nullptr)!=kIOReturnSuccess) {
        if (!clock(ClockController::SetRate,66,960000)) return false;
    }
    if (!clock(ClockController::RetainClock,66)) return false;
    fCoreVote=true;
    if (gpio("setGPIOCFG",fPowerPin,0x600)!=kIOReturnSuccess) return false;
    fPowered=true; IOSleep(4);
    if (gpio("setGPIOCFG",fResetPin,0x600)!=kIOReturnSuccess ||
        gpio("setGPIOCFG",fIRQPin,fVariant==Variant::FPC ? 1 : 3<<6)!=kIOReturnSuccess) return false;
    return resetSensor();
}
bool FingerprintHID::resetSensor() {
    if (gpio("setGPIOData",fResetPin,1)!=kIOReturnSuccess) return false;
    IODelay(100);
    if (gpio("setGPIOData",fResetPin,0)!=kIOReturnSuccess) return false;
    IOSleep(5);
    if (gpio("setGPIOData",fResetPin,1)!=kIOReturnSuccess) return false;
    IOSleep(5); return true;
}
bool FingerprintHID::initializeSensor(IOService *p) {
    const char *key=fVariant==Variant::FPC ? "fpc-firmware" : "goodix-firmware";
    auto *image=OSDynamicCast(OSData,p->getProperty(key));
    if (!image || !fApp.initialize(this)) { HOME_LOG("%s: secure transport initialization failed\n",getName()); return false; }
    if (!fApp.load(fVariant==Variant::FPC ? "fpcsgt" : "goodixfp",image)) {
        HOME_LOG("%s: trusted application load failed\n",getName()); return false;
    }
    if (fVariant==Variant::FPC) {
        int32_t status;
        // Sensor info establishes that TA startup recognized actual hardware.
        if (!fApp.fpc(6,2,status)) { HOME_LOG("FPCFingerprint: sensor information command failed\n"); return false; }
        HOME_LOG("FPCFingerprint: sensor information status=%d\n",status);
        return status==0 && fApp.fpc(10,4,status) && status==0;
    }
    uint32_t status;
    bzero(fApp.payload(),352);
    if (!fApp.goodix(1000,352,status) || status || !goodixProfileSupported(read32(fApp.payload()+100))) {
        HOME_LOG("GoodixFingerprint: unsupported sensor protocol or initialization failure\n"); return false;
    }
    setProperty("GoodixSensorProfile",read32(fApp.payload()+100),32);
    // The measured Milan F/A/HV branches share a 168-byte init message and
    // 223776-byte interrupt message. Reject unreviewed families above.
    bzero(fApp.payload(),168);
    if (!fApp.goodix(1001,168,status) || status) return false;
    bzero(fApp.payload(),100);
    return fApp.goodix(1005,100,status) && !status;
}
bool FingerprintHID::configureSensor(bool enabled) {
    if (fVariant==Variant::FPC) {
        int32_t status;
        return fApp.fpc(10,enabled ? 2 : 4,status) && !status;
    }
    uint32_t status;
    bzero(fApp.payload(),96);
    if (!fApp.goodix(enabled ? 1017 : 1018,96,status) || status) return false;
    bzero(fApp.payload(),100); write32(fApp.payload()+96,enabled ? 4 : 0); // Z-only navigation
    return fApp.goodix(1071,100,status) && !status;
}
bool FingerprintHID::sampleSensor(bool &down,bool &changed) {
    if (fVariant==Variant::FPC) {
        int32_t status=INT32_MIN;
        if (!fApp.fpc(10,0,status) || fpcPresence(status,down)!=Result::Success) {
            HOME_LOG("FPCFingerprint: presence query failed, status=%d\n",status); return false;
        }
        changed=true;
        // Presence queries leave the sensor in sleep. Arm the opposite edge
        // after each confirmed state, including the initial held-key sample.
        const unsigned command=down ? 1 : 2;
        if (!fApp.fpc(10,command,status) || status) {
            HOME_LOG("FPCFingerprint: arm command=%u failed, status=%d\n",command,status); return false;
        }
        return true;
    }
    auto *p=fApp.payload(); uint32_t status;
    bzero(p,GoodixIRQSize);
    // Original HAL's interrupt envelope selects a 512-byte transfer and stage
    // 1. Stages 4 and 5 request a bounded second secure-world transfer.
    write32(p+223201,0x02000000); write32(p+223248,512); write32(p+GoodixIRQStage,1);
    for (unsigned i=0;i<3;++i) {
        if (!fApp.goodix(1016,GoodixIRQSize,status) || status) return false;
        auto stage=read32(p+GoodixIRQStage);
        if (stage!=4 && stage!=5) return goodixPresence(read32(p+GoodixIRQType),down,changed)==Result::Success;
    }
    return false;
}
bool FingerprintHID::handleStart(IOService *p) {
    if (!super::handleStart(p) || fVariant==Variant::None) return false;
    // Missing firmware is a configuration failure, before power/reset writes.
    if (!OSDynamicCast(OSData,p->getProperty(fVariant==Variant::FPC ? "fpc-firmware" : "goodix-firmware"))) return false;
    if (p->getProperty("FingerprintSecureSessionQuarantined")) return false;
    HOME_LOG("%s: preparing physical Home sensor\n",getName());
    if (!prepareResources(p)) {
        HOME_LOG("%s: GPIO/clock preparation failed\n",getName()); cleanup(); return false;
    }
    if (!initializeSensor(p)) {
        if (fApp.blocked()) p->setProperty("FingerprintSecureSessionQuarantined",true);
        cleanup(); return false;
    }
    fGate=IOCommandGate::commandGate(this);
    fTimer=IOTimerEventSource::timerEventSource(this,timerAction);
    fReportBuffer=IOBufferMemoryDescriptor::withCapacity(1,kIODirectionOutIn);
    fIRQ=IOInterruptEventSource::interruptEventSource(this,irqAction,p,0);
    if (!fLoop || !fGate || !fTimer || !fReportBuffer || !fIRQ || fLoop->addEventSource(fGate)!=kIOReturnSuccess) goto fail;
    fGateAdded=true;
    if (fLoop->addEventSource(fTimer)!=kIOReturnSuccess) goto fail;
    fTimerAdded=true;
    // Only explicitly staged diagnostic personalities expose this input.
    // A separate timer releases it even while a secure transaction runs.
    fDiagnosticEnabled=getProperty("HIDDiagnosticInputEnabled")==kOSBooleanTrue;
    if (fDiagnosticEnabled) {
        fDiagnosticTimer=IOTimerEventSource::timerEventSource(this,diagnosticRelease);
        if (!fDiagnosticTimer || fLoop->addEventSource(fDiagnosticTimer)!=kIOReturnSuccess) goto fail;
        fDiagnosticTimerAdded=true;
    }
    if (fLoop->addEventSource(fIRQ)!=kIOReturnSuccess) goto fail;
    fIRQAdded=true; fIRQ->disable();
    fWorker=thread_call_allocate_with_options(secureWorker,this,THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
    if (!fWorker) goto fail;
    setProperty("Built-In",true); setProperty("HIDDefaultBehavior","");
    setProperty("HomeSecureWorker",true);
    setProperty("HomeTraceEnabled",fTrace);
    setProperty("FingerprintAuthenticationSupported",false);
    setProperty("HomeDeepSleepWakeSupported",false);
    return true;
fail:
    cleanup(); return false;
}
bool FingerprintHID::start(IOService *p) {
    UInt32 trace=0; PE_parse_boot_argn("input-report-trace",&trace,sizeof(trace)); fTrace=trace!=0;
    // Native IOHIDDevice obtains getWorkLoop() before calling handleStart.
    // Its report lock, asynchronous getReport and our command gate must use
    // this same loop to avoid acquiring the two gates in opposite orders.
    if (fLoop) return false;
    fLoop=IOWorkLoop::workLoop();
    if (!fLoop) return false;
    if (!super::start(p)) { HOME_LOG("%s: native HID start failed\n",getName()); cleanup(); return false; }
    if (fGate->runAction(resume)!=kIOReturnSuccess) { super::stop(p); return false; }
    IOPMPowerState states[2]={};
    states[0].version=states[1].version=1;
    states[0].inputPowerRequirement=kIOPMPowerOn;
    states[1].capabilityFlags=kIOPMDeviceUsable;
    states[1].inputPowerRequirement=states[1].outputPowerCharacter=kIOPMPowerOn;
    PMinit(); fPM=true; p->joinPMtree(this);
    if (registerPowerDriver(this,states,2)!=kIOReturnSuccess) { super::stop(p); return false; }
    changePowerStateTo(1);
    HOME_LOG("%s: native Home HID registered; secure worker pending\n",getName());
    registerService(); return true;
}
bool FingerprintHID::report(bool down) {
    UInt8 byte=down;
    if (fReportBuffer->writeBytes(0,&byte,1)!=1 || handleReport(fReportBuffer)!=kIOReturnSuccess) return false;
    if (fTrace && down!=fState.reported()) kprintf("%s: HID Home %s\n",getName(),down ? "down" : "up");
    setProperty("HomeInputReports",++fInputReports,64);setProperty("HomeLastDown",down);
    if (fDiagnosticEnabled && !fDiagnosticActive && !down) setProperty("HIDDiagnosticLastReleased",true);
    fState.delivered(down); return true;
}
void FingerprintHID::sample() {
    if (fStopping) return;
    if (!fOnline) {
        if (fState.reported() && !report(false)) fTimer->setTimeoutMS(20);
        return;
    }
    if (fDiagnosticActive) return;
    fTimer->cancelTimeout();
    fIRQ->disable();
    if (fState.pending() && !report(fState.desired())) { fTimer->setTimeoutMS(2);return; }
    if (fWork.queueSample()) thread_call_enter(fWorker);
}
void FingerprintHID::secureWorker(thread_call_param_t context,thread_call_param_t) {
    auto *s=static_cast<FingerprintHID *>(context);
    // ONCE prevents a second invocation from sharing the TA request buffer.
    // Never hold the native HID gate across QSEE or wait for this worker in it.
    for (;;) {
        WorkResult result;
        if (s->fGate->runAction(takeWork,&result)!=kIOReturnSuccess) return;
        uint64_t begin,end,ns;clock_get_uptime(&begin);
        result.success=result.work.kind==WorkerQueue::Kind::Configure ?
            s->configureSensor(result.work.enabled) : s->sampleSensor(result.down,result.changed);
        clock_get_uptime(&end);absolutetime_to_nanoseconds(end-begin,&ns);result.durationUS=ns/1000;
        result.blocked=s->fApp.blocked();
        s->fGate->runAction(completeWork,&result);
    }
}
IOReturn FingerprintHID::takeWork(OSObject *o,void *p,void *,void *,void *) {
    auto *s=static_cast<FingerprintHID *>(o);auto &r=*static_cast<WorkResult *>(p);
    r.work=s->fWork.next();r.down=s->fState.desired();
    if (r.work.kind!=WorkerQueue::Kind::None) {
        // A timer/IRQ can queue a follow-up while the previous secure call is
        // running. Each transaction must begin masked, including that rerun.
        s->fTimer->cancelTimeout();s->fIRQ->disable();
    }
    return r.work.kind==WorkerQueue::Kind::None ? kIOReturnNotReady : kIOReturnSuccess;
}
IOReturn FingerprintHID::completeWork(OSObject *o,void *p,void *,void *,void *) {
    auto *s=static_cast<FingerprintHID *>(o);const auto &r=*static_cast<WorkResult *>(p);
    s->fQuarantined=r.blocked;
    if (!s->fWork.current(r.work)) return kIOReturnAborted;
    if (r.durationUS>s->fMaxSecureUS) s->fMaxSecureUS=r.durationUS;
    s->setProperty("HomeLastSecureUS",r.durationUS,64);s->setProperty("HomeMaxSecureUS",s->fMaxSecureUS,64);
    if (r.work.kind==WorkerQueue::Kind::Configure) {
        if (!r.success) {
            s->fWork.cancel(); // latch offline; no unbounded retry
            s->setProperty("HIDInputOffline",true);
            HOME_WORK_LOG(s,"%s: Home secure configuration failed (quarantine=%u)\n",s->getName(),r.blocked);
            return kIOReturnIOError;
        }
        if (r.work.enabled) {
            s->fOnline=true;s->setProperty("HIDInputOffline",false);
            HOME_WORK_LOG(s,"%s: native Home HID ready; secure calls outside HID workloop\n",s->getName());
            s->sample();
        }
        return kIOReturnSuccess;
    }
    if (!r.success) {
        if (++s->fErrors<3 && !r.blocked) {
            if (!s->fWork.samplePending()) s->fTimer->setTimeoutMS(20);
            return kIOReturnSuccess;
        }
        s->fOnline=false;s->fState.release();s->fIRQ->disable();s->setProperty("HIDInputOffline",true);
        s->fWork.setEnabled(false);
        if (r.blocked) s->fWork.cancel();
        HOME_WORK_LOG(s,"%s: Home input stopped after %u errors (secure quarantine=%u)\n",s->getName(),s->fErrors,r.blocked);
        if (s->fState.reported() && !s->report(false)) s->fTimer->setTimeoutMS(20);
        return kIOReturnIOError;
    }
    s->setProperty("HomeSensorSamples",++s->fSensorSamples,64);
    UInt32 irqLevel=0;
    if(s->gpio("getGPIOData",s->fIRQPin,reinterpret_cast<uintptr_t>(&irqLevel))==kIOReturnSuccess)
        s->setProperty("HomeIRQLevel",irqLevel,32);
    s->fErrors=0;s->fState.update(r.down,r.changed);
    if (!s->fDiagnosticActive && s->fState.pending() && !s->report(s->fState.desired())) { s->fTimer->setTimeoutMS(2);return kIOReturnSuccess; }
    // FPC presence queries/rearming generate a new edge while the source is
    // masked. Drain that consumed notification before unmasking; otherwise
    // each secure query immediately schedules the next (~46ms forever).
    if (s->fVariant==Variant::FPC && s->fGPIO->callPlatformFunction("acknowledgeGPIOInterrupt",false,
            s->getProvider(),nullptr,nullptr,nullptr)!=kIOReturnSuccess) {
        s->fOnline=false;s->fWork.setEnabled(false);s->fState.release();s->setProperty("HIDInputOffline",true);
        if (s->fState.reported() && !s->report(false)) s->fTimer->setTimeoutMS(20);
        return kIOReturnIOError;
    }
    // Keep the pad masked across queued work. Unmasking here while another
    // sample is already pending lets its query/rearm generate the next IRQ,
    // feeding the worker forever even after the physical key is released.
    if (s->fWork.samplePending()) return kIOReturnSuccess;
    s->fIRQ->enable();
    // IRQ drives idle input; bounded polling confirms release after lost IRQs.
    s->fTimer->setTimeoutMS(s->fState.desired() ? 20 : 1000);
    return kIOReturnSuccess;
}
void FingerprintHID::irqAction(OSObject *o,IOInterruptEventSource *,int) {
    auto *s=static_cast<FingerprintHID *>(o);s->setProperty("HomeIRQRequests",++s->fIRQRequests,64);s->sample();
}
void FingerprintHID::timerAction(OSObject *o,IOTimerEventSource *) {
    auto *s=static_cast<FingerprintHID *>(o);s->setProperty("HomeTimerRequests",++s->fTimerRequests,64);s->sample();
}
IOReturn FingerprintHID::setProperties(OSObject *object) {
    auto *properties=OSDynamicCast(OSDictionary,object);
    if (!properties || !properties->getObject("HIDDiagnosticPulse")) return super::setProperties(object);
    if (properties->getCount()!=1 || properties->getObject("HIDDiagnosticPulse")!=kOSBooleanTrue) return kIOReturnBadArgument;
    if (IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess) return kIOReturnNotPrivileged;
    if (!fDiagnosticEnabled) return kIOReturnUnsupported;
    return fGateAdded ? fGate->runAction(diagnosticPulse) : kIOReturnNotReady;
}
IOReturn FingerprintHID::diagnosticPulse(OSObject *o,void *,void *,void *,void *) {
    auto *s=static_cast<FingerprintHID *>(o);
    if (!s->fOnline || s->fStopping || !s->fDiagnosticTimerAdded) return kIOReturnNotReady;
    if (s->fDiagnosticActive || s->fState.desired() || s->fState.reported()) return kIOReturnBusy;
    if (!s->report(true)) return kIOReturnIOError;
    s->fDiagnosticActive=true;
    s->setProperty("HIDDiagnosticPulses",++s->fDiagnosticPulses,64);
    s->setProperty("HIDDiagnosticLastReleased",false);
    s->fDiagnosticTimer->setTimeoutMS(80);
    return kIOReturnSuccess;
}
void FingerprintHID::diagnosticRelease(OSObject *o,IOTimerEventSource *) {
    auto *s=static_cast<FingerprintHID *>(o);
    s->fDiagnosticActive=false;
    const bool released=!s->fState.reported() || s->report(false);
    s->setProperty("HIDDiagnosticLastReleased",released);
    if (!released) s->fTimer->setTimeoutMS(2);
    else s->sample();
}
IOReturn FingerprintHID::resume(OSObject *o,void *,void *,void *,void *) {
    auto *s=static_cast<FingerprintHID *>(o);
    if (s->fStopping || s->fQuarantined || !s->fWorker) return kIOReturnNotReady;
    if (s->fWork.isEnabled()) return kIOReturnSuccess;
    s->fErrors=0;s->fWork.setEnabled(true);thread_call_enter(s->fWorker);
    return kIOReturnSuccess;
}
IOReturn FingerprintHID::quiesce(OSObject *o,void *,void *,void *,void *) {
    auto *s=static_cast<FingerprintHID *>(o); s->fOnline=false; s->fState.release();
    s->fDiagnosticActive=false;
    if (s->fDiagnosticTimer) s->fDiagnosticTimer->cancelTimeout();
    if (s->fTimer) s->fTimer->cancelTimeout();
    if (s->fIRQ) s->fIRQ->disable();
    if (s->fState.reported() && s->fReportBuffer && !s->report(false) && s->fTimerAdded) s->fTimer->setTimeoutMS(20);
    if (!s->fStopping && s->fWorker && s->fWork.setEnabled(false)) thread_call_enter(s->fWorker);
    return kIOReturnSuccess;
}
IOReturn FingerprintHID::shutdown(OSObject *o,void *,void *,void *,void *) {
    auto *s=static_cast<FingerprintHID *>(o);s->fStopping=true;s->fWork.stop();
    return quiesce(o,nullptr,nullptr,nullptr,nullptr);
}
IOReturn FingerprintHID::snapshot(OSObject *o,void *p,void *,void *,void *) {
    *static_cast<UInt8 *>(p)=static_cast<FingerprintHID *>(o)->fState.reported(); return kIOReturnSuccess;
}
IOReturn FingerprintHID::getReport(IOMemoryDescriptor *p,IOHIDReportType type,IOOptionBits options) {
    if (!p || p->getLength()<1) return kIOReturnBadArgument;
    if (type!=kIOHIDReportTypeInput || (options&255)) return kIOReturnUnsupported;
    if (!fGateAdded) return kIOReturnNotReady;
    UInt8 b; auto r=fGate->runAction(snapshot,&b);
    return r ? r : p->writeBytes(0,&b,1)==1 ? kIOReturnSuccess : kIOReturnIOError;
}
IOReturn FingerprintHID::newReportDescriptor(IOMemoryDescriptor **out) const {
    if (!out) return kIOReturnBadArgument;
    *out=IOBufferMemoryDescriptor::withBytes(ReportDescriptor,sizeof(ReportDescriptor),kIODirectionOutIn);
    return *out ? kIOReturnSuccess : kIOReturnNoMemory;
}
OSString *FingerprintHID::newTransportString() const { return OSString::withCString("SPI"); }
OSString *FingerprintHID::newManufacturerString() const { return OSString::withCString(fVariant==Variant::FPC ? "Fingerprint Cards" : "Goodix"); }
OSString *FingerprintHID::newProductString() const { return OSString::withCString("Fingerprint Home"); }
OSNumber *FingerprintHID::newPrimaryUsageNumber() const { return OSNumber::withNumber(1,32); }
OSNumber *FingerprintHID::newPrimaryUsagePageNumber() const { return OSNumber::withNumber(12,32); }
IOReturn FingerprintHID::setPowerState(unsigned long state,IOService *) {
    if (state>1) return kIOPMNoSuchState;
    if (fGateAdded) {
        IOReturn result=fGate->runAction(state ? resume : quiesce);
        HOME_LOG("%s: power state=%lu result=0x%x\n",getName(),state,result);
    }
    return IOPMAckImplied;
}
bool FingerprintHID::requestTerminate(IOService *,IOOptionBits) { return false; }
IOWorkLoop *FingerprintHID::getWorkLoop() const { return fLoop ? fLoop : super::getWorkLoop(); }
void FingerprintHID::drainWorker() {
    if (fGateAdded) fGate->runAction(shutdown);
    if (fWorker) { thread_call_cancel_wait(fWorker);thread_call_free(fWorker);fWorker=nullptr; }
}
void FingerprintHID::stop(IOService *p) { drainWorker(); super::stop(p); }
void FingerprintHID::handleStop(IOService *p) { cleanup(); super::handleStop(p); }
void FingerprintHID::cleanup() {
    if (fPM) { PMstop(); fPM=false; }
    drainWorker();
    if (fIRQAdded) { fLoop->removeEventSource(fIRQ); fIRQAdded=false; }
    if (fTimerAdded) { fLoop->removeEventSource(fTimer); fTimerAdded=false; }
    if (fDiagnosticTimerAdded) { fLoop->removeEventSource(fDiagnosticTimer); fDiagnosticTimerAdded=false; }
    if (fGateAdded) { fLoop->removeEventSource(fGate); fGateAdded=false; }
    OSSafeReleaseNULL(fIRQ); OSSafeReleaseNULL(fTimer); OSSafeReleaseNULL(fGate);
    OSSafeReleaseNULL(fDiagnosticTimer);
    OSSafeReleaseNULL(fReportBuffer);
    fApp.release();
    if (fApp.blocked() && getProvider()) getProvider()->setProperty("FingerprintSecureSessionQuarantined",true);
    if (!fApp.blocked()) {
        if (fPowered) { gpio("setGPIOData",fResetPin,0); gpio("setGPIOData",fPowerPin,0); fPowered=false; }
        if (fCoreVote) { clock(ClockController::ReleaseClock,66); fCoreVote=false; }
        if (fBusVote) { clock(ClockController::ReleaseClock,54); fBusVote=false; }
        OSSafeReleaseNULL(fGPIO); OSSafeReleaseNULL(fClock);
    }
}
void FingerprintHID::free() {
    cleanup();
    // handleStop runs before native HID removes its own event sources. Keep
    // getWorkLoop stable through native stop/free, then drop our final ref.
    auto *loop=fLoop;
    super::free();
    if (loop) loop->release();
}
