// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

// Internal bring-up ABI, shared by the Kext and its IOKit transport. This is
// deliberately versioned independently of Apple's private IOGPU selectors.
namespace GPU {
constexpr uint32_t ProtocolVersion = 2;
constexpr uint32_t ConnectionType = 0x47505502;
constexpr uint32_t ChipID = 0x05040001; // Adreno 540 revision 1, from compatible.
constexpr uint32_t GMEMBytes = 1024 * 1024;
constexpr uint32_t PageBytes = 4096;
constexpr uint32_t MaxClients = 8, MaxContexts = 8, MaxQueues = 8, MaxBuffers = 32;
constexpr uint64_t MaxBufferBytes = 16 * 1024 * 1024;
// Logical resource bytes stay exact. Only pinned DMA/page-table spans round.
inline uint64_t pageSpanBytes(uint64_t bytes,uint64_t pageBytes=PageBytes) {
    if(!bytes || bytes>MaxBufferBytes || pageBytes<PageBytes || pageBytes>65536 || (pageBytes&(pageBytes-1)))return 0;
    const auto span=(bytes+pageBytes-1)&~(pageBytes-1);
    return span<=MaxBufferBytes?span:0;
}
constexpr uint64_t MaxSessionBytes = 64 * 1024 * 1024;
constexpr uint32_t MaxTextureDimension = 16384, MaxTextureRowBytes = 0x3fffc0;
constexpr size_t MaxCopyBytes = 4096; // IOKit's in-band structure limit; OOL is rejected.
enum Feature : uint64_t {
    HostBuffers = 1, Contexts = 2, Queues = 4,
    HardwareSubmit = 8, GPUVirtualMemory = 16, SharedSurfaces = 32, Metal = 64,
    NativeSubmission = 128, AsynchronousSubmit = 256, IsolatedSubmit = 512
};
constexpr uint64_t FrameworkFeatures = HostBuffers | Contexts | Queues;
enum State : uint32_t { FrameworkReady, Suspended, Stopped, Faulted };
enum Requirement : uint64_t {
    PowerClocks = 1, Interconnect = 2, IsolatedSMMU = 4,
    CommandFirmware = 8, SecureZap = 16, PowerFirmware = 32,
    InterruptCompletion = 64, CommandValidation = 128,
    NativeGPUABI = 256, MetalBackend = 512
};
constexpr uint64_t HardwareRequirements = PowerClocks | Interconnect | IsolatedSMMU |
    CommandFirmware | SecureZap | PowerFirmware | InterruptCompletion | CommandValidation;
constexpr uint64_t AllRequirements = HardwareRequirements | NativeGPUABI | MetalBackend;
// Submit buffers contain these descriptors, never raw PM4 or GPU addresses.
enum CommandOpcode : uint32_t { Fill32 = 1, Barrier = 2, Copy32 = 3, ShaderDispatch = 4, ShaderDraw = 5 };
struct Command {
    uint32_t opcode, reserved;
    uint64_t destination, offset;
    uint32_t bytes, value;
};
static_assert(sizeof(Command) == 32, "GPU command wire layout");
constexpr uint32_t MaxCommands = 64, MaxJobs = 16;
enum Selector : uint32_t {
    GetInfo, GetLimits, CreateContext, DestroyContext, CreateQueue, DestroyQueue,
    CreateBuffer, DestroyBuffer, WriteBuffer, ReadBuffer, Submit, GetFence, SelectorCount
};
struct Shape { uint32_t inputs, outputs; };
inline bool shape(uint32_t selector, Shape &s) {
    switch (selector) {
        case GetInfo: s = {0, 6}; break; // version, chip, GMEM, features, state, blockers
        case GetLimits: s = {0, 6}; break; // contexts, queues, buffers, buffer/session/copy bytes
        case CreateContext: s = {0, 1}; break;
        case DestroyContext: case DestroyQueue: case DestroyBuffer: s = {1, 0}; break;
        case CreateQueue: s = {1, 1}; break;
        case CreateBuffer: s = {3, 2}; break; // context, bytes, flags(0) -> handle, rounded bytes
        case WriteBuffer: case ReadBuffer: s = {2, 0}; break; // buffer, offset; inline bytes
        case Submit: s = {4, 1}; break; // queue, buffer, byte offset/length -> fence
        case GetFence: s = {2, 1}; break; // queue, issued fence -> completion
        default: return false;
    }
    return true;
}
inline bool validCall(uint32_t selector, uint32_t inputs, uint32_t outputs,
    size_t inputBytes, size_t outputBytes) {
    Shape s{};
    if (!shape(selector, s) || inputs != s.inputs || outputs != s.outputs) return false;
    if (selector == WriteBuffer) return inputBytes && inputBytes <= MaxCopyBytes && !outputBytes;
    if (selector == ReadBuffer) return outputBytes && outputBytes <= MaxCopyBytes && !inputBytes;
    return !inputBytes && !outputBytes;
}
inline bool range(uint64_t capacity, uint64_t offset, uint64_t bytes) {
    return bytes && offset <= capacity && bytes <= capacity - offset;
}
inline bool stridedRange(uint64_t capacity,uint64_t offset,uint64_t bytes,uint64_t stride,uint64_t rows) {
    return rows && stride>=bytes && range(capacity,offset,bytes) &&
        rows-1<=(capacity-offset-bytes)/stride;
}
inline bool allocationSize(uint64_t bytes, uint64_t &rounded) {
    if (!bytes || bytes > MaxBufferBytes) return false;
    rounded = (bytes + PageBytes - 1) & ~uint64_t(PageBytes - 1);
    return true;
}
}
