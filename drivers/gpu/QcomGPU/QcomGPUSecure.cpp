// SPDX-License-Identifier: BSD-2-Clause
#include "QcomGPUSecure.h"
#include <libkern/c++/OSSymbol.h>

QcomGPUSecure *QcomGPUSecure::create(IOService *owner) {
    auto *provider = owner->getProvider();
    auto *bus = provider ? provider->getProvider() : nullptr;
    auto *matching = IOService::serviceMatching("QcomSCM");
    if (!bus || !matching) { OSSafeReleaseNULL(matching); return nullptr; }
    auto *services = IOService::getMatchingServices(matching);
    matching->release();
    if (!services) return nullptr;
    IOService *service = nullptr, *next;
    bool duplicate = false;
    while ((next = OSDynamicCast(IOService, services->getNextObject()))) {
        auto *nub = next->getProvider();
        if (nub && nub->getProvider() == bus) {
            if (service) { duplicate = true; break; }
            service = next; service->retain();
        }
    }
    services->release();
    if (!service || duplicate) { OSSafeReleaseNULL(service); return nullptr; }
    auto *secure = new QcomGPUSecure;
    if (!secure) { service->release(); return nullptr; }
    secure->fOwner = owner; secure->fService = service;
    secure->fFunction = OSSymbol::withCStringNoCopy(SecureFirmware::Function);
    if (!secure->fFunction) { delete secure; return nullptr; }
    return secure;
}
QcomGPUSecure::~QcomGPUSecure() {
    OSSafeReleaseNULL(fFunction); OSSafeReleaseNULL(fService);
}
IOReturn QcomGPUSecure::request(SecureFirmware::Action action) {
    SecureFirmware::Request r;
    r.action = action; r.token = fToken;
    const auto status = fService->callPlatformFunction(fFunction, false, &r, fOwner, nullptr, nullptr);
    if (r.version != 1 || r.size != sizeof(r) || r.action != action ||
        r.peripheral != SecureFirmware::GPUPeripheral || r.metadata || r.memory || r.reserved ||
        r.capabilities & ~(SecureFirmware::RemoteState | SecureFirmware::PeripheralAuthentication) ||
        r.flags & ~SecureFirmware::Quarantined || r.state > SecureFirmware::Faulted ||
        (action != SecureFirmware::Action::Acquire && action != SecureFirmware::Action::Release && r.token != fToken) ||
        (action == SecureFirmware::Action::Release && r.token != (status == kIOReturnSuccess ? 0 : fToken)) ||
        (action == SecureFirmware::Action::Acquire && (status == kIOReturnSuccess ? !r.token : r.token != 0))) {
        fFault = true; return kIOReturnIOError;
    }
    if (r.flags & SecureFirmware::Quarantined) fFault = true;
    if (status == kIOReturnSuccess) {
        if (action == SecureFirmware::Action::Acquire || action == SecureFirmware::Action::Release) fToken = r.token;
        if ((action == SecureFirmware::Action::Resume || (action == SecureFirmware::Action::Validate && fResumed)) &&
            (!(r.capabilities & SecureFirmware::RemoteState) || r.state != SecureFirmware::Warm)) fFault = true;
    }
    return fFault ? kIOReturnIOError : status;
}
IOReturn QcomGPUSecure::resume() {
    if (fFault) return kIOReturnIOError;
    if (!fOpen) {
        if (!fService->open(fOwner)) return kIOReturnExclusiveAccess;
        fOpen = true;
    }
    if (!fToken) {
        const auto status = request(SecureFirmware::Action::Acquire);
        if (status != kIOReturnSuccess) return status;
    }
    if (fResumed) return healthy() ? kIOReturnSuccess : kIOReturnIOError;
    const auto status = request(SecureFirmware::Action::Resume);
    if (status == kIOReturnSuccess) fResumed = true;
    return status;
}
bool QcomGPUSecure::healthy() {
    return fOpen && fToken && fResumed && !fFault && request(SecureFirmware::Action::Validate) == kIOReturnSuccess;
}
IOReturn QcomGPUSecure::restore() {
    if(!healthy())return kIOReturnNotReady;
    fResumed=false;
    const auto status=request(SecureFirmware::Action::Resume);
    if(status==kIOReturnSuccess)fResumed=true;
    return status;
}
bool QcomGPUSecure::release() {
    if (fFault) return false;
    if (fToken && request(SecureFirmware::Action::Release) != kIOReturnSuccess) return false;
    if (fOpen) { fService->close(fOwner); fOpen = false; }
    fResumed = false; return true;
}
