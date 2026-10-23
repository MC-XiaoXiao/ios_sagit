// SPDX-License-Identifier: BSD-2-Clause
#include "QcomWLAN.h"
#include "QcomWLANUserClient.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>

#define super IOService
OSDefineMetaClassAndStructors(QcomWLAN, super);

bool QcomWLAN::validProvider(IOService *provider) {
    if (!provider || !provider->metaCast("AppleARMIODevice")) return false;
    OSString *name = OSString::withCString("qcom,wcn3990-wifi");
    if (!name) return false;
    const bool matches = IODTCompareNubName(provider,name,static_cast<OSString **>(nullptr));
    name->release();
    if (!matches || provider->getDeviceMemoryCount() != 1) return false;
    IOMemoryDescriptor *memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory || memory->getPhysicalAddress() != WLAN::RegisterBase ||
        memory->getLength() != WLAN::RegisterSize) return false;
    auto *controllers = OSDynamicCast(OSArray,provider->getProperty("IOInterruptControllers"));
    auto *specifiers = OSDynamicCast(OSArray,provider->getProperty("IOInterruptSpecifiers"));
    if (!controllers || !specifiers || controllers->getCount() != WLAN::CopyEngineCount ||
        specifiers->getCount() != WLAN::CopyEngineCount) return false;
    // The native interrupt controller validates actual vectors when a future
    // backend registers its event sources. Reject malformed lists now.
    for (unsigned i = 0; i < WLAN::CopyEngineCount; ++i) {
        auto *controller = OSDynamicCast(OSSymbol,controllers->getObject(i));
        auto *specifier = OSDynamicCast(OSData,specifiers->getObject(i));
        if (!controller || !specifier || !specifier->getLength()) return false;
    }
    return true;
}

IOService *QcomWLAN::probe(IOService *provider, SInt32 *score) {
    if (!super::probe(provider,score) || !validProvider(provider)) return nullptr;
    return this;
}

bool QcomWLAN::start(IOService *provider) {
    if (!super::start(provider)) return false;
    if (!validProvider(provider)) goto fail;
    {
        const auto copyData = [](IOService *service,const char *name) {
            auto *object = service->copyProperty(name);
            auto *data = OSDynamicCast(OSData,object);
            if (object && !data) object->release();
            return data;
        };
        fBoard = copyData(provider,"wlan-board-data");
        fFirmwareConfiguration = copyData(provider,"wlan-firmware-configuration");
    }
    if (fBoard && fFirmwareConfiguration && fFirmwareAssets.bind(fFirmwareConfiguration->getBytesNoCopy(),
            fFirmwareConfiguration->getLength(),fBoard->getBytesNoCopy(),fBoard->getLength()) == WLAN::Result::Success)
        fMissing &= ~WLAN::BoardData;
    fWorkLoop = IOWorkLoop::workLoop();
    fGate = IOCommandGate::commandGate(this);
    if (!fWorkLoop || !fGate || fWorkLoop->addEventSource(fGate) != kIOReturnSuccess) goto fail;
    fLifecycle.attach(); fActive = true;
    setProperty("WLANTransportVersion",WLAN::ProtocolVersion,32);
    setProperty("WLANChipset","WCN3990");
    setProperty("WLANBus","SNOC");
    setProperty("WLANDMAAddressBits",WLAN::DMAAddressBits,32);
    setProperty("WLANNativeInterfaceReady",false);
    setProperty("WLANHardwareAccessEnabled",false);
    publishState();
    {
        static IOPMPowerState states[2] = {
            {1,0,0,0,0,0,0,0,0,0,0,0},
            {1,IOPMDeviceUsable,IOPMPowerOn,IOPMPowerOn,0,0,0,0,0,0,0,0}
        };
        PMinit(); fPMInitialized = true;
        provider->joinPMtree(this);
        if (registerPowerDriver(this,states,2) != kIOReturnSuccess) goto fail;
    }
    // Publish the transport for diagnostics and DriverKit matching. No BSD
    // interface, IRQ, register mapping, or DMA allocation is published here.
    registerService();
    return true;
fail:
    cleanup(); super::stop(provider); return false;
}

void QcomWLAN::publishState() {
    setProperty("WLANState",WLAN::stateName(fLifecycle.currentState()));
    setProperty("WLANTransportReady",fLifecycle.currentState() == WLAN::State::TransportReady);
    setProperty("WLANMissingRequirements",fMissing,32);
    setProperty("WLANResourceMask",fLifecycle.resources(),32);
    setProperty("WLANGeneration",fLifecycle.epoch(),64);
}

IOReturn QcomWLAN::perform(uint32_t selector, uint64_t *output, uint64_t value) {
    if (!fActive || isInactive()) return kIOReturnNotAttached;
    if (selector == WLAN::GetInfo) {
        if (!output) return kIOReturnBadArgument;
        output[0] = WLAN::ProtocolVersion; output[1] = WLAN::CopyEngineCount;
        output[2] = WLAN::DMAAddressBits; output[3] = WLAN::RegisterBase;
        output[4] = WLAN::RegisterSize; output[5] = fMissing;
    } else if (selector == WLAN::GetStatus) {
        if (!output) return kIOReturnBadArgument;
        output[0] = uint32_t(fLifecycle.currentState()); output[1] = fLifecycle.resources();
        output[2] = fLifecycle.epoch(); output[3] = uint32_t(fLifecycle.lastResult());
        output[4] = fMissing; output[5] = 0; // native interface is not registered
    } else if (selector == WLAN::SetPower) {
        if (value > 1) return kIOReturnBadArgument;
        const auto result = value ? fLifecycle.start() : fLifecycle.stop();
        publishState();
        switch (result) {
        case WLAN::Result::Success: return kIOReturnSuccess;
        case WLAN::Result::NotReady: return kIOReturnNotReady;
        case WLAN::Result::Busy: return kIOReturnBusy;
        case WLAN::Result::Timeout: return kIOReturnTimeout;
        default: return kIOReturnError;
        }
    } else return kIOReturnUnsupported;
    return kIOReturnSuccess;
}

IOReturn QcomWLAN::gatedRequest(OSObject *owner, void *selector, void *output, void *value, void *) {
    return static_cast<QcomWLAN *>(owner)->perform(uint32_t(reinterpret_cast<uintptr_t>(selector)),
        static_cast<uint64_t *>(output),reinterpret_cast<uintptr_t>(value));
}

IOReturn QcomWLAN::request(uint32_t selector, uint64_t *output, uint64_t value) {
    // The gate and workloop live until free(). Clients retain this service,
    // so stop() can close admission without racing a gate-pointer release.
    return fGate ? fGate->runAction(gatedRequest,reinterpret_cast<void *>(uintptr_t(selector)),
        output,reinterpret_cast<void *>(uintptr_t(value))) : kIOReturnNotAttached;
}

IOReturn QcomWLAN::setPowerState(unsigned long state, IOService *) {
    if (state > 1) return kIOReturnBadArgument;
    // Initial PM only quiesces. An On notification must not initiate firmware
    // boot before real platform ownership, calibration and mapper are bound.
    if (!state) request(WLAN::SetPower,nullptr,0);
    return IOPMAckImplied;
}
IOWorkLoop *QcomWLAN::getWorkLoop() const { return fWorkLoop; }

IOReturn QcomWLAN::newUserClient(task_t task, void *token, UInt32 type, OSDictionary *properties, IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    if (type != WLAN::DiagnosticClient) return kIOReturnUnsupported;
    if (IOUserClient::clientHasPrivilege(task,kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    if (isInactive()) return kIOReturnNotAttached;
    auto *client = new QcomWLANUserClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task,token,type,properties)) { client->release(); return kIOReturnError; }
    if (!client->attach(this)) { client->release(); return kIOReturnNotAttached; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnNotReady; }
    *handler = client; return kIOReturnSuccess;
}

void QcomWLAN::cleanup() {
    if (fPMInitialized) { PMstop(); fPMInitialized = false; }
    if (fGate && fGate->getWorkLoop()) fGate->runAction(gatedStop);
}
IOReturn QcomWLAN::gatedStop(OSObject *owner, void *, void *, void *, void *) {
    auto *controller = static_cast<QcomWLAN *>(owner);
    controller->fActive = false;
    // The initial backend cannot own any resource. Future hardware bindings
    // must reject termination or retain their owner if isolation fails.
    controller->fLifecycle.detach(); controller->publishState();
    return kIOReturnSuccess;
}
void QcomWLAN::stop(IOService *provider) { cleanup(); super::stop(provider); }
void QcomWLAN::free() {
    cleanup();
    if (fGate && fWorkLoop && fGate->getWorkLoop()) fWorkLoop->removeEventSource(fGate);
    OSSafeReleaseNULL(fGate); OSSafeReleaseNULL(fWorkLoop);
    OSSafeReleaseNULL(fBoard); OSSafeReleaseNULL(fFirmwareConfiguration);
    super::free();
}
