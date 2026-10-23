// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOCommand.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostIsochronous.h>
class AppleUSBRequestPool;
class AppleUSBRequestCompleter;
class IOService;

// Wire setup packet; field values are little endian. This is separate from
// the opaque Family request object and must be decoded before hardware use.
struct AppleUSBSetupPacket {
    UInt8 requestType,request;
    UInt16 value,index,length;
} __attribute__((packed));
static_assert(sizeof(AppleUSBSetupPacket)==8,"USB setup wire layout");

// Pinned iOS18 projection. Do not transplant the iOS12 getter order: the
// completer, prepared MD and original client are slots23/24/27; completion is21. Unknown
// entries preserve dispatch only; no runtime code may call them as declared.
// The Family owns the original request, its DMA preparation and completion.
class AppleUSBRequest : public IOCommand {
    OSDeclareAbstractStructors(AppleUSBRequest);
    UInt8 nativeState[24];
public:
    void free() APPLE_KEXT_OVERRIDE; // 14
    virtual bool initWithPool(AppleUSBRequestPool *); // 15
    virtual void setNextRequest(AppleUSBRequest *); // 16
    virtual AppleUSBRequest *getNextRequest() const; // 17
    virtual void activate(); // 18
    // Publish one completed ISO frame. The Native implementation advances
    // its optional legacy conversion cursor exactly once per call.
    virtual IOReturn update(); // 19
    virtual void finish(IOReturn); // 20
    virtual void complete(); // 21
    virtual void cancel(); // 22
    virtual AppleUSBRequestCompleter *getRequestCompleter() const; // 23
    // Descriptive alias: concrete request retains this MD after prepare(0)
    // and completes it before callback. This entry is not a timeout getter.
    virtual IOMemoryDescriptor *getPreparedBuffer() const; // 24
    virtual UInt32 getCompletionTimeout() const; // 25, milliseconds
    virtual UInt64 getIOLength() const; // 26, native LDR X0 at offset520
    // Original control client, retained by the prepared Native request. This
    // is not an IOMemoryDescriptor; use slot24 for CPU data access.
    virtual IOService *getClient() const; // 27, offset64
    virtual const AppleUSBSetupPacket *getRequest() const; // 28
    virtual UInt64 getStartFrame() const; // 29
    virtual IOUSBHostIsochronousTransaction *getFrameList() const; // 30
    virtual UInt32 getNumFrames() const; // 31
    virtual void setStatus(IOReturn); // 32
    virtual void setBytesTransferred(UInt32); // 33
    virtual void setTimeStamp(UInt64); // 34
    virtual IOReturn getStatus() const; // 35
    virtual UInt32 getBytesTransferred() const; // 36
    virtual UInt64 getTimestamp() const; // 37
    virtual UInt64 getRequestID() const; // 38
    virtual void setRequestID(UInt64); // 39
    virtual bool isBundled() const; // 40, concrete completion kind3
    virtual uintptr_t nativeSlot41(); // 41
    virtual uintptr_t nativeSlot42(); // 42
    virtual uintptr_t nativeSlot43(); // 43
    virtual void *getCompletionContext() const; // 44
    virtual uintptr_t nativeSlot45(); // 45
    virtual bool isSynchronous() const; // 46
    virtual uintptr_t nativeSlot47(); // 47
    virtual uintptr_t nativeSlot48(); // 48
    virtual uintptr_t nativeSlot49(); // 49
};
static_assert(sizeof(AppleUSBRequest)==56,"iOS18 common request instance ABI");
