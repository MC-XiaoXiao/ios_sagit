// SPDX-License-Identifier: GPL-2.0-only
#include "QcomBaseband.h"
#include "BasebandFirmware.h"
#include "BasebandQ6.h"
#include "BasebandNative.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>

OSDefineMetaClassAndStructors(QcomBaseband, IOService);
OSDefineMetaClassAndStructors(QcomBasebandUserClient, IOUserClient);

struct QcomBaseband::FirmwareState {
    OSData *mba = nullptr, *modem = nullptr;
    Baseband::FirmwareImage image, mbaImage;
    Baseband::MemoryHandoff handoff;
    bool native = false;
    ~FirmwareState() { OSSafeReleaseNULL(mba); OSSafeReleaseNULL(modem); }
    bool load(IOService *provider) {
        const auto r = Baseband::readMemoryHandoff(handoff);
        if (r == Baseband::Result::OK) {
            native = Baseband::copyFirmware(handoff, mba, modem) == Baseband::Result::OK;
            return native && image.parse(modem->getBytesNoCopy(), modem->getLength()) == Baseband::Result::OK &&
                mbaImage.parse(mba->getBytesNoCopy(), mba->getLength()) == Baseband::Result::OK;
        }
        if (r != Baseband::Result::NotReady) return false;
        auto *m = OSDynamicCast(OSData, provider->getProperty("baseband-modem-image"));
        auto *b = OSDynamicCast(OSData, provider->getProperty("baseband-mba-image"));
        if (!m || !b || !m->getBytesNoCopy() || !b->getBytesNoCopy() ||
            !m->getLength() || m->getLength() > 256 * 1024 * 1024 ||
            !b->getLength() || b->getLength() > 2 * 1024 * 1024)
            return false;
        // Copy untrusted provider buffers before validation so later property
        // replacement cannot change authenticated metadata or segment bytes.
        modem = OSData::withBytes(m->getBytesNoCopy(), m->getLength());
        mba = OSData::withBytes(b->getBytesNoCopy(), b->getLength());
        return modem && mba && image.parse(modem->getBytesNoCopy(), modem->getLength())
            == Baseband::Result::OK &&
            mbaImage.parse(mba->getBytesNoCopy(), mba->getLength()) == Baseband::Result::OK;
    }
};
bool QcomBaseband::validProvider(IOService *provider) {
    if (!provider || !provider->metaCast("AppleARMIODevice") ||
        (provider->getDeviceMemoryCount() != 2 && provider->getDeviceMemoryCount() != 4)) return false;
    auto *name = OSString::withCString("qcom,msm8998-mss-pil");
    if (!name) return false;
    const bool match = IODTCompareNubName(provider, name, static_cast<OSString **>(nullptr));
    name->release();
    if (!match) return false;
    const uint64_t bases[] = {Baseband::Q6::Base, Baseband::Q6::RMBBase, 0x17911000, 0x01f60000};
    const uint64_t sizes[] = {4096, 4096, 4096, 0x20000};
    for (unsigned i = 0; i < provider->getDeviceMemoryCount(); ++i) {
        auto *memory = provider->getDeviceMemoryWithIndex(i);
        if (!memory || memory->getPhysicalAddress() != bases[i] ||
            memory->getLength() != sizes[i]) return false;
    }
    return true;
}
IOService *QcomBaseband::probe(IOService *provider, SInt32 *score) {
    return IOService::probe(provider, score) && validProvider(provider) ? this : nullptr;
}
bool QcomBaseband::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    if (!validProvider(provider)) goto fail;
    fLoop = IOWorkLoop::workLoop(); fGate = IOCommandGate::commandGate(this);
    fFirmware = new FirmwareState;
    fTimer = IOTimerEventSource::timerEventSource(this, nativeTimer);
    if (!fLoop || !fGate || !fFirmware || !fTimer || fLoop->addEventSource(fGate) != kIOReturnSuccess ||
        fLoop->addEventSource(fTimer) != kIOReturnSuccess)
        goto fail;
    if (fFirmware->load(provider)) fMissing &= ~Baseband::Firmware;
    setProperty("BasebandProtocolVersion", Baseband::ProtocolVersion, 32);
    setProperty("BasebandChipset", "MSM8998-integrated-Q6");
    setProperty("BasebandTransport", "SMEM-GLINK-QRTR");
    setProperty("BasebandState", "offline");
    setProperty("BasebandMissingRequirements", fMissing, 32);
    setProperty("BasebandHardwareReady", false);
    setProperty("BasebandTelephonyReady", false);
    // Native Apple matching and control are not yet implemented. Keep the
    // provider's modem name; renaming it to baseband would start CommCenter.
    fActive = true;
    if (fFirmware->native && fFirmware->image.valid() && fFirmware->mbaImage.valid())
        fGate->runAction(gatedNativeStart);
    registerService();
    return true;
fail:
    cleanup(); IOService::stop(provider); return false;
}
IOReturn QcomBaseband::gatedStatus(OSObject *object, void *request, void *buffer, void *, void *) {
    auto *self = static_cast<QcomBaseband *>(object);
    if (!self->fActive || self->isInactive()) return kIOReturnNotAttached;
    const auto selector = reinterpret_cast<uintptr_t>(request);
    if (selector >= Baseband::SelectorCount) return kIOReturnUnsupported;
    auto *out = static_cast<uint64_t *>(buffer);
    if (!out) return kIOReturnBadArgument;
    for (size_t i = 0; i < Baseband::StatusScalars; ++i) out[i] = 0;
    out[0] = Baseband::ProtocolVersion;
    if (selector == Baseband::GetInfo) {
        out[1] = Baseband::Q6::Base; out[2] = Baseband::Q6::RMBBase;
        out[3] = 1; // SMEM remote PID.
        out[4] = self->fFirmware->image.segmentCount();
        out[5] = self->fFirmware->image.memorySpan();
    } else out[1] = uint32_t(self->fNative ? self->fNative->state() : Baseband::State::Offline);
    out[6] = self->fMissing;
    out[7] = self->fNative ? self->fNative->generation() : 0;
    return kIOReturnSuccess;
}
IOReturn QcomBaseband::status(uint32_t selector, uint64_t *output) {
    return fGate ? fGate->runAction(gatedStatus,
        reinterpret_cast<void *>(uintptr_t(selector)), output) : kIOReturnNotAttached;
}
IOReturn QcomBaseband::gatedStop(OSObject *object, void *, void *, void *, void *) {
    auto *self = static_cast<QcomBaseband *>(object);
    self->fActive = false;
    if (self->fTimer) self->fTimer->cancelTimeout();
    if (self->fNative) self->fNative->stop();
    self->nativeStatus(); return kIOReturnSuccess;
}
IOReturn QcomBaseband::gatedNativeStart(OSObject *object, void *, void *, void *, void *) {
    auto *self = static_cast<QcomBaseband *>(object);
    self->fNative = new Baseband::NativeController;
    if (!self->fNative) return kIOReturnNoMemory;
    const Baseband::NativeActions actions{nativeIRQ, nativeIRQ, nativeIRQ, nativeWatchdog};
    auto r = self->fNative->bind(self, self->fLoop, self->fFirmware->handoff,
                               self->fFirmware->mba, self->fFirmware->modem, actions);
    if (r == Baseband::Result::OK) r = self->fNative->start();
    self->setProperty("BasebandBootResult", uint32_t(r), 32);
    self->nativeStatus();
    if (self->fNative->state() == Baseband::State::Running) self->fTimer->setTimeoutMS(1);
    return kIOReturnSuccess;
}
bool QcomBaseband::nativeWatchdog(OSObject *object, IOFilterInterruptEventSource *) {
    auto *self = static_cast<QcomBaseband *>(object);
    if (self->fNative) self->fNative->watchdog();
    return true;
}
void QcomBaseband::nativeIRQ(OSObject *object, IOInterruptEventSource *, int) {
    static_cast<QcomBaseband *>(object)->nativeProgress();
}
void QcomBaseband::nativeTimer(OSObject *object, IOTimerEventSource *) {
    static_cast<QcomBaseband *>(object)->nativeProgress();
}
void QcomBaseband::nativeProgress() {
    if (!fActive || !fNative) return;
    const auto r = fNative->poll();
    if (r != Baseband::Result::OK && r != Baseband::Result::Busy && r != Baseband::Result::NotReady)
        setProperty("BasebandRuntimeResult", uint32_t(r), 32);
    nativeStatus();
    if (fNative->state() == Baseband::State::Running) fTimer->setTimeoutMS(1);
}
void QcomBaseband::nativeStatus() {
    const auto state = fNative ? fNative->state() : Baseband::State::Offline;
    if (fNative) fMissing = fNative->missing();
    if (fFirmware && fFirmware->image.valid() && fFirmware->mbaImage.valid()) fMissing &= ~Baseband::Firmware;
    const bool quarantined = fNative && fNative->quarantined();
    if (fReportedState == uint32_t(state) && fReportedMissing == fMissing && fReportedQuarantined == quarantined) return;
    fReportedState = uint32_t(state); fReportedMissing = fMissing; fReportedQuarantined = quarantined;
    const char *names[] = {"offline", "prepared", "mba-starting", "mba-ready", "metadata", "loading",
                           "authenticating", "waiting-ready", "running", "stopping", "faulted"};
    setProperty("BasebandState", names[uint32_t(state)]);
    setProperty("BasebandMissingRequirements", fMissing, 32);
    setProperty("BasebandHardwareReady", state == Baseband::State::Running &&
        !(fMissing & ~(Baseband::AppleTelephony | Baseband::DataPlane)));
    setProperty("BasebandTelephonyReady", false);
    setProperty("BasebandNativeQuarantined", quarantined);
}
void QcomBaseband::cleanup() {
    fActive = false;
    if (fNative && fNative->close()) { delete fNative; fNative = nullptr; }
    delete fFirmware; fFirmware = nullptr;
}
void QcomBaseband::stop(IOService *provider) {
    if (fGate) fGate->runAction(gatedStop);
    cleanup(); IOService::stop(provider);
}
void QcomBaseband::free() {
    cleanup();
    if (fLoop && fTimer) fLoop->removeEventSource(fTimer);
    OSSafeReleaseNULL(fTimer);
    if (fLoop && fGate) fLoop->removeEventSource(fGate);
    OSSafeReleaseNULL(fGate); OSSafeReleaseNULL(fLoop); IOService::free();
}
IOWorkLoop *QcomBaseband::getWorkLoop() const { return fLoop; }
bool QcomBaseband::requestTerminate(IOService *provider, IOOptionBits options) {
    return (!fNative || !fNative->quarantined()) && IOService::requestTerminate(provider, options);
}
IOReturn QcomBaseband::newUserClient(task_t task, void *token, UInt32 type,
                                    OSDictionary *properties, IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    auto *client = new QcomBasebandUserClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, token, type, properties)) {
        client->release(); return kIOReturnNotPrivileged;
    }
    if (!client->attach(this)) { client->release(); return kIOReturnError; }
    if (!client->start(this)) {
        client->detach(this); client->release(); return kIOReturnNotAttached;
    }
    *handler = client; return kIOReturnSuccess;
}
bool QcomBasebandUserClient::initWithTask(task_t task, void *token, UInt32 type,
                                        OSDictionary *properties) {
    return type == Baseband::DiagnosticClient &&
        clientHasPrivilege(task, kIOClientPrivilegeAdministrator) == kIOReturnSuccess &&
        IOUserClient::initWithTask(task, token, type, properties);
}
bool QcomBasebandUserClient::start(IOService *provider) {
    auto *controller = OSDynamicCast(QcomBaseband, provider);
    if (!controller || !IOUserClient::start(provider)) return false;
    fController = controller; fController->retain(); return true;
}
void QcomBasebandUserClient::free() {
    OSSafeReleaseNULL(fController); IOUserClient::free();
}
IOReturn QcomBasebandUserClient::clientClose() { terminate(); return kIOReturnSuccess; }
IOReturn QcomBasebandUserClient::externalMethod(UInt32 selector, IOExternalMethodArguments *a,
                                               IOExternalMethodDispatch *, OSObject *, void *) {
    if (!fController || isInactive()) return kIOReturnNotAttached;
    if (!a || !Baseband::validCall(selector, a->scalarInputCount, a->scalarOutputCount) ||
        !a->scalarOutput || a->structureInputSize || a->structureOutputSize ||
        a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncReferenceCount)
        return kIOReturnBadArgument;
    return fController->status(selector, a->scalarOutput);
}
