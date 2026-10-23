// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUUserProtocol.h>
#include <GPURenderPass.h>
namespace GPU { namespace Native {
// Pinned IOGPUFamily device selectors. Command bytes are the vendor encoding
// below; resource/queue/shmem handles and notification tokens belong to Apple.
enum Selector : uint32_t {
    NewQueue = 7, DeleteQueue = 8, NewResource = 9, DeleteResource = 10,
    NewShmem = 13, DeleteShmem = 14, NewNotificationQueue = 15,
    DeleteNotificationQueue = 16, SetNotificationQueue = 25, Submit = 26, SetQueuePriority = 27
};
enum ResourceType : uint32_t { Buffer = 0, ClientBuffer = 0x80, IOSurface = 0x82 };
// Bounded shared uploads also carry a complete pointer-free JIT program.
constexpr uint32_t MaxShmemBytes = 256 * 1024;
struct QueueArgs { char name[1024]; uint32_t priority; uint8_t flags, reserved[3]; };
struct QueueData { uint64_t id, uniqueID; };
// Original IOGPUCommandQueueSetPriorityAndBackground: one queue scalar,
// three input DWORDs and one scalar applied-foreground-priority result.
struct QueuePriority { uint32_t foreground, background, reserved; };
inline bool validQueuePriority(const QueuePriority &p) {
    return p.foreground<=5 && p.foreground!=3 && (p.background==2 || p.background==3) && !p.reserved;
}
static_assert(sizeof(QueuePriority)==12,"native queue priority ABI");
struct ResourceArgs {
    uint32_t type, cacheMode;
    uint8_t native8[12];
    uint32_t flags;
    uint64_t protection, gpuAddress, gpuLength;
    uint32_t alignment, tag;
    uint64_t clientAddress, clientBase, bytes, parentID;
};
// For type 0x80, +56 is the exposed address and +64 is the allocation base,
// not a length. The Family imports the owning task's original descriptor.
// Accept whole native pages only; subranges/parent allocations stay closed.
inline bool validClientBuffer(uint64_t address,uint64_t bytes,uint64_t pageBytes) {
    return pageBytes>=PageBytes && !(pageBytes&(pageBytes-1)) && address && bytes &&
        bytes<=MaxBufferBytes && !(address&(pageBytes-1)) && !(bytes&(pageBytes-1)) &&
        bytes<=UINT64_MAX-address;
}
// IOSurface uses the Family's existing +56/+60 fields. No task address is
// accepted for this resource type; the Family resolves the ID in its task.
inline uint64_t surfaceHandle(uint32_t id, uint32_t plane) {
    return uint64_t(id) | (uint64_t(plane) << 32);
}
struct ResourceData {
    uint64_t gpuAddress, cpuAddress, sharedInfoAddress;
    uint32_t flags, id;
    uint64_t bytes, uniqueID, allocationID, reserved, protection, mappedBytes;
    uint8_t privateInfo[128];
};
struct ShmemData { uint64_t address; uint32_t bytes, id; };
struct NotificationData { uint64_t address; uint32_t id, reserved; };
// Actual IOGPUBlockFence notification payload, produced by the Family, not a
// vendor completion ring. Start notifications carry a CPU admission interval;
// only the admitted end token denotes retirement of the actual native event.
struct Notification {
    uint64_t token, startTime, endTime;
    uint32_t status, reserved;
    uint64_t auxiliary;
};
constexpr uint32_t CompletionVersion = 1;
struct SubmitArgs {
    uint32_t commands, segments, sideband, flags;
    uint64_t startToken, endToken;
    uint32_t resources, reserved;
};
constexpr uint32_t CommandMagic = 0x47505543, CommandVersion = 4;
// The upload accommodates all 64 Draw payloads, including task metadata.
constexpr uint32_t BatchCommandVersion = 7, MaxBatchCommands = MaxCommands;
constexpr uint32_t RenderPassCommandVersion = 8;
constexpr uint32_t MaxResponsibleTaskIDs = 64, ResponsibleTaskVersion = 1;
struct CommandHeader { uint32_t magic, version, count, responsibleTaskCount; };
// Apple's Metal attribution hint contains DWORD task IDs, not task pointers.
// These hints never change the device's task or resource access rights.
// The bounded ID array follows this header, padded to 16 bytes, then commands.
constexpr uint32_t commandOffset(uint32_t tasks) {
    return tasks<=MaxResponsibleTaskIDs ? sizeof(CommandHeader)+((tasks*4+15)&~15u) : 0;
}
// A pass keeps the normal Draw array, resource IDs and original IOGPU queue.
// Its attachment descriptor follows the padded task attribution metadata.
constexpr uint32_t commandOffset(uint32_t tasks,uint32_t version) {
    return !commandOffset(tasks)?0:commandOffset(tasks)+
        (version==RenderPassCommandVersion?sizeof(RenderPass::Description):0);
}
// Canonical packed shader payloads follow the entire command array. Every
// offset and extent is validated against one immutable kernel snapshot.
inline bool shaderBatchLayout(const Command *commands,uint32_t count,uint32_t offset,
    uint64_t length,uint32_t dispatchBytes,uint32_t drawBytes,uint32_t &end) {
    if(!commands || !count || count>MaxBatchCommands || !offset ||
        uint64_t(offset)+uint64_t(count)*sizeof(Command)>length)return false;
    uint64_t cursor=uint64_t(offset)+uint64_t(count)*sizeof(Command);
    for(uint32_t i=0;i<count;++i) {
        const auto &c=commands[i];const uint32_t bytes=c.opcode==ShaderDraw?drawBytes:c.opcode==ShaderDispatch?dispatchBytes:0;
        if(!bytes || c.reserved || c.destination || c.value || c.offset!=cursor || c.bytes!=bytes ||
            cursor>length || bytes>length-cursor)return false;
        cursor+=bytes;
    }
    if(cursor>MaxShmemBytes)return false;
    end=uint32_t(cursor);return true;
}
struct ResponsibleTaskInfo {
    uint32_t version=ResponsibleTaskVersion, count=0;
    uint32_t ids[MaxResponsibleTaskIDs]={};
};
static_assert(commandOffset(0)==16 && commandOffset(MaxResponsibleTaskIDs)==272 &&
    commandOffset(MaxResponsibleTaskIDs+1)==0,"bounded native task metadata");
// Followed by Command[count]. Fill32 uses a native 32-bit destination ID.
// Copy32 packs source/destination IDs and byte offsets into high/low halves;
// value and reserved must be zero. No command accepts a GPU virtual address.
inline Command copyCommand(uint32_t source, uint32_t sourceOffset,
    uint32_t destination, uint32_t destinationOffset, uint32_t bytes) {
    return {Copy32, 0, (uint64_t(source) << 32) | destination,
        (uint64_t(sourceOffset) << 32) | destinationOffset, bytes, 0};
}
static_assert(sizeof(QueueArgs) == 1032 && sizeof(QueueData) == 16, "native queue ABI");
static_assert(sizeof(ResourceArgs) == 88 && sizeof(ResourceData) == 208, "native resource ABI");
static_assert(__builtin_offsetof(ResourceArgs,clientAddress)==56 && __builtin_offsetof(ResourceArgs,clientBase)==64 &&
    __builtin_offsetof(ResourceArgs,bytes)==72,"native client buffer fields");
static_assert(sizeof(ShmemData) == 16 && sizeof(NotificationData) == 16, "native shmem ABI");
static_assert(sizeof(Notification) == 40, "native completion ABI");
static_assert(sizeof(SubmitArgs) == 40 && sizeof(CommandHeader) == 16, "native submission ABI");
} }
