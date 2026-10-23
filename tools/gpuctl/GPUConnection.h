// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKitUser.h>
#include <GPUUserProtocol.h>
#include <GPUNativeProtocol.h>
#include <GPUNativeCompletion.h>
#include <GPUJITProtocol.h>
#include <GPUDiagnostics.h>
#include <GPUMetalTiming.h>
#include <GPUMetalCapabilities.h>

namespace GPU {
struct NativeInfo { char name[64]; uint32_t stampCount, stampOffsets[128]; };
// IOGPUFamily device connection type 1. These are native Family selectors,
// distinct from the versioned diagnostic connection below.
class NativeConnection {
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t query(uint32_t, void *, size_t);
    kern_return_t call(uint32_t, const uint64_t *, uint32_t, const void *, size_t, void *, size_t);
public:
    NativeConnection() = default;
    ~NativeConnection() { close(); }
    NativeConnection(const NativeConnection &) = delete;
    NativeConnection &operator=(const NativeConnection &) = delete;
    kern_return_t open();
    kern_return_t open(mach_port_t service);
    void close();
    kern_return_t info(NativeInfo &);
    kern_return_t metalCapabilities(MetalCapabilities::Snapshot &);
    kern_return_t newQueue(const char *, Native::QueueData &);
    kern_return_t deleteQueue(uint32_t);
    kern_return_t setQueuePriority(uint32_t, const Native::QueuePriority &, uint32_t &);
    kern_return_t newResource(uint64_t, Native::ResourceData &);
    kern_return_t importClientBuffer(void *, uint64_t, Native::ResourceData &);
    kern_return_t importSurface(uint32_t id, uint32_t plane, Native::ResourceData &);
    kern_return_t deleteResource(uint32_t);
    kern_return_t newShmem(uint64_t, Native::ShmemData &);
    kern_return_t deleteShmem(uint32_t);
    kern_return_t newNotificationQueue(uint32_t, Native::NotificationData &);
    kern_return_t deleteNotificationQueue(uint32_t);
    kern_return_t setNotificationQueue(uint32_t, uint32_t);
    kern_return_t setNotificationPort(uint32_t notifications, mach_port_t receivePort);
    static kern_return_t receiveNotification(mach_port_t, uint32_t timeoutMS, bool &received);
    kern_return_t drainNotifications(const Native::NotificationData &);
    kern_return_t pollCompletions(const Native::NotificationData &, Native::CompletionWindow &);
    kern_return_t waitCompletion(const Native::NotificationData &, uint64_t endToken, uint32_t timeoutMS = 2000);
    kern_return_t waitCompletion(const Native::NotificationData &, Native::CompletionWindow &, uint64_t endToken, uint32_t timeoutMS = 2000);
    kern_return_t writeCommands(const Native::ShmemData &, const Command *, uint32_t,
        const uint32_t *responsibleTasks=nullptr, uint32_t taskCount=0);
    kern_return_t writeShaderCommand(const Native::ShmemData &, uint32_t, const void *, size_t,
        const uint32_t *responsibleTasks=nullptr, uint32_t taskCount=0);
    kern_return_t writeShaderBatch(const Native::ShmemData &,const uint32_t *,const void *const *,const size_t *,uint32_t,
        const uint32_t *responsibleTasks=nullptr,uint32_t taskCount=0,const RenderPass::Description *pass=nullptr);
    kern_return_t submit(uint32_t queue, uint32_t shmem, uint64_t startToken, uint64_t endToken);
    kern_return_t registerProgram(const QGPUProgramImage &, JIT::ProgramData &);
    kern_return_t deleteProgram(uint32_t);
    kern_return_t reportMetalTiming(const MetalTiming::Record &);
    kern_return_t metalTimingSnapshot(uint64_t &);
    kern_return_t reportMetalFailure(const Diagnostics::MetalFailure &);
    kern_return_t metalDiagnosticConfiguration(bool registryEnabled,uint64_t &version);
    kern_return_t liveStatistics(uint32_t scope,Diagnostics::LiveSnapshot &);
};
struct Info { uint64_t version, chip, gmemBytes, features, state, blockers; };
struct Limits { uint64_t contexts, queues, buffers, bufferBytes, sessionBytes, copyBytes; };
static_assert(sizeof(Info) == 6 * sizeof(uint64_t), "GPU info wire layout");
static_assert(sizeof(Limits) == 6 * sizeof(uint64_t), "GPU limits wire layout");
// Diagnostic transport; the Metal adapter uses NativeConnection above.
class Connection {
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t scalar(Selector, const uint64_t *, uint32_t, uint64_t *, uint32_t);
public:
    Connection() = default;
    ~Connection() { close(); }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    kern_return_t open();
    void close();
    kern_return_t info(Info &);
    kern_return_t limits(Limits &);
    kern_return_t createContext(uint64_t &);
    kern_return_t destroyContext(uint64_t);
    kern_return_t createQueue(uint64_t context, uint64_t &);
    kern_return_t destroyQueue(uint64_t);
    kern_return_t createBuffer(uint64_t context, uint64_t bytes, uint64_t &handle, uint64_t &allocated);
    kern_return_t destroyBuffer(uint64_t);
    kern_return_t write(uint64_t buffer, uint64_t offset, const void *, size_t);
    kern_return_t read(uint64_t buffer, uint64_t offset, void *, size_t);
    kern_return_t submit(uint64_t queue, uint64_t buffer, uint64_t offset, uint64_t bytes, uint64_t &fence);
    kern_return_t fence(uint64_t queue, uint64_t issued, bool &completed);
};
}
