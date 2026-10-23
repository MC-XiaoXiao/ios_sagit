// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/serial/IOSerialDriverSync.h>
#include "BluetoothSerial.h"
class QcomBluetooth;
namespace Bluetooth {
enum SerialOperation {
    Acquire,
    Release,
    SetState,
    GetState,
    WatchState,
    NextEvent,
    ExecuteEvent,
    RequestEvent,
    EnqueueEvent,
    DequeueEvent,
    EnqueueData,
    DequeueData
};
struct SerialRequest {
    SerialOperation operation;
    uint32_t value = 0, mask = 0, size = 0, minimum = 0;
    uint32_t *output = nullptr, *count = nullptr;
    uint8_t* buffer = nullptr;
    bool sleep = false;
};
}
class QcomBluetoothSerial : public IOSerialDriverSync {
    OSDeclareDefaultStructors(QcomBluetoothSerial);
    QcomBluetooth* fOwner = nullptr;
    IOSerialStreamSync* fStream = nullptr;
    IOReturn call(Bluetooth::SerialRequest&);

public:
    bool start(IOService*) APPLE_KEXT_OVERRIDE;
    void stop(IOService*) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn acquirePort(bool, void*) APPLE_KEXT_OVERRIDE;
    IOReturn releasePort(void*) APPLE_KEXT_OVERRIDE;
    IOReturn setState(UInt32, UInt32, void*) APPLE_KEXT_OVERRIDE;
    UInt32 getState(void*) APPLE_KEXT_OVERRIDE;
    IOReturn watchState(UInt32*, UInt32, void*) APPLE_KEXT_OVERRIDE;
    UInt32 nextEvent(void*) APPLE_KEXT_OVERRIDE;
    IOReturn executeEvent(UInt32, UInt32, void*) APPLE_KEXT_OVERRIDE;
    IOReturn requestEvent(UInt32, UInt32*, void*) APPLE_KEXT_OVERRIDE;
    IOReturn enqueueEvent(UInt32, UInt32, bool, void*) APPLE_KEXT_OVERRIDE;
    IOReturn dequeueEvent(UInt32*, UInt32*, bool, void*) APPLE_KEXT_OVERRIDE;
    IOReturn enqueueData(UInt8*, UInt32, UInt32*, bool, void*) APPLE_KEXT_OVERRIDE;
    IOReturn dequeueData(UInt8*, UInt32, UInt32*, UInt32, void*) APPLE_KEXT_OVERRIDE;
};
