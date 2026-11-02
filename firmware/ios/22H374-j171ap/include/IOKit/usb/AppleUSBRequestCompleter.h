// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOEventSource.h>
class AppleUSBRequest;

// Implemented by Apple's USBCommon Kext. The native device owns its completer;
// the HCD submits the original request after hardware retirement and CPU copy.
// Native enqueue selects its thread-call path or workloop queue using the
// prepared request's own policy. Do not invoke client completion
// inline while holding the controller or role-nub hardware gate.
class AppleUSBRequestCompleter : public IOEventSource {
    OSDeclareDefaultStructors(AppleUSBRequestCompleter);
    // Both SDK and pinned native IOEventSource are72 bytes; the native USB
    // completer adds104. Access its queues through native methods only.
    UInt8 nativeState[104];
public:
    void free() APPLE_KEXT_OVERRIDE; // 14
    bool checkForWork() APPLE_KEXT_OVERRIDE; // 16
    virtual uintptr_t nativeSlot27();
    virtual uintptr_t nativeSlot28();
    virtual IOReturn enqueueRequest(AppleUSBRequest *); // 29
    virtual uintptr_t nativeSlot30();
    virtual uintptr_t nativeSlot31();
    virtual uintptr_t nativeSlot32();
    virtual uintptr_t nativeSlot33();
    virtual uintptr_t nativeSlot34();
    virtual uintptr_t nativeSlot35();
    virtual uintptr_t nativeSlot36();
    virtual uintptr_t nativeSlot37();
    virtual uintptr_t nativeSlot38();
};
static_assert(sizeof(AppleUSBRequestCompleter)==176,"iOS18 native USB completer instance ABI");
