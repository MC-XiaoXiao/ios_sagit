// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <stddef.h>

class IOUSBDevicePipe;
// Native enum name is required for C++ override identity. Values1/2 are
// opaque hardware hints also used by USB2 audio. The pinned USB2 reference
// ignores them; they must not be reinterpreted as burst/stream settings.
enum tIOUSBDeviceInterfacePipeProperty : UInt32;
// The Family enum differs from the hardware backend's USB::Speed. In
// particular, FS=1 and HS=2 are confirmed by the pinned native controller.
enum class IOUSBDeviceConnectionSpeed : UInt32 {
    Full = 1, High = 2, Super = 3, Unknown = 0xffffffffu
};
// Audited native 24-byte stride. The original framework reads requested at4,
// writes actual at6/status at0 and an AbsoluteTime at8. Tail meaning remains
// opaque: adapters preserve it and never repurpose it for private cookies.
struct IOUSBDeviceIsocFrame {
    IOReturn status;
    UInt16 requested,actual;
    UInt64 timestamp;
    UInt8 opaque16[8];
};
static_assert(sizeof(IOUSBDeviceIsocFrame)==24,"native Device isochronous frame stride");
static_assert(offsetof(IOUSBDeviceIsocFrame,requested)==4 && offsetof(IOUSBDeviceIsocFrame,actual)==6 &&
    offsetof(IOUSBDeviceIsocFrame,timestamp)==8,"native Device isochronous frame fields");
struct IOUSBDeviceCompletion {
    void *target;
    void (*action)(void *target, void *parameter, IOReturn status, UInt32 bytesTransferred);
    void *parameter;
};
struct IOUSBDeviceIsocCompletion {
    void *target;
    void (*action)(void *target, void *parameter, IOReturn status, IOUSBDeviceIsocFrame *frames);
    void *parameter;
};
static_assert(sizeof(IOUSBDeviceCompletion) == 24, "native regular completion ABI");
static_assert(sizeof(IOUSBDeviceIsocCompletion) == 24, "native isochronous completion ABI");

// Fields read by createPipeGated at native 0xfffffff006181f1c.
struct tCreatePipeParameters {
    int type, direction, maximumPacketSize, interval;
    // Descriptor synchronization bits, distinct from the runtime property
    // passed by activateEndpoint. The Family assigns the endpoint later.
    UInt32 synchronization;
};
static_assert(sizeof(tCreatePipeParameters) == 20, "native device pipe arguments");
static_assert(offsetof(tCreatePipeParameters,direction)==4 &&
    offsetof(tCreatePipeParameters,synchronization)==16,"native pipe creation fields");

// Version-locked iOS 18 arm64 request projection. The Family allocates and
// owns these requests. Uninterpreted storage is deliberately opaque; a
// hardware adapter must not attach its own state to it. Complete through
// IOUSBDeviceController::completeEndpointIO, which runs the packet filter and
// callback, releases the Family's buffer retain, and frees non-internal
// requests. The adapter must finish/clear its DMA map before that call.
struct _IOUSBDeviceIORequestTag {
    int endpoint;
    uint32_t padding;
    IOMemoryDescriptor *buffer;
    IODMACommand *dma;
    uint8_t opaque24[8];
    _IOUSBDeviceIORequestTag *next;
    IOReturn status;
    uint32_t timeoutMS;
    uint8_t isochronous;
    uint8_t opaque49[7];
    uint64_t requestID;
    uint64_t lengthOrFrameCount;
    union {
        uint64_t bytesTransferred;
        IOUSBDeviceIsocFrame *frames;
    } result;
    union {
        IOUSBDeviceCompletion regular;
        IOUSBDeviceIsocCompletion isochronous;
    } completion;
};
static_assert(sizeof(_IOUSBDeviceIORequestTag) == 104, "native device request allocation");
static_assert(offsetof(_IOUSBDeviceIORequestTag, buffer) == 8, "request buffer offset");
static_assert(offsetof(_IOUSBDeviceIORequestTag, dma) == 16, "request DMA offset");
static_assert(offsetof(_IOUSBDeviceIORequestTag, next) == 32, "request linkage offset");
static_assert(offsetof(_IOUSBDeviceIORequestTag, status) == 40, "request status offset");
static_assert(offsetof(_IOUSBDeviceIORequestTag, timeoutMS) == 44, "request timeout offset");
static_assert(offsetof(_IOUSBDeviceIORequestTag, isochronous) == 48, "request transfer-kind offset");
static_assert(offsetof(_IOUSBDeviceIORequestTag, requestID) == 56, "request identifier offset");
static_assert(offsetof(_IOUSBDeviceIORequestTag, lengthOrFrameCount) == 64, "request length offset");
static_assert(offsetof(_IOUSBDeviceIORequestTag, result) == 72, "request progress offset");
static_assert(offsetof(_IOUSBDeviceIORequestTag, completion) == 80, "request completion offset");
