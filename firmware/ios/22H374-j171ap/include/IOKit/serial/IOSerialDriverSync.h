// SPDX-License-Identifier: BSD-2-Clause
// ABI projection of the hash-pinned iOS 18 provider. The twelve methods match
// Apple's published interface; this provider has no legacy reserved slots.
#pragma once
#include <IOKit/IOService.h>
class IOSerialDriverSync : public IOService {
    OSDeclareAbstractStructors(IOSerialDriverSync);

public:
    virtual IOReturn acquirePort(bool, void*) = 0;
    virtual IOReturn releasePort(void*) = 0;
    virtual IOReturn setState(UInt32, UInt32, void*) = 0;
    virtual UInt32 getState(void*) = 0;
    virtual IOReturn watchState(UInt32*, UInt32, void*) = 0;
    virtual UInt32 nextEvent(void*) = 0;
    virtual IOReturn executeEvent(UInt32, UInt32, void*) = 0;
    virtual IOReturn requestEvent(UInt32, UInt32*, void*) = 0;
    virtual IOReturn enqueueEvent(UInt32, UInt32, bool, void*) = 0;
    virtual IOReturn dequeueEvent(UInt32*, UInt32*, bool, void*) = 0;
    virtual IOReturn enqueueData(UInt8*, UInt32, UInt32*, bool, void*) = 0;
    virtual IOReturn dequeueData(UInt8*, UInt32, UInt32*, UInt32, void*) = 0;
};
class IOSerialStreamSync : public IOService {
    OSDeclareDefaultStructors(IOSerialStreamSync);

protected:
    IOSerialDriverSync* fProvider;
    void* fRefCon;

public:
    virtual IOReturn acquirePort(bool);
    virtual IOReturn releasePort();
    virtual IOReturn setState(UInt32, UInt32);
    virtual UInt32 getState();
    virtual IOReturn watchState(UInt32*, UInt32);
    virtual UInt32 nextEvent();
    virtual IOReturn executeEvent(UInt32, UInt32);
    virtual IOReturn requestEvent(UInt32, UInt32*);
    virtual IOReturn enqueueEvent(UInt32, UInt32, bool);
    virtual IOReturn dequeueEvent(UInt32*, UInt32*, bool);
    virtual IOReturn enqueueData(UInt8*, UInt32, UInt32*, bool);
    virtual IOReturn dequeueData(UInt8*, UInt32, UInt32*, UInt32);
    virtual bool init(OSDictionary* = nullptr, void* = nullptr);
    bool attach(IOService*) APPLE_KEXT_OVERRIDE;
};
static_assert(sizeof(IOSerialDriverSync) == 136, "native serial driver ABI");
static_assert(sizeof(IOSerialStreamSync) == 152, "native serial stream ABI");
