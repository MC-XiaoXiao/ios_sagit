// SPDX-License-Identifier: BSD-2-Clause
#include "GPUConnection.h"
#include <GPUSurfaceProtocol.h>
#include <GPUShaderProtocol.h>
#include <IOKit/IOReturn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace GPU {
namespace {
constexpr kern_return_t NotOpen = kIOReturnNotOpen;
constexpr kern_return_t BadArgument = kIOReturnBadArgument;
constexpr kern_return_t NotFound = kIOReturnNotFound;
constexpr kern_return_t Unsupported = kIOReturnUnsupported;
constexpr kern_return_t Error = kIOReturnError;
constexpr uint32_t NativeDeviceType = 1;
kern_return_t openDevice(uint32_t type, mach_port_t *port)
{
    auto matching = IOServiceMatching("QcomGPU");
    if (!matching) return Error;
    mach_port_t iterator = MACH_PORT_NULL, service;
    auto r = IOServiceGetMatchingServices(MACH_PORT_NULL, matching, &iterator);
    if (r) return r;
    // Match exactly one service before opening. No hard-coded registry path.
    service = IOIteratorNext(iterator);
    mach_port_t duplicate = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (!service) return NotFound;
    if (duplicate) { IOObjectRelease(duplicate); IOObjectRelease(service); return Error; }
    r = IOServiceOpen(service, mach_task_self(), type, port);
    IOObjectRelease(service);
    if (r) *port = MACH_PORT_NULL;
    return r;
}
}
kern_return_t NativeConnection::query(uint32_t selector, void *data, size_t bytes)
{
    return call(selector, nullptr, 0, nullptr, 0, data, bytes);
}
kern_return_t NativeConnection::call(uint32_t selector, const uint64_t *in, uint32_t count,
    const void *data, size_t bytes, void *out, size_t outBytes)
{
    if (!port) return NotOpen;
    uint32_t scalars = 0; size_t actual = outBytes;
    auto r = IOConnectCallMethod(port, selector, in, count, data, bytes,
        nullptr, &scalars, out, &actual);
    return !r && (scalars || actual != outBytes) ? Error : r;
}
kern_return_t NativeConnection::open()
{
    close(); return openDevice(NativeDeviceType, &port);
}
kern_return_t NativeConnection::open(mach_port_t service)
{
    close();
    if (!service || !IOObjectConformsTo(service, "QcomGPU")) return BadArgument;
    auto r = IOServiceOpen(service, mach_task_self(), NativeDeviceType, &port);
    if (r) port = MACH_PORT_NULL;
    return r;
}
void NativeConnection::close() { if (port) { IOServiceClose(port); port = MACH_PORT_NULL; } }
kern_return_t NativeConnection::metalCapabilities(MetalCapabilities::Snapshot &out) {
    return query(MetalCapabilities::Selector,&out,sizeof(out));
}
kern_return_t NativeConnection::info(NativeInfo &out)
{
    NativeInfo result{};
    struct StampMap {
        uint64_t address;
        uint32_t clientInfoBytes, native12, native16, count, offsets[128];
    } stamps{};
    static_assert(sizeof(StampMap) == 536, "IOGPU native query 2 layout");
    auto r = query(1, result.name, sizeof(result.name));
    if (!r) r = query(2, &stamps, sizeof(stamps));
    if (!r && (result.name[63] || stamps.count > 128)) r = Error;
    if (!r) {
        result.stampCount = stamps.count;
        memcpy(result.stampOffsets, stamps.offsets, sizeof(stamps.offsets));
        out = result;
    }
    return r;
}
kern_return_t NativeConnection::newQueue(const char *name, Native::QueueData &out)
{
    if (!name || strlen(name) >= sizeof(Native::QueueArgs::name)) return BadArgument;
    Native::QueueArgs args{}; memcpy(args.name, name, strlen(name));
    return call(Native::NewQueue, nullptr, 0, &args, sizeof(args), &out, sizeof(out));
}
kern_return_t NativeConnection::deleteQueue(uint32_t id) {
    uint64_t in[] = {id}; return call(Native::DeleteQueue, in, 1, nullptr, 0, nullptr, 0);
}
kern_return_t NativeConnection::setQueuePriority(uint32_t id,const Native::QueuePriority &priority,uint32_t &applied) {
    if(!port)return NotOpen;
    if(!id || !Native::validQueuePriority(priority))return BadArgument;
    uint64_t input=id,output=0;uint32_t count=1;size_t bytes=0;
    auto result=IOConnectCallMethod(port,Native::SetQueuePriority,&input,1,&priority,sizeof(priority),&output,&count,nullptr,&bytes);
    if(!result) {
        if(count!=1 || bytes || output>5 || output==3)return Error;
        applied=uint32_t(output);
    }
    return result;
}
kern_return_t NativeConnection::newResource(uint64_t bytes, Native::ResourceData &out) {
    if (!bytes || bytes > MaxBufferBytes || (bytes & (PageBytes - 1))) return BadArgument;
    Native::ResourceArgs args{}; args.cacheMode = 0x400; args.bytes = bytes;
    return call(Native::NewResource, nullptr, 0, &args, sizeof(args), &out, sizeof(out));
}
kern_return_t NativeConnection::importClientBuffer(void *pointer,uint64_t bytes,Native::ResourceData &out) {
    out={};auto address=uint64_t(uintptr_t(pointer));auto pageBytes=getpagesize();
    if(pageBytes<=0 || !Native::validClientBuffer(address,bytes,uint64_t(pageBytes)))return BadArgument;
    Native::ResourceArgs args{};args.type=Native::ClientBuffer;
    args.clientAddress=args.clientBase=address;args.bytes=bytes;
    auto status=call(Native::NewResource,nullptr,0,&args,sizeof(args),&out,sizeof(out));
    if(status){out={};return status;}
    bool valid=out.id && out.gpuAddress && out.cpuAddress==address && out.bytes==bytes &&
        out.mappedBytes==bytes && !out.protection;
    for(auto value:out.privateInfo)if(value)valid=false;
    if(!valid){if(out.id)deleteResource(out.id);out={};return Error;}
    return KERN_SUCCESS;
}
kern_return_t NativeConnection::importSurface(uint32_t id,uint32_t plane,Native::ResourceData &out) {
    const auto pageBytes=getpagesize();
    if (!id || plane || pageBytes<=0) return BadArgument;
    Native::ResourceArgs args{};args.type=Native::IOSurface;args.clientAddress=Native::surfaceHandle(id,plane);
    out={};auto status=call(Native::NewResource,nullptr,0,&args,sizeof(args),&out,sizeof(out));
    if (status) return status;
    Surface::Info surface{};memcpy(&surface,out.privateInfo,sizeof(surface));
    bool valid=Surface::resourceLayout(surface,out.bytes,out.mappedBytes,uint64_t(pageBytes)) && surface.id==id && surface.plane==plane &&
        out.id && out.gpuAddress && !out.cpuAddress;
    for (size_t i=sizeof(surface);i<sizeof(out.privateInfo);++i) if (out.privateInfo[i]) valid=false;
    if (!valid) {if (out.id) deleteResource(out.id);out={};return BadArgument;}
    return KERN_SUCCESS;
}
kern_return_t NativeConnection::deleteResource(uint32_t id) {
    uint64_t in[] = {id}; return call(Native::DeleteResource, in, 1, nullptr, 0, nullptr, 0);
}
kern_return_t NativeConnection::newShmem(uint64_t bytes, Native::ShmemData &out) {
    uint64_t in[] = {bytes, 0}; return call(Native::NewShmem, in, 2, nullptr, 0, &out, sizeof(out));
}
kern_return_t NativeConnection::deleteShmem(uint32_t id) {
    uint64_t in[] = {id}; return call(Native::DeleteShmem, in, 1, nullptr, 0, nullptr, 0);
}
kern_return_t NativeConnection::registerProgram(const QGPUProgramImage &image, JIT::ProgramData &out) {
    static_assert(JIT::UploadChunkBytes<=Native::MaxShmemBytes,"bounded shader upload pieces");
    Native::ShmemData memory[JIT::UploadChunks]{};uint64_t in[JIT::UploadChunks]{};
    kern_return_t status=0;
    for(unsigned part=0;part<JIT::UploadChunks;++part) {
        const uint32_t bytes=JIT::uploadChunkBytes(part),allocated=(bytes+PageBytes-1)&~(PageBytes-1);
        status=newShmem(allocated,memory[part]);if(status)break;
        if(!memory[part].address || memory[part].bytes<bytes){status=Error;break;}
        memcpy(reinterpret_cast<void *>(uintptr_t(memory[part].address)),
            reinterpret_cast<const uint8_t *>(&image)+part*JIT::UploadChunkBytes,bytes);
        in[part]=memory[part].id;
    }
    if(!status){__atomic_thread_fence(__ATOMIC_RELEASE);status=call(JIT::RegisterProgram,in,JIT::UploadChunks,nullptr,0,&out,sizeof(out));}
    for(auto &part:memory)if(part.id)deleteShmem(part.id);
    return status;
}
kern_return_t NativeConnection::reportMetalTiming(const MetalTiming::Record &r) {
    return call(MetalTiming::ReportSelector,nullptr,0,&r,sizeof(r),nullptr,0);
}
kern_return_t NativeConnection::metalTimingSnapshot(uint64_t &h) {
    return call(MetalTiming::CopySelector,nullptr,0,nullptr,0,&h,sizeof(h));
}
kern_return_t NativeConnection::reportMetalFailure(const Diagnostics::MetalFailure &failure) {
    return call(Diagnostics::MetalFailureSelector,nullptr,0,&failure,sizeof(failure),nullptr,0);
}
kern_return_t NativeConnection::metalDiagnosticConfiguration(bool registryEnabled,uint64_t &version) {
    uint64_t input=registryEnabled?1:0;version=0;
    return call(Diagnostics::MetalConfigurationSelector,&input,1,nullptr,0,&version,sizeof(version));
}
kern_return_t NativeConnection::liveStatistics(uint32_t scope,Diagnostics::LiveSnapshot &snapshot) {
    uint64_t input=scope;return call(Diagnostics::LiveSelector,&input,1,nullptr,0,&snapshot,sizeof(snapshot));
}
kern_return_t NativeConnection::deleteProgram(uint32_t id) {
    uint64_t in[] = {id}; return call(JIT::DeleteProgram, in, 1, nullptr, 0, nullptr, 0);
}
kern_return_t NativeConnection::newNotificationQueue(uint32_t entries, Native::NotificationData &out) {
    uint64_t in[] = {entries, 40}; return call(Native::NewNotificationQueue, in, 2, nullptr, 0, &out, sizeof(out));
}
kern_return_t NativeConnection::deleteNotificationQueue(uint32_t id) {
    uint64_t in[] = {id}; return call(Native::DeleteNotificationQueue, in, 1, nullptr, 0, nullptr, 0);
}
kern_return_t NativeConnection::setNotificationQueue(uint32_t queue, uint32_t notifications) {
    uint64_t in[] = {queue, notifications}; return call(Native::SetNotificationQueue, in, 2, nullptr, 0, nullptr, 0);
}
kern_return_t NativeConnection::setNotificationPort(uint32_t notifications, mach_port_t receivePort) {
    if (!port) return NotOpen;
    if (!notifications || !MACH_PORT_VALID(receivePort)) return BadArgument;
    return IOConnectSetNotificationPort(port, 0, receivePort, notifications);
}
kern_return_t NativeConnection::receiveNotification(mach_port_t receivePort,uint32_t timeoutMS,bool &received) {
    received=false;
    if (!MACH_PORT_VALID(receivePort) || timeoutMS>10000) return BadArgument;
    struct {mach_msg_header_t header;mach_msg_max_trailer_t trailer;} message{};
    auto result=mach_msg(&message.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(message),
        receivePort,timeoutMS,MACH_PORT_NULL);
    if(result==MACH_RCV_TIMED_OUT)return KERN_SUCCESS;
    if(result)return result;
    // A notification carries no completion token. Only pollCompletions may
    // advance retirement after validating the actual shared queue records.
    const bool valid=!(message.header.msgh_bits&MACH_MSGH_BITS_COMPLEX) &&
        message.header.msgh_size==sizeof(mach_msg_header_t);
    mach_msg_destroy(&message.header);
    if(!valid)return Error;
    received=true;return KERN_SUCCESS;
}
kern_return_t NativeConnection::drainNotifications(const Native::NotificationData &data) {
    if (!port) return NotOpen;
    if (!data.address || !data.id) return BadArgument;
    auto *memory = reinterpret_cast<IODataQueueMemory *>(uintptr_t(data.address));
    while (IODataQueueDataAvailable(memory)) {
        Native::Notification event{}; uint32_t bytes = sizeof(event);
        auto r = IODataQueueDequeue(memory, &event, &bytes);
        if (r) return r;
        if (bytes != sizeof(event) || !event.token || event.reserved) return Error;
        if (event.status) return kIOReturnIOError;
    }
    return KERN_SUCCESS;
}
kern_return_t NativeConnection::pollCompletions(const Native::NotificationData &data,Native::CompletionWindow &window) {
    if(!port)return NotOpen;
    if(!data.address || !data.id)return BadArgument;
    if(window.failed())return Error;
    auto *memory=reinterpret_cast<IODataQueueMemory *>(uintptr_t(data.address));
    while(IODataQueueDataAvailable(memory)) {
        Native::Notification event{};uint32_t bytes=sizeof(event);
        auto status=IODataQueueDequeue(memory,&event,&bytes);
        if(status || bytes!=sizeof(event)) {
            fprintf(stderr,"GPU_NOTIFICATION dequeue status=0x%x bytes=%u expected=%zu\n",status,bytes,sizeof(event));
            return status?status:Error;
        }
        auto observed=window.observe(event);
        if(observed==Native::CompletionWindow::Observation::Malformed) {
            fprintf(stderr,"GPU_NOTIFICATION malformed token=%llu start=%llu end=%llu status=0x%x reserved=0x%x issued=%llu retired=%llu pending=%u\n",
                (unsigned long long)event.token,(unsigned long long)event.startTime,(unsigned long long)event.endTime,
                event.status,event.reserved,(unsigned long long)window.issued(),(unsigned long long)window.retired(),window.pending());
            return Error;
        }
        if(observed==Native::CompletionWindow::Observation::Failed)return kIOReturnIOError;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);return KERN_SUCCESS;
}
kern_return_t NativeConnection::waitCompletion(const Native::NotificationData &data,uint64_t endToken,uint32_t timeoutMS) {
    Native::CompletionWindow window;
    if(!endToken || !window.admit(endToken-1,endToken))return BadArgument;
    return waitCompletion(data,window,endToken,timeoutMS);
}
kern_return_t NativeConnection::waitCompletion(const Native::NotificationData &data,Native::CompletionWindow &window,uint64_t endToken,uint32_t timeoutMS) {
    if (!port) return NotOpen;
    if (!data.address || !data.id || !endToken || endToken>window.issued() || !timeoutMS || timeoutMS>10000) return BadArgument;
    auto now=[](uint64_t &value) {
        timespec time{};if (clock_gettime(CLOCK_MONOTONIC,&time)) return false;
        value=uint64_t(time.tv_sec)*1000000000+uint64_t(time.tv_nsec);return true;
    };
    uint64_t start=0,current=0;if (!now(start)) return Error;
    for (;;) {
        auto status=pollCompletions(data,window);if(status)return status;
        if(window.retired()>=endToken)return KERN_SUCCESS;
        if (!now(current)) return Error;
        if(current<start)return Error;
        if (current-start>=uint64_t(timeoutMS)*1000000) return kIOReturnTimeout;
        usleep(1000);
    }
}
kern_return_t NativeConnection::writeCommands(const Native::ShmemData &shmem, const Command *commands, uint32_t count,
    const uint32_t *responsibleTasks, uint32_t taskCount)
{
    if (!port) return NotOpen;
    const auto offset=Native::commandOffset(taskCount);
    if (!offset || (taskCount && !responsibleTasks) || !shmem.address || !commands || !count || count > MaxCommands ||
        offset + count * sizeof(Command) > shmem.bytes) return BadArgument;
    Native::CommandHeader header{Native::CommandMagic, Native::CommandVersion, count, taskCount};
    auto *base = reinterpret_cast<uint8_t *>(uintptr_t(shmem.address));
    memcpy(base + offset, commands, count * sizeof(Command));
    memset(base+sizeof(header),0,offset-sizeof(header));
    if(taskCount)memcpy(base+sizeof(header),responsibleTasks,taskCount*sizeof(*responsibleTasks));
    memcpy(base, &header, sizeof(header));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return KERN_SUCCESS;
}
kern_return_t NativeConnection::submit(uint32_t queue, uint32_t shmem, uint64_t startToken, uint64_t endToken)
{
    Native::SubmitArgs args{shmem, 0, 0, 0, startToken, endToken, 0, 0};
    uint64_t in[] = {queue, 0, 1, sizeof(args)};
    // Family admission is asynchronous. Wait for the native end token before
    // CPU access or operation completion; start records do not prove GPU work.
    return call(Native::Submit, in, 4, &args, sizeof(args), nullptr, 0);
}
kern_return_t NativeConnection::writeShaderCommand(const Native::ShmemData &shmem, uint32_t opcode,
    const void *payload, size_t bytes, const uint32_t *responsibleTasks, uint32_t taskCount)
{
    if (!port) return NotOpen;
    size_t expected = opcode == ShaderDispatch ? sizeof(Shader::Dispatch) : opcode == ShaderDraw ? sizeof(Shader::Draw) : 0;
    const auto commands=Native::commandOffset(taskCount);const size_t offset=commands+sizeof(Command);
    if (!commands || (taskCount && !responsibleTasks) || !payload || !expected || bytes != expected || !shmem.address || shmem.bytes < offset + bytes) return BadArgument;
    Native::CommandHeader header{Native::CommandMagic, Native::CommandVersion, 1, taskCount};
    Command command{opcode, 0, 0, offset, (uint32_t)bytes, 0};
    auto *base = reinterpret_cast<uint8_t *>(uintptr_t(shmem.address));
    memcpy(base+offset,payload,bytes);memcpy(base+commands,&command,sizeof(command));
    memset(base+sizeof(header),0,commands-sizeof(header));
    if(taskCount)memcpy(base+sizeof(header),responsibleTasks,taskCount*sizeof(*responsibleTasks));
    memcpy(base,&header,sizeof(header));
    __atomic_thread_fence(__ATOMIC_RELEASE); return KERN_SUCCESS;
}
kern_return_t NativeConnection::writeShaderBatch(const Native::ShmemData &shmem,const uint32_t *opcodes,
    const void *const *payloads,const size_t *sizes,uint32_t count,const uint32_t *tasks,uint32_t taskCount,
    const RenderPass::Description *pass)
{
    if(!port)return NotOpen;
    const uint32_t version=pass?Native::RenderPassCommandVersion:Native::BatchCommandVersion;
    const uint32_t offset=Native::commandOffset(taskCount,version);
    if(!offset || !count || count>Native::MaxBatchCommands || !opcodes || !payloads || !sizes ||
        !shmem.address || (taskCount && !tasks) || (pass && !RenderPass::valid(*pass)))return BadArgument;
    Command commands[Native::MaxBatchCommands]{};size_t cursor=offset+count*sizeof(Command);
    for(uint32_t i=0;i<count;++i) {
        const size_t expected=opcodes[i]==ShaderDraw?sizeof(Shader::Draw):opcodes[i]==ShaderDispatch?sizeof(Shader::Dispatch):0;
        if(!expected || sizes[i]!=expected || !payloads[i] || cursor>shmem.bytes || expected>shmem.bytes-cursor)return BadArgument;
        if(pass && (opcodes[i]!=ShaderDraw || !RenderPass::drawMatches(*pass,*static_cast<const Shader::Draw *>(payloads[i]))))return BadArgument;
        commands[i]={opcodes[i],0,0,cursor,uint32_t(expected),0};cursor+=expected;
    }
    auto *base=reinterpret_cast<uint8_t *>(uintptr_t(shmem.address));
    for(uint32_t i=0;i<count;++i)memcpy(base+commands[i].offset,payloads[i],sizes[i]);
    memcpy(base+offset,commands,count*sizeof(Command));
    memset(base+sizeof(Native::CommandHeader),0,offset-sizeof(Native::CommandHeader));
    if(taskCount)memcpy(base+sizeof(Native::CommandHeader),tasks,taskCount*4);
    if(pass)memcpy(base+Native::commandOffset(taskCount),pass,sizeof(*pass));
    Native::CommandHeader header{Native::CommandMagic,version,count,taskCount};memcpy(base,&header,sizeof(header));
    __atomic_thread_fence(__ATOMIC_RELEASE);return KERN_SUCCESS;
}
kern_return_t Connection::scalar(Selector selector, const uint64_t *in, uint32_t ni, uint64_t *out, uint32_t no)
{
    if (!port) return NotOpen;
    if (!validCall(selector, ni, no, 0, 0)) return BadArgument;
    uint32_t count = no;
    kern_return_t r = IOConnectCallScalarMethod(port, selector, in, ni, out, &count);
    return !r && count != no ? Error : r;
}
kern_return_t Connection::open()
{
    close();
    auto r = openDevice(ConnectionType, &port);
    if (r) return r;
    Info found{};
    r = info(found);
    if (!r && (found.version != ProtocolVersion || found.chip != ChipID)) r = Unsupported;
    if (r) close();
    return r;
}
void Connection::close() { if (port) { IOServiceClose(port); port = MACH_PORT_NULL; } }
kern_return_t Connection::info(Info &out) {
    uint64_t values[6] = {};
    auto r = scalar(GetInfo, nullptr, 0, values, 6);
    if (!r) memcpy(&out, values, sizeof(out));
    return r;
}
kern_return_t Connection::limits(Limits &out) {
    uint64_t values[6] = {};
    auto r = scalar(GetLimits, nullptr, 0, values, 6);
    if (!r) memcpy(&out, values, sizeof(out));
    return r;
}
kern_return_t Connection::createContext(uint64_t &handle) {
    uint64_t value = 0; auto r = scalar(CreateContext, nullptr, 0, &value, 1);
    if (!r) handle = value;
    return r;
}
kern_return_t Connection::destroyContext(uint64_t handle) { return scalar(DestroyContext, &handle, 1, nullptr, 0); }
kern_return_t Connection::createQueue(uint64_t context, uint64_t &handle) {
    uint64_t value = 0; auto r = scalar(CreateQueue, &context, 1, &value, 1);
    if (!r) handle = value;
    return r;
}
kern_return_t Connection::destroyQueue(uint64_t handle) { return scalar(DestroyQueue, &handle, 1, nullptr, 0); }
kern_return_t Connection::createBuffer(uint64_t context, uint64_t bytes, uint64_t &handle, uint64_t &allocated) {
    uint64_t in[] = {context, bytes, 0}, out[2] = {};
    auto r = scalar(CreateBuffer, in, 3, out, 2);
    if (!r) { handle = out[0]; allocated = out[1]; }
    return r;
}
kern_return_t Connection::destroyBuffer(uint64_t handle) { return scalar(DestroyBuffer, &handle, 1, nullptr, 0); }
kern_return_t Connection::write(uint64_t buffer, uint64_t offset, const void *data, size_t bytes) {
    if (!port) return NotOpen;
    if (!data || !validCall(WriteBuffer, 2, 0, bytes, 0)) return BadArgument;
    uint64_t in[] = {buffer, offset}; uint32_t no = 0; size_t outBytes = 0;
    auto r = IOConnectCallMethod(port, WriteBuffer, in, 2, data, bytes, nullptr, &no, nullptr, &outBytes);
    return !r && (no || outBytes) ? Error : r;
}
kern_return_t Connection::read(uint64_t buffer, uint64_t offset, void *data, size_t bytes) {
    if (!port) return NotOpen;
    if (!data || !validCall(ReadBuffer, 2, 0, 0, bytes)) return BadArgument;
    uint64_t in[] = {buffer, offset}; uint32_t no = 0; size_t outBytes = bytes;
    auto r = IOConnectCallMethod(port, ReadBuffer, in, 2, nullptr, 0, nullptr, &no, data, &outBytes);
    return !r && (no || outBytes != bytes) ? Error : r;
}
kern_return_t Connection::submit(uint64_t queue, uint64_t buffer, uint64_t offset, uint64_t bytes, uint64_t &fence) {
    uint64_t in[] = {queue, buffer, offset, bytes}, out = 0;
    auto r = scalar(Submit, in, 4, &out, 1);
    if (!r) fence = out;
    return r;
}
kern_return_t Connection::fence(uint64_t queue, uint64_t issued, bool &completed) {
    uint64_t in[] = {queue, issued}, out = 0;
    auto r = scalar(GetFence, in, 2, &out, 1);
    if (!r && out > 1) return Error;
    if (!r) completed = out != 0;
    return r;
}
}
