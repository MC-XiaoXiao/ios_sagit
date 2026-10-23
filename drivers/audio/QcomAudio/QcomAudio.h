// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/audio/IOAudio2Device.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "AudioOutputBackend.h"

class QcomAudio : public IOAudio2Device {
    OSDeclareDefaultStructors(QcomAudio);
    AudioOutputBackend *fBackend = nullptr;
    IOBufferMemoryDescriptor *fPCM = nullptr;
    IOWorkLoop *fLoop = nullptr;
    IOCommandGate *fGate = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    AudioOutput::Configuration fConfig{};
    AudioOutput::Session fSession;
    AudioOutput::Status *fStatus = nullptr;
    uint64_t fTimeout = 0;
    bool fPrepared = false, fPinned = false, fStopping = false, fSuspended = false;
    bool fActive = true, fNativeStarted = false, fPM = false, fPublished = false;
    bool publishFormat();
    void publishPosition();
    bool releaseBackend();
    IOReturn startGated();
    IOReturn stopGated();
    IOReturn gate(unsigned, void * = nullptr, void * = nullptr);
    static IOReturn action(OSObject *, void *, void *, void *, void *);
    static void tick(OSObject *, IOTimerEventSource *);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    void registerService(IOOptionBits = 0) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn startIO() APPLE_KEXT_OVERRIDE;
    IOReturn stopIO() APPLE_KEXT_OVERRIDE;
    IOReturn handleChangeSampleRate(const UInt64 *, UInt64) APPLE_KEXT_OVERRIDE;
    IOReturn handleChangeStreamFormat(UInt32, const AudioOutput::Format *, UInt64) APPLE_KEXT_OVERRIDE;
    IOReturn setStreamActive(UInt32, UInt32) APPLE_KEXT_OVERRIDE;
    IOReturn getStreamBuffer(UInt32, IOOptionBits *, IOMemoryDescriptor **) APPLE_KEXT_OVERRIDE;
    IOReturn performConfigChange(const AudioOutput::ConfigChange *) APPLE_KEXT_OVERRIDE;
};
