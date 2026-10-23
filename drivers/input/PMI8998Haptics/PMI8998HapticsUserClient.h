// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/IOUserClient.h>
class PMI8998Haptics;
class PMI8998HapticsUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PMI8998HapticsUserClient);
    friend class PMI8998Haptics;
    PMI8998Haptics *controller = nullptr;
    bool closed = false; // Accessed only on the controller's command gate.
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(UInt32, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *) APPLE_KEXT_OVERRIDE;
};
