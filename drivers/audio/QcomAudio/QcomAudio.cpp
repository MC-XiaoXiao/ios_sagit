// SPDX-License-Identifier: BSD-2-Clause
#include "QcomAudio.h"
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <kern/clock.h>

OSDefineMetaClassAndAbstractStructors(AudioOutputBackend, IOService);
OSDefineMetaClassAndStructors(QcomAudio, IOAudio2Device);

namespace {
enum Operation { OpStart, OpStop, OpActive, OpRateChange, OpFormatChange, OpConfigChange, OpBuffer, OpSuspend, OpShutdown };
struct BufferRequest { UInt32 stream; IOOptionBits *options; IOMemoryDescriptor **memory; };
IOReturn ioResult(AudioOutput::Result r) {
    using AudioOutput::Result;
    switch (r) {
    case Result::OK: return kIOReturnSuccess;
    case Result::Invalid: return kIOReturnBadArgument;
    case Result::NotReady: return kIOReturnNotReady;
    case Result::Busy: return kIOReturnBusy;
    case Result::Stale: return kIOReturnNotReady;
    case Result::Timeout: return kIOReturnTimeout;
    case Result::Retained: return kIOReturnNotResponding;
    default: return kIOReturnIOError;
    }
}
bool number(OSDictionary *d, const char *key, uint64_t n, unsigned bits = 32) {
    auto *value = OSNumber::withNumber(n, bits);
    bool ok = value && d->setObject(key, value);
    OSSafeReleaseNULL(value);
    return ok;
}
OSDictionary *format(bool range) {
    auto *d = OSDictionary::withCapacity(9);
    if (!d) return nullptr;
    bool ok = range ? (number(d, "min sample rate", AudioOutput::Rate, 64) &&
                      number(d, "max sample rate", AudioOutput::Rate, 64)) :
                     number(d, "sample rate", AudioOutput::Rate, 64);
    ok = ok && number(d, "format ID", AudioOutput::PCM) &&
        number(d, "format flags", AudioOutput::PCMFlags) &&
        number(d, "bytes per packet", 4) && number(d, "frames per packet", 1) &&
        number(d, "bytes per frame", 4) && number(d, "channels per frame", 2) &&
        number(d, "bits per channel", 16);
    if (!ok) OSSafeReleaseNULL(d);
    return d;
}
}

bool QcomAudio::publishFormat() {
    auto *current = format(false), *available = format(true);
    auto *formats = OSArray::withCapacity(1), *streams = OSArray::withCapacity(1);
    auto *stream = OSDictionary::withCapacity(4);
    bool ok = current && available && formats && streams && stream &&
        formats->setObject(available) && number(stream, "stream ID", AudioOutput::StreamID) &&
        number(stream, "starting channel", 1) &&
        stream->setObject("current format", current) &&
        stream->setObject("available formats", formats) && streams->setObject(stream) &&
        setProperty("output streams", streams) &&
        setProperty("device UID", "org.darwin.audio.qcom.output") &&
        setProperty("device name", "Qualcomm Built-in Output") &&
        setProperty("device manufacturer", "Qualcomm") &&
        setProperty("transport type", UInt64(0x626c746e), 32) && // 'bltn'
        setProperty("sample rate", AudioOutput::Rate, 64) &&
        setProperty("io buffer frame size", UInt64(fConfig.ringFrames), 32) &&
        setProperty("output latency", UInt64(fConfig.latencyFrames), 32) &&
        setProperty("output safety offset", UInt64(fConfig.safetyFrames), 32);
    OSSafeReleaseNULL(stream); OSSafeReleaseNULL(streams); OSSafeReleaseNULL(formats);
    OSSafeReleaseNULL(available); OSSafeReleaseNULL(current);
    return ok;
}

bool QcomAudio::start(IOService *provider) {
    fBackend = OSDynamicCast(AudioOutputBackend, provider);
    if (!fBackend) return false;
    fBackend->retain();
    if (!fBackend->open(this)) { OSSafeReleaseNULL(fBackend); return false; }
    if (fBackend->outputConfiguration(fConfig) != AudioOutput::Result::OK ||
        !AudioOutput::validConfiguration(fConfig)) goto fail;
    fLoop = IOWorkLoop::workLoop();
    fGate = IOCommandGate::commandGate(this);
    fTimer = IOTimerEventSource::timerEventSource(this, tick);
    if (!fLoop || !fGate || !fTimer ||
        fLoop->addEventSource(fGate) != kIOReturnSuccess ||
        fLoop->addEventSource(fTimer) != kIOReturnSuccess) goto fail;
    // The real backend performs device-specific IODMACommand/IOMapper mapping.
    // Never assume that the CPU physical address is a DSP DMA address.
    fPCM = IOBufferMemoryDescriptor::withOptions(kIOMemoryKernelUserShared | kIODirectionOut,
                                               uint64_t(fConfig.ringFrames) * 4, PAGE_SIZE);
    if (!fPCM || !fPCM->getBytesNoCopy()) goto fail;
    bzero(fPCM->getBytesNoCopy(), fPCM->getLength());
    {
        auto result = fBackend->prepareOutput(fPCM, fConfig);
        if (result == AudioOutput::Result::Retained) {
            fPrepared = true; retain(); fPinned = true;
            goto fail;
        }
        if (result != AudioOutput::Result::OK) goto fail;
    }
    fPrepared = true;
    retain(); fPinned = true; // retain until all DMA mappings have been retired
    clock_interval_to_absolutetime_interval(250, kMillisecondScale, &fTimeout);
    if (!publishFormat() || !IOAudio2Device::start(provider)) goto fail;
    fNativeStarted = true;
    fStatus = getStatusBuffer();
    if (!fStatus) goto fail;
    bzero(fStatus, sizeof(*fStatus));
    {
        static IOPMPowerState states[2] = {
            {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            {1, IOPMDeviceUsable | IOPMPowerOn, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
        };
        PMinit(); fPM = true; provider->joinPMtree(this);
        if (registerPowerDriver(this, states, 2) != kIOReturnSuccess) goto fail;
    }
    fPublished = true;
    IOService::registerService();
    return true;
fail:
    fStopping = true;
    if (fPM) { PMstop(); fPM = false; }
    if (fNativeStarted) { IOAudio2Device::stop(provider); fNativeStarted = false; }
    releaseBackend();
    return false;
}

void QcomAudio::registerService(IOOptionBits options) {
    // Native start() registers immediately. Delay it until shared status, PM
    // and the backend are all initialized; no client sees a partial device.
    if (fPublished) IOService::registerService(options);
}
IOWorkLoop *QcomAudio::getWorkLoop() const { return fLoop; }

bool QcomAudio::releaseBackend() {
    if (fPrepared) {
        if (!fSession.quiescent() || fBackend->releaseOutput() != AudioOutput::Result::OK) {
            setProperty("AudioDMAResourcesRetained", true);
            return false;
        }
        fPrepared = false;
    }
    if (fBackend) { fBackend->close(this); OSSafeReleaseNULL(fBackend); }
    if (fPinned) { fPinned = false; release(); }
    return true;
}
void QcomAudio::stop(IOService *provider) {
    gate(OpShutdown);
    if (fPM) { PMstop(); fPM = false; }
    if (fNativeStarted) { IOAudio2Device::stop(provider); fNativeStarted = false; }
}
void QcomAudio::free() {
    // Failed drain/release keeps the explicit self-reference and cannot reach
    // this path. User mappings hold their own descriptor references.
    if (fTimer) fTimer->cancelTimeout();
    if (fLoop && fTimer) fLoop->removeEventSource(fTimer);
    if (fLoop && fGate) fLoop->removeEventSource(fGate);
    OSSafeReleaseNULL(fTimer); OSSafeReleaseNULL(fGate); OSSafeReleaseNULL(fLoop);
    OSSafeReleaseNULL(fPCM);
    if (fBackend) { fBackend->close(this); OSSafeReleaseNULL(fBackend); }
    IOAudio2Device::free();
}
IOReturn QcomAudio::gate(unsigned op, void *a, void *b) {
    return fGate ? fGate->runAction(action, reinterpret_cast<void *>(uintptr_t(op)), a, b) :
                   kIOReturnNotReady;
}
IOReturn QcomAudio::startIO() { return gate(OpStart); }
IOReturn QcomAudio::stopIO() { return gate(OpStop); }

IOReturn QcomAudio::startGated() {
    if (!fPrepared || !fStatus || fStopping || fSuspended || !fActive)
        return kIOReturnNotReady;
    if (fSession.state() == AudioOutput::State::Running) return kIOReturnSuccess;
    if (!fSession.quiescent()) return kIOReturnBusy;
    bzero(fPCM->getBytesNoCopy(), fPCM->getLength());
    auto result = fSession.start(*fBackend, mach_absolute_time, fTimeout);
    if (result != AudioOutput::Result::OK) return ioResult(result);
    // These three words are driver-owned; the two client positions are never
    // used for bounds, ownership, DMA addresses, or generation validation.
    fStatus->sampleTime = fStatus->hostTime = 0;
    __atomic_store_n(&fStatus->seed, fSession.generation(), __ATOMIC_RELEASE);
    auto ret = fTimer->setTimeoutUS((uint64_t(fConfig.periodFrames) * 1000000) / fConfig.rate);
    if (ret != kIOReturnSuccess) { fSession.stop(*fBackend); return ret; }
    return IOAudio2Device::startIO(); // native is-running property + Mach notification
}
IOReturn QcomAudio::stopGated() {
    if (fTimer) fTimer->cancelTimeout();
    const auto result = fBackend ? fSession.stop(*fBackend) : AudioOutput::Result::OK;
    if (fNativeStarted) IOAudio2Device::stopIO();
    if (result != AudioOutput::Result::OK) setProperty("AudioDMAResourcesRetained", true);
    if (fStopping && result == AudioOutput::Result::OK && !releaseBackend()) return kIOReturnIOError;
    return ioResult(result);
}
void QcomAudio::publishPosition() {
    // Follow the native sample-frame/host-tick status layout. Seed changes on
    // restart, not on every sample. Do not overwrite client-owned positions.
    __atomic_store_n(&fStatus->sampleTime, fSession.frames(), __ATOMIC_RELAXED);
    __atomic_store_n(&fStatus->hostTime, fSession.hostTime(), __ATOMIC_RELEASE);
}
void QcomAudio::tick(OSObject *owner, IOTimerEventSource *timer) {
    auto *self = OSDynamicCast(QcomAudio, owner);
    if (!self || !self->fBackend || self->fSession.state() != AudioOutput::State::Running) return;
    const auto result = self->fSession.poll(*self->fBackend, mach_absolute_time);
    if (result != AudioOutput::Result::OK) {
        self->setProperty("AudioOutputError", UInt64(ioResult(result)), 32);
        self->IOAudio2Device::stopIO();
        if (!self->fSession.quiescent()) self->setProperty("AudioDMAResourcesRetained", true);
        return;
    }
    if (self->fSession.hostTime()) self->publishPosition();
    if (timer->setTimeoutUS((uint64_t(self->fConfig.periodFrames) * 1000000) / self->fConfig.rate)
        != kIOReturnSuccess) self->stopGated();
}

IOReturn QcomAudio::action(OSObject *owner, void *operation, void *a, void *b, void *) {
    auto *self = OSDynamicCast(QcomAudio, owner);
    if (!self) return kIOReturnBadArgument;
    const auto op = uintptr_t(operation);
    if (op == OpStop) return self->stopGated();
    if (op == OpShutdown) { self->fStopping = true; return self->stopGated(); }
    if (op == OpSuspend) {
        self->fSuspended = uintptr_t(a) == 0;
        return self->fSuspended ? self->stopGated() : kIOReturnSuccess;
    }
    if (self->fStopping || !self->fPrepared) return kIOReturnNotReady;
    switch (op) {
    case OpStart: return self->startGated();
    case OpActive:
        if (uintptr_t(a) != AudioOutput::StreamID || uintptr_t(b) > 1) return kIOReturnBadArgument;
        if (!uintptr_t(b)) {
            auto ret = self->stopGated();
            self->fActive = false;
            return ret;
        }
        self->fActive = true;
        return kIOReturnSuccess;
    case OpRateChange:
        self->requestConfigChange(4, 0, AudioOutput::Rate, 0);
        return kIOReturnSuccess;
    case OpFormatChange:
        self->requestConfigChange(5, AudioOutput::StreamID, AudioOutput::Rate, 0);
        return kIOReturnSuccess;
    case OpConfigChange: {
        const auto &c = *static_cast<const AudioOutput::ConfigChange *>(a);
        // All supported changes are no-ops to the single advertised format.
        // Validate the reply again: native ack's outstanding count is not a
        // reason to trust arbitrary configuration payloads.
        if (c.selector != 0x636f6e66 || c.objectID || c.context ||
            (c.action != 4 && c.action != 5) || c.value != AudioOutput::Rate ||
            c.streamID != (c.action == 4 ? 0 : AudioOutput::StreamID))
            return kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    case OpBuffer: {
        auto &r = *static_cast<BufferRequest *>(a);
        if (r.stream != AudioOutput::StreamID) return kIOReturnNotFound;
        self->fPCM->retain();
        *r.memory = self->fPCM;
        *r.options = kIOMapAnywhere; // writable PCM; never expose a DMA address
        return kIOReturnSuccess;
    }
    default: return kIOReturnUnsupported;
    }
}
IOReturn QcomAudio::setPowerState(unsigned long state, IOService *) {
    if (state > 1) return kIOReturnBadArgument;
    auto ret = gate(OpSuspend, reinterpret_cast<void *>(state));
    if (ret != kIOReturnSuccess) setProperty("AudioPowerStopError", UInt64(ret), 32);
    // Backend resources remain powered/retained if drain failed. This class
    // never authorizes a parent power-domain collapse; backend PM owns it.
    return IOPMAckImplied;
}
IOReturn QcomAudio::handleChangeSampleRate(const UInt64 *rate, UInt64 size) {
    if (!rate || size != sizeof(*rate)) return kIOReturnBadArgument;
    UInt64 value; memcpy(&value, rate, sizeof(value));
    return value == AudioOutput::Rate ? gate(OpRateChange) : kIOReturnUnsupported;
}
IOReturn QcomAudio::handleChangeStreamFormat(UInt32 stream, const AudioOutput::Format *f, UInt64 size) {
    if (!f || size != sizeof(*f) || stream != AudioOutput::StreamID) return kIOReturnBadArgument;
    AudioOutput::Format copy; memcpy(&copy, f, sizeof(copy));
    return AudioOutput::validFormat(copy) ? gate(OpFormatChange) : kIOReturnUnsupported;
}
IOReturn QcomAudio::setStreamActive(UInt32 stream, UInt32 active) {
    return gate(OpActive, reinterpret_cast<void *>(uintptr_t(stream)), reinterpret_cast<void *>(uintptr_t(active)));
}
IOReturn QcomAudio::getStreamBuffer(UInt32 stream, IOOptionBits *options, IOMemoryDescriptor **memory) {
    if (!options || !memory) return kIOReturnBadArgument;
    *options = 0; *memory = nullptr;
    BufferRequest request{stream, options, memory};
    return gate(OpBuffer, &request);
}
IOReturn QcomAudio::performConfigChange(const AudioOutput::ConfigChange *c) {
    if (!c) return kIOReturnBadArgument;
    AudioOutput::ConfigChange copy; memcpy(&copy, c, sizeof(copy));
    return gate(OpConfigChange, &copy);
}
