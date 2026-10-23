// SPDX-License-Identifier: BSD-2-Clause
#include "QcomGPUPlatform.h"
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include "../../bus/QcomSPMI/SPMIRegulators.h"
#include <mach/mach_time.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

namespace {
constexpr ClockController::Controller controllers[]={ClockController::RPM,ClockController::GCC,ClockController::GPUCC,ClockController::SPMI};
}
QcomGPUPlatform::QcomGPUPlatform(IOService *provider):fProvider(provider),fBus(provider->getProvider()) {
    fProvider->retain(); if(fBus)fBus->retain();
    fLock=IOLockAlloc(); fFunction=OSSymbol::withCStringNoCopy(ClockController::Function);
    int timing=0;PE_parse_boot_argn("gpu-platform-timing",&timing,sizeof(timing));fTiming=timing!=0;
    if(auto *p=fProvider->getProperty("qcom,gpu-gfx-handoff")) {
        auto *data=OSDynamicCast(OSData,p);QcomPower::GPURegulatorHandoff h{};
        if(data && data->getLength()==sizeof(h))memcpy(&h,data->getBytesNoCopy(),sizeof(h));
        if(!h.valid())fFault=true;else fControllerCount=4;
    }
}
QcomGPUPlatform::~QcomGPUPlatform() {
    for(auto *&controller:fControllers)OSSafeReleaseNULL(controller);
    OSSafeReleaseNULL(fFunction); OSSafeReleaseNULL(fBus); OSSafeReleaseNULL(fProvider);
    if(fLock)IOLockFree(fLock);
}
IOReturn QcomGPUPlatform::resolveController(unsigned i) {
    // The resident controller that issues a lease must remain its recipient
    // for validation/release. Discovery is nonblocking and scoped exactly as
    // GenericARMIO::clockRequest; never redirect a live opaque token.
    if(!fBus || i>=fControllerCount)return kIOReturnNotAttached;
    if(fControllers[i])return kIOReturnSuccess;
    auto *matching=IOService::serviceMatching(ClockController::Marker);
    if(!matching)return kIOReturnNoMemory;
    auto *services=IOService::getMatchingServices(matching);matching->release();
    if(!services)return kIOReturnNotReady;
    IOService *controller=nullptr,*service=nullptr;
    while((service=OSDynamicCast(IOService,services->getNextObject()))) {
        auto *nub=service->getProvider();
        auto *kind=OSDynamicCast(OSNumber,service->getProperty("ClockControllerKind"));
        if(nub && nub->getProvider()==fBus && kind && kind->unsigned32BitValue()==controllers[i]) {
            if(controller){controller->release();services->release();return kIOReturnExclusiveAccess;}
            controller=service;controller->retain();
        }
    }
    services->release();
    if(!controller)return kIOReturnNotReady;
    fControllers[i]=controller;return kIOReturnSuccess;
}
IOReturn QcomGPUPlatform::request(unsigned i,ClockController::Operation op,uint64_t &value) {
    using namespace ClockController;
    if(!fBus || i>=fControllerCount)return kIOReturnNotAttached;
    if(!fFunction)return kIOReturnNoMemory;
    const auto resolved=resolveController(i);if(resolved)return resolved;
    auto *controller=fControllers[i],*nub=controller->getProvider();
    auto *kind=OSDynamicCast(OSNumber,controller->getProperty("ClockControllerKind"));
    if(controller->isInactive() || !nub || nub->getProvider()!=fBus || !kind ||
        kind->unsigned32BitValue()!=controllers[i]) {
        fFault=true;return kIOReturnOffline;
    }
    auto r=targeted(controllers[i],op,0,value,GPUConsumer);
    const auto status=controller->callPlatformFunction(fFunction,false,&r.request,nullptr,nullptr,nullptr);
    if(status!=kIOReturnSuccess && (fLastController!=i || fLastOperation!=unsigned(op) || fLastResult!=status)) {
        fLastController=i;fLastOperation=unsigned(op);fLastResult=status;
        IOLog("QcomGPU: platform controller=%u operation=%u status=%x token=%llx\n",
            unsigned(controllers[i]),unsigned(op),status,value);
        fProvider->setProperty("GPUPlatformController",uint64_t(controllers[i]),32);
        fProvider->setProperty("GPUPlatformOperation",uint64_t(op),32);
        fProvider->setProperty("GPUPlatformResult",uint64_t(uint32_t(status)),32);
    }
    if(!valid(r.request) || r.request.version!=2 || r.request.size!=sizeof(r) || r.controller!=controllers[i] ||
        r.consumer!=GPUConsumer || r.flags || r.reserved || r.request.id || r.request.operation!=op || r.request.name ||
        (op!=RetainGPUState && r.request.value!=value)) {
        fFault=true; return kIOReturnIOError;
    }
    value=r.request.value; return status;
}
IOReturn QcomGPUPlatform::validate(bool bootstrap) {
    if(fFault)return kIOReturnIOError;
    const bool measure=fTiming && fReady && !bootstrap && !fTimingReported;
    if(measure)fTimingReported=true;
    for(unsigned i=0;i<fControllerCount;++i)if(fTokens[i]) {
        uint64_t token=fTokens[i];
        const auto start=measure?mach_absolute_time():0;
        const auto status=request(i,ClockController::ValidateGPUState,token);
        if(measure) {
            const auto end=mach_absolute_time();uint64_t ns=0;
            if(end>=start)absolutetime_to_nanoseconds(end-start,&ns);
            kprintf("QcomGPU: platform validation controller=%u ns=%llu status=%x\n",
                unsigned(controllers[i]),ns,status);
        }
        if(status!=kIOReturnSuccess) {if(!bootstrap || !retryable(status))fFault=true;return status;}
    }
    return kIOReturnSuccess;
}
IOReturn QcomGPUPlatform::acquire() {
    if(!fLock)fLock=IOLockAlloc();
    if(!fLock)return kIOReturnNoMemory;
    IOLockLock(fLock);
    if(!fFunction)fFunction=OSSymbol::withCStringNoCopy(ClockController::Function);
    auto result=validate(true);
    for(unsigned i=0;result==kIOReturnSuccess && i<fControllerCount;++i)if(!fTokens[i]) {
        uint64_t token=0;const auto status=request(i,ClockController::RetainGPUState,token);
        fTokens[i]=token;
        result=status==kIOReturnSuccess && !token?kIOReturnIOError:status;
        if(result!=kIOReturnSuccess && (token || result==kIOReturnIOError || result==kIOReturnTimeout))fFault=true;
    }
    if(result==kIOReturnSuccess)result=validate(true);
    fReady=result==kIOReturnSuccess;
    IOLockUnlock(fLock);return result;
}
bool QcomGPUPlatform::healthy() {
    if(!fLock)return false;
    IOLockLock(fLock);const bool ok=fReady && validate()==kIOReturnSuccess;IOLockUnlock(fLock);return ok;
}
bool QcomGPUPlatform::release() {
    if(!fLock)return !fFault && !fReady;
    IOLockLock(fLock);bool ok=validate()==kIOReturnSuccess;
    for(unsigned i=fControllerCount;ok && i;--i)if(fTokens[i-1]) {
        uint64_t token=fTokens[i-1];
        ok=request(i-1,ClockController::ReleaseGPUState,token)==kIOReturnSuccess;
        if(ok)fTokens[i-1]=0;else fFault=true;
    }
    if(ok)fReady=false;
    IOLockUnlock(fLock);return ok;
}
