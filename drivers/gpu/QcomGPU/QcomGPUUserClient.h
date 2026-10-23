// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOUserClient.h>
#include "QcomGPUBuffer.h"
#include "GPUResources.h"
class QcomGPU;
class QcomGPUUserClient : public IOUserClient {
    OSDeclareDefaultStructors(QcomGPUUserClient);
    QcomGPU *fGPU = nullptr;
    GPU::Session fSession;
    QcomGPUBuffer *fMemory[GPU::MaxBuffers] = {};
    bool fRegistered = false, fClosed = false;
    void releaseResources(); // Only under the provider's command gate, or free after detach.
    friend class QcomGPU;
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(UInt32, IOExternalMethodArguments *, IOExternalMethodDispatch *,
        OSObject *, void *) APPLE_KEXT_OVERRIDE;
};
