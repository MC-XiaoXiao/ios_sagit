// SPDX-License-Identifier: BSD-2-Clause
#include "QcomUSB.h"
#include "QcomUSBMapper.h"
#include "USBDirectDMA.h"
#include "USBIODMA.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>

OSDefineMetaClassAndStructors(QcomUSB, IOService);
OSDefineMetaClassAndStructors(QcomUSBController, IOService);

namespace {
USB::Result platformResult(IOReturn r) {
    if (r == kIOReturnSuccess) return USB::Result::Success;
    if (r == kIOReturnAborted) return USB::Result::Aborted;
    if (r == kIOReturnNotReady) return USB::Result::NotReady;
    if (r == kIOReturnTimeout) return USB::Result::Timeout;
    return USB::Result::HardwareError;
}
}
bool QcomUSBController::prepare(QcomUSB *bus, USB::Role role, UInt64 epoch, const USB::ConnectorState &route) {
    if (!bus || !epoch || role == USB::Role::None || route.desired() != role || !init()) return false;
    fBus = bus; fBus->retain(); fRole = role; fEpoch = epoch;
    fHardwareLoop = bus->getWorkLoop();
    if (!fHardwareLoop) return false;
    fHardwareLoop->retain();
    fRoute = route; fAdmissionEpoch = bus->connectionEpoch();
    fResources = bus->fContract;
    fMapper = bus->fMapper;
    if (fMapper) fMapper->retain();
    fLock = IOSimpleLockAlloc();
    if (!fLock) return false;
    auto *memory = OSArray::withCapacity(1);
    if (!memory) return false;
    const bool added = memory->setObject(bus->fPlatform->getDeviceMemoryWithIndex(0));
    if (added) setDeviceMemory(memory);
    memory->release();
    if (!added) return false;
    // The Family copies a mapper from this nub itself; lookup does not walk
    // provider ancestors. Install the actual retained mapper object, not an
    // invented iommu-parent identifier or a platform-wide default mapper.
    if (bus->fMapper && !setProperty("iommu-parent", bus->fMapper)) return false;
    const char *keys[] = {"IOInterruptControllers", "IOInterruptSpecifiers"};
    for (const char *key : keys) {
        auto *values=OSDynamicCast(OSArray,bus->fPlatform->getProperty(key));
        auto *coreSource=OSArray::withCapacity(1);
        const bool copied=values && values->getCount() && coreSource && coreSource->setObject(values->getObject(0)) &&
            setProperty(key,coreSource);
        OSSafeReleaseNULL(coreSource);if (!copied) return false;
    }
    setName(role == USB::Role::Host ? "usb-host" : "usb-device");
    if (role == USB::Role::Device) {
        // This synthetic role nub is only in IOService. Native Family looks
        // for configuration-string on its immediate provider in IODT, so
        // carry the board's explicit configuration onto that provider.
        auto *configuration=OSDynamicCast(OSData,bus->fPlatform->getProperty("configuration-string"));
        if (configuration && !setProperty("configuration-string",configuration)) return false;
        int console = 0;
        PE_parse_boot_argn("qcom.usb-cdc", &console, sizeof(console));
        if (!setProperty("USBDevicePersonality", console == 1 ? "CDCACM" : "Native")) return false;
        if (console == 1) {
            unsigned vendor = 0, product = 0;
            PE_parse_boot_argn("qcom.usb-cdc-vid", &vendor, sizeof(vendor));
            PE_parse_boot_argn("qcom.usb-cdc-pid", &product, sizeof(product));
            // Missing/invalid identity leaves the CDC personality selected;
            // its start fails visibly rather than silently exposing Native.
            if (!setProperty("USBConsoleVendorID", vendor, 32) ||
                !setProperty("USBConsoleProductID", product, 32)) return false;
        }
    }
    // Never assert hardware snooping to a native Family merely because our
    // own rings/bounce buffers can instead use inhibited-cache allocations.
    if ((fResources.flags & USB::PlatformContract::Coherent) && !setProperty("dma-coherent", true)) return false;
    return setProperty("USBRoleEpoch", epoch, 64) &&
        setProperty("USBConnection", route.connection, 64) &&
        setProperty("USBOrientation", route.orientation == USB::Orientation::Reverse ? "Reverse" : "Normal") &&
        setProperty("USBPowerRole", route.power == USB::PowerRole::Source ? "Source" : "Sink") &&
        setProperty("USBRole", role == USB::Role::Host ? "Host" : "Device") &&
        setProperty("USBDMAMemoryMode", (fResources.flags & USB::PlatformContract::Coherent) ?
            "HardwareCoherent" : "Uncached");
}
bool QcomUSBController::handleOpen(IOService *client, IOOptionBits, void *) {
    if (!client || !fLock) return false;
    client->retain();
    const auto state = IOSimpleLockLockDisableInterrupt(fLock);
    const bool accepted = !fRetiring && fBus->acceptsConnection(fRoute, fAdmissionEpoch) && (!fClient || fClient == client);
    const bool installed = accepted && !fClient;
    if (installed) fClient = client;
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    if (!installed) client->release();
    return accepted;
}
void QcomUSBController::handleClose(IOService *client, IOOptionBits) {
    IOService *released = nullptr;
    const auto state = IOSimpleLockLockDisableInterrupt(fLock);
    if (fClient == client) { fHostFrameCounter=0;released = fClient; fClient = nullptr; }
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    if (released) released->release();
}
bool QcomUSBController::handleIsOpen(const IOService *client) const {
    if (!fLock) return false;
    const auto state = IOSimpleLockLockDisableInterrupt(fLock);
    const bool result = client ? fClient == client : fClient != nullptr;
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    return result;
}
bool QcomUSBController::accepts(IOService *client, UInt64 epoch) const {
    if (!fLock) return false;
    const auto state = IOSimpleLockLockDisableInterrupt(fLock);
    const bool result = !fRetiring && client && fClient == client && epoch == fEpoch && fBus->acceptsConnection(fRoute, fAdmissionEpoch);
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    return result;
}
void QcomUSBController::invalidate() {
    const auto state = IOSimpleLockLockDisableInterrupt(fLock);
    fRetiring = true;
    fHostFrameCounter = 0;
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
}
void QcomUSBController::isolated() {
    const auto state = IOSimpleLockLockDisableInterrupt(fLock);
    fIsolated = true;
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
}
bool QcomUSBController::dmaIsolated() const {
    const auto state = IOSimpleLockLockDisableInterrupt(fLock);
    const bool result = fIsolated;
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    return result;
}
void QcomUSBController::dmaRetired() {
    const auto state=IOSimpleLockLockDisableInterrupt(fLock);
    if (fRetiring && fIsolated) fDMARetired=true;
    IOSimpleLockUnlockEnableInterrupt(fLock,state);
}
bool QcomUSBController::dmaRecoveryReady() const {
    if (!fLock) return false;
    const auto state=IOSimpleLockLockDisableInterrupt(fLock);
    const bool result=fRetiring && fIsolated && fDMARetired;
    IOSimpleLockUnlockEnableInterrupt(fLock,state);return result;
}
void QcomUSBController::reportFault(IOService *client,UInt64 epoch) {
    if (!fLock) return;
    const auto state=IOSimpleLockLockDisableInterrupt(fLock);
    const bool valid=client && fClient==client && epoch==fEpoch && !fRetiring;
    if (valid) {fRetiring=true;fHostFrameCounter=0;}
    IOSimpleLockUnlockEnableInterrupt(fLock,state);
    if (valid) fBus->controllerFault(fRoute,fAdmissionEpoch);
}
IOWorkLoop *QcomUSBController::getWorkLoop() const { return fHardwareLoop; }
USB::PlatformContract QcomUSBController::resourceContract() const {
    const auto state = IOSimpleLockLockDisableInterrupt(fLock);
    const auto result = fRetiring || !fBus->acceptsConnection(fRoute, fAdmissionEpoch) ? USB::PlatformContract{} : fResources;
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    return result;
}
IOMapper *QcomUSBController::copyDeviceMapper() const {
    const auto state = IOSimpleLockLockDisableInterrupt(fLock);
    auto *mapper = fRetiring || !fBus->acceptsConnection(fRoute, fAdmissionEpoch) ? nullptr : fMapper;
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    // The nub owns its own mapper retain until free, even if an external
    // observer retains the old nub after it has detached from the platform.
    if (mapper) mapper->retain();
    return mapper;
}
USB::MMIO *QcomUSBController::registers(IOService *client, UInt64 epoch) const {
    return fHardwareLoop && fHardwareLoop->inGate() && accepts(client, epoch) ? fBus->fRegisters : nullptr;
}
DWC3::Core<USB::MMIO> *QcomUSBController::deviceCore(IOService *client, UInt64 epoch) const {
    return fRole == USB::Role::Device && fHardwareLoop && fHardwareLoop->inGate() &&
        accepts(client, epoch) ? fBus->fCore : nullptr;
}
IOReturn QcomUSBController::armHostFrameCounter(IOService *client, UInt64 epoch, UInt32 offset) {
    if (fRole!=USB::Role::Host || !fHardwareLoop || !fHardwareLoop->inGate()) return kIOReturnNotPermitted;
    const auto state=IOSimpleLockLockDisableInterrupt(fLock);
    IOReturn result=kIOReturnNotReady;
    if (!fRetiring && fClient==client && client && epoch==fEpoch &&
        fBus->acceptsConnection(fRoute,fAdmissionEpoch)) {
        auto *io=fBus->fRegisters;
        const auto runtime=io ? io->readSnapshot32(24) : UINT32_MAX;
        if (runtime==UINT32_MAX) result=kIOReturnIOError;
        else if (offset<0x420 || (offset&31) || offset!=(runtime&~31u) || offset>0xc0c0) result=kIOReturnBadArgument;
        else { fHostFrameCounter=offset;result=kIOReturnSuccess; }
    }
    IOSimpleLockUnlockEnableInterrupt(fLock,state);
    return result;
}
void QcomUSBController::disarmHostFrameCounter(IOService *client, UInt64 epoch) {
    if (!fLock) return;
    const auto state=IOSimpleLockLockDisableInterrupt(fLock);
    if (fClient==client && epoch==fEpoch) fHostFrameCounter=0;
    IOSimpleLockUnlockEnableInterrupt(fLock,state);
}
IOReturn QcomUSBController::readHostFrameCounter(IOService *client, UInt64 epoch, UInt32 &raw) const {
    if (!fLock) return kIOReturnNotReady;
    const auto state=IOSimpleLockLockDisableInterrupt(fLock);
    IOReturn result=kIOReturnNotReady;
    if (!fRetiring && fRole==USB::Role::Host && fHostFrameCounter && fClient==client && client && epoch==fEpoch &&
        fBus->acceptsConnection(fRoute,fAdmissionEpoch)) {
        // fBus keeps this mapping until invalidate has acquired fLock. No
        // mutable HCD State or sleeping gate participates in this read.
        const auto value=fBus->fRegisters->readSnapshot32(fHostFrameCounter);
        if (value==UINT32_MAX) result=kIOReturnIOError;
        else { raw=value&0x3fff;result=kIOReturnSuccess; }
    }
    IOSimpleLockUnlockEnableInterrupt(fLock,state);
    return result;
}
namespace {
struct USBHardwareActionArguments {
    QcomUSBController *nub;
    IOService *client;
    UInt64 epoch;
    QcomUSBController::HardwareAction action;
    void *context;
};
}
IOReturn QcomUSBController::runHardwareAction(IOService *client, UInt64 epoch, HardwareAction action, void *context) {
    if (!client || !action) return kIOReturnBadArgument;
    if (!fHardwareLoop) return kIOReturnNotReady;
    USBHardwareActionArguments arguments{this, client, epoch, action, context};
    // Keeping the role loop alive also makes a retained, retired nub safe to
    // query after its parent's stop: the gated admission check rejects it.
    // No event-source/command-gate pointer can disappear during this call.
    return fHardwareLoop->runAction(hardwareAction, this, &arguments);
}
IOReturn QcomUSBController::hardwareAction(OSObject *owner, void *arg, void *, void *, void *) {
    auto *nub = static_cast<QcomUSBController *>(owner);
    auto &request = *static_cast<USBHardwareActionArguments *>(arg);
    if (request.nub != nub || !nub->fHardwareLoop->inGate()) return kIOReturnNotPermitted;
    auto *registers = nub->registers(request.client, request.epoch);
    if (!registers) return kIOReturnAborted;
    return request.action(request.client, *registers, request.context);
}
void QcomUSBController::free() {
    if (fLock) { IOSimpleLockFree(fLock); fLock = nullptr; }
    OSSafeReleaseNULL(fMapper); OSSafeReleaseNULL(fHardwareLoop); OSSafeReleaseNULL(fBus);
    IOService::free();
}

IOReturn QcomUSB::platform(USB::PlatformRequest &request) {
    if (!fResources || !fFunction || !USB::validPlatformRequest(request)) return kIOReturnBadArgument;
    const auto operation = request.action;
    const auto lease = request.lease;
    const auto route = request.connector;
    const auto next = request.next;
    const auto source = request.source;
    const auto result = fResources->callPlatformFunction(fFunction, false, &request, this, nullptr, nullptr);
    if (!USB::validPlatformResponse(request, operation, lease, result == kIOReturnSuccess) ||
        request.next != next || request.source != source) return kIOReturnBadArgument;
    if (operation != USB::PlatformAction::Sample && operation != USB::PlatformAction::Acquire &&
        (!request.connector.sameRoute(route) || request.connector.vbus != route.vbus ||
         request.connector.overCurrent != route.overCurrent)) return kIOReturnBadArgument;
    return result;
}
USB::Result QcomUSB::source(bool enabled) {
    USB::PlatformRequest request; request.action = USB::PlatformAction::Source;
    request.lease = fLease; request.source = enabled;
    if (enabled) {
        request.connector = fTarget; request.next = fTarget.desired();
        if (!acceptsConnection(fTarget)) return USB::Result::Aborted;
    }
    return platformResult(platform(request));
}
USB::Result QcomUSB::safePower(USB::Role role, const USB::ConnectorState &target) {
    fTarget = target;
    USB::PlatformRequest request; request.action = USB::PlatformAction::SafePower;
    request.lease = fLease; request.next = role; request.connector = target;
    if (role != USB::Role::None && !acceptsConnection(target)) return USB::Result::Aborted;
    return platformResult(platform(request));
}
IOReturn QcomUSB::gated(GateOperation operation, USB::Role role, QcomUSBController *nub) {
    GateArguments arguments{operation, role, nub};
    return fGate->runAction(action, &arguments);
}
IOReturn QcomUSB::action(OSObject *owner, void *arg, void *, void *, void *) {
    auto *self = static_cast<QcomUSB *>(owner);
    const auto &a = *static_cast<GateArguments *>(arg);
    switch (a.operation) {
    case GateOperation::Quiesce:
        if (self->fRoleIsolated) return kIOReturnSuccess;
        if (self->fNub) {
            self->fNub->invalidate();
            // The adapter registered the vector using this nub, rather than
            // the AppleARMIODevice ancestor. Interrupt-controller ownership
            // checks require the same nub/source pair for masking it.
            self->fNub->disableInterrupt(0);
        }
        if (self->fCore->halt() != USB::Result::Success) return kIOReturnTimeout;
        if (self->fNub) self->fNub->isolated();
        return DWC3::setVbusOverride(*self->fScratch, false) == USB::Result::Success &&
            !self->fScratch->failed() ? kIOReturnSuccess : kIOReturnIOError;
    case GateOperation::RecordIsolation:
        self->fRoleIsolated=true;return kIOReturnSuccess;
    case GateOperation::Configure:
        if (self->stopping() || !self->acceptsConnection(self->fTarget)) return kIOReturnAborted;
        if (self->fNub) return kIOReturnNotReady;
        // Platform Prepare may have asserted BCR to detach a former SMMU
        // root. Re-probe the actual role; no cached halt/event state survives.
        if (self->fRoleIsolated) {
            auto *fresh=new DWC3::Core<USB::MMIO>(*self->fRegisters);
            if (!fresh) return kIOReturnNoMemory;
            delete self->fCore;self->fCore=fresh;self->fRoleIsolated=false;
            if (fresh->probe(self->fCoreMap->getLength(),self->fContract.addressBits)!=USB::Result::Success ||
                fresh->halt()!=USB::Result::Success) return kIOReturnNotReady;
        }
        if (self->fCore->select(a.role)!=USB::Result::Success) return kIOReturnNotReady;
        return DWC3::setVbusOverride(*self->fScratch, a.role == USB::Role::Device) == USB::Result::Success &&
            !self->fScratch->failed() ? kIOReturnSuccess : kIOReturnIOError;
    case GateOperation::AdoptNub:
        if (self->stopping() || !self->acceptsConnection(self->fTarget)) return kIOReturnAborted;
        if (self->fNub || !a.nub) return kIOReturnNotReady;
        self->fNub = a.nub;
        return kIOReturnSuccess;
    case GateOperation::ArmTimer:
        return self->stopping() ? kIOReturnNotReady : self->fTimer->setTimeoutMS(10);
    case GateOperation::DisableTimer:
        self->fTimer->cancelTimeout();
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
USB::Result QcomUSB::configure(USB::Role role, const USB::ConnectorState &target) {
    USB::PlatformRequest request; request.action = USB::PlatformAction::Prepare;
    request.lease = fLease; request.next = role; request.connector = target;
    if (!acceptsConnection(target)) return USB::Result::Aborted;
    auto r = platform(request);
    if (r != kIOReturnSuccess) return platformResult(r);
    return platformResult(gated(GateOperation::Configure, role));
}
bool QcomUSB::retireNub() {
    if (!fHardwareOwned) return true;
    if (gated(GateOperation::Quiesce) != kIOReturnSuccess) return false;
    USB::PlatformRequest request;request.action=USB::PlatformAction::Isolate;request.lease=fLease;
    if (platform(request)!=kIOReturnSuccess || gated(GateOperation::RecordIsolation)!=kIOReturnSuccess) return false;
    auto *nub = fNub;
    if (!nub) return true;
    // Query the retained mapper outside both role/Family gates. This immutable
    // nub incarnation can permit cleanup retries only after actual root
    // removal, not merely the controller halt recorded by isolated().
    if (USB::IOMemoryAPI::retirementReady(fMapper)) nub->dmaRetired();
    // Never wait for a child stop while holding its work-loop gate. A native
    // Family stop and the adapter's completion worker both need that gate.
    if (!nub->terminate(kIOServiceRequired | kIOServiceSynchronous) || nub->isOpen()) return false;
    if (nub->getProvider() == this) nub->detach(this);
    fNub = nullptr;
    nub->release();
    return true;
}
bool QcomUSB::stop(USB::Role, UInt64) { return retireNub(); }
USB::Result QcomUSB::start(USB::Role role, UInt64 epoch, const USB::ConnectorState &target) {
    USB::PlatformRequest request; request.action = USB::PlatformAction::Validate;
    request.lease = fLease; request.next = role; request.connector = target;
    if (!acceptsConnection(target)) return USB::Result::Aborted;
    auto status = platform(request);
    if (status != kIOReturnSuccess) return platformResult(status);
    auto *nub = new QcomUSBController;
    if (!nub) return USB::Result::NotReady;
    if (!nub->prepare(this, role, epoch, target) || !nub->attach(this)) { nub->release(); return USB::Result::NotReady; }
    status = gated(GateOperation::AdoptNub, role, nub);
    if (status != kIOReturnSuccess) {
        nub->detach(this); nub->release(); return platformResult(status);
    }
    nub->registerService();
    return USB::Result::Success;
}
void QcomUSB::timer(OSObject *owner, IOTimerEventSource *) {
    auto *self = static_cast<QcomUSB *>(owner);
    // The work item samples real connector hardware outside the gate.
    self->schedule();
}
bool QcomUSB::stopping() const {
    if (!fNotifyLock) return true;
    const auto state = IOSimpleLockLockDisableInterrupt(fNotifyLock);
    const bool result = fStopping;
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock, state);
    return result;
}
bool QcomUSB::acceptsConnection(const USB::ConnectorState &signal, UInt64 admissionEpoch) const {
    if (!fNotifyLock) return false;
    const auto state = IOSimpleLockLockDisableInterrupt(fNotifyLock);
    const bool accepted = fReady && !fStopping && fConnector.accepts(signal, admissionEpoch);
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock, state);
    return accepted;
}
UInt64 QcomUSB::connectionEpoch() const {
    const auto state = IOSimpleLockLockDisableInterrupt(fNotifyLock);
    const auto epoch = fConnector.epoch();
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock, state);
    return epoch;
}
void QcomUSB::disableScheduling() {
    if (!fNotifyLock) return;
    const auto state = IOSimpleLockLockDisableInterrupt(fNotifyLock);
    fReady = false; fStopping = true;
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock, state);
}
void QcomUSB::controllerFault(const USB::ConnectorState &route,UInt64 admissionEpoch) {
    if (!fNotifyLock) return;
    const auto state=IOSimpleLockLockDisableInterrupt(fNotifyLock);
    if (fReady && !fStopping && fWorker && fConnector.accepts(route,admissionEpoch)) {
        fConnector.notice(0);thread_call_enter(fWorker);
    }
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock,state);
}
void QcomUSB::schedule() {
    if (!fNotifyLock) return;
    const auto state = IOSimpleLockLockDisableInterrupt(fNotifyLock);
    if (fReady && !fStopping && fWorker) thread_call_enter(fWorker);
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock, state);
}
IOReturn QcomUSB::message(UInt32 type, IOService *provider, void *argument) {
    if (type != iokit_vendor_specific_msg(USB::ConnectorChanged))
        return IOService::message(type, provider, argument);
    if (!argument || !fNotifyLock) return kIOReturnBadArgument;
    const auto &notice = *static_cast<const USB::ConnectorNotice *>(argument);
    const auto state = IOSimpleLockLockDisableInterrupt(fNotifyLock);
    const bool valid = provider == fResources && notice.version == USB::PlatformVersion && notice.size == sizeof(notice) &&
        notice.lease && notice.lease == fLease && fReady && !fStopping && fWorker;
    if (valid) { fConnector.notice(notice.connection); thread_call_enter(fWorker); }
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock, state);
    return valid ? kIOReturnSuccess : kIOReturnNotReady;
}
void QcomUSB::work(thread_call_param_t arg, thread_call_param_t) {
    auto *self = static_cast<QcomUSB *>(arg);
    if (self->stopping()) return;
    USB::PlatformRequest request; request.action = USB::PlatformAction::Sample; request.lease = self->fLease;
    const auto status = self->platform(request);
    const auto lock = IOSimpleLockLockDisableInterrupt(self->fNotifyLock);
    const auto observed = self->fConnector.sample(request.connector, status == kIOReturnSuccess);
    const bool interrupted = self->fConnector.takeInterruption();
    IOSimpleLockUnlockEnableInterrupt(self->fNotifyLock, lock);
    UInt64 ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    // Losing the connector provider must remove source power and stop the
    // current controller; it must never retain a stale Host/source decision.
    auto signal = observed == USB::Result::Success ? request.connector : USB::ConnectorState{};
    if (observed == USB::Result::Invalid || status == kIOReturnBadArgument) signal.attached = true; // invalid => close/fault
    // Even if a transient observation fault recovered to the same physical
    // connection, a child may already have stopped on its closed admission
    // stamp. Drain/recreate it instead of reopening that old controller.
    auto result = self->fRoles->update(signal, ns / 1000, interrupted);
    if (self->fRoles->failed()) {
        // Teardown may have closed admission while this transition was in
        // flight. cleanup will prove halt and release, or pin the fault itself.
        if (!self->stopping()) self->pinFault();
        return;
    }
    self->setProperty("USBRoleEpoch", self->fRoles->epoch(), 64);
    self->setProperty("USBRole", self->fRoles->current() == USB::Role::Host ? "Host" :
        self->fRoles->current() == USB::Role::Device ? "Device" : "None");
    if (result == USB::Result::Success || result == USB::Result::Busy) self->gated(GateOperation::ArmTimer);
}
void QcomUSB::pinFault() {
    if (fNotifyLock) {
        const auto state=IOSimpleLockLockDisableInterrupt(fNotifyLock);
        fSleepUnsafe=true;IOSimpleLockUnlockEnableInterrupt(fNotifyLock,state);
    }
    if (!fPinned) { retain(); fPinned = true; }
    disableScheduling();
    if (fGateAdded && fTimerAdded) gated(GateOperation::DisableTimer);
    if (fHardwareOwned && fCore && fGateAdded) gated(GateOperation::Quiesce);
    setProperty("USBControllerFault", true);
    IOLog("QcomUSB: role transition failed; controller, DMA owner and platform lease retained\n");
}
IOReturn QcomUSB::refreshMapper() {
    auto *object=fPlatform->copyProperty("iommu-parent");
    IOMapper *mapper=nullptr;
    IOReturn result=kIOReturnSuccess;
    if (fContract.flags&USB::PlatformContract::DMABypass) {
        if (object) result=kIOReturnBadArgument;
    } else if (!OSDynamicCast(IOMapper,object)) result=kIOReturnNotReady;
    else {
        // The provider resolves and pins the actual object before returning
        // its contract, so this lookup never waits for an unresolved DT ID.
        mapper=IOMapper::copyMapperForDevice(fPlatform);
        if (!mapper || mapper!=object || mapper->isInactive()) result=kIOReturnNotReady;
    }
    OSSafeReleaseNULL(object);
    if (result==kIOReturnSuccess) { OSSafeReleaseNULL(fMapper);fMapper=mapper; }
    else OSSafeReleaseNULL(mapper);
    return result;
}
void QcomUSB::closePowerAdmission() {
    const auto state=IOSimpleLockLockDisableInterrupt(fNotifyLock);
    fReady=false;fConnector.notice(0);
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock,state);
    if (fGateAdded && fTimerAdded) gated(GateOperation::DisableTimer);
}
void QcomUSB::drainRoleWork() { if (fWorker) thread_call_cancel_wait(fWorker); }
USB::Result QcomUSB::parkRole() { return fRoles->suspend(); }
USB::Result QcomUSB::suspendResources() {
    // parkRole synchronously joined the old Family and its DMA workers.
    // A failed RPC can already have asserted reset or removed clocks.
    fHardwareOwned=false;
    USB::PlatformRequest request;request.action=USB::PlatformAction::Suspend;request.lease=fLease;
    IOReturn result=kIOReturnBusy;
    for (unsigned n=0;n<100 && result==kIOReturnBusy;++n) { result=platform(request);if (result==kIOReturnBusy) IOSleep(1); }
    if (result==kIOReturnSuccess) { fContract={};OSSafeReleaseNULL(fMapper); }
    return platformResult(result);
}
USB::Result QcomUSB::resumeResources() {
    USB::PlatformRequest request;request.action=USB::PlatformAction::Resume;request.lease=fLease;
    const auto result=platform(request);
    if (result!=kIOReturnSuccess) return result==kIOReturnBusy ? USB::Result::Busy : platformResult(result);
    if (!request.resources.valid() || fNub) return USB::Result::HardwareError;
    fContract=request.resources;
    if (refreshMapper()!=kIOReturnSuccess) return USB::Result::HardwareError;
    // No old Native controller, frame reader or DMA worker survives suspend.
    // Probe fresh views after the provider's reset; no old MMIO latch, event
    // pointer, MFINDEX epoch or USB device address crosses this boundary.
    delete fCore;fCore=nullptr;delete fRegisters;fRegisters=nullptr;delete fScratch;fScratch=nullptr;
    fRegisters=new USB::MMIO(reinterpret_cast<volatile void *>(fCoreMap->getVirtualAddress()),fCoreMap->getLength());
    fScratch=new USB::MMIO(reinterpret_cast<volatile void *>(fScratchMap->getVirtualAddress()),fScratchMap->getLength());
    if (!fRegisters || !fScratch) return USB::Result::HardwareError;
    fCore=new DWC3::Core<USB::MMIO>(*fRegisters);
    if (!fCore || fCore->probe(fCoreMap->getLength(),fContract.addressBits)!=USB::Result::Success)
        return USB::Result::HardwareError;
    fContract.addressBits=fCore->dmaBits();fHardwareOwned=true;
    return source(false)==USB::Result::Success && retireNub() ? USB::Result::Success : USB::Result::HardwareError;
}
USB::Result QcomUSB::restartRole() {
    const auto result=fRoles->resume();
    if (result!=USB::Result::Success) return result;
    const auto state=IOSimpleLockLockDisableInterrupt(fNotifyLock);
    fConnector=USB::ConnectorFence{};fTarget={};fReady=!fStopping && !fSystemSleeping && fPowerWanted;
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock,state);
    schedule();return USB::Result::Success;
}
void QcomUSB::powerFault() {
    pinFault();
    // The fault state holds its parent domain and vetoes subsequent sleep;
    // it never acknowledges an unknown teardown as safe power ownership.
    if (fPM) changePowerStateTo(2);
}
USB::Result QcomUSB::changePower(bool on) {
    if (!fPower || !fPowerLock) return USB::Result::NotReady;
    IOLockLock(fPowerLock);
    const auto lock=IOSimpleLockLockDisableInterrupt(fNotifyLock);
    const bool cancelled=fStopping || (on && (fSystemSleeping || !fPowerWanted));
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock,lock);
    const auto result=cancelled ? USB::Result::Aborted : on ? fPower->resume() : fPower->suspend();
    const char *state=fPower->failed() ? "Fault" : fPower->suspended() ? "Suspended" : "Active";
    IOLockUnlock(fPowerLock);
    setProperty("USBPowerState",state);
    return result;
}
IOReturn QcomUSB::setPowerState(unsigned long state,IOService *) {
    if (state>2) return kIOPMNoSuchState;
    if (!fNotifyLock || !fPower || !fPowerWorker) return IOPMAckImplied;
    const auto lock=IOSimpleLockLockDisableInterrupt(fNotifyLock);
    if (fStopping) { IOSimpleLockUnlockEnableInterrupt(fNotifyLock,lock);return IOPMAckImplied; }
    fPowerWanted=state!=0;
    const bool defer=state && fSystemSleeping;
    const bool asynchronous=!defer && state!=2;
    if (asynchronous) {
        // Native child termination can itself wait for IOKit PM. It must
        // never run inside the parent's setPowerState callout or gate.
        if (!state) { fReady=false;fConnector.notice(0); }
        fPowerAck=true;thread_call_enter(fPowerWorker);
    }
    IOSimpleLockUnlockEnableInterrupt(fNotifyLock,lock);
    return asynchronous ? 30000000 : IOPMAckImplied;
}
IOReturn QcomUSB::sleepInterest(void *target,void *,UInt32 type,IOService *,void *argument,vm_size_t size) {
    auto *self=static_cast<QcomUSB *>(target);
    if (!self->fNotifyLock) return kIOReturnNotReady;
    if (type==kIOMessageCanSystemSleep || type==kIOMessageSystemWillSleep) {
        if (!argument || (size && size<sizeof(sleepWakeNote))) return kIOReturnBadArgument;
        auto &note=*static_cast<sleepWakeNote *>(argument);note.returnValue=0;
        const auto lock=IOSimpleLockLockDisableInterrupt(self->fNotifyLock);
        const bool refused=self->fSleepUnsafe || self->fStopping || !self->fPowerWorker || self->fSleepReference;
        if (!refused && type==kIOMessageSystemWillSleep) {
            self->fSystemSleeping=true;self->fReady=false;self->fConnector.notice(0);
            self->fSleepReference=note.powerRef;note.returnValue=30000000;
            thread_call_enter(self->fPowerWorker);
        }
        IOSimpleLockUnlockEnableInterrupt(self->fNotifyLock,lock);
        if (refused) vetoSleepWakeNotification(note.powerRef);
        else if (type==kIOMessageCanSystemSleep) acknowledgeSleepWakeNotification(note.powerRef);
        return kIOReturnSuccess;
    }
    if (type==kIOMessageSystemWillNotSleep || type==kIOMessageSystemHasPoweredOn) {
        const auto lock=IOSimpleLockLockDisableInterrupt(self->fNotifyLock);
        self->fSystemSleeping=false;
        if (!self->fStopping && self->fPowerWorker) thread_call_enter(self->fPowerWorker);
        IOSimpleLockUnlockEnableInterrupt(self->fNotifyLock,lock);return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}
void QcomUSB::powerWork(void *context,void *) {
    auto *self=static_cast<QcomUSB *>(context);
    const auto lock=IOSimpleLockLockDisableInterrupt(self->fNotifyLock);
    void *reference=self->fSleepReference;self->fSleepReference=nullptr;
    const bool powerAck=self->fPowerAck;
    const bool on=self->fPowerWanted && !self->fSystemSleeping;
    IOSimpleLockUnlockEnableInterrupt(self->fNotifyLock,lock);
    auto result=USB::Result::NotReady;
    for (unsigned n=0;n<100;++n) {
        result=self->changePower(on);
        if (!on || (result!=USB::Result::NotReady && result!=USB::Result::Busy)) break;
        IOSleep(20); // PMIC/connector parent restoration, with no gate held
    }
    if (reference) {
        if (result==USB::Result::Success) acknowledgeSleepWakeNotification(reference);
        else vetoSleepWakeNotification(reference);
    }
    if (powerAck) {
        const auto lock=IOSimpleLockLockDisableInterrupt(self->fNotifyLock);
        self->fPowerAck=false;
        IOSimpleLockUnlockEnableInterrupt(self->fNotifyLock,lock);
        self->acknowledgeSetPowerState();
    }
    if (result!=USB::Result::Success && result!=USB::Result::Aborted) self->powerFault();
}
IOWorkLoop *QcomUSB::getWorkLoop() const { return fLoop; }
bool QcomUSB::requestTerminate(IOService *, IOOptionBits) { return !fHardwareOwned && !fLease && !fPinned; }
bool QcomUSB::start(IOService *provider) {
    // The resource producer matches in another category on this same nub.
    // IOResourceMatch would block its configuration thread before that
    // category can start. The producer rematches the nub after publication.
    if (!getResourceService()->getProperty(USB::PlatformResource)) {
        provider->setProperty("USBControllerWait","PlatformResources");return false;
    }
    if (!IOService::start(provider)) return false;
    provider->setProperty("USBControllerWait","ResourceAcquire");
    fPlatform = provider; fPlatform->retain();
    if (!provider->open(this)) goto fail;
    fOpened = true;
    fNotifyLock = IOSimpleLockAlloc();
    fPowerLock=IOLockAlloc();
    if (!fNotifyLock || !fPowerLock) goto fail;
    fFunction = OSSymbol::withCString(USB::PlatformFunction);
    if (!fFunction || provider->getResources() != kIOReturnSuccess) goto fail;
    {
        OSObject *object = getResourceService()->copyProperty(USB::PlatformResource);
        fResources = OSDynamicCast(IOService, object);
        if (!fResources) { OSSafeReleaseNULL(object); goto fail; }
    }
    {
        USB::PlatformRequest request; request.action = USB::PlatformAction::Acquire;
        const auto acquired = platform(request);
        // Record any returned lease even on an invalid response, so failure
        // cleanup can return it instead of silently dropping resource holds.
        const auto state = IOSimpleLockLockDisableInterrupt(fNotifyLock);
        fLease = request.lease; fContract = request.resources;
        IOSimpleLockUnlockEnableInterrupt(fNotifyLock, state);
        if (acquired != kIOReturnSuccess || !fLease || !fContract.valid()) goto fail;
    }
    if (refreshMapper()!=kIOReturnSuccess) goto fail;
    // Platform DT exposes core first, QSCRATCH second, and the core interrupt
    // as source0. Reject swapped apertures before any register write.
    {
        const bool ownedSMMU=provider->getProperty("qcom,usb-smmu-handoff")!=nullptr;
        const bool bypass=provider->getProperty(SMMU::BypassHandoffProperty)!=nullptr;
        const bool physical=provider->getProperty(USB::FirmwarePhysical32Property)!=nullptr;
        if ((ownedSMMU && (bypass || physical)) || (bypass && physical) ||
            provider->getDeviceMemoryCount() != ((ownedSMMU || bypass || physical) ? 3u:2u)) goto fail;
        auto *controllers = OSDynamicCast(OSArray, provider->getProperty("IOInterruptControllers"));
        auto *specifiers = OSDynamicCast(OSArray, provider->getProperty("IOInterruptSpecifiers"));
        if (!controllers || !specifiers || controllers->getCount()!=specifiers->getCount()) goto fail;
        if (ownedSMMU) {
            SMMU::ContextHandoff handoff;
            if (!QcomUSBMapper::configuration(provider,fContract.addressBits,handoff)) goto fail;
        } else if(bypass) {
            SMMU::BypassHandoff handoff;
            if(!QcomUSBMapper::bypassConfiguration(provider,fContract.addressBits,handoff))goto fail;
        } else if (controllers->getCount()!=1 ||
            (physical && !QcomUSBMapper::firmwarePhysicalConfiguration(provider,fContract.addressBits))) goto fail;
    }
    fCoreMap = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    fScratchMap = provider->mapDeviceMemoryWithIndex(1, kIOMapInhibitCache);
    if (!fCoreMap || !fScratchMap || fCoreMap->getPhysicalAddress() != 0x0a800000 ||
        fCoreMap->getLength() < DWC3::Aperture || fScratchMap->getPhysicalAddress() != 0x0a8f8800 ||
        fScratchMap->getLength() < 0x400) goto fail;
    fRegisters = new USB::MMIO(reinterpret_cast<volatile void *>(fCoreMap->getVirtualAddress()), fCoreMap->getLength());
    fScratch = new USB::MMIO(reinterpret_cast<volatile void *>(fScratchMap->getVirtualAddress()), fScratchMap->getLength());
    if (!fRegisters || !fScratch) goto fail;
    fCore = new DWC3::Core<USB::MMIO>(*fRegisters);
    fRoles = new USB::RoleSwitch<QcomUSB>(*this);
    fPower = new USB::ControllerPower<QcomUSB>(*this);
    fLoop = IOWorkLoop::workLoop();
    fGate = IOCommandGate::commandGate(this);
    fTimer = IOTimerEventSource::timerEventSource(this, timer);
    {
        auto *worker = thread_call_allocate_with_options(work, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
        fPowerWorker=thread_call_allocate_with_options(powerWork,this,THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
        const auto state = IOSimpleLockLockDisableInterrupt(fNotifyLock);
        fWorker = worker;
        IOSimpleLockUnlockEnableInterrupt(fNotifyLock, state);
    }
    if (!fCore || !fRoles || !fPower || !fLoop || !fGate || !fTimer || !fWorker || !fPowerWorker) goto fail;
    if (fLoop->addEventSource(fGate) != kIOReturnSuccess) goto fail;
    fGateAdded = true;
    if (fLoop->addEventSource(fTimer) != kIOReturnSuccess) goto fail;
    fTimerAdded = true;
    if (fCore->probe(fCoreMap->getLength(), fContract.addressBits) != USB::Result::Success) goto fail;
    fContract.addressBits = fCore->dmaBits();
    fHardwareOwned = true;
    if (source(false) != USB::Result::Success || !retireNub() ||
        safePower(USB::Role::None, {}) != USB::Result::Success) goto fail;
    registerService();
    {
        // Acquire may have enabled connector notifications before the gate,
        // core and worker were ready. Admit them only now; the initial sample
        // below also catches any change that arrived during initialization.
        const auto state = IOSimpleLockLockDisableInterrupt(fNotifyLock);
        fReady = true;
        IOSimpleLockUnlockEnableInterrupt(fNotifyLock, state);
    }
    {
        IOPMPowerState states[3]{};
        for (auto &state:states) state.version=1;
        states[0].inputPowerRequirement=kIOPMPowerOn;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        states[2].capabilityFlags=kIOPMDeviceUsable|kIOPMPreventSystemSleep;
        states[1].outputPowerCharacter=states[1].inputPowerRequirement=kIOPMPowerOn;
        states[2].outputPowerCharacter=states[2].inputPowerRequirement=kIOPMPowerOn;
        PMinit();fPM=true;provider->joinPMtree(this);
        if (registerPowerDriver(this,states,3)!=kIOReturnSuccess) goto fail;
        fSleepNotifier=registerPrioritySleepWakeInterest(sleepInterest,this);
        if (!fSleepNotifier) goto fail;
        setProperty("USBPowerPolicy","ColdSuspend");setProperty("USBRemoteWakeSupported",false);
        changePowerStateTo(1);
    }
    schedule();
    provider->setProperty("USBControllerWait","Ready");
    return true;
fail:
    provider->setProperty("USBControllerWait","StartFailed");
    IOLog("QcomUSB: platform acquisition or controller initialization failed\n");
    cleanup(); IOService::stop(provider); return false;
}
bool QcomUSB::cleanup() {
    disableScheduling();
    if (fSleepNotifier) { fSleepNotifier->remove();fSleepNotifier=nullptr; }
    if (fPowerWorker) thread_call_cancel_wait(fPowerWorker);
    if (fPowerAck) { fPowerAck=false;acknowledgeSetPowerState(); }
    if (fPM) { PMstop();fPM=false; }
    if (fSleepReference) { vetoSleepWakeNotification(fSleepReference);fSleepReference=nullptr; }
    if (fGateAdded && fTimerAdded) gated(GateOperation::DisableTimer);
    if (fWorker) thread_call_cancel_wait(fWorker);
    // Retain all resources if source removal or hardware isolation failed.
    if (fHardwareOwned) {
        const bool off=source(false)==USB::Result::Success;
        const bool idle=retireNub();
        if (!off || !idle) { pinFault();return false; }
    }
    // Retirement proved DMA idle and drained the old Family. Release may now
    // reset the core or remove clocks even when later cleanup fails. Never
    // re-enter MMIO from pinFault after that ownership transfer starts.
    fHardwareOwned = false;
    if (fLease) {
        USB::PlatformRequest request; request.action = USB::PlatformAction::Release; request.lease = fLease;
        IOReturn released=kIOReturnBusy;
        for (unsigned attempt=0;attempt<100;++attempt) {
            released=platform(request);
            if (released!=kIOReturnBusy) break;
            IOSleep(1); // a notification in flight retains the same lease
        }
        if (released != kIOReturnSuccess) { pinFault(); return false; }
        fLease = 0;
    }
    fHardwareOwned = false;
    if (fWorker) { thread_call_free(fWorker); fWorker = nullptr; }
    if (fPowerWorker) { thread_call_free(fPowerWorker);fPowerWorker=nullptr; }
    if (fTimerAdded) { fLoop->removeEventSource(fTimer); fTimerAdded = false; }
    if (fGateAdded) { fLoop->removeEventSource(fGate); fGateAdded = false; }
    OSSafeReleaseNULL(fTimer); OSSafeReleaseNULL(fGate); OSSafeReleaseNULL(fLoop);
    delete fRoles; fRoles = nullptr; delete fCore; fCore = nullptr;
    delete fPower;fPower=nullptr;
    delete fScratch; fScratch = nullptr; delete fRegisters; fRegisters = nullptr;
    OSSafeReleaseNULL(fCoreMap); OSSafeReleaseNULL(fScratchMap); OSSafeReleaseNULL(fMapper);
    if (fOpened) { fPlatform->close(this); fOpened = false; }
    OSSafeReleaseNULL(fFunction); OSSafeReleaseNULL(fResources); OSSafeReleaseNULL(fPlatform);
    if (fNotifyLock) { IOSimpleLockFree(fNotifyLock); fNotifyLock = nullptr; }
    if (fPowerLock) { IOLockFree(fPowerLock);fPowerLock=nullptr; }
    return true;
}
void QcomUSB::stop(IOService *provider) { cleanup(); IOService::stop(provider); }
void QcomUSB::free() { if (!cleanup()) return; IOService::free(); }
