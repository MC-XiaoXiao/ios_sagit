// SPDX-License-Identifier: BSD-2-Clause
// Subclass projection of the pinned iPad7c iOS 18 arm64 IOAudio2Family.
// All imports resolve to the original Apple provider, never local stubs.
// Unknown/unused hooks keep slot names instead of claiming a public API.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <AudioOutputTypes.h>

class IOAudio2Device : public IOService {
    OSDeclareAbstractStructors(IOAudio2Device);
    UInt8 nativeState[96];
public:
    bool init(OSDictionary *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
    IOReturn configureReport(IOReportChannelList *, IOReportConfigureAction, void *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn updateReport(IOReportChannelList *, IOReportUpdateAction, void *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
    virtual bool isRunning();                                                   // 168
    virtual IOReturn startIO();                                                  // 169
    virtual IOReturn stopIO();                                                   // 170
    virtual IOReturn startIOWithFlags(UInt32, UInt64 *);                          // 171
    virtual IOReturn stopIOWithFlags(UInt32, UInt64 *);                           // 172
    virtual IOReturn nativeSlot173();
    virtual AudioOutput::Status *getStatusBuffer();                              // 174
    virtual IOReturn nativeSlot175();
    virtual IOReturn nativeSlot176();
    virtual IOReturn nativeSlot177();
    virtual void clientClosed();                                                // 178
    virtual IOReturn registerNotificationPort(void *, UInt32, UInt32);            // 179
    virtual void notify(UInt32, UInt32, UInt32, UInt32, UInt64, UInt64);           // 180
    virtual void nativeSlot181(UInt32, const void *);
    virtual IOReturn clientMemoryForType(UInt32, IOOptionBits *, IOMemoryDescriptor **); // 182
    virtual IOReturn nativeSlot183();
    virtual IOReturn nativeSlot184();
    virtual IOReturn nativeSlot185();
    virtual void requestConfigChange(UInt32, UInt32, UInt64, UInt64);             // 186
    virtual IOReturn handleConfigChange(const AudioOutput::ConfigChange *, UInt64); // 187
    virtual IOReturn handleChangeSampleRate(const UInt64 *, UInt64);              // 188
    virtual IOReturn handleChangeStreamFormat(UInt32, const AudioOutput::Format *, UInt64); // 189
    virtual IOReturn setStreamActive(UInt32, UInt32);                             // 190
    virtual IOReturn nativeSlot191();
    virtual IOReturn nativeSlot192();
    virtual IOReturn getStreamBuffer(UInt32, IOOptionBits *, IOMemoryDescriptor **); // 193
    virtual IOReturn nativeSlot194();
    virtual IOReturn nativeSlot195();
    virtual IOReturn performConfigChange(const AudioOutput::ConfigChange *);      // 196
};
static_assert(sizeof(IOService) == 136, "native IOService ABI");
static_assert(sizeof(IOAudio2Device) == 232, "native IOAudio2Device ABI");
