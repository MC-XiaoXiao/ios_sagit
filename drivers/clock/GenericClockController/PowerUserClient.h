// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOUserClient.h>
class GenericClockController;
class PowerUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PowerUserClient);
    GenericClockController *fController = nullptr;
    bool fAdmin = false;
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(UInt32, IOExternalMethodArguments *, IOExternalMethodDispatch *,
        OSObject *, void *) APPLE_KEXT_OVERRIDE;
};
