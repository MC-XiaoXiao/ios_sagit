// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOUserClient.h>
class QcomBluetooth;
class QcomBluetoothUserClient : public IOUserClient {
    OSDeclareDefaultStructors(QcomBluetoothUserClient);
    QcomBluetooth *fController = nullptr;
    bool fBootstrap = false;
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(UInt32, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *) APPLE_KEXT_OVERRIDE;
};
