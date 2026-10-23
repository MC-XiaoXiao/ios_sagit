// SPDX-License-Identifier: BSD-2-Clause
#include "QcomGPU.h"
#include <sys/proc.h>
#include "QcomGPUNative.h"
#include <GPUNativeCompletion.h>
#include "QcomGPUMapper.h"
#include "AdrenoPipeline.h"
#include "NativeTimeline.h"
#include <GPUPrograms.h>
#include <GPUJITProtocol.h>
#include <GPUBlit.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLocks.h>
#include <IOKit/graphics/IOSurface.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <stdarg.h>
#include <kern/clock.h>
#include <kern/task.h>

static_assert(GPU::MaxCommands==GPU::OperationActivity::CommandCapacity,"private operation timing cell budget");
static_assert(Adreno::IBDwords == GPU::Blit::IBDwords, "Metal blit PM4 budget must match the native backend");
static_assert(uint64_t(Adreno::MaxIndirectBuffers)*Adreno::IBDwords*4<=GPU::Native::FramebufferIndirectLimit,
    "a complete maximum-size IB chain must fit the pre-Family admission budget");

static bool nativeCPUParameterBinding(const GPU::Shader::Program *program,unsigned &slot,unsigned &bytes) {
    if(!program || program!=GPUPrograms::lookup(program->id))return false;
    const bool words=!strcmp(program->name,"blit_copy_words") || !strcmp(program->name,"blit_fill_words");
    const bool copy=!strcmp(program->name,"blit_copy_words") || !strcmp(program->name,"blit_copy_bytes");
    if(!words && !copy && strcmp(program->name,"blit_fill_bytes") && strcmp(program->name,"blit_fill_rows") &&
        strcmp(program->name,"blit_fill_pairs") && strcmp(program->name,"blit_fill_rows_2d") && strcmp(program->name,"blit_fill_pairs_2d") &&
        strcmp(program->name,"blit_fill_rows_2d_tail") && strcmp(program->name,"blit_fill_pairs_2d_tail"))return false;
    slot=copy?2:1;bytes=words?16:32;return true;
}

class QcomGPUTask : public IOGPUTask {
    OSDeclareDefaultStructors(QcomGPUTask);
};
OSDefineMetaClassAndStructors(QcomGPUTask, IOGPUTask);

class QcomGPUSysMemory : public IOGPUSysMemory {
    OSDeclareDefaultStructors(QcomGPUSysMemory);
};
OSDefineMetaClassAndStructors(QcomGPUSysMemory, IOGPUSysMemory);
OSDefineMetaClassAndStructors(QcomGPUMemoryMap, IOGPUMemoryMap);
OSDefineMetaClassAndStructors(QcomGPUResource, IOGPUResource);
OSDefineMetaClassAndStructors(QcomGPUCommandQueue, IOGPUCommandQueue);
OSDefineMetaClassAndStructors(QcomGPUScheduler, IOGPUScheduler);
OSDefineMetaClassAndStructors(QcomGPUWorkQueue, IOGPUWorkQueue);
OSDefineMetaClassAndStructors(QcomGPUNativeSubmission, IOGPUCommandDescriptor);

// Shader ISA registration independently rechecks administrator or signed
// compositor task authorization. This trusted compiler ABI is not an ISA sandbox.
class QcomGPUProgram : public OSObject {
    OSDeclareDefaultStructors(QcomGPUProgram);
    OSData *fImage = nullptr;
public:
    GPU::Shader::Program program{};
    GPU::Shader::Program msaaProgram{};
    GPU::Shader::Program windowProgram{},msaaWindowProgram{};
    bool initWithImage(OSData *image, UInt32 id) {
        if (!OSObject::init() || !image || image->getLength() != sizeof(QGPUProgramImage)) return false;
        auto *data = static_cast<const QGPUProgramImage *>(image->getBytesNoCopy());
        if (!GPU::JIT::valid(*data, GPUPrograms::Registry)) return false;
        fImage = image; image->retain();
        memcpy(&program, &data->info, sizeof(data->info)); program.id = id;
        program.code = data->code; program.immediates = data->immediates; program.name = data->name;
        if (program.stage==GPU::Shader::Fragment) {
            memcpy(&msaaProgram,&data->msaa.info,sizeof(data->msaa.info));msaaProgram.id=id;
            msaaProgram.code=data->msaa.code;msaaProgram.immediates=data->msaa.immediates;msaaProgram.name=data->name;
            program.msaa=&msaaProgram;
            if(data->constantWindow.info.codeWords) {
                memcpy(&windowProgram,&data->constantWindow.info,sizeof(data->constantWindow.info));windowProgram.id=id;
                windowProgram.code=data->constantWindow.code;windowProgram.immediates=data->constantWindow.immediates;windowProgram.name=data->name;
                memcpy(&msaaWindowProgram,&data->msaaConstantWindow.info,sizeof(data->msaaConstantWindow.info));msaaWindowProgram.id=id;
                msaaWindowProgram.code=data->msaaConstantWindow.code;msaaWindowProgram.immediates=data->msaaConstantWindow.immediates;msaaWindowProgram.name=data->name;
                program.constantWindow=&windowProgram;msaaProgram.constantWindow=&msaaWindowProgram;
            }
        }
        return true;
    }
    void free() APPLE_KEXT_OVERRIDE { OSSafeReleaseNULL(fImage); OSObject::free(); }
};
OSDefineMetaClassAndStructors(QcomGPUProgram, OSObject);

bool QcomGPUMemoryMap::initWithBuffer(QcomGPUBuffer *buffer)
{
    if (!initNativeObject() || !buffer || !buffer->address) return false;
    fBuffer = buffer; buffer->retain(); return true;
}
void QcomGPUMemoryMap::nativeSlot15() { free(); }
UInt64 QcomGPUMemoryMap::address() { return fBuffer ? fBuffer->address : 0; }
UInt64 QcomGPUMemoryMap::gpuAddress() { return address(); }
UInt64 QcomGPUMemoryMap::length() { return fBuffer ? fBuffer->length : 0; }
UInt64 QcomGPUMemoryMap::mappedLength() { return GPU::pageSpanBytes(length()); }
// The device IOMapper already owns page tables and TLB synchronization. This
// map never enters Apple's address-space map lists or modifies their tables.
bool QcomGPUMemoryMap::updateGPUPageTable() { return false; }
void QcomGPUMemoryMap::nativeSlot27() {}
void QcomGPUMemoryMap::free()
{
    OSSafeReleaseNULL(fBuffer);
    IOGPUMemoryMap::free(); // native owner/list fields stayed zero
}
namespace {
// Family also uses 0x80 for an IOSurface cache root, with no device owner.
constexpr UInt32 SurfaceCacheRoot = GPU::Native::ClientBuffer;
bool surfaceMemoryLayout(IOMemoryDescriptor *memory,UInt64 allocation) {
    if(!memory)return false;
    uint64_t dmaOffset=0;auto dmaBytes=memory->getDMAMapLength(&dmaOffset);
    return GPU::Surface::descriptorLayout(allocation,memory->getLength(),dmaBytes,dmaOffset,PAGE_SIZE);
}
IOSurface *copySurface(QcomGPU *gpu,IOGPUDevice *device,const GPU::Native::ResourceArgs &args,
    GPU::Surface::Info &info)
{
    UInt32 id=args.clientAddress,plane=args.clientAddress>>32;
    if (!gpu || !device || !id || plane) return nullptr;
    auto *surface=gpu->nativeCopySurfaceForTask(id,device->owningTask());
    if (!surface) return nullptr;
    auto *memory=surface->nativeMemoryDescriptor();
    UInt64 width=surface->nativeWidth(),height=surface->nativeHeight(),pitch=surface->nativeRowBytes();
    UInt64 base=surface->nativeBaseOffset(),offset=surface->nativePlaneOffset(plane);
    bool valid=surface->nativeID()==id && !surface->nativePlaneCount() &&
        !surface->nativeProtection() && !surface->nativeTiled() && !surface->nativeTileFormat() &&
        surface->nativeElementWidth()==1 && surface->nativeElementHeight()==1 &&
        width<=UINT32_MAX && height<=UINT32_MAX && pitch<=UINT32_MAX &&
        base<=UINT32_MAX && offset<=UINT32_MAX-base && memory &&
        surfaceMemoryLayout(memory,surface->nativeAllocationSize());
    if (valid) {
        info={GPU::Surface::Version,GPU::Surface::Linear,id,plane,UInt32(width),UInt32(height),UInt32(pitch),
            surface->nativePixelFormat(),base+offset,surface->nativeAllocationSize(),pitch*height,
            surface->nativeBytesPerElement(),0};
        valid=GPU::Surface::valid(info);
    }
    if (!valid) {
        static UInt32 reports=0;
        if(gpu->getProperty("GPUMetalDiagnosticVersion") && __atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)<16) {
            uint64_t dmaOffset=0;auto dmaBytes=memory?memory->getDMAMapLength(&dmaOffset):0;
            kprintf("QcomGPU: IOSurface layout rejected id=%u size=%llux%llu row=%llu allocation=%llu descriptor=%llu dma=%llu dma-offset=%llu base=%llu plane-offset=%llu format=0x%x\n",
                id,width,height,pitch,surface->nativeAllocationSize(),memory?memory->getLength():0,dmaBytes,dmaOffset,base,offset,surface->nativePixelFormat());
        }
        surface->release();return nullptr;
    }
    return surface;
}
}
bool QcomGPUResource::initWithGPU(IOGPU *gpu, IOGPUDevice *device, UInt32 type)
{
    if (!initNativeObject()) return false;
    auto *provider = OSDynamicCast(QcomGPU, gpu);
    if (!provider || (type!=GPU::Native::Buffer && type!=GPU::Native::ClientBuffer && type!=GPU::Native::IOSurface) ||
        (!device && type!=SurfaceCacheRoot)) return false;
    fGPU = provider; provider->retain(); fOwner = device; fType=type;
    fDiagnosticCompositor=device && QcomGPU::nativeCompositorTask(device->owningTask());
    fNativeInitialized = true;
    return IOGPUResource::initWithGPU(gpu, device, type);
}
bool QcomGPUResource::initialize(IOGPUDevice *device, const void *data, UInt64 bytes)
{
    auto rejectImport = [&](const char *stage, IOMemoryDescriptor *descriptor=nullptr) {
        static UInt32 reports=0;
        if(fGPU && fOwner && fType==GPU::Native::ClientBuffer &&
            fGPU->getProperty("GPUMetalDiagnosticVersion") && __atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)<16)
            kprintf("QcomGPU: client import rejected stage=%s owner=%u native-length=%llu descriptor-length=%llu direction=%u dma-length=%llu parent=%u offset=%u:%u client-map=%u\n",
                stage,device==fOwner,nativeLength,descriptor?descriptor->getLength():0,
                descriptor?descriptor->getDirection():0,descriptor?descriptor->getDMAMapLength():0,
                nativeParent!=nullptr,nativeSurfaceOffset,nativePlane,nativeClientMap!=nullptr);
        return false;
    };
    if (fType==SurfaceCacheRoot && !fOwner) {
        auto *memory=OSDynamicCast(IOGPUSysMemory,nativeMemory);
        auto *descriptor=memory?memory->memoryDescriptor():nullptr;
        // The real IOSurfaceDeviceCache owns this object. Retaining its
        // IOSurface here would create a cache -> resource -> surface cycle.
        const bool valid=!device && !data && !bytes && !nativeProtection && descriptor && nativeLength &&
            nativeLength<=GPU::MaxBufferBytes && surfaceMemoryLayout(descriptor,descriptor->getLength()) &&
            GPU::Surface::descriptorLayout(descriptor->getLength(),descriptor->getLength(),nativeLength,0,PAGE_SIZE) &&
            IOGPUResource::initialize(nullptr,nullptr,0);
        if(!valid) {
            static UInt32 reports=0;
            if(fGPU && fGPU->getProperty("GPUMetalDiagnosticVersion") && __atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)<16) {
                uint64_t dmaOffset=0;auto dmaBytes=descriptor?descriptor->getDMAMapLength(&dmaOffset):0;
                kprintf("QcomGPU: IOSurface cache root rejected native=%llu descriptor=%llu dma=%llu dma-offset=%llu\n",
                    nativeLength,descriptor?descriptor->getLength():0,dmaBytes,dmaOffset);
            }
        }
        return valid;
    }
    if (fType==GPU::Native::IOSurface) {
        if (device!=fOwner || !data || bytes!=sizeof(GPU::Native::ResourceArgs) || fSurface || fCharge) return false;
        const auto &args=*static_cast<const GPU::Native::ResourceArgs *>(data);
        auto *parent=OSDynamicCast(QcomGPUResource,nativeParent);
        auto *memory=OSDynamicCast(IOGPUSysMemory,nativeMemory);
        GPU::Surface::Info info{};
        auto *surface=copySurface(fGPU,device,args,info);
        const auto charge=GPU::pageSpanBytes(info.allocationBytes,PAGE_SIZE);
        bool valid=surface && args.type==fType && parent && parent->fType==SurfaceCacheRoot && !parent->fOwner &&
            parent->fGPU==fGPU && parent->nativeMemory==nativeMemory && memory &&
            memory->memoryDescriptor()==surface->nativeMemoryDescriptor() &&
            GPU::Surface::descriptorLayout(info.allocationBytes,memory->memoryDescriptor()->getLength(),parent->nativeLength,0,PAGE_SIZE) && nativePlane==info.plane &&
            nativeSurfaceOffset==info.offset && !nativeProtection &&
            charge && IOGPUResource::initialize(device,data,bytes) && fGPU->reserveNative(charge);
        if (!valid) {if (surface) surface->release();return false;}
        fCharge=charge;fSurface=surface;fSurfaceInfo=info;
        surface->nativeIncrementUseCount();
        return true; // CPU mapping stays with IOSurface, no cache-mode alias
    }
    if (device != fOwner || !data || bytes != sizeof(GPU::Native::ResourceArgs) ||
        !nativeLength || nativeLength > GPU::MaxBufferBytes) return rejectImport("arguments");
    if (!IOGPUResource::initialize(device, data, bytes)) return rejectImport("family-initialize");
    auto *memory = OSDynamicCast(IOGPUSysMemory, nativeMemory);
    auto *descriptor = memory ? memory->memoryDescriptor() : nullptr;
    if (!descriptor || descriptor->getLength() != nativeLength) return rejectImport("descriptor",descriptor);
    if(fType==GPU::Native::ClientBuffer) {
        const auto &args=*static_cast<const GPU::Native::ResourceArgs *>(data);
        if(args.type!=fType || args.clientBase!=args.clientAddress || args.bytes!=nativeLength ||
            !GPU::Native::validClientBuffer(args.clientAddress,args.bytes,PAGE_SIZE) ||
            nativeParent || nativePlane || nativeSurfaceOffset || nativeClientMap ||
            (descriptor->getDirection()!=kIODirectionInOut && descriptor->getDirection()!=kIODirectionOut) ||
            descriptor->getDMAMapLength()!=nativeLength)
            return rejectImport("client-layout",descriptor);
        if(!fGPU->reserveNative(nativeLength))return rejectImport("budget",descriptor);
        // No replacement allocation or CPU alias. The original Family factory
        // has resolved the range against device->owningTask() and its VM map.
        fCharge=nativeLength;fClientAddress=args.clientAddress;return true;
    }
    if (!fGPU->reserveNative(nativeLength)) return false;
    fCharge = nativeLength;
    if (!nativeClientMap) nativeClientMap = descriptor->createMappingInTask(
        device->owningTask(), 0, kIOMapAnywhere, 0, 0);
    return nativeClientMap != nullptr;
}
UInt64 QcomGPUResource::mappingOptions(const void *data, UInt64 bytes)
{
    return IOGPUResource::mappingOptions(data, bytes) | 4; // request native map() hook
}
bool QcomGPUResource::map(UInt64 options, UInt32 alignment)
{
    if (!fGPU || alignment > 1 || (options & ~UInt64(7))) return false;
    if (fType==GPU::Native::IOSurface) {
        if (nativeMap) return fSurfaceView!=nullptr;
        auto *parent=OSDynamicCast(QcomGPUResource,nativeParent);
        if (!fSurface || !parent || parent->fType!=SurfaceCacheRoot || parent->fOwner || !parent->map(options,alignment)) return false;
        auto *view=QcomGPUBuffer::view(parent->buffer(),fSurfaceInfo.offset,fSurfaceInfo.imageBytes);
        if (!view) return false;
        fSurfaceView=view;nativeMap=parent->nativeMap;nativeMap->retain();return true;
    }
    if (nativeMap) return true;
    auto *memory = OSDynamicCast(IOGPUSysMemory, nativeMemory);
    auto *buffer = memory ? fGPU->mapNativeMemory(memory->memoryDescriptor(),fType==SurfaceCacheRoot) : nullptr;
    if (!buffer) return false;
    auto *mapping = new QcomGPUMemoryMap;
    bool ok = mapping && mapping->initWithBuffer(buffer);
    buffer->release();
    if (!ok) { if (mapping) mapping->release(); return false; }
    nativeMap = mapping; return true;
}
bool QcomGPUResource::mapAtAddressLength(UInt64, UInt64, UInt64) { return false; }
void QcomGPUResource::nativeSlot27() {} // resource groups are rejected at entry
UInt64 QcomGPUResource::nativeSlot28() { return 0; }
QcomGPUBuffer *QcomGPUResource::buffer() const
{
    if (fType==GPU::Native::IOSurface) return fSurfaceView;
    auto *mapping = OSDynamicCast(QcomGPUMemoryMap, nativeMap);
    return mapping ? mapping->buffer() : nullptr;
}
void QcomGPUResource::markSurfaceWritten()
{
    if (fSurface) fSurface->nativeMarkGPUWrite(1,1);
}
void QcomGPUResource::free()
{
    const auto releaseBegin=mach_absolute_time();
    const bool compositor=fDiagnosticCompositor;
    auto *gpu = fGPU; auto charge = fCharge;
    auto *surface=fSurface;
    OSSafeReleaseNULL(fSurfaceView);
    fGPU = nullptr; fCharge = 0;
    if (fNativeInitialized) IOGPUResource::free();
    else OSObject::free();
    if (surface) {surface->nativeDecrementUseCount();surface->release();}
    // Base free may destroy this object. Only saved locals are used below.
    if (gpu) { if (charge) gpu->releaseNative(charge,releaseBegin,compositor); gpu->release(); }
}
bool QcomGPUCommandQueue::initWithGPU(IOGPU *gpu, IOGPUDevice *device, const void *args)
{
    if (!initNativeObject()) return false;
    auto *provider = OSDynamicCast(QcomGPU, gpu);
    if (!provider || !device || !args || !provider->nativeAddressSpacesReady()) return false;
    fGPU = provider; provider->retain(); fOwner = device;
    fNativeInitialized = true;
    if(!IOGPUCommandQueue::initWithGPU(gpu, device, args))return false;
    fEventChannel=provider->reserveNativeEventChannel();
    if(fEventChannel==UINT32_MAX)return false;
    fWorkQueue=provider->createNativeWorkQueue(this);
    if(!fWorkQueue)return false;
    nativeSlot22();
    return true;
}
bool QcomGPUCommandQueue::reserveTokens(const GPU::Native::SubmitArgs &args)
{
    if (fFaulted || !args.startToken || args.startToken <= fLastToken || args.startToken == UINT64_MAX ||
        args.endToken != args.startToken + 1 || args.endToken == UINT64_MAX) return false;
    fLastToken = args.endToken; fResult = kIOReturnNotReady; return true;
}
void QcomGPUCommandQueue::nativeSlot22()
{
    // Family applies its task foreground and realtime-entitlement policy
    // before invoking this hook. It can hold the queue mutex here; publish
    // only the effective class, without entering submission/fence machinery.
    __atomic_store_n(&fPriority,nativeEffectivePriority,__ATOMIC_RELEASE);
    if(fWorkQueue) {
        static const UInt32 keys[]={0,2,3,3,1,0};
        fWorkQueue->nativeSetPriority(keys[nativeEffectivePriority<=5?nativeEffectivePriority:2]);
    }
}
void QcomGPUCommandQueue::submitCommandBuffer(const void *data)
{
    fResult = data && fGPU ? fGPU->submitNative(this, fOwner,
        *static_cast<const GPU::Native::SubmitArgs *>(data),&fScheduledSubmission) : kIOReturnBadArgument;
    // IOGPU::submitCommandBuffers returns admission status; the Family fence
    // records this error and writes the actual completion notification.
    if (fResult != kIOReturnSuccess) nativeSubmissionError =
        fResult == kIOReturnBadArgument || fResult == kIOReturnUnsupported ? 10 : 8;
}
void QcomGPUCommandQueue::free()
{
    auto *gpu = fGPU; fGPU = nullptr;
    const auto channel=fEventChannel;
    OSSafeReleaseNULL(fWorkQueue);
    if (fNativeInitialized) IOGPUCommandQueue::free();
    else OSObject::free();
    if (gpu) {if(channel!=UINT32_MAX)gpu->releaseNativeEventChannel(channel);gpu->release();}
}
IOGPUEventFence *QcomGPUCommandQueue::copyCurrentFence()
{
    auto *fence=OSDynamicCast(IOGPUEventFence,nativeCurrentFence);
    if (fence) fence->retain();
    return fence;
}
bool QcomGPUCommandQueue::eventStateValid() const
{
    const IOGPUEventSet *sets[]={&nativeCurrentEvents,&nativePriorEvents};
    for (auto *set:sets) if ((set->mask&~1u) ||
        ((set->mask&1) && (set->values[0]&255)!=fEventChannel)) return false;
    return true;
}
void QcomGPUNativeSubmission::free()
{
    // An admission failure can occur before this ticket is installed in a job.
    // A failed IOFence never exposes an unfinished surface as successful.
    finishSurfaceFences(kIOReturnAborted);
    auto *owner=device;device=nullptr;
    for (auto &resource : resources) OSSafeReleaseNULL(resource);
    OSSafeReleaseNULL(fence);
    OSSafeReleaseNULL(queue); IOGPUCommandDescriptor::free();
    // Device owns the namespace and task state referenced by the native
    // resources. Keep it alive even when the user closes after submission.
    // OSObject::free may destroy this object, so release a saved pointer.
    if (owner) owner->release();
}

void QcomGPUWorkQueue::submit(IOGPUCommandDescriptor *descriptor)
{
    static_cast<QcomGPU *>(nativeGPU)->submitScheduled(static_cast<QcomGPUNativeSubmission *>(descriptor));
}
void QcomGPUWorkQueue::restart() {static_cast<QcomGPU *>(nativeGPU)->restartScheduled();}
bool QcomGPUNativeSubmission::initSubmission(QcomGPU *gpu,QcomGPUCommandQueue *context)
{
    return IOGPUCommandDescriptor::initWithGPU(gpu,context,nullptr);
}
IOReturn QcomGPUNativeSubmission::prepare(UInt32)
{
    return static_cast<QcomGPU *>(nativeGPU)->prepareScheduled(this);
}
void QcomGPUNativeSubmission::cancel()
{
    IOGPUCommandDescriptor::cancel();
    static_cast<QcomGPU *>(nativeGPU)->cancelScheduled(this);
    // Failed hardware work stays owned by the backend until it has stopped.
    // The Family must not manufacture a successful completion for that work.
    // Keep the channel/value for original didSubmit bookkeeping, but exclude
    // it from the cancelled descriptor's completion set.
    nativeCompletionEvents.mask=0;
}
void QcomGPUNativeSubmission::setCompletionEvent()
{
    nativeCompletionEvents.clear();nativeCompletionEvents.mask=1;
    nativeCompletionEvents.values[0]=stamp|queue->eventChannel();
    // A540 alias/IOSurface readiness is checked by prepare(). Queue FIFO is
    // owned by IOGPUWorkQueue; unrelated queues do not inherit a global wait.
    nativeDependencies.clear();
    nativeFence=fence;if(nativeFence)nativeFence->retain();
}

namespace {
// A non-null callback selects native asynchronous preparation. No job/device
// context is borrowed: cancellation and late native wakeups cannot use freed
// ticket state. Dispatch reads the native ready byte under the existing gate.
void surfaceFenceReady(void *,UInt64,IOSurface *,IOReturn) {}
}
IOReturn QcomGPUNativeSubmission::prepareSurfaceFences(UInt32 accelerator,UInt32 &deferred)
{
    deferred=0;
    if(surfaceFenceCount || surfaceFencesFinished)return kIOReturnBadArgument;
    for(UInt32 i=0;i<resourceCount;++i) {
        auto *resource=OSDynamicCast(QcomGPUResource,resources[i]);
        auto *surface=resource?resource->surface():nullptr;
        if(!surface)continue;
        bool seen=false,write=false;
        for(UInt32 n=0;n<resourceCount;++n) {
            auto *other=OSDynamicCast(QcomGPUResource,resources[n]);
            if(other && other->surface()==surface) {
                if(n<i)seen=true;
                write|=(surfaceWrites&GPU::Native::Memory::writeBit(n))!=0;
            }
        }
        if(seen)continue;
        IOFence *native=nullptr;
        // Direction1 is read,2 write. Do not opt into AGX's same-accelerator
        // bypass: native IOSurface ordering includes current display readers.
        const bool ready=QcomIOSurfaceCreateAccessFence(surface,&native,write?2:1,accelerator,
            surfaceFenceReady,nullptr,0,0);
        if(!native)return kIOReturnNoMemory;
        const auto index=surfaceFenceCount++;surfaceFences[index]=native;
        if(ready)surfaceInitiallyReady|=GPU::Native::Memory::writeBit(index);
        else ++deferred;
    }
    return kIOReturnSuccess;
}
GPU::Native::SurfaceAccessObservation QcomGPUNativeSubmission::surfaceAccess() const
{
    using namespace GPU::Native;
    if(surfaceFencesFinished)return {SurfaceAccessState::Failed,UInt32(kIOReturnAborted)};
    return surfaceAccesses(surfaceFenceCount,[&](unsigned i) {
        return surfaceFences[i]?surfaceFences[i]->nativeAccessObservation(
            (surfaceInitiallyReady&Memory::writeBit(i))!=0):SurfaceAccessObservation{SurfaceAccessState::Failed,UInt32(kIOReturnAborted)};
    });
}
bool QcomGPUNativeSubmission::finishSurfaceFences(IOReturn status)
{
    if(surfaceFencesFinished)return true;
    surfaceFencesFinished=true;bool ok=true;
    for(auto *&native:surfaceFences)if(native) {
        const auto access=native->nativeAccessObservation(false);
        // Native signal rewrites status even when already completed. Preserve
        // a terminal native result; never turn it into successful access.
        if(native->nativeCompleted()) {
            if(status==kIOReturnSuccess && access.state==GPU::Native::SurfaceAccessState::Failed)ok=false;
        } else if(QcomIOFenceSignal(native,status)!=kIOReturnSuccess)ok=false;
        native->release();native=nullptr;
    }
    return ok;
}

void QcomGPUSurfaceWaitLog::append(const char *format,...)
{
    // Persist bounded failure evidence as well as emitting it. UART readers
    // may attach after a fault; no dependency snapshot should rely on an
    // uninterrupted serial reader. Never called with a native surface lock.
    if(count>160)return;
    char line[512];
    if(count==160)strlcpy(line,"QcomGPU: surface-wait-truncated limit=160",sizeof(line));
    else {va_list args;va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);}
    ++count;kprintf("%s\n",line);
    if(records)if(auto *value=OSString::withCString(line)){records->setObject(value);value->release();}
}
void QcomGPUNativeSubmission::logSurfaceFenceWait(unsigned jobOffset,QcomGPUSurfaceWaitLog &log) const
{
    // Failure-only observation of the pinned native ABI. The held IOFence
    // retains its IOSurface. Snapshot list links under that surface's original
    // lock; never print, allocate, signal or call back while holding it.
    auto word=[](const void *base,unsigned at) {UInt32 v;memcpy(&v,static_cast<const UInt8 *>(base)+at,4);return v;};
    auto pointer=[](const void *base,unsigned at) {void *v;memcpy(&v,static_cast<const UInt8 *>(base)+at,sizeof(v));return v;};
    struct Record {void *fence=nullptr,*transaction=nullptr,*callback=nullptr,*context=nullptr,*cookie=nullptr;UInt64 created=0;UInt32 accelerator=0,direction=0,status=0,completed=0,flags=0;};
    for(UInt32 i=0;i<surfaceFenceCount;++i) {
        auto *own=surfaceFences[i];
        if(!own || (surfaceInitiallyReady&GPU::Native::Memory::writeBit(i)) || own->nativeReady())continue;
        auto *surface=pointer(own,16);if(!surface)continue;
        auto *lock=static_cast<IOLock *>(pointer(surface,80));if(!lock)continue;
        Record records[17]{};unsigned used=1,counts[2]{},more[2]{};UInt32 surfaceID=0;
        auto snapshot=[&](Record &out,void *fence) {
            out.fence=fence;out.transaction=pointer(fence,24);out.callback=pointer(fence,40);
            out.context=pointer(fence,48);out.cookie=pointer(fence,56);
            memcpy(&out.created,static_cast<const UInt8 *>(fence)+144,8);
            out.accelerator=word(fence,32);out.direction=word(fence,36);
            out.status=word(fence,72);out.completed=word(fence,76);out.flags=word(fence,80);
        };
        if(!IOLockTryLock(lock)) {
            log.append("QcomGPU: surface-wait-lock-busy job=%u stamp=%u access=%u fence=%p",jobOffset,stamp,i,own);
            continue;
        }
        surfaceID=word(surface,16);snapshot(records[0],own);
        for(unsigned list=0;list<2;++list) {
            auto *node=pointer(surface,list?576:560);
            while(node && counts[list]<8) {
                snapshot(records[used++],node);++counts[list];node=pointer(node,88);
            }
            more[list]=node!=nullptr;
        }
        IOLockUnlock(lock);
        const auto &self=records[0];
        log.append("QcomGPU: surface-wait job=%u stamp=%u compositor=%u access=%u surface=%u fence=%p accelerator=%u direction=%u status=%x completed=%u flags=%x current=%u waiting=%u more=%u,%u",
            jobOffset,stamp,compositor,i,surfaceID,self.fence,self.accelerator,self.direction,self.status,self.completed,self.flags,counts[0],counts[1],more[0],more[1]);
        unsigned index=1;
        for(unsigned list=0;list<2;++list)for(unsigned n=0;n<counts[list];++n) {
            const auto &entry=records[index++];
            log.append("QcomGPU: surface-predecessor surface=%u list=%s index=%u fence=%p accelerator=%u direction=%u status=%x completed=%u flags=%x transaction=%p callback=%p context=%p cookie=%p created=%llu",
                surfaceID,list?"waiting":"current",n,entry.fence,entry.accelerator,entry.direction,entry.status,entry.completed,entry.flags,entry.transaction,entry.callback,entry.context,entry.cookie,entry.created);
        }
    }
}

// Native CPU stamps reflect retired A540 jobs, not command admission. The
// separate hardware fence is read and cache/IOSurface work finishes first.
class QcomGPUEventMachine : public IOGPUEventMachine {
    OSDeclareDefaultStructors(QcomGPUEventMachine);
    IOMemoryDescriptor *fMemory = nullptr;
    UInt64 fChannels=0,fEpoch=0;
public:
    GPU::Native::Timeline timelines[QcomGPUEventChannels];
    UInt32 reserveChannel() {
        auto used=__atomic_load_n(&fChannels,__ATOMIC_ACQUIRE);
        for(;;) {
            const auto free=~used;
            if(!free)return UINT32_MAX;
            const auto index=UInt32(__builtin_ctzll(free));
            if(index>=QcomGPUEventChannels)return UINT32_MAX;
            if(__atomic_compare_exchange_n(&fChannels,&used,used|(UInt64(1)<<index),false,
                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return index;
        }
    }
    void releaseChannel(UInt32 index) {__atomic_fetch_and(&fChannels,~(UInt64(1)<<index),__ATOMIC_RELEASE);}
    bool needsRebase() const {
        for(const auto &timeline:timelines)if(timeline.needsRebase())return true;
        return false;
    }
    UInt64 epoch() const {return fEpoch;}
    bool initWithGPU(IOGPU *gpu, UInt64 timeout, UInt32 count) APPLE_KEXT_OVERRIDE {
        auto *device = OSDynamicCast(QcomGPU, gpu);
        if (!device || count != QcomGPUEventChannels || !device->stampMemory() ||
            !IOGPUEventMachine::initWithGPU(gpu, timeout, count)) return false;
        fMemory = device->stampMemory(); fMemory->retain();
        setStampMemory(static_cast<IOBufferMemoryDescriptor *>(fMemory)->getBytesNoCopy());
        for(UInt32 index=0;index<count;++index)if(nativeIssuedStamp(index)!=0)return false;
        return true;
    }
    bool consistent(UInt32 index) {
        auto *memory=static_cast<IOBufferMemoryDescriptor *>(fMemory);
        return memory && nativeIssuedStamp(index)+GPU::Native::Timeline::Step==timelines[index].next() &&
            __atomic_load_n(static_cast<UInt32 *>(memory->getBytesNoCopy())+index,__ATOMIC_ACQUIRE)==timelines[index].completed();
    }
    bool rebase() {
        for(const auto &timeline:timelines)
            if(timeline.completed()+GPU::Native::Timeline::Step!=timeline.next())return false;
        auto *memory=static_cast<IOBufferMemoryDescriptor *>(fMemory);
        // All queues, native descriptors and Family fences are idle. Channels
        // retain monotonic stamps across queue reuse; only exhausted ones reset.
        for(UInt32 index=0;index<QcomGPUEventChannels;++index)if(timelines[index].needsRebase()) {
            if(!timelines[index].rebase(true,true,true))return false;
            nativeStampState[index].issued=0;nativeSubmittedStamp(index,0);
            __atomic_store_n(static_cast<UInt32 *>(memory->getBytesNoCopy())+index,0,__ATOMIC_RELEASE);
        }
        ++fEpoch;return true;
    }
    void free() APPLE_KEXT_OVERRIDE {
        OSSafeReleaseNULL(fMemory);
        IOGPUEventMachine::free();
    }
    void encodeStamp(UInt32 index, UInt32 *out, UInt32 stamp) APPLE_KEXT_OVERRIDE {
        out[0] = stamp; out[1] = index;
    }
    void clearEvent(void *event) APPLE_KEXT_OVERRIDE { bzero(event, 184); }
    bool supportsEvents() APPLE_KEXT_OVERRIDE { return false; }
    void appendEvent(UInt32 *event, UInt32 index, UInt32 stamp) APPLE_KEXT_OVERRIDE {
        // Original event record has a count followed by15 triples.
        if(event[0]>=15)return;
        auto *entry=event+1+3*event[0]++;entry[0]=stamp;entry[1]=index;entry[2]=0;
    }
};
OSDefineMetaClassAndStructors(QcomGPUEventMachine, IOGPUEventMachine);

class QcomGPUDeviceUserClient : public IOGPUDeviceUserClient {
    OSDeclareDefaultStructors(QcomGPUDeviceUserClient);
    QcomGPU *fGPU = nullptr;
    bool fActive = false, fClosing = false, fCloseIssued = false, fStandaloneClose = false;
    bool fDiagnosticCompositor = false; // immutable owning-task diagnostic classification
    IOExternalMethodArguments *fActiveArguments = nullptr;
    QcomGPUNativeSubmission *fScheduledSubmission = nullptr;
    GPU::Native::Memory::ResourceTable<> fResources;
    UInt32 fQueues[GPU::MaxQueues] = {}, fShmem[GPU::MaxBuffers] = {}, fNotifications[GPU::MaxQueues] = {};
    QcomGPUProgram *fPrograms[GPU::JIT::MaxPrograms] = {};
    UInt32 fNextProgram = GPU::JIT::FirstID;
    // Internal gate operation, never accepted by externalMethod.
    static constexpr UInt32 RegisterCompletionPort = UINT32_MAX - 3;
    void finishCall(IOExternalMethodArguments *arguments) {
        if (fGPU->nativeQuery(this, UINT32_MAX - 1, arguments) == kIOReturnNotOpen) {
            // Family teardown can wait for fences; keep it outside our gate.
            IOGPUDeviceUserClient::clientClose();
            fGPU->nativeQuery(this, UINT32_MAX - 2, arguments);
        }
    }
    template <typename T, unsigned N> static unsigned empty(const T (&ids)[N]) {
        unsigned i = 0; while (i < N && ids[i]) ++i; return i;
    }
    template <unsigned N> static void remove(UInt32 (&ids)[N], UInt32 id) {
        for (auto &value : ids) if (value == id) { value = 0; return; }
    }
public:
    QcomGPUNativeSubmission *takeScheduledSubmission() {auto *result=fScheduledSubmission;fScheduledSubmission=nullptr;return result;}
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        auto *gpu = OSDynamicCast(QcomGPU, provider);
        if (!gpu || !gpu->reserveNative(0)) return false;
        fGPU = gpu;
        fGPU->retain();
        const bool started=IOGPUDeviceUserClient::start(provider) && fGPU->registerNativeClient(this);
        if(started)fDiagnosticCompositor=device() && QcomGPU::nativeCompositorTask(device()->owningTask());
        return started;
    }
    void free() APPLE_KEXT_OVERRIDE {
        // Native device/task cleanup still needs its provider. Hold that
        // reference across base free, which can delete this client object.
        auto *gpu = fGPU; fGPU = nullptr;
        if (gpu) gpu->unregisterNativeClient(this);
        for (auto &program : fPrograms) OSSafeReleaseNULL(program);
        IOGPUDeviceUserClient::free();
        if (gpu) { gpu->releaseNative(0); gpu->release(); }
    }
    IOReturn externalMethod(UInt32 selector, IOExternalMethodArguments *a,
        IOExternalMethodDispatch *, OSObject *, void *) APPLE_KEXT_OVERRIDE {
        UInt32 ni = 0, no = 0, ib = 0, ob = 0;
        constexpr UInt32 sizes[] = {64, 64, 536, 8, 32, 16};
        if (!fGPU || isInactive()) return kIOReturnNotAttached;
        if ((selector==GPU::JIT::RegisterProgram || selector==GPU::JIT::DeleteProgram ||
             selector==GPU::Diagnostics::MetalFailureSelector || selector==GPU::Diagnostics::LiveSelector ||
             selector==GPU::Diagnostics::MetalConfigurationSelector || selector==GPU::MetalTiming::ReportSelector || selector==GPU::MetalTiming::CopySelector ||
             selector==GPU::MetalCapabilities::Selector) &&
            (!device() || !QcomGPU::nativeShaderTask(device()->owningTask())))
            return kIOReturnNotPrivileged;
        if (selector < 6) ob = sizes[selector];
        else switch (selector) {
            case GPU::Native::NewQueue: ib = sizeof(GPU::Native::QueueArgs); ob = 16; break;
            case GPU::Native::NewResource: ib = sizeof(GPU::Native::ResourceArgs); ob = sizeof(GPU::Native::ResourceData); break;
            case GPU::Native::DeleteQueue: case GPU::Native::DeleteResource:
            case GPU::Native::DeleteShmem: case GPU::Native::DeleteNotificationQueue: ni = 1; break;
            case GPU::Native::NewShmem: case GPU::Native::NewNotificationQueue: ni = 2; ob = 16; break;
            case GPU::Native::SetNotificationQueue: ni = 2; break;
            case GPU::Native::Submit: ni = 4; ib = sizeof(GPU::Native::SubmitArgs); break;
            case GPU::Native::SetQueuePriority: ni=1;no=1;ib=sizeof(GPU::Native::QueuePriority);break;
            case GPU::JIT::RegisterProgram: ni = GPU::JIT::UploadChunks; ob = sizeof(GPU::JIT::ProgramData); break;
            case GPU::JIT::DeleteProgram: ni = 1; break;
            case GPU::MetalTiming::ReportSelector: ib=sizeof(GPU::MetalTiming::Record);break;
            case GPU::MetalTiming::CopySelector: ob=sizeof(uint64_t);break;
            case GPU::MetalCapabilities::Selector: ob=sizeof(GPU::MetalCapabilities::Snapshot);break;
            case GPU::Diagnostics::MetalFailureSelector: ib=sizeof(GPU::Diagnostics::MetalFailure);break;
            case GPU::Diagnostics::MetalConfigurationSelector: ni=1;ob=sizeof(uint64_t);break;
            case GPU::Diagnostics::LiveSelector: ni=1;ob=sizeof(GPU::Diagnostics::LiveSnapshot);break;
            default: return kIOReturnUnsupported;
        }
        if (!a || a->scalarInputCount != ni || a->scalarOutputCount != no ||
            a->structureInputSize != ib || a->structureOutputSize != ob || a->structureInputDescriptor ||
            a->structureOutputDescriptor || a->asyncReference || a->asyncReferenceCount ||
            (ni && !a->scalarInput) || (ib && !a->structureInput) || (ob && !a->structureOutput)) return kIOReturnBadArgument;
        if (ob) bzero(a->structureOutput, ob);
        auto result = fGPU->nativeQuery(this, selector, a);
        finishCall(a);
        return result;
    }
    IOReturn clientClose() APPLE_KEXT_OVERRIDE {
        if (!fGPU) return kIOReturnSuccess;
        retain();
        auto result = fGPU->nativeQuery(this, UINT32_MAX, nullptr);
        if (result == kIOReturnNotOpen) {
            result = IOGPUDeviceUserClient::clientClose();
            fGPU->nativeQuery(this, UINT32_MAX - 2, nullptr);
        }
        release(); return result;
    }
    bool belongsTo(QcomGPU *gpu) const { return fGPU == gpu; }
    bool diagnosticCompositor() const { return fDiagnosticCompositor; }
    bool copyIdleQueues(QcomGPUCommandQueue **queues,UInt32 &count) {
        for (auto id:fQueues) if (id) {
            if (count==GPU::MaxClients*GPU::MaxQueues || !device()) return false;
            auto *object=device()->copyCommandQueue(id);
            auto *queue=OSDynamicCast(QcomGPUCommandQueue,object);
            if (!queue || queue->pending() || !queue->eventStateValid() ||
                !queue->workQueue()->nativeIdle()) {
                if (object) object->release();return false;
            }
            queues[count++]=queue;
        }
        return true;
    }
    const GPU::Shader::Program *lookupProgram(UInt32 id) const {
        for (auto *p : fPrograms) if (p && p->program.id == id) return &p->program;
        return nullptr;
    }
    IOReturn perform(UInt32 selector, IOExternalMethodArguments *a) {
        if (selector == UINT32_MAX) {
            fClosing = true;
            if (fActive || fCloseIssued) return kIOReturnSuccess;
            fCloseIssued = fStandaloneClose = true;
            fGPU->beginNativeCall(); return kIOReturnNotOpen;
        }
        if (selector == UINT32_MAX - 1) {
            if (!fActive || fActiveArguments != a) return kIOReturnSuccess;
            if (fClosing && !fCloseIssued) { fCloseIssued = true; return kIOReturnNotOpen; }
            fActive = false; fActiveArguments = nullptr; fGPU->endNativeCall(); return kIOReturnSuccess;
        }
        if (selector == UINT32_MAX - 2) {
            if (fActive && fActiveArguments == a) {
                fActive = false; fActiveArguments = nullptr; fGPU->endNativeCall();
            } else if (!a && fStandaloneClose) {
                fStandaloneClose = false; fGPU->endNativeCall();
            }
            return kIOReturnSuccess;
        }
        if (fClosing) return kIOReturnNotAttached;
        // Keep one Family call per connection. Teardown runs outside this
        // gate and may wait on its queue lock/fences; overlapping calls must
        // not block the IRQ work which retires those fences.
        if (fActive) return kIOReturnBusy;
        if (selector == RegisterCompletionPort) {
            const auto *input = a->scalarInput;
            bool owned = false;
            for (auto id : fNotifications) if (id && id == input[2]) owned = true;
            if (!owned || !device()) return kIOReturnBadArgument;
            fActive = true; fActiveArguments = a; fGPU->beginNativeCall();
            // Original Family type 0 resolves refCon in this device's
            // notification namespace and takes ownership of the send right
            // only on success. Its queue frees that right on teardown.
            return IOGPUDeviceUserClient::registerNotificationPort(
                reinterpret_cast<mach_port_t>(uintptr_t(input[0])), 0, input[2]);
        }
        if(selector==GPU::MetalCapabilities::Selector) {
            GPU::MetalCapabilities::Snapshot caps;
            auto number=[&](const char *key)->uint64_t {
                auto *value=OSDynamicCast(OSNumber,fGPU->getProperty(key));
                return value?value->unsigned64BitValue():0;
            };
            auto flag=[&](const char *key,uint32_t bit) {
                if(fGPU->getProperty(key)==kOSBooleanTrue)caps.flags|=bit;
            };
            caps.command=GPU::Native::CommandVersion;caps.completion=GPU::Native::CompletionVersion;
            caps.shader=GPU::Shader::Version;caps.jit=GPU::JIT::Version;
            caps.blit=GPU::Blit::NativeLayoutVersion;caps.maxJobs=GPU::MaxJobs;
            caps.backpressure=GPU::Native::AdmissionBackpressureVersion;caps.maxShmem=GPU::Native::MaxShmemBytes;
            caps.registry=GPUPrograms::Registry;caps.memory=fGPU->nativeMemoryLimits();
            caps.batch=uint32_t(number("IOGPUNativeShaderBatchVersion"));
            flag("IOGPUNativeSubmission",GPU::MetalCapabilities::Ready);
            flag("IOGPUNativeAddressSpaces",GPU::MetalCapabilities::AddressSpaces);
            flag("IOGPUNativeImmutableBlitParameters",GPU::MetalCapabilities::ImmutableBlit);
            caps.flags|=GPU::MetalCapabilities::GMEM;
            const struct Feature {const char *key;uint32_t bit;} features[]={
                {"IOGPUNativeMixedShaderBatchVersion",GPU::MetalCapabilities::MixedBatch},
                {"IOGPUNativePipelineVersion",GPU::MetalCapabilities::Pipeline},
                {"IOGPUNativeCompletionNotificationVersion",GPU::MetalCapabilities::Notifications},
                {"IOGPUNativeSurfaceFenceVersion",GPU::MetalCapabilities::SurfaceFences},
                {"GPUMetalDiagnosticVersion",GPU::MetalCapabilities::Diagnostics},
                {"GPUMetalShaderCaptureVersion",GPU::MetalCapabilities::ShaderCapture},
                {"GPUMetalTimingVersion",GPU::MetalCapabilities::Timing}};
            for(const auto &feature:features) {
                if(number(feature.key)==1)caps.flags|=feature.bit;
            }
            memcpy(a->structureOutput,&caps,sizeof(caps));return kIOReturnSuccess;
        }
        if(selector==GPU::Diagnostics::LiveSelector) {
            if(a->scalarInput[0]>1)return kIOReturnBadArgument;
            return fGPU->copyLiveStatistics(UInt32(a->scalarInput[0]),*static_cast<GPU::Diagnostics::LiveSnapshot *>(a->structureOutput));
        }
        if(selector==GPU::MetalTiming::ReportSelector || selector==GPU::MetalTiming::CopySelector) {
            auto *version=OSDynamicCast(OSNumber,fGPU->getProperty("GPUMetalTimingVersion"));
            if(!version || version->unsigned32BitValue()!=GPU::MetalTiming::Version)return kIOReturnUnsupported;
            if(selector==GPU::MetalTiming::CopySelector) {
                return fGPU->publishMetalTiming(*static_cast<uint64_t *>(a->structureOutput));
            }
            return fGPU->recordMetalTiming(*static_cast<const GPU::MetalTiming::Record *>(a->structureInput),proc_selfpid(),diagnosticCompositor());
        }
        if(selector==GPU::Diagnostics::MetalConfigurationSelector) {
            if(a->scalarInput[0]>1)return kIOReturnBadArgument;
            auto *version=OSDynamicCast(OSNumber,fGPU->getProperty("GPUMetalDiagnosticVersion"));
            const uint64_t enabled=version && version->unsigned32BitValue()==GPU::Diagnostics::MetalFailureVersion?
                GPU::Diagnostics::MetalFailureVersion:0;
            memcpy(a->structureOutput,&enabled,sizeof(enabled));
            static unsigned observations=0;
            if(enabled && observations++<32) {
                auto *creator=OSDynamicCast(OSString,getProperty("IOUserClientCreator"));
                kprintf("QcomGPU: Metal diagnostic configuration creator=%s registry-enabled=%llu enabled=%llu\n",
                    creator?creator->getCStringNoCopy():"unknown",a->scalarInput[0],enabled);
            }
            return kIOReturnSuccess;
        }
        if (selector==GPU::Diagnostics::MetalFailureSelector) {
            auto *version=OSDynamicCast(OSNumber,fGPU->getProperty("GPUMetalDiagnosticVersion"));
            if(!version || version->unsigned32BitValue()!=GPU::Diagnostics::MetalFailureVersion)return kIOReturnUnsupported;
            GPU::Diagnostics::MetalFailure failure;
            memcpy(&failure,a->structureInput,sizeof(failure));
            if(!GPU::Diagnostics::validMetalFailure(failure))return kIOReturnBadArgument;
            // Untrusted text is data, never a format string or a line break.
            for(auto &c:failure.message)if(c && (UInt8(c)<32 || UInt8(c)==127))c=' ';
            auto *previous=OSDynamicCast(OSArray,fGPU->getProperty("GPUMetalFailureReports"));
            if(previous)for(unsigned i=0;i<previous->getCount();++i) {
                auto *entry=OSDynamicCast(OSDictionary,previous->getObject(i));
                auto *stage=entry?OSDynamicCast(OSNumber,entry->getObject("Stage")):nullptr;
                auto *code=entry?OSDynamicCast(OSNumber,entry->getObject("Code")):nullptr;
                auto *message=entry?OSDynamicCast(OSString,entry->getObject("Message")):nullptr;
                if(stage && code && message && stage->unsigned32BitValue()==failure.stage &&
                    code->unsigned32BitValue()==failure.code && message->isEqualTo(failure.message))return kIOReturnSuccess;
            }
            if(previous && previous->getCount()>=GPU::Diagnostics::MaxMetalFailures)return kIOReturnNoResources;
            auto *reports=previous?OSArray::withArray(previous):OSArray::withCapacity(GPU::Diagnostics::MaxMetalFailures);
            auto *entry=OSDictionary::withCapacity(6);
            auto *stage=OSNumber::withNumber(failure.stage,32),*code=OSNumber::withNumber(failure.code,32);
            auto *time=OSNumber::withNumber(mach_absolute_time(),64);
            auto *message=OSString::withCString(failure.message);
            bool compositor=device() && QcomGPU::nativeCompositorTask(device()->owningTask());
            bool ok=reports && entry && stage && code && time && message &&
                entry->setObject("Stage",stage) && entry->setObject("Code",code) && entry->setObject("Timestamp",time) &&
                entry->setObject("Message",message) && entry->setObject("Compositor",compositor?kOSBooleanTrue:kOSBooleanFalse);
            auto *creator=OSDynamicCast(OSString,getProperty("IOUserClientCreator"));
            if(ok && creator)ok=entry->setObject("Creator",creator);
            // Publish a fresh array: concurrent registry readers retain an
            // immutable snapshot while another client reports a failure.
            if(ok)ok=reports->setObject(entry) && fGPU->setProperty("GPUMetalFailureReports",reports);
            if(ok) {
                fGPU->setProperty("GPUMetalFailureCount",reports->getCount(),32);
                kprintf("QcomGPU: Metal failure compositor=%u stage=%u code=%x message=%s\n",
                    compositor,failure.stage,failure.code,failure.message);
            }
            OSSafeReleaseNULL(reports);OSSafeReleaseNULL(entry);OSSafeReleaseNULL(stage);
            OSSafeReleaseNULL(code);OSSafeReleaseNULL(time);OSSafeReleaseNULL(message);
            return ok?kIOReturnSuccess:kIOReturnNoMemory;
        }
        if (selector == GPU::JIT::RegisterProgram) {
            UInt32 slot = 0; while (slot < GPU::JIT::MaxPrograms && fPrograms[slot]) ++slot;
            if (slot == GPU::JIT::MaxPrograms || fNextProgram == UINT32_MAX) return kIOReturnNoResources;
            // Keep the shared-memory transport's 64 KiB per-object bound.
            // Copy both owned pieces before validating any image metadata;
            // submitted programs retain only this private immutable image.
            auto *image=OSData::withCapacity(sizeof(QGPUProgramImage));
            if(!image)return kIOReturnNoMemory;
            IOReturn copied=kIOReturnSuccess;
            for(unsigned part=0;part<GPU::JIT::UploadChunks;++part) {
                const UInt64 id=a->scalarInput[part];bool owned=false;
                for(auto value:fShmem)if(value==id)owned=true;
                if(!id || id>UINT32_MAX || !owned || (part && id==a->scalarInput[0])){copied=kIOReturnBadArgument;break;}
                auto *shmem=device()->copyShmem(UInt32(id));
                if(!shmem){copied=kIOReturnBadArgument;break;}
                const unsigned bytes=GPU::JIT::uploadChunkBytes(part);
                auto *data=shmem->length()>=bytes?shmem->mapKernel():nullptr;
                if(!data)copied=kIOReturnBadArgument;
                else if(!image->appendBytes(data,bytes))copied=kIOReturnNoMemory;
                if(data)shmem->unmapKernel();shmem->release();
                if(copied!=kIOReturnSuccess)break;
            }
            if(copied!=kIOReturnSuccess){image->release();return copied;}
            auto *program = new QcomGPUProgram;
            bool ok = program && program->initWithImage(image, fNextProgram);
            image->release();
            if (!ok) { if (program) program->release(); return kIOReturnBadArgument; }
            fPrograms[slot] = program;
            *static_cast<GPU::JIT::ProgramData *>(a->structureOutput) = {GPU::JIT::Version, fNextProgram++};
            return kIOReturnSuccess;
        }
        if (selector == GPU::JIT::DeleteProgram) {
            UInt64 id = a->scalarInput[0];
            if (id < GPU::JIT::FirstID || id > UINT32_MAX) return kIOReturnBadArgument;
            for (auto &program : fPrograms) if (program && program->program.id == id) {
                OSSafeReleaseNULL(program); return kIOReturnSuccess;
            }
            return kIOReturnBadArgument;
        }
        // Resource construction prepares CPU objects/descriptors. Any new DMA
        // map validates the actual hardware in the mapper before PTE writes
        // and TLBSYNC; an existing cached map needs no hardware access here.
        // Submit still independently validates hardware before admission.
        if (selector == GPU::Native::NewResource && !fGPU->nativeCPUPreparationReady()) return kIOReturnNotReady;
        if ((selector == GPU::Native::NewQueue || selector == GPU::Native::Submit) &&
            !fGPU->nativeAddressSpacesReady()) return kIOReturnNotReady;
        auto *in = a->scalarInput;
        QcomGPUCommandQueue *queue = nullptr;
        if (selector == GPU::Native::NewQueue) {
            auto *args = static_cast<const GPU::Native::QueueArgs *>(a->structureInput);
            if (empty(fQueues) == GPU::MaxQueues) return kIOReturnNoResources;
            if (args->priority || args->flags || args->reserved[0] || args->reserved[1] || args->reserved[2] ||
                args->name[sizeof(args->name) - 1]) return kIOReturnBadArgument;
        } else if(selector==GPU::Native::SetQueuePriority) {
            const auto &priority=*static_cast<const GPU::Native::QueuePriority *>(a->structureInput);
            if(!in[0] || in[0]>UINT32_MAX || !GPU::Native::validQueuePriority(priority))return kIOReturnBadArgument;
            auto *object=device()->copyCommandQueue(UInt32(in[0]));
            auto *target=OSDynamicCast(QcomGPUCommandQueue,object);
            if(object)object->release();
            if(!target)return kIOReturnBadArgument;
            // The original Family selector performs foreground/realtime
            // authorization and returns the applied priority. Its native
            // slot22 hook publishes that result to the A540 scheduler.
        } else if (selector == GPU::Native::NewResource) {
            const auto &args = *static_cast<const GPU::Native::ResourceArgs *>(a->structureInput);
            const auto limits=fGPU->nativeMemoryLimits();
            if (args.flags || args.protection || args.gpuAddress || args.gpuLength ||
                args.alignment > 1 || args.tag || args.parentID) return kIOReturnBadArgument;
            for (auto value : args.native8) if (value) return kIOReturnBadArgument;
            UInt64 charge=args.bytes;
            if (args.type==GPU::Native::IOSurface) {
                if (args.cacheMode || args.bytes || args.clientBase) return kIOReturnBadArgument;
                GPU::Surface::Info info{}; auto *surface=copySurface(fGPU,device(),args,info);
                if (!surface) return kIOReturnBadArgument;
                charge=GPU::pageSpanBytes(info.allocationBytes,PAGE_SIZE);surface->release();
            } else if(args.type==GPU::Native::ClientBuffer) {
                if(args.cacheMode || args.clientBase!=args.clientAddress ||
                    !GPU::Native::validClientBuffer(args.clientAddress,args.bytes,PAGE_SIZE))return kIOReturnBadArgument;
            } else if (args.type!=GPU::Native::Buffer || args.cacheMode!=kIOMapWriteCombineCache ||
                args.clientAddress || args.clientBase || !args.bytes || args.bytes>limits.resourceBytes ||
                (args.bytes&(GPU::PageBytes-1))) return kIOReturnBadArgument;
            // Native page_size can exceed the diagnostic protocol's 4 KiB.
            if(charge>limits.resourceBytes || !fResources.canAdd((charge+16383)&~UInt64(16383),limits.clientBytes))
                return kIOReturnNoResources;
        } else if (selector == GPU::Native::NewShmem) {
            if (empty(fShmem) == GPU::MaxBuffers) return kIOReturnNoResources;
            if (!in[0] || in[0] > GPU::Native::MaxShmemBytes || in[1]) return kIOReturnBadArgument;
        } else if (selector == GPU::Native::NewNotificationQueue) {
            if (empty(fNotifications) == GPU::MaxQueues) return kIOReturnNoResources;
            if (!in[0] || in[0] > 256 || in[1] != 40) return kIOReturnBadArgument;
        } else if (selector == GPU::Native::Submit) {
            const auto &args = *static_cast<const GPU::Native::SubmitArgs *>(a->structureInput);
            if (!in[0] || in[0] > UINT32_MAX || in[1] || in[2] != 1 || in[3] != sizeof(args) ||
                !args.commands || args.segments || args.sideband || args.flags || args.resources || args.reserved)
                return kIOReturnBadArgument;
            auto *native = device()->copyCommandQueue(static_cast<UInt32>(in[0]));
            queue = OSDynamicCast(QcomGPUCommandQueue, native);
            if (!queue) {
                if (native) native->release(); return kIOReturnBadArgument;
            }
            // Reject temporary pressure before reserveTokens or any original
            // Family fence/error bookkeeping. Retry uses the same tokens.
            bool indexed=false,framebuffer=false,cpuReadHazard=false,planValid=true;
            const bool pipelined=fGPU->nativePipelinedAdmission();
            const bool inplace=fGPU->nativeFramebufferInplace(device()->owningTask());
            GPU::Native::AdmissionMemory request{};
            auto cpuRead=[&](UInt32 id,UInt64 offset,UInt64 bytes) {
                auto *native=id?device()->copyResource(id):nullptr;
                auto *resource=OSDynamicCast(QcomGPUResource,native);
                auto *buffer=resource && resource->ownedBy(device())?resource->buffer():nullptr;
                const bool valid=buffer && GPU::range(buffer->length,offset,bytes);
                if(valid)cpuReadHazard|=fGPU->nativeCPUReadBlocked(buffer);
                if(native)native->release();return valid;
            };
            auto *shmem=device()->copyShmem(args.commands);
            if(shmem) {
                const auto length=shmem->length();auto *bytes=shmem->mapKernel();
                const UInt32 copied=UInt32(length<GPU::Native::MaxShmemBytes?length:GPU::Native::MaxShmemBytes);
                OSData *copy=bytes && copied>=sizeof(GPU::Native::CommandHeader)?OSData::withBytes(bytes,copied):nullptr;
                if(bytes)shmem->unmapKernel();shmem->release();
                if(copy) {
                    const auto *h=static_cast<const GPU::Native::CommandHeader *>(copy->getBytesNoCopy());
                    const UInt32 commandsOffset=GPU::Native::commandOffset(h->responsibleTaskCount,h->version);
                    const auto *commands=commandsOffset && h->count && h->count<=GPU::MaxCommands?
                        static_cast<const GPU::Command *>(copy->getBytesNoCopy(commandsOffset,h->count*sizeof(GPU::Command))):nullptr;
                    UInt32 end=0;
                    const bool batch=commands && h->magic==GPU::Native::CommandMagic &&
                        (h->version==GPU::Native::BatchCommandVersion || h->version==GPU::Native::RenderPassCommandVersion || (h->version==GPU::Native::CommandVersion && h->count==1)) &&
                        GPU::Native::shaderBatchLayout(commands,h->count,commandsOffset,copied,
                            sizeof(GPU::Shader::Dispatch),sizeof(GPU::Shader::Draw),end);
                    const auto *pass=batch && h->version==GPU::Native::RenderPassCommandVersion?
                        static_cast<const GPU::RenderPass::Description *>(copy->getBytesNoCopy(
                            GPU::Native::commandOffset(h->responsibleTaskCount),sizeof(GPU::RenderPass::Description))):nullptr;
                    framebuffer|=pass!=nullptr;
                    if(pass && (!GPU::RenderPass::valid(*pass) || pass->registry!=GPUPrograms::Registry))planValid=false;
                    if(batch) {for(UInt32 i=0;i<h->count;++i)if(commands[i].opcode==GPU::ShaderDraw) {
                        const auto *draw=static_cast<const GPU::Shader::Draw *>(copy->getBytesNoCopy(commands[i].offset,sizeof(GPU::Shader::Draw)));
                        indexed|=draw->indexSize!=0;
                        if(pass && !GPU::RenderPass::drawMatches(*pass,*draw))planValid=false;
                        auto *program=draw->fragment<GPU::JIT::FirstID?GPUPrograms::lookup(draw->fragment):lookupProgram(draw->fragment);
                        if(program)framebuffer|=GPU::Shader::rasterProgram(*program,draw->rasterSamples).framebufferMask!=0;
                        if(pipelined) {
                            Adreno::FramebufferStorage storage;
                            if(pass) {
                                GPU::RenderPass::TileLayout tile;
                                if(!GPU::RenderPass::tileLayout(*pass,tile))planValid=false;
                                else request.snapshotBytes=GPU::RenderPass::scratchBytes(*pass,tile);
                            }
                            else if(!program || !Adreno::framebufferStoragePlan(*draw,GPU::Shader::rasterProgram(*program,draw->rasterSamples),inplace,false,storage,fGPU->nativeFramebufferCompact()))planValid=false;
                            else if(storage.bytes>request.snapshotBytes)request.snapshotBytes=storage.bytes;
                            if(draw->indexSize) {
                                UInt32 offset=0;
                                if(!GPU::Native::appendIndexSnapshot(draw->indices.bytes,request.indexBytes,offset) ||
                                    !cpuRead(draw->indices.resource,draw->indices.offset,draw->indices.bytes))planValid=false;
                            }
                        }
                    } else if(pass)planValid=false;
                    else if(pipelined && commands[i].opcode==GPU::ShaderDispatch) {
                        const auto *dispatch=static_cast<const GPU::Shader::Dispatch *>(copy->getBytesNoCopy(commands[i].offset,sizeof(GPU::Shader::Dispatch)));
                        const auto *program=GPUPrograms::lookup(dispatch->program);unsigned slot=0,bytes=0;
                        if(nativeCPUParameterBinding(program,slot,bytes)) {
                            const auto &parameters=dispatch->buffers[slot];
                            if(parameters.bytes<bytes || !cpuRead(parameters.resource,parameters.offset,bytes))planValid=false;
                        }
                    }}
                    if(batch && pipelined) {
                        request.indexBytes=GPU::Native::indexSnapshotAllocation(request.indexBytes);
                        if(!framebuffer && !pass)request.snapshotBytes=0; // Ordinary 4KiB completion scratch is not in the framebuffer pool.
                        if(framebuffer || pass || (h->version==GPU::Native::BatchCommandVersion && h->count>1))
                            request.indirectBytes=uint64_t(Adreno::MaxIndirectBuffers)*Adreno::IBDwords*4;
                    }
                    copy->release();
                } else if(bytes){queue->release();return kIOReturnNoMemory;}
            }
            if(!planValid){queue->release();return kIOReturnBadArgument;}
            if(fGPU->nativeAdmissionBlocked(indexed,framebuffer,request,cpuReadHazard)) {
                queue->release();return IOReturn(GPU::Native::AdmissionDeferredStatus);
            }
            if(!queue->reserveTokens(args)){queue->release();return kIOReturnBadArgument;}
        } else if (selector==GPU::Native::DeleteQueue || selector==GPU::Native::SetNotificationQueue) {
            if (!in[0] || in[0]>UINT32_MAX || (selector==GPU::Native::SetNotificationQueue && (!in[1] || in[1]>UINT32_MAX)))
                return kIOReturnBadArgument;
            auto *object=device()->copyCommandQueue(UInt32(in[0]));
            auto *target=OSDynamicCast(QcomGPUCommandQueue,object);
            bool busy=target && target->pending();
            if (object) object->release();
            if (!target) return kIOReturnBadArgument;
            if (busy) return kIOReturnBusy;
        } else if (selector > 5) {
            for (UInt32 i = 0; i < a->scalarInputCount; ++i)
                if (!in[i] || in[i] > UINT32_MAX) return kIOReturnBadArgument;
        }
        fActive = true; fActiveArguments = a; fGPU->beginNativeCall();
        auto result = IOGPUDeviceUserClient::externalMethod(selector, a, nullptr, nullptr, nullptr);
        if (!result) {
            if (selector == GPU::Native::NewQueue) fQueues[empty(fQueues)] =
                static_cast<UInt32>(static_cast<GPU::Native::QueueData *>(a->structureOutput)->id);
            else if (selector == GPU::Native::NewResource) {
                auto *out = static_cast<GPU::Native::ResourceData *>(a->structureOutput);
                auto *native = device()->copyResource(out->id);
                auto *resource = OSDynamicCast(QcomGPUResource, native);
                bzero(out->privateInfo,sizeof(out->privateInfo));
                if (resource) {
                    out->cpuAddress=resource->cpuAddress();
                    if (auto *surface=resource->surfaceInfo()) memcpy(out->privateInfo,surface,sizeof(*surface));
                }
                const auto charge=resource?resource->chargedBytes():0;
                if(!resource || !resource->ownedBy(device()) || !fResources.add(out->id,charge,fGPU->nativeMemoryLimits().clientBytes)) {
                    // The Family has created the object already. Roll back
                    // unexpected accounting/rounding failure before returning
                    // an ID which this connection cannot track or release.
                    IOExternalMethodArguments rollback{};UInt64 id=out->id;
                    rollback.version=a->version;rollback.scalarInput=&id;rollback.scalarInputCount=1;
                    if(!id || IOGPUDeviceUserClient::externalMethod(GPU::Native::DeleteResource,&rollback,nullptr,nullptr,nullptr)!=kIOReturnSuccess)
                        fClosing=true; // finish outside this gate through the existing close path
                    bzero(out,sizeof(*out));result=kIOReturnNoResources;
                }
                if(native)native->release();
            } else if (selector == GPU::Native::NewShmem) fShmem[empty(fShmem)] =
                static_cast<GPU::Native::ShmemData *>(a->structureOutput)->id;
            else if (selector == GPU::Native::NewNotificationQueue) fNotifications[empty(fNotifications)] =
                static_cast<GPU::Native::NotificationData *>(a->structureOutput)->id;
            else if (selector == GPU::Native::DeleteResource) {
                fResources.remove(UInt32(in[0]));
            } else if (selector == GPU::Native::DeleteQueue) remove(fQueues, in[0]);
            else if (selector == GPU::Native::DeleteShmem) remove(fShmem, in[0]);
            else if (selector == GPU::Native::DeleteNotificationQueue) remove(fNotifications, in[0]);
            else if (queue) result = queue->result();
        }
        if (queue) {fScheduledSubmission=queue->takeScheduledSubmission();queue->release();}
        return result;
    }
    IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t reference) APPLE_KEXT_OVERRIDE {
        if (!fGPU || isInactive()) return kIOReturnNotAttached;
        if (type || !port || !reference || reference > UINT32_MAX) return kIOReturnBadArgument;
        UInt64 input[] = {uintptr_t(port), type, reference};
        IOExternalMethodArguments arguments{};
        arguments.scalarInput = input; arguments.scalarInputCount = 3;
        auto result = fGPU->nativeQuery(this, RegisterCompletionPort, &arguments);
        finishCall(&arguments);
        return result;
    }
    IOExternalTrap *getTargetAndTrapForIndex(IOService **target, UInt32) APPLE_KEXT_OVERRIDE {
        if (target) *target = nullptr;
        return nullptr;
    }
};
OSDefineMetaClassAndStructors(QcomGPUDeviceUserClient, IOGPUDeviceUserClient);

static_assert(GPU::Native::AdmissionDeferredStatus==uint32_t(iokit_vendor_specific_err(0x100)),"native IOKit deferred status");
bool QcomGPU::nativeFramebufferInplace(task_t task) const {
    return task && (fFramebufferFetchMode==2 || fFramebufferFetchMode==3 || (fFramebufferFetchMode==1 && !nativeCompositorTask(task) &&
        IOUserClient::clientHasPrivilege(task,kIOClientPrivilegeAdministrator)==kIOReturnSuccess));
}
bool QcomGPU::nativeCPUReadBlocked(const QcomGPUBuffer *buffer,UInt32 ownUses) const {
    if(!buffer)return true;
    const auto *readRoot=buffer->backing();
    GPU::Native::CPUReadHazards hazards(buffer->pendingGPU(),ownUses);
    for(UInt32 i=0;i<fJobCount;++i) {
        const auto &job=fJobs[(fJobHead+i)%GPU::MaxJobs];
        for(UInt32 n=0;n<job.bufferCount;++n) {
            const auto *pending=job.buffers[n];if(!pending)return true;
            if(job.native && n>=job.native->resourceCount)return true;
            const bool written=!job.native || (job.native->surfaceWrites&GPU::Native::Memory::writeBit(n));
            // synchronize() flushes the entire backing descriptor, not just
            // the index/layout span. Check its full range against GPU writes.
            const auto *pendingRoot=pending->backing();
            const bool alias=written && readRoot->overlaps(pendingRoot,0,readRoot->length,0,pendingRoot->length);
            hazards.use(readRoot==pendingRoot,alias);
        }
    }
    return hazards.blocked();
}
bool QcomGPU::nativeAdmissionBlocked(bool indexed,bool framebuffer,const GPU::Native::AdmissionMemory &request,bool cpuReadHazard) {
    auto *events=OSDynamicCast(QcomGPUEventMachine,nativeEventMachine());
    // Rebase requires a real idle epoch. Stop new admissions before entering
    // the Family so a continuous asynchronous stream can drain naturally.
    const bool epochDrain=events && events->needsRebase() && (fJobCount || fNativeCalls);
    const GPU::Native::AdmissionMemory live{fFramebufferSnapshotBytes,fFramebufferIBBytes,fIndexSnapshotBytes};
    const bool blocked=epochDrain || (fPipelinedAdmission?GPU::Native::pipelinedAdmissionBlocked(fJobCount,cpuReadHazard,request,live):
        GPU::Native::admissionBlocked(fJobCount,indexed,framebuffer,fFramebufferSnapshotBytes,fFramebufferIBBytes));
    if(blocked) {
        GPU::Diagnostics::add(fNativeBackpressure);
        if(fPipelinedAdmission && !epochDrain)GPU::Diagnostics::add(cpuReadHazard?fCPUReadDeferrals:fPipelineCapacityDeferrals);
    }
    return blocked;
}

IOReturn QcomGPU::prepareNativeCompletion(QcomGPUNativeSubmission *ticket)
{
    auto *events=OSDynamicCast(QcomGPUEventMachine,nativeEventMachine());
    auto *fences=nativeFenceMachine();
    if (!ticket || !ticket->queue || !events || !fences || !events->consistent(ticket->queue->eventChannel()) ||
        !ticket->queue->eventStateValid()) return kIOReturnIOError;
    ticket->fence=ticket->queue->copyCurrentFence();
    if (!ticket->fence) return kIOReturnIOError;
    if (!events->needsRebase()) return kIOReturnSuccess;
    // No native notification may still refer to the old epoch. Sweep first;
    // the current Family fence exists, but has not entered the pending list.
    fences->nativeRetire();
    fences->nativeLock();
    QcomGPUCommandQueue *queues[GPU::MaxClients*GPU::MaxQueues]={};UInt32 count=0;
    bool idle=!fJobCount && fNativeCalls==1 && !fences->pendingLocked();
    for (auto *object:fNativeClientRegistry) if (idle && object) {
        auto *client=OSDynamicCast(QcomGPUDeviceUserClient,object);
        idle=client && client->copyIdleQueues(queues,count);
    }
    if (idle) {
        for (UInt32 i=0;i<count;++i) queues[i]->clearNativeEvents();
        ticket->fence->clearNativeStartEvents();
        idle=events->rebase();
    }
    for (UInt32 i=0;i<count;++i) queues[i]->release();
    fences->nativeUnlock();
    if (idle) setProperty("IOGPUNativeCompletionEpoch",events->epoch(),64);
    return idle?kIOReturnSuccess:kIOReturnBusy;
}
bool QcomGPU::armNativeCompletion(QcomGPUNativeSubmission *ticket)
{
    auto *events=OSDynamicCast(QcomGPUEventMachine,nativeEventMachine());
    if(!ticket || ticket->armed || !events)return false;
    const auto channel=ticket->queue->eventChannel();auto &timeline=events->timelines[channel];
    if(!events->consistent(channel) || !timeline.reserve(ticket->stamp))return false;
    events->nativeAdvanceStamp(channel);ticket->queue->addNativeEvent(events);
    ticket->armed=true;ticket->queue->beginSubmission();
    return events->nativeIssuedStamp(channel)+GPU::Native::Timeline::Step==timeline.next();
}
bool QcomGPU::completeNative(QcomGPUNativeSubmission *ticket,bool success)
{
    if (!ticket || ticket->done) return true;
    if(!ticket->armed) {
        // Quarantine retains tickets and DMA mappings, so free() may never
        // run. Cancel surface access without advancing an unreserved stamp.
        auto *fences=nativeFenceMachine();
        if(fences)fences->nativeLock();
        if(ticket->fence)ticket->fence->failNativeEvents();
        if(fences)fences->nativeUnlock();
        const bool cancelled=ticket->finishSurfaceFences(kIOReturnAborted);
        GPU::Diagnostics::add(fSurfaceFencesFailed,ticket->surfaceFenceCount);
        if(ticket->queue)ticket->queue->fail();
        ticket->done=true;ticket->result=kIOReturnAborted;
        ++fNativeFailed;if(ticket->compositor)++fCompositorFailed;
        if(success || !cancelled)fRecoveryFailed=true;
        return !success && cancelled;
    }
    const auto publishBegin=mach_absolute_time();
    auto *events=OSDynamicCast(QcomGPUEventMachine,nativeEventMachine());
    auto *fences=nativeFenceMachine();
    const auto channel=ticket->queue->eventChannel();
    bool valid=events && fences && ticket->fence && events->consistent(channel);
    if (fences) fences->nativeLock();
    if (valid) valid=events->timelines[channel].retire(ticket->stamp);
    if (ticket->fence) {
        if (valid) ticket->fence->setNativeStatus(success?0:8);
        else ticket->fence->failNativeEvents(); // failure only, never GPU success
    }
    // No CPU stamp is advanced before cache/IOSurface completion in finishJob.
    // A quarantined job may signal a failed fence while retaining all DMA.
    if (fences) fences->nativeUnlock();
    // finishJob has already retired private translations and published GPU
    // writes/coherence. Validate the real timeline before signalling success.
    // IOSurface callbacks run outside the Family's nonrecursive fence lock.
    if(!ticket->finishSurfaceFences(success && valid?kIOReturnSuccess:kIOReturnIOError)) {
        valid=false;
        if(fences)fences->nativeLock();
        if(ticket->fence)ticket->fence->failNativeEvents();
        if(fences)fences->nativeUnlock();
    }
    GPU::Diagnostics::add(success && valid?fSurfaceFencesCompleted:fSurfaceFencesFailed,ticket->surfaceFenceCount);
    // Signalling can synchronously enter the Family scrubber; do not hold its
    // nonrecursive fence lock across that wakeup.
    if (valid) events->nativeSignalStamp(channel,ticket->stamp);
    if (fences) fences->nativeRetire();
    ticket->queue->endSubmission();
    if (!success || !valid) ticket->queue->fail();
    if (!valid) fRecoveryFailed=true;
    ticket->done=true;ticket->result=success && valid?kIOReturnSuccess:kIOReturnIOError;
    if(success && valid) {
        ++fNativeCompleted;if(ticket->compositor)++fCompositorCompleted;
        if(ticket->commandKinds&(GPU::Diagnostics::Transfer|GPU::Diagnostics::Compute|GPU::Diagnostics::Render))
            fLastResponsibleTasks=ticket->responsibleTasks;
    } else {
        ++fNativeFailed;if(ticket->compositor)++fCompositorFailed;
    }
    fDiagnostics.native.kinds.finish(ticket->commandKinds,success && valid);
    if(ticket->compositor)fDiagnostics.compositor.kinds.finish(ticket->commandKinds,success && valid);
    sampleNative(GPU::Diagnostics::NativePublish,publishBegin,mach_absolute_time(),ticket->compositor);
    return valid;
}

IOReturn QcomGPU::nativeQuery(IOGPUDeviceUserClient *client, UInt32 selector, IOExternalMethodArguments *a)
{
    if (!fAdded) return kIOReturnNotReady;
    QcomGPUNativeSubmission *submission=nullptr;
    const auto result=fGate->runAction(nativeQueryAction, client,
        reinterpret_cast<void *>(uintptr_t(selector)), a,&submission);
    if(submission) {
        // The client remains active until finishCall(), serializing its next
        // submit. Family scheduling can sleep; never hold the A540 gate here.
        submission->queue->workQueue()->nativeAppend(submission);
        submission->release();
    }
    return result;
}
IOReturn QcomGPU::nativeQueryAction(OSObject *owner, void *client, void *selector, void *arguments, void *scheduled)
{
    auto *gpu = static_cast<QcomGPU *>(owner);
    auto *connection = OSDynamicCast(QcomGPUDeviceUserClient, static_cast<IOGPUDeviceUserClient *>(client));
    auto index = static_cast<UInt32>(reinterpret_cast<uintptr_t>(selector));
    if (!connection || !connection->belongsTo(gpu)) return kIOReturnNotAttached;
    if (index < UINT32_MAX - 2 && (!gpu->fStarted || gpu->fState != GPU::FrameworkReady ||
        connection->isInactive() || connection->getProvider() != gpu)) return kIOReturnOffline;
    GPU::Diagnostics::Stage stage=GPU::Diagnostics::StageCount;
    switch(index) {
    case GPU::Native::Submit:stage=GPU::Diagnostics::WrapperSubmit;break;
    case GPU::Native::NewResource:stage=GPU::Diagnostics::WrapperResourceCreate;break;
    case GPU::Native::DeleteResource:stage=GPU::Diagnostics::WrapperResourceDelete;break;
    case GPU::Native::NewShmem:stage=GPU::Diagnostics::WrapperShmemCreate;break;
    case GPU::JIT::RegisterProgram:stage=GPU::Diagnostics::WrapperProgram;break;
    }
    const auto begin=stage<GPU::Diagnostics::StageCount?mach_absolute_time():0;
    auto *a=static_cast<IOExternalMethodArguments *>(arguments);
    // Capture factory input before Family entry. No raw pointer is exported.
    const auto resourceType=index==GPU::Native::NewResource?static_cast<const GPU::Native::ResourceArgs *>(a->structureInput)->type:0;
    const auto requestedBytes=index==GPU::Native::NewResource?static_cast<const GPU::Native::ResourceArgs *>(a->structureInput)->bytes:0;
    const auto result=connection->perform(index,a);
    if(scheduled)*static_cast<QcomGPUNativeSubmission **>(scheduled)=connection->takeScheduledSubmission();
    // Includes the entire Family call and nested native admission, including
    // retries/waits. These are overlapping elapsed intervals, not additive CPU
    // busy time. The same gate serializes this update and registry snapshots.
    if(begin) {
        const auto end=mach_absolute_time();const bool compositor=connection->diagnosticCompositor();
        gpu->sampleNative(stage,begin,end,compositor);
        if(index==GPU::Native::NewResource) {
            const auto bytes=result?requestedBytes:static_cast<const GPU::Native::ResourceData *>(a->structureOutput)->bytes;
            gpu->fResourceActivity.sample(compositor,resourceType,bytes,begin,end,result==kIOReturnSuccess);
        }
    }
    return result;
}

bool QcomGPU::configureDevice(IOService *provider)
{
    auto *memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory || memory->getPhysicalAddress() != Adreno::RegisterBase ||
        memory->getLength() != Adreno::RegisterBytes) return false;
    fStampMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryMapperNone, GPU::PageBytes, GPU::PageBytes);
    if (!fStampMemory || !fStampMemory->getBytesNoCopy()) return false;
    bzero(fStampMemory->getBytesNoCopy(), GPU::PageBytes);
    return setProperty("IOGPUNativeService", true) &&
        setProperty("IOGPUNativeDeviceQueries", true) &&
        setProperty("IOGPUNativeCommandVersion", GPU::Native::CommandVersion, 32) &&
        setProperty("IOGPUNativeResponsibleTaskIDsVersion",GPU::Native::ResponsibleTaskVersion,32) &&
        setProperty("IOGPUNativeResponsibleTaskIDsMax",GPU::Native::MaxResponsibleTaskIDs,32) &&
        setProperty("IOGPUNativeTaskEnergyAccounting",false) &&
        setProperty("IOGPUNativeClientBufferImport",true) &&
        setProperty("IOGPUShaderCommandVersion", GPU::Shader::Version, 32) &&
        setProperty("IOGPUShaderRegistry", GPUPrograms::Registry, 64) &&
        setProperty("IOGPUJITVersion", GPU::JIT::Version, 32) &&
        setProperty("IOGPUNativeMaxShmemBytes", GPU::Native::MaxShmemBytes, 32) &&
        setProperty("IOGPUJITRequiresTrustedTask",true) &&
        setProperty("IOGPUCompositorAccessEntitlement","com.apple.backboardd") &&
        setProperty("IOGPUNativeCompletionVersion",GPU::Native::CompletionVersion,32) &&
        setProperty("IOGPUNativeCompletionEpoch",UInt64(0),64) &&
        setProperty("IOGPUNativeSubmission", false);
}
IOMemoryDescriptor *QcomGPU::getStampMemory(UInt32 *offset)
{
    if (!offset) return nullptr;
    *offset = 0; return fStampMemory;
}
UInt32 QcomGPU::reserveNativeEventChannel() {
    return static_cast<QcomGPUEventMachine *>(nativeEventMachine())->reserveChannel();
}
void QcomGPU::releaseNativeEventChannel(UInt32 channel) {
    static_cast<QcomGPUEventMachine *>(nativeEventMachine())->releaseChannel(channel);
}
IOGPUEventMachine *QcomGPU::allocEventMachine() { return new QcomGPUEventMachine; }
IOGPUScheduler *QcomGPU::allocScheduler() { return new QcomGPUScheduler; }
IOGPUDeviceUserClient *QcomGPU::allocDeviceUserClient() { return new QcomGPUDeviceUserClient; }
IOGPUTask *QcomGPU::createGPUTask(task_t task)
{
    if (!task) return nullptr;
    auto *gpuTask = new QcomGPUTask;
    if (gpuTask && !gpuTask->initWithGPU(this, task, 0, nullptr)) {
        gpuTask->release(); return nullptr;
    }
    return gpuTask;
}
IOGPUTask *QcomGPU::createKernelGPUTask() { return createGPUTask(kernel_task); }
void QcomGPU::getGPUConfiguration(IOGPUConfiguration *configuration)
{
    // Preserve the defaults installed by IOGPU::start; do not copy AGX flags,
    // Apple GPU family identifiers, or Metal capabilities into this device.
    configuration->name = "Adreno 540";
    configuration->flags = configuration->family = configuration->revision = 0;
    configuration->maxWidth=configuration->maxHeight=GPU::MaxTextureDimension;
    configuration->stampCount = QcomGPUEventChannels;
    configuration->clientInfoBytes = 0;
}
// The corresponding AGX hook is a no-op. Keep it out of the hardware path.
void QcomGPU::nativeSlot196() {}
IOGPUSysMemory *QcomGPU::allocSystemMemory() { return new QcomGPUSysMemory; }
IOGPUResource *QcomGPU::allocResource() { return new QcomGPUResource; }
IOGPUCommandQueue *QcomGPU::allocCommandQueue() { return new QcomGPUCommandQueue; }
OSObject *QcomGPU::allocWorkQueue() { return new QcomGPUWorkQueue; }
QcomGPUWorkQueue *QcomGPU::createNativeWorkQueue(QcomGPUCommandQueue *context)
{
    auto *queue=static_cast<QcomGPUWorkQueue *>(allocWorkQueue());
    if(queue && !queue->initWithGPU(this,context,GPU::MaxJobs)){queue->release();return nullptr;}
    return queue;
}
IOBufferMemoryDescriptor *QcomGPU::createBufferMemoryDescriptor(task_t task, IOOptionBits options,
    vm_size_t bytes, vm_size_t alignment)
{
    if (!bytes || bytes > GPU::MaxBufferBytes) return nullptr;
    // MapperNone overlaps kIOMapCacheMask in Buffer MD allocation options.
    // DMA always uses an explicit device mapper; do not OR the two modes.
    options = (options & ~kIOMapCacheMask) | kIOMapWriteCombineCache;
    auto *memory = IOBufferMemoryDescriptor::inTaskWithOptions(task, options, bytes, alignment);
    if (!memory) return nullptr;
    // Pageable Buffer MDs allocated in a user task can return a user VA from
    // getBytesNoCopy(). Clear through a kernel mapping, never dereference that
    // user pointer under PAN or pass it to a kernel bzero.
    auto *mapping = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere, 0, 0);
    if (!mapping || !mapping->getVirtualAddress()) {
        if (mapping) mapping->release(); memory->release(); return nullptr;
    }
    bzero(reinterpret_cast<void *>(mapping->getVirtualAddress()), bytes);
    mapping->release();
    return memory;
}
IOReturn QcomGPU::nativeOpenAction(OSObject *owner, void *task, void *token, void *type, void *out)
{
    auto *gpu = static_cast<QcomGPU *>(owner);
    if (!gpu->fStarted || gpu->fState != GPU::FrameworkReady) return kIOReturnOffline;
    return gpu->IOGPU::newUserClient(static_cast<task_t>(task), token,
        static_cast<UInt32>(reinterpret_cast<uintptr_t>(type)), static_cast<IOUserClient **>(out));
}
IOReturn QcomGPU::nativeAccountingAction(OSObject *owner, void *amount, void *release, void *timing, void *attribution)
{
    auto *gpu = static_cast<QcomGPU *>(owner);
    UInt64 bytes = reinterpret_cast<uintptr_t>(amount);
    if (release) {
        // Existing accounting gate also serializes resource-free diagnostics;
        // do not add a lock or touch the destroyed resource/base object.
        if(timing)gpu->sampleNative(GPU::Diagnostics::NativeResourceFree,reinterpret_cast<uintptr_t>(timing),
            mach_absolute_time(),attribution!=nullptr);
        const bool valid=bytes?gpu->fNativeMemoryUsage.remove(bytes):gpu->fNativeMemoryUsage.removeClient();
        if(!valid) {
            gpu->fNativeAccountingFault=true;gpu->fRecoveryFailed=true;
            return kIOReturnBadArgument;
        }
        return kIOReturnSuccess;
    }
    if(gpu->fNativeAccountingFault)return kIOReturnNotReady;
    if (!bytes) {
        return gpu->fNativeMemoryUsage.addClient()?kIOReturnSuccess:kIOReturnNoResources;
    }
    return gpu->fNativeMemoryUsage.add(bytes,gpu->nativeMemoryLimits())?kIOReturnSuccess:kIOReturnNoResources;
}
GPU::Native::Memory::Limits QcomGPU::nativeMemoryLimits() const {
    auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
    return GPU::Native::Memory::limitsForAperture(mapper?mapper->apertureBytes():0);
}
IOReturn QcomGPU::nativeDrainAction(OSObject *owner, void *, void *, void *, void *)
{
    auto *gpu = static_cast<QcomGPU *>(owner);
    UInt64 timeout = 0; nanoseconds_to_absolutetime(1000000000ULL, &timeout);
    AbsoluteTime deadline = mach_absolute_time() + timeout;
    while (gpu->fNativeCalls) {
        auto wake = gpu->fLoop->onThread() ? THREAD_TIMED_OUT :
            gpu->fGate->commandSleep(&gpu->fNativeCalls, deadline, THREAD_UNINT);
        if (gpu->fNativeCalls && wake != THREAD_AWAKENED) {
            if (!gpu->fQuarantined) { gpu->fQuarantined = true; gpu->retain(); }
            IOLog("QcomGPU: native Family call did not drain; teardown quarantined\n");
            return kIOReturnTimeout;
        }
    }
    return kIOReturnSuccess;
}
bool QcomGPU::reserveNative(UInt64 bytes)
{
    return fAdded && fGate->runAction(nativeAccountingAction,
        reinterpret_cast<void *>(uintptr_t(bytes))) == kIOReturnSuccess;
}
void QcomGPU::releaseNative(UInt64 bytes,UInt64 releaseBegin,bool compositor)
{
    if (fAdded) fGate->runAction(nativeAccountingAction,
        reinterpret_cast<void *>(uintptr_t(bytes)), reinterpret_cast<void *>(uintptr_t(1)),
        reinterpret_cast<void *>(uintptr_t(releaseBegin)),reinterpret_cast<void *>(uintptr_t(compositor)));
}
IOReturn QcomGPU::nativeRegistryAction(OSObject *owner, void *object, void *operation, void *output, void *)
{
    auto *gpu = static_cast<QcomGPU *>(owner);
    auto op = reinterpret_cast<uintptr_t>(operation);
    if (op == 2) {
        auto index = reinterpret_cast<uintptr_t>(object);
        if (index >= GPU::MaxClients) return kIOReturnBadArgument;
        auto *client = gpu->fNativeClientRegistry[index];
        if (client) client->retain();
        *static_cast<IOGPUDeviceUserClient **>(output) = client; return kIOReturnSuccess;
    }
    auto *client = static_cast<IOGPUDeviceUserClient *>(object);
    if (op == 1) {
        for (auto &entry : gpu->fNativeClientRegistry) if (entry == client) entry = nullptr;
        return kIOReturnSuccess;
    }
    for (auto &entry : gpu->fNativeClientRegistry) if (!entry) { entry = client; return kIOReturnSuccess; }
    return kIOReturnNoResources;
}
bool QcomGPU::registerNativeClient(IOGPUDeviceUserClient *client)
{
    return fGate->runAction(nativeRegistryAction, client) == kIOReturnSuccess;
}
void QcomGPU::unregisterNativeClient(IOGPUDeviceUserClient *client)
{
    fGate->runAction(nativeRegistryAction, client, reinterpret_cast<void *>(uintptr_t(1)));
}
void QcomGPU::closeNativeClients()
{
    for (unsigned i = 0; i < GPU::MaxClients; ++i) {
        IOGPUDeviceUserClient *client = nullptr;
        fGate->runAction(nativeRegistryAction, reinterpret_cast<void *>(uintptr_t(i)),
            reinterpret_cast<void *>(uintptr_t(2)), &client);
        if (client) { client->clientClose(); client->release(); }
    }
}
QcomGPUBuffer *QcomGPU::mapNativeMemory(IOMemoryDescriptor *memory,bool shared)
{
    // Preparing/pinning this descriptor can sleep. Do not carry an earlier
    // hardware validation through that wait: iovmMapMemory performs its own
    // full check under the mapper lock immediately before publishing PTEs.
    return nativeCPUPreparationReady() ? QcomGPUBuffer::mapDescriptor(memory, fMapper, shared) : nullptr;
}
IOReturn QcomGPU::submitNative(QcomGPUCommandQueue *queue, IOGPUDevice *device,
    const GPU::Native::SubmitArgs &args,QcomGPUNativeSubmission **scheduled)
{
    const auto prepareBegin=mach_absolute_time();
    auto nativeClock=[](){return mach_absolute_time();};
    GPU::Diagnostics::AdmissionTrace<decltype(nativeClock)> trace(fDiagnostics.native,nativeClock);
    auto outcome=[&](IOReturn result){return IOReturn(trace.finish(uint32_t(result)));};
    auto admissionReady=[&](){return fStagedAdmissionChecks?nativeCPUPreparationReady():nativeAddressSpacesReady();};
    // Called under the shared gate from the real Family submit wrapper. Copy
    // and validate admission here; completion is delivered by its event fence.
    if (!admissionReady()) return outcome(kIOReturnNotReady);
    if (fLoop->onThread()) return outcome(kIOReturnNotPermitted);
    if (!queue || !device || !args.commands || args.segments || args.sideband ||
        args.flags || args.resources || args.reserved) return outcome(kIOReturnBadArgument);
    auto *shmem = device->copyShmem(args.commands);
    if (!shmem) return outcome(kIOReturnBadArgument);
    const bool traceCompositor=nativeCompositorTask(device->owningTask());
    if(traceCompositor)trace.compositor(&fDiagnostics.compositor);
    GPU::Native::CommandHeader header{};
    // Snapshot once into bounded kernel-owned OSData. Keeping the command
    // array and Draw union on this call stack consumed almost5 KiB before
    // entering the pipeline encoder or the Family's submit wrappers.
    struct CommandSnapshot {
        OSData *storage=nullptr,*encoding=nullptr,*indices=nullptr;
        ~CommandSnapshot(){if(indices)indices->release();if(encoding)encoding->release();if(storage)storage->release();}
    } snapshot;
    const auto mapBegin=mach_absolute_time();
    auto length = shmem->length();
    auto *data = shmem->mapKernel();
    auto detailCursor=mach_absolute_time();
    trace.sample(GPU::Diagnostics::SnapshotMap,mapBegin,detailCursor);
    constexpr UInt32 snapshotBytes=GPU::Native::MaxShmemBytes;
    if(data && length>=sizeof(header))snapshot.storage=OSData::withBytes(data,UInt32(length<snapshotBytes?length:snapshotBytes));
    const auto copyEnd=mach_absolute_time();
    trace.sample(GPU::Diagnostics::SnapshotCopy,detailCursor,copyEnd);
    const bool allocationFailed=data && length>=sizeof(header) && !snapshot.storage;
    const auto *bytes=snapshot.storage?static_cast<const UInt8 *>(snapshot.storage->getBytesNoCopy()):nullptr;
    length=snapshot.storage?snapshot.storage->getLength():0;
    const GPU::Command *commands=nullptr;
    const GPU::RenderPass::Description *renderPass=nullptr;
    bool valid = bytes && length >= sizeof(header);
    if (valid) {
        memcpy(&header, bytes, sizeof(header));
        const auto taskEnd=GPU::Native::commandOffset(header.responsibleTaskCount);
        const auto commandsOffset=GPU::Native::commandOffset(header.responsibleTaskCount,header.version);
        valid = header.magic == GPU::Native::CommandMagic &&
            (header.version == GPU::Native::CommandVersion || header.version == GPU::Native::BatchCommandVersion || header.version == GPU::Native::RenderPassCommandVersion) &&
            commandsOffset && header.count && header.count <= GPU::MaxCommands &&
            length >= commandsOffset + sizeof(GPU::Command) * header.count;
        if(valid) {
            for(UInt32 i=sizeof(header)+header.responsibleTaskCount*4;i<taskEnd;++i)
                if(bytes[i]){valid=false;break;}
            commands=reinterpret_cast<const GPU::Command *>(bytes+commandsOffset);
        }
        if(valid && (header.version==GPU::Native::BatchCommandVersion || header.version==GPU::Native::RenderPassCommandVersion)) {
            UInt32 end=0;
            valid=GPU::Native::shaderBatchLayout(commands,header.count,commandsOffset,length,
                sizeof(GPU::Shader::Dispatch),sizeof(GPU::Shader::Draw),end);
        } else if(valid) for(UInt32 i=0;i<header.count;++i) {
            const auto &c=commands[i];
            if(c.opcode!=GPU::ShaderDispatch && c.opcode!=GPU::ShaderDraw)continue;
            UInt32 end=0;
            valid=header.count==1 && GPU::Native::shaderBatchLayout(commands,1,commandsOffset,length,
                sizeof(GPU::Shader::Dispatch),sizeof(GPU::Shader::Draw),end);break;
        }
    }
    if(valid && header.version==GPU::Native::RenderPassCommandVersion) {
        renderPass=reinterpret_cast<const GPU::RenderPass::Description *>(bytes+GPU::Native::commandOffset(header.responsibleTaskCount));
        valid=GPU::RenderPass::valid(*renderPass) && renderPass->registry==GPUPrograms::Registry;
        for(UInt32 i=0;valid && i<header.count;++i)
            valid=commands[i].opcode==GPU::ShaderDraw && GPU::RenderPass::drawMatches(*renderPass,
                *reinterpret_cast<const GPU::Shader::Draw *>(bytes+commands[i].offset));
    }
    if (data) shmem->unmapKernel();
    shmem->release();
    const auto validateEnd=mach_absolute_time();
    trace.sample(GPU::Diagnostics::SnapshotValidate,copyEnd,validateEnd);
    if (!valid) return outcome(allocationFailed?kIOReturnNoMemory:kIOReturnBadArgument);
    retireJobs();
    const auto drainEnd=mach_absolute_time();
    trace.sample(GPU::Diagnostics::PreflightRetire,validateEnd,drainEnd);
    // retireJobs has checked actual hardware before polling. CPU-only encoding
    // follows; createAddressSpace checks actual hardware again before it can
    // publish any new GPU translation. Do not repeat that MMIO scan here.
    if (!admissionReady()) return outcome(kIOReturnIOError);
    if (fJobCount == GPU::MaxJobs) return outcome(kIOReturnNoResources);
    struct EncodingBindings {
        Adreno::ShaderBuffers bindings;
        Adreno::GraphicsTextures sampled;
        Adreno::GMEMLayout gmem;
        UInt64 renderTargets[QGPU_COLOR_TARGET_COUNT];
        GPU::Shader::Texture planes[QGPU_COLOR_TARGET_COUNT+2+2*QGPU_STAGE_TEXTURE_COUNT];
        GPU::MappingBounds::Plan mappingPlan;
        UInt32 resourceIDs[GPU::Native::Memory::ResourcesPerJob];
        GPU::OperationActivity::ProgramDescriptions programDescriptions;
    };
    static_assert(sizeof(Adreno::PipelineEncoder)%alignof(EncodingBindings)==0,"aligned native encoding workspace");
    static_assert(__is_trivially_destructible(Adreno::PipelineEncoder),"CPU encoder storage must not own resources");
    static_assert(__is_trivially_destructible(EncodingBindings),"CPU binding workspace must not own resources");
    constexpr UInt32 encodingBytes=sizeof(Adreno::PipelineEncoder)+sizeof(EncodingBindings);
    // Family/SVC frames and interrupt preemption share a bounded kernel
    // stack. Keep draw expansion, constants and validation arrays off it.
    // No workspace pointer survives synchronous encoding or reaches the GPU.
    snapshot.encoding=OSData::withCapacity(encodingBytes);
    if(!snapshot.encoding || !snapshot.encoding->appendBytes(nullptr,encodingBytes))return outcome(kIOReturnNoMemory);
    auto &encodingBindings=*reinterpret_cast<EncodingBindings *>(
        static_cast<UInt8 *>(const_cast<void *>(snapshot.encoding->getBytesNoCopy()))+sizeof(Adreno::PipelineEncoder));
    auto *ticket = new QcomGPUNativeSubmission;
    if (!ticket || !ticket->initSubmission(this,queue)) { if (ticket) ticket->release(); return outcome(kIOReturnNoMemory); }
    ticket->queue = queue; queue->retain();ticket->device=device;device->retain();
    ticket->responsibleTasks.count=header.responsibleTaskCount;
    if(header.responsibleTaskCount)memcpy(ticket->responsibleTasks.ids,bytes+sizeof(header),header.responsibleTaskCount*4);
    // Attribution is an immutable hint on this job. Resource authority and
    // lifetime remain tied to the actual Family device's owning task.
    ticket->compositor=traceCompositor;
    trace.sample(GPU::Diagnostics::PreflightTicket,drainEnd,mach_absolute_time());
    trace.mark(GPU::Diagnostics::Preflight);
    auto &job = fJobs[(fJobHead + fJobCount) % GPU::MaxJobs];
    job.prepareBegin=prepareBegin;
    job.preflightTicks=mach_absolute_time()-prepareBegin;
    if(auto *process=static_cast<proc_t>(get_bsdtask_info(device->owningTask())))job.diagnosticPID=proc_pid(process);
    job.ib = acquireIndirectBuffer();
    trace.mark(GPU::Diagnostics::IBAcquire);
    const auto encodingBegin=mach_absolute_time();
    IOReturn result = job.ib ? kIOReturnSuccess : kIOReturnNoMemory;
    Adreno::Encoder encoder(job.ib ? static_cast<UInt32 *>(job.ib->bytes()) : nullptr, Adreno::IBDwords);
    auto retainBuffer = [&](UInt32 id,QcomGPUResource **out=nullptr,bool write=false) -> QcomGPUBuffer * {
        // The descriptor retains the resolved namespace objects for this
        // entire batch. Repeated bindings need no new Family lookup/retain.
        for(UInt32 n=0;n<ticket->resourceCount;++n)if(encodingBindings.resourceIDs[n]==id) {
            auto *resource=static_cast<QcomGPUResource *>(ticket->resources[n]);
            auto *buffer=job.buffers[n];
            if(write && !buffer->gpuWritable())return nullptr;
            if(out)*out=resource;
            if(write)ticket->surfaceWrites|=GPU::Native::Memory::writeBit(n);
            return buffer;
        }
        auto *native = id ? device->copyResource(id) : nullptr;
        auto *resource = OSDynamicCast(QcomGPUResource, native);
        auto *buffer = resource && resource->ownedBy(device) ? resource->buffer() : nullptr;
        if (!buffer || (write && !buffer->gpuWritable())) { if (native) native->release(); return nullptr; }
        if (out) *out=resource;
        UInt32 n = 0;
        while (n < ticket->resourceCount && ticket->resources[n] != resource) ++n;
        if (n == ticket->resourceCount) {
            if (n == GPU::Native::Memory::ResourcesPerJob) { native->release(); return nullptr; }
            encodingBindings.resourceIDs[n]=id;
            ticket->resources[ticket->resourceCount++] = native;
            buffer->retain(); buffer->beginGPUUse(); job.buffers[job.bufferCount++] = buffer;
        } else native->release();
        if (write) ticket->surfaceWrites|=GPU::Native::Memory::writeBit(n);
        return buffer;
    };
    UInt64 synchronizedIndexRoots=0;
    auto synchronizeIndices=[&](const QcomGPUBuffer *buffer) {
        for(UInt32 n=0;n<job.bufferCount;++n)if(job.buffers[n]->backing()==buffer->backing()) {
            const auto bit=GPU::Native::Memory::writeBit(n);
            if(synchronizedIndexRoots&bit)return true;
            if(!buffer->synchronize())return false;
            synchronizedIndexRoots|=bit;return true;
        }
        return false;
    };
    auto recordAddress=[&](UInt64 address,UInt64 bytes) -> bool {
        return encodingBindings.mappingPlan.recordAddress(job.buffers,job.bufferCount,address,bytes);
    };
    auto recordBindings=[&](const GPU::Shader::Program &program,const Adreno::ShaderBuffers &resolved,
        UInt32 extra=0,UInt32 base=0,UInt32 skip=0) -> bool {
        for(UInt32 slot=0;slot<GPU::Shader::MaxBindings;++slot)
            if(((program.bufferMask|extra)&~skip)&(1u<<slot))
                if(!recordAddress(resolved.addresses[base+slot],resolved.bytes[base+slot]))return false;
        return true;
    };
    auto cpuReadBlocked=[&](const QcomGPUBuffer *buffer) {
        if(!buffer)return true;UInt32 ownUses=0;
        for(UInt32 i=0;i<job.bufferCount;++i)if(job.buffers[i]->backing()==buffer->backing())++ownUses;
        return nativeCPUReadBlocked(buffer,ownUses);
    };
    auto resolveBindings = [&](const GPU::Shader::Program &program, const GPU::Shader::Binding *bindings,
        UInt64 elements, Adreno::ShaderBuffers &resolved, UInt64 instances = 0, UInt32 vertexBindings = 0,
        UInt32 destination = 0) -> bool {
        if (destination!=0 && destination!=QGPU_FRAGMENT_BUFFER_BASE) return false;
        for (UInt32 i=0;i<GPU::Shader::MaxBindings;++i) {
            const auto &binding = bindings[i];
            if (binding.reserved) return false;
            if (!((program.bufferMask|vertexBindings)&(1u<<i))) {
                if (binding.resource || binding.offset || binding.bytes) return false;
                continue;
            }
            auto *buffer = retainBuffer(binding.resource,nullptr,(program.writeMask&(1u<<i))!=0);
            UInt64 required=0;
            if (!buffer || (binding.offset&3) || !GPU::range(buffer->length,binding.offset,binding.bytes) ||
                !GPU::Shader::bindingBytes(program,i,elements,instances,required) || required>binding.bytes) return false;
            resolved.addresses[destination+i] = buffer->address+binding.offset; resolved.bytes[destination+i] = binding.bytes;
        }
        return true;
    };
    auto resolveTexture = [&](const GPU::Shader::Texture &texture,bool write=false) -> UInt64 {
        UInt64 bytes = 0;
        if (!GPU::Shader::textureBytes(texture,bytes)) return 0;
        QcomGPUResource *resource=nullptr;
        auto *buffer = retainBuffer(texture.resource,&resource,write);
        if (resource) if (auto *s=resource->surfaceInfo()) {
            // Views retain the imported element size, including R8/RG8 masks.
            // Authorization and the imported storage geometry still apply.
            if (!GPU::Shader::colorFormat(texture.format) || texture.cube || texture.layers>1 || texture.mipLevels>1 || texture.offset || texture.width!=s->width || texture.height!=s->height ||
                texture.pitch!=s->rowBytes || texture.samples || GPU::Shader::formatBytes(texture.format)!=s->bytesPerElement) return 0;
        }
        return buffer && GPU::range(buffer->length,texture.offset,bytes) &&
            recordAddress(buffer->address+texture.offset,bytes) ? buffer->address+texture.offset : 0;
    };
    auto textureRanges = [](const GPU::Shader::Texture &texture,GPU::StridedRange &range) -> bool {
        UInt64 bytes=0;if(!GPU::Shader::textureBytes(texture,bytes))return false;
        range={texture.offset,bytes,0,1};
        if(texture.layers || texture.cube) {
            GPU::Shader::TextureLayout layout;auto single=texture;single.layers=single.layerStride=single.cube=0;
            if(!GPU::Shader::textureStorageLayout(texture,layout,false) || !GPU::Shader::textureBytes(single,bytes))return false;
            range={texture.offset,bytes,layout.layerStride,GPU::Shader::textureSlices(texture)};
        }
        return true;
    };
    auto lookupProgram = [&](UInt32 id) -> const GPU::Shader::Program * {
        // The gate protects this lookup and the encoder's code/constant copy.
        // Once the wait releases the gate, the private IB owns all GPU bytes;
        // deletion/client close cannot invalidate an in-flight program.
        if (id < GPU::JIT::FirstID) return GPUPrograms::lookup(id);
        for (auto *native : fNativeClientRegistry) {
            auto *client = OSDynamicCast(QcomGPUDeviceUserClient, native);
            if (client && client->device() == device) return client->lookupProgram(id);
        }
        return nullptr;
    };
    // Compute-only batches need private flush timestamps and GPU progress too.
    UInt32 scratchBytes=renderPass || header.count>1 || fOperationActivity?GPU::PageBytes:0,indexOffsets[GPU::MaxCommands]={};UInt64 indexBytes=0;
    // A standalone builtin clear also needs a private color-flush cell.
    for(UInt32 i=0;!scratchBytes && i<header.count;++i)if(commands[i].opcode==GPU::ShaderDispatch) {
        const auto &d=*reinterpret_cast<const GPU::Shader::Dispatch *>(bytes+commands[i].offset);
        const auto *program=GPUPrograms::lookup(d.program);
        if(program && (!strcmp(program->name,"blit_fill_rows_2d") || !strcmp(program->name,"blit_fill_pairs_2d") ||
            !strcmp(program->name,"blit_fill_rows_2d_tail") || !strcmp(program->name,"blit_fill_pairs_2d_tail")))scratchBytes=GPU::PageBytes;
    }
    const bool inplace=nativeFramebufferInplace(device->owningTask());
    UInt32 fullScratchBytes=scratchBytes;
    bool framebuffer=renderPass!=nullptr;
    if(renderPass) {
        if(!Adreno::gmemLayout(*renderPass,encodingBindings.gmem))result=kIOReturnBadArgument;
        else scratchBytes=fullScratchBytes=GPU::RenderPass::scratchBytes(*renderPass,encodingBindings.gmem);
    }
    for(UInt32 i=0;result==kIOReturnSuccess && i<header.count;++i)if(commands[i].opcode==GPU::ShaderDraw) {
        const auto &d=*reinterpret_cast<const GPU::Shader::Draw *>(bytes+commands[i].offset);
        const auto *fragment=lookupProgram(d.fragment);Adreno::FramebufferStorage storage;
        if(!fragment || (!renderPass && !Adreno::framebufferStoragePlan(d,GPU::Shader::rasterProgram(*fragment,d.rasterSamples),inplace,false,storage,nativeFramebufferCompact()))) {
            result=kIOReturnBadArgument;break;
        }
        if(storage.bytes>scratchBytes)scratchBytes=storage.bytes;
        if(storage.snapshots.bytes>fullScratchBytes)fullScratchBytes=storage.snapshots.bytes;
        framebuffer|=GPU::Shader::rasterProgram(*fragment,d.rasterSamples).framebufferMask!=0;
        if(d.indexSize) {
            if(!GPU::Native::appendIndexSnapshot(d.indices.bytes,indexBytes,indexOffsets[i])) {
                result=kIOReturnNoResources;break;
            }
        }
    }
    if(result==kIOReturnSuccess && indexBytes) {
        indexBytes=GPU::Native::indexSnapshotAllocation(indexBytes);
        const auto indexBegin=mach_absolute_time();
        if(indexBytes>IndexSnapshotLimit-fIndexSnapshotBytes)result=kIOReturnNoResources;
        else if(!(job.indices=acquireIndexSnapshot(indexBytes)))result=kIOReturnNoMemory;
        else fIndexSnapshotBytes+=indexBytes;
        if(result==kIOReturnSuccess) {
            // The GPU snapshot is non-cacheable. Scan the source once in
            // ordinary cached kernel memory before publishing identical bytes
            // to that snapshot; a large indexed draw otherwise stalls the gate
            // with individual non-cacheable byte reads during validation.
            snapshot.indices=OSData::withCapacity(UInt32(indexBytes));
            if(!snapshot.indices || !snapshot.indices->appendBytes(nullptr,UInt32(indexBytes)))result=kIOReturnNoMemory;
        }
        const auto indexEnd=mach_absolute_time();job.indexAcquireTicks=indexEnd-indexBegin;job.indexBytes=UInt32(indexBytes);
        trace.sample(GPU::Diagnostics::IndexAcquire,indexBegin,indexEnd);
    }
    if(result==kIOReturnSuccess && scratchBytes) {
        if(framebuffer && !GPU::Native::admissionFits(fFramebufferSnapshotBytes,scratchBytes,GPU::Native::FramebufferSnapshotLimit))result=kIOReturnNoResources;
        else if(!(job.scratch=acquireScratch(scratchBytes)))result=kIOReturnNoMemory;
        else {
            if(framebuffer){job.framebufferBytes=job.scratch->length;job.framebufferFullBytes=fullScratchBytes;fFramebufferSnapshotBytes+=job.framebufferBytes;}
            if(!job.scratch->synchronize())result=kIOReturnIOError;
        }
    }
    if(result==kIOReturnSuccess && (framebuffer || (header.version==GPU::Native::BatchCommandVersion && header.count>1))) {
        if(!GPU::Native::admissionFits(fFramebufferIBBytes,job.ib->length,GPU::Native::FramebufferIndirectLimit))result=kIOReturnNoResources;
        else {
            job.commandBuffers=OSArray::withCapacity(header.count);job.commandList=OSData::withCapacity(header.count*sizeof(Adreno::IndirectBuffer));
            if(!job.commandBuffers || !job.commandList || !job.commandBuffers->setObject(job.ib))result=kIOReturnNoMemory;
            else {job.framebufferIBBytes=job.ib->length;fFramebufferIBBytes+=job.framebufferIBBytes;
                if(!encoder.reserveTail(2))result=kIOReturnNoResources;}
        }
    }
    struct Allocator : Adreno::IndirectBufferAllocator {
        QcomGPU &gpu;Job &job;
        Allocator(QcomGPU &g,Job &j):gpu(g),job(j){}
        GPU::Result next(Adreno::Encoder &commands,uint64_t &address) override {
            if(!job.commandBuffers || job.commandBuffers->getCount()>=Adreno::MaxIndirectBuffers ||
                !GPU::Native::admissionFits(gpu.fFramebufferIBBytes,Adreno::IBDwords*4,GPU::Native::FramebufferIndirectLimit))return GPU::Result::NoResources;
            auto *next=gpu.fSecondaryIBCache?gpu.acquireIndirectBuffer():QcomGPUBuffer::allocate(Adreno::IBDwords*4,gpu.fMapper);
            if(!next)return GPU::Result::NoResources;
            commands.reserveTail(0);const bool sealed=commands.barrier()==GPU::Result::Success;
            Adreno::IndirectBuffer item{address,commands.size(),0};
            if(!sealed || !job.commandList->appendBytes(&item,sizeof(item)) || !job.commandBuffers->setObject(next)) {
                next->release();return GPU::Result::NoResources;
            }
            job.framebufferIBBytes+=next->length;gpu.fFramebufferIBBytes+=next->length;
            address=next->address;commands=Adreno::Encoder(static_cast<UInt32 *>(next->bytes()),Adreno::IBDwords,2);
            next->release();return GPU::Result::Success;
        }
    } allocator(*this,job);
    UInt64 ibAddress=job.ib?job.ib->address:0;
    auto &pipeline=*new(const_cast<void *>(snapshot.encoding->getBytesNoCopy())) Adreno::PipelineEncoder(
        encoder,ibAddress,job.scratch?job.scratch->address:0,
        job.scratch?UInt32(job.scratch->length):GPU::PageBytes,fDirectShaderUploads);
    if(renderPass)pipeline.renderInGMEM(encodingBindings.gmem);
    pipeline.enableFramebuffer2D(fFramebuffer2DMode==2 || (fFramebuffer2DMode==1 && !ticket->compositor &&
        IOUserClient::clientHasPrivilege(device->owningTask(),kIOClientPrivilegeAdministrator)==kIOReturnSuccess));
    pipeline.enableFramebufferXCrop(fFramebufferXCrop);
    pipeline.enableFramebufferWord32(fFramebufferWord32);
    pipeline.enableFramebufferInplace(inplace);
    pipeline.enableFramebufferCompact(nativeFramebufferCompact());
    if(fOperationActivity && job.scratch && job.scratch->bytes() && job.scratch->length>=GPU::PageBytes) {
        job.operationProfile=OSData::withCapacity(header.count*sizeof(GPU::OperationActivity::Record));
        if(!job.operationProfile || !job.operationProfile->appendBytes(nullptr,header.count*sizeof(GPU::OperationActivity::Record))) {
            OSSafeReleaseNULL(job.operationProfile);
            auto &profile=*static_cast<GPU::OperationActivity::Snapshot *>(const_cast<void *>(fOperationActivity->getBytesNoCopy()));
            ++profile.allocationSkips;
        }
    }
    if(fOperationActivity && (!job.scratch || !job.scratch->bytes() || job.scratch->length<GPU::PageBytes)) {
        auto &profile=*static_cast<GPU::OperationActivity::Snapshot *>(const_cast<void *>(fOperationActivity->getBytesNoCopy()));
        profile.scratchSkips+=header.count;
    }
    auto *operationRecords=job.operationProfile?static_cast<GPU::OperationActivity::Record *>(
        const_cast<void *>(job.operationProfile->getBytesNoCopy())):nullptr;
    for (UInt32 i = 0; result == kIOReturnSuccess && i < header.count; ++i) {
        if(i && job.commandBuffers && !fCompactIndirectBuffers) {
            if(allocator.next(encoder,ibAddress)!=GPU::Result::Success){result=kIOReturnNoResources;break;}
            pipeline.beginOperation(ibAddress);
        }
        const UInt64 previousWrites=ticket->surfaceWrites;
        const auto &command = commands[i];
        auto *operation=operationRecords?&operationRecords[i]:nullptr;
        if(operation){operation->index=i;operation->description.opcode=command.opcode;
            operation->flags=ticket->compositor?GPU::OperationActivity::Compositor:0;
            pipeline.profileNextOperation(i,fShaderCounterProfile==i+1 && command.opcode==GPU::ShaderDraw);}
        const auto beforeFull=pipeline.fullRasterSetups(),beforeReused=pipeline.reusedRasterPrimitives();
        const auto beforeInplace=pipeline.inplaceFramebufferPrimitives(),beforeSnapshot=pipeline.framebufferPrimitives();
        const auto beforeFlush=pipeline.sequence();const auto beforeCopied=pipeline.copiedFramebufferBytes();
        if (command.reserved) { result = kIOReturnBadArgument; break; }
        GPU::Result encoded;bool shaderWrites=false;
        if (command.opcode == GPU::Barrier) {
            if (command.destination || command.offset || command.bytes || command.value) {
                result = kIOReturnBadArgument; break;
            }
            encoded = pipeline.operation(job.commandBuffers?&allocator:nullptr,fCompactIndirectBuffers,
                [&](Adreno::IndirectBufferAllocator *) {return encoder.barrier();});
        } else if (command.opcode == GPU::Fill32) {
            if (!command.destination || command.destination > UINT32_MAX ||
                (command.offset & 3) || (command.bytes & 3)) { result = kIOReturnBadArgument; break; }
            auto *buffer = retainBuffer(static_cast<UInt32>(command.destination),nullptr,true);
            if (!buffer || !GPU::range(buffer->length, command.offset, command.bytes) ||
                !recordAddress(buffer->address+command.offset,command.bytes)) {
                result = kIOReturnBadArgument; break;
            }
            encoded = pipeline.operation(job.commandBuffers?&allocator:nullptr,fCompactIndirectBuffers,
                [&](Adreno::IndirectBufferAllocator *) {return encoder.fill(buffer->address+command.offset,command.bytes,command.value);});
        } else if (command.opcode == GPU::Copy32) {
            UInt32 sourceID = command.destination >> 32, destinationID = command.destination;
            UInt32 sourceOffset = command.offset >> 32, destinationOffset = command.offset;
            auto *source = retainBuffer(sourceID), *destination = retainBuffer(destinationID,nullptr,true);
            if (command.value || !source || !destination || (sourceOffset & 3) ||
                (destinationOffset & 3) || (command.bytes & 3) ||
                !GPU::range(source->length, sourceOffset, command.bytes) ||
                !GPU::range(destination->length, destinationOffset, command.bytes) ||
                source->overlaps(destination,sourceOffset,command.bytes,destinationOffset,command.bytes) ||
                !recordAddress(source->address+sourceOffset,command.bytes) ||
                !recordAddress(destination->address+destinationOffset,command.bytes)) {
                result = kIOReturnBadArgument; break;
            }
            encoded = pipeline.operation(job.commandBuffers?&allocator:nullptr,fCompactIndirectBuffers,
                [&](Adreno::IndirectBufferAllocator *) {return encoder.copy(source->address+sourceOffset,
                    destination->address+destinationOffset,command.bytes);});
        } else if (command.opcode == GPU::ShaderDispatch) {
            const auto &d = *reinterpret_cast<const GPU::Shader::Dispatch *>(bytes+command.offset); const auto *program = lookupProgram(d.program);
            auto &bindings=encodingBindings.bindings;__builtin_memset(&bindings,0,sizeof(bindings));
            UInt64 elements=0;unsigned dimensions=0;
            if (d.registry != GPUPrograms::Registry || d.version != GPU::Shader::Version || !program ||
                !GPU::Shader::dispatchFootprint(d,*program,elements,dimensions) ||
                !resolveBindings(*program,d.buffers,elements,bindings)) {
                result = kIOReturnBadArgument; break;
            }
            shaderWrites=(program->writeMask|program->imageWriteMask)!=0;
            const bool builtin=program==GPUPrograms::lookup(d.program);
            const bool byteCopy=builtin && !strcmp(program->name,"blit_copy_bytes");
            const bool byteFill=builtin && !strcmp(program->name,"blit_fill_bytes");
            const bool wordCopy=builtin && !strcmp(program->name,"blit_copy_words");
            const bool wordFill=builtin && !strcmp(program->name,"blit_fill_words");
            const bool vectorFill=builtin && (!strcmp(program->name,"blit_fill_rows_2d") || !strcmp(program->name,"blit_fill_pairs_2d") ||
                !strcmp(program->name,"blit_fill_rows_2d_tail") || !strcmp(program->name,"blit_fill_pairs_2d_tail"));
            const bool rowFill=builtin && (!strcmp(program->name,"blit_fill_rows") || !strcmp(program->name,"blit_fill_rows_2d") || !strcmp(program->name,"blit_fill_rows_2d_tail"));
            const bool pairFill=builtin && (!strcmp(program->name,"blit_fill_pairs") || !strcmp(program->name,"blit_fill_pairs_2d") || !strcmp(program->name,"blit_fill_pairs_2d_tail"));
            UInt32 frozenParameters[8]{},frozenSlot=0;bool freezeParameters=false;
            if(byteCopy || byteFill || wordCopy || wordFill || rowFill || pairFill)
                ticket->commandKinds|=GPU::Diagnostics::Transfer;
            if (wordCopy || wordFill || byteCopy || byteFill || rowFill || pairFill) {
                const bool copy=wordCopy || byteCopy;
                const unsigned destinationSlot=copy?1:0,parameterSlot=copy?2:1;
                const auto &parameters=d.buffers[parameterSlot], &to=d.buffers[destinationSlot];
                auto *parameterBuffer=retainBuffer(parameters.resource);
                UInt32 values[8]{};GPU::Blit::ComputePlan plan;
                // Freeze the small layout in the job's read-only IB. The
                // geometry checked below must be the geometry the GPU sees,
                // even if the client later changes its parameter resource.
                const unsigned parameterBytes=wordCopy || wordFill?16:sizeof(values);
                // This metadata is read by the CPU before this batch executes.
                // It cannot consume an earlier shader's GPU-produced layout.
                bool producer=false;
                if(parameterBuffer)for(UInt32 n=0;n<ticket->resourceCount;++n)
                    if((previousWrites&GPU::Native::Memory::writeBit(n)) &&
                        parameterBuffer->overlaps(job.buffers[n],parameters.offset,parameterBytes,0,job.buffers[n]->length))producer=true;
                if(producer){result=kIOReturnBadArgument;break;}
                if(fPipelinedAdmission && parameterBuffer && cpuReadBlocked(parameterBuffer)){result=kIOReturnBusy;break;}
                if (!parameterBuffer || parameters.bytes<parameterBytes || !parameterBuffer->synchronize() ||
                    !parameterBuffer->copyBytes(parameters.offset,values,parameterBytes)) {
                    result=kIOReturnBadArgument;break;
                }
                bool valid=pairFill ? GPU::Blit::pairFillParameters(to.offset,values,plan) :
                    rowFill ? GPU::Blit::rowFillParameters(to.offset,values,plan) :
                    wordFill ? GPU::Blit::fillPlan(to.offset,UInt64(values[0])*4,values[3],plan,false) :
                    wordCopy ? GPU::Blit::copyPlan(d.buffers[0].offset,to.offset,UInt64(values[0])*4,
                    UInt64(values[2])*4,UInt64(values[3])*4,values[1],plan,false) : byteCopy ?
                    GPU::Blit::copyBytePlan(UInt64(d.buffers[0].offset)+values[4],UInt64(to.offset)+values[5],
                        values[0],values[2],values[3],values[1],plan) :
                    GPU::Blit::fillBytePlan(UInt64(to.offset)+values[5],values[0],values[6],plan);
                GPU::Blit::VectorFillGeometry geometry;
                const bool geometryValid=vectorFill?(GPU::Blit::vectorFillGeometry(plan,geometry) && geometry.matches(d) && !strcmp(program->name,GPU::Blit::vectorFillKernel(plan))):
                    (elements>=plan.threads && elements-plan.threads<d.local[0] && dimensions==1);
                if (!valid || memcmp(values,plan.parameters,sizeof(values)) ||
                    to.offset!=plan.destinationOffset || to.bytes<plan.destinationBytes || !geometryValid) {
                    result=kIOReturnBadArgument;break;
                }
                if (copy) {
                    const auto &from=d.buffers[0];
                    auto *source=retainBuffer(from.resource),*destination=retainBuffer(to.resource,nullptr,true);
                    const UInt64 unit=wordCopy?4:1;
                    if (from.offset!=plan.sourceOffset || from.bytes<plan.sourceBytes || !source || !destination ||
                        source->overlapsRanges(destination,{UInt64(from.offset)+values[4],UInt64(values[0])*unit,UInt64(values[2])*unit,values[1]},
                            {UInt64(to.offset)+values[5],UInt64(values[0])*unit,UInt64(values[3])*unit,values[1]})) {
                        result=kIOReturnBadArgument;break;
                    }
                    bindings.bytes[0]=plan.sourceBytes;
                }
                memcpy(frozenParameters,values,sizeof(values));frozenSlot=parameterSlot;freezeParameters=true;
                bindings.bytes[parameterSlot]=sizeof(values);bindings.bytes[destinationSlot]=plan.destinationBytes;
            }
            for (UInt32 slot=0;slot<QGPU_COMPUTE_IMAGE_COUNT;++slot) if ((program->imageMask|program->sampleMask)&(1u<<slot)) {
                const auto &image=d.images[slot];UInt64 bytes=0;
                UInt64 address=resolveTexture(image,(program->imageWriteMask&(1u<<slot))!=0);
                if (!address || !GPU::Shader::textureBytes(image,bytes)) {result=kIOReturnBadArgument;break;}
                bindings.addresses[QGPU_COMPUTE_IMAGE_BASE+slot]=address;bindings.bytes[QGPU_COMPUTE_IMAGE_BASE+slot]=UInt32(bytes);
            }
            // Sampling and SSBO stores have distinct cache paths. Reject
            // physical aliases within a dispatch, including separate imports.
            for(UInt32 slot=0;slot<QGPU_COMPUTE_IMAGE_COUNT && result==kIOReturnSuccess;++slot) if(program->sampleMask&(1u<<slot)) {
                const auto &sample=d.images[slot];auto *a=retainBuffer(sample.resource);GPU::StridedRange sampledRange;
                if(!a || !textureRanges(sample,sampledRange)){result=kIOReturnBadArgument;break;}
                for(UInt32 other=0;other<QGPU_COMPUTE_IMAGE_COUNT;++other)if(program->imageWriteMask&(1u<<other)) {
                    const auto &image=d.images[other];auto *b=retainBuffer(image.resource);GPU::StridedRange writtenRange;
                    if(!b || !textureRanges(image,writtenRange) || a->overlapsRanges(b,sampledRange,writtenRange))result=kIOReturnBadArgument;
                }
                for(UInt32 other=0;other<GPU::Shader::MaxBindings;++other)if(program->writeMask&(1u<<other)) {
                    const auto &binding=d.buffers[other];auto *b=retainBuffer(binding.resource);
                    if(!b || a->overlapsRanges(b,sampledRange,{binding.offset,binding.bytes,0,1}))result=kIOReturnBadArgument;
                }
            }
            if (result!=kIOReturnSuccess) break;
            // Explicit diagnostic boot only, shared with single-IB draw
            // traces (at most three shader commands). Capture before CP.
            const bool trace=fSubmitTraceEnabled && fSubmitTraceCount<fSubmitTraceLimit;
            if(trace) {
                ++fSubmitTraceCount;UInt32 hash=2166136261u;
                for(UInt32 i=0;i<program->codeWords;++i){hash^=program->code[i];hash*=16777619u;}
                kprintf("QcomGPU: compute trace program=%s id=%u ib=%llx code-words=%u code-fnv=%x instrlen=%u constlen=%u mask=%x local=%u,%u,%u groups=%u,%u,%u\n",
                    program->name,d.program,job.ib->address,program->codeWords,hash,program->instrlen,program->constlen,
                    program->bufferMask,d.local[0],d.local[1],d.local[2],d.groups[0],d.groups[1],d.groups[2]);
                for(UInt32 i=0;i<QGPU_COMPUTE_BUFFER_COUNT;++i)if(bindings.bytes[i])
                    kprintf("QcomGPU: compute binding slot=%u va=%llx bytes=%x\n",i,bindings.addresses[i],bindings.bytes[i]);
            }
            const bool hardwareClear=vectorFill && freezeParameters &&
                Adreno::hardwareClearEligible(bindings.addresses[0],bindings.bytes[0],frozenParameters,pairFill);
            encoded = pipeline.operation(job.commandBuffers?&allocator:nullptr,fCompactIndirectBuffers,
                [&](Adreno::IndirectBufferAllocator *) {
                    if(hardwareClear)return pipeline.clearRows(bindings.addresses[0],bindings.bytes[0],frozenParameters,pairFill);
                    if(freezeParameters) {
                        UInt32 offset=0;auto status=encoder.data(frozenParameters,8,offset);
                        if(status!=GPU::Result::Success)return status;
                        bindings.addresses[frozenSlot]=pipeline.address()+offset;
                    }
                    return pipeline.dispatch(d,*program,bindings);
                });
            if(operation) {
                auto &info=operation->description;encodingBindings.programDescriptions.describe(info.first,*program);
                info.firstID=d.program;info.count=uint32_t(elements);info.instances=1;
                for(unsigned axis=0;axis<3;++axis){info.groups[axis]=d.groups[axis];info.local[axis]=d.local[axis];info.grid[axis]=d.grid[axis];}
                if(freezeParameters) {
                    memcpy(info.blitParameters,frozenParameters,sizeof(frozenParameters));
                    info.blitKind=hardwareClear?(pairFill?8:7):wordCopy?1:wordFill?2:byteCopy?3:byteFill?4:rowFill?5:6;
                    info.blitSourceBytes=wordCopy || byteCopy?bindings.bytes[0]:0;
                    info.blitDestinationBytes=bindings.bytes[wordCopy || byteCopy?1:0];
                }
            }
            // Blit geometry may have narrowed SSBO extents after validation.
            // Frozen parameter bytes now reside in the immutable IB, which is
            // mapped in full independently; never treat that IB as a client VA.
            if(encoded==GPU::Result::Success && !recordBindings(*program,bindings,0,0,freezeParameters?(1u<<frozenSlot):0))
                encoded=GPU::Result::Invalid;
            if(trace) {
                const auto *words=static_cast<const UInt32 *>(job.ib->bytes());
                const auto limit=encoder.size()<2048?encoder.size():2048;
                for(UInt32 i=0;i<limit;i+=8) {
                    UInt32 value[8]{};for(UInt32 j=0;j<8 && i+j<limit;++j)value[j]=words[i+j];
                    kprintf("QcomGPU: compute pm4 offset=%u words=%x,%x,%x,%x,%x,%x,%x,%x\n",i,
                        value[0],value[1],value[2],value[3],value[4],value[5],value[6],value[7]);
                }
                kprintf("QcomGPU: compute pm4 result=%u words=%u captured=%u\n",unsigned(encoded),encoder.size(),limit);
            }
        } else if (command.opcode == GPU::ShaderDraw) {
            auto drawCursor=mach_absolute_time();
            auto drawPhase=[&](UInt64 &ticks) {const auto now=mach_absolute_time();ticks+=now-drawCursor;drawCursor=now;};
            const auto &d = *reinterpret_cast<const GPU::Shader::Draw *>(bytes+command.offset); const auto *vertex = lookupProgram(d.vertex), *fragment = lookupProgram(d.fragment);
            shaderWrites=vertex && fragment && (vertex->writeMask|fragment->writeMask)!=0;
            if (fragment) fragment=&GPU::Shader::rasterProgram(*fragment,d.rasterSamples);
            auto &bindings=encodingBindings.bindings;__builtin_memset(&bindings,0,sizeof(bindings));
            UInt32 width=0,height=0;
            auto resolvePlane = [&](const GPU::Shader::Texture &plane,UInt32 format) -> UInt64 {
                UInt64 bytes=0;if (!GPU::Shader::planeSize(plane,format,bytes)) return 0;
                QcomGPUResource *resource=nullptr;
                auto *buffer=retainBuffer(plane.resource,&resource,true);
                if (resource && resource->surfaceInfo()) return 0;
                return buffer && GPU::range(buffer->length,plane.offset,bytes) &&
                    recordAddress(buffer->address+plane.offset,bytes) ? buffer->address+plane.offset : 0;
            };
            UInt64 target = d.target.resource?resolveTexture(d.target,true):0, elements = UInt64(d.start)+d.count;
            UInt64 additionalTargets[QGPU_COLOR_TARGET_COUNT-1]={};bool colorTargetsValid=true;
            for(unsigned i=1;i<QGPU_COLOR_TARGET_COUNT;++i)if(d.colorTargets[i-1].resource) {
                additionalTargets[i-1]=resolveTexture(d.colorTargets[i-1],true);if(!additionalTargets[i-1])colorTargetsValid=false;
            }
            auto &sampled=encodingBindings.sampled;sampled={};
            UInt64 depth=d.depth.resource?resolvePlane(d.depth,GPU::Shader::Depth32):0;
            UInt64 stencil=d.stencil.resource?resolvePlane(d.stencil,GPU::Shader::Stencil8):0;
            if (d.registry != GPUPrograms::Registry || d.version != GPU::Shader::Version || !vertex || !fragment ||
                vertex->stage != GPU::Shader::Vertex || fragment->stage != GPU::Shader::Fragment ||
                !GPU::Shader::validAttachments(d,width,height) ||
                !colorTargetsValid || (d.target.resource && !target) || (d.depth.resource && !depth) || (d.stencil.resource && !stencil) ||
                d.reserved || !GPU::Shader::validDraw(d) ||
                !d.scissor[2] || !d.scissor[3] ||
                !GPU::Shader::validRenderState(d.state) ||
                !GPU::range(width,d.scissor[0],d.scissor[2]) || !GPU::range(height,d.scissor[1],d.scissor[3]) ||
                vertex->position == 252) {
                result = kIOReturnBadArgument; break;
            }
            drawPhase(job.drawAttachmentsTicks);
            if (d.indexSize) {
                auto *indexBuffer = retainBuffer(d.indices.resource);
                if (!indexBuffer || !GPU::range(indexBuffer->length,d.indices.offset,d.indices.bytes)) {
                    result = kIOReturnBadArgument; break;
                }
                bool busy=fPipelinedAdmission?cpuReadBlocked(indexBuffer):indexBuffer->pendingGPU()>1;
                for (UInt32 j=0;!fPipelinedAdmission && j<fJobCount && !busy;++j) {
                    const auto &pending=fJobs[(fJobHead+j)%GPU::MaxJobs];
                    for (UInt32 n=0;n<pending.bufferCount;++n) if (indexBuffer->overlaps(pending.buffers[n],
                        d.indices.offset,d.indices.bytes,0,pending.buffers[n]->length)) {busy=true;break;}
                }
                if (busy) {result=kIOReturnBusy;break;}
                // No batch GPU work runs during this gated CPU snapshot.
                // Publish each retained backing once; dependencies and each
                // draw's actual index extent are still checked independently.
                if (!synchronizeIndices(indexBuffer)) {result=kIOReturnIOError;break;}
                if(!job.indices || !GPU::range(job.indices->length,indexOffsets[i],d.indices.bytes) ||
                    !snapshot.indices || !GPU::range(snapshot.indices->getLength(),indexOffsets[i],d.indices.bytes)) {result=kIOReturnNoResources;break;}
                for(UInt32 n=0;n<ticket->resourceCount;++n)if(previousWrites&GPU::Native::Memory::writeBit(n))
                    if(indexBuffer->overlaps(job.buffers[n],d.indices.offset,d.indices.bytes,0,job.buffers[n]->length))busy=true;
                if(busy){result=kIOReturnBadArgument;break;} // CPU snapshot must not precede a batch GPU producer
                const auto indexSnapshotBegin=mach_absolute_time();
                auto *indexCopy=static_cast<UInt8 *>(job.indices->bytes())+indexOffsets[i];
                auto *cachedIndices=static_cast<UInt8 *>(const_cast<void *>(snapshot.indices->getBytesNoCopy()))+indexOffsets[i];
                const bool indexValid=indexBuffer->copyBytes(d.indices.offset,cachedIndices,d.indices.bytes) &&
                    GPU::Shader::indexFootprint(d,cachedIndices,elements);
                if(indexValid)memcpy(indexCopy,cachedIndices,d.indices.bytes);
                const auto indexSnapshotEnd=mach_absolute_time();job.indexSnapshotTicks+=indexSnapshotEnd-indexSnapshotBegin;
                trace.sample(GPU::Diagnostics::IndexSnapshot,indexSnapshotBegin,indexSnapshotEnd);
                if (!indexValid) {
                    result = kIOReturnBadArgument; break;
                }
            }
            drawPhase(job.drawIndicesTicks);
            UInt32 vertexBindings=0;
            if (!GPU::Shader::vertexFootprint(*vertex,d,elements,vertexBindings) ||
                !resolveBindings(*vertex,d.buffers,elements,bindings,UInt64(d.baseInstance)+d.instances,vertexBindings) ||
                !resolveBindings(*fragment,d.fragmentBuffers,0,bindings,0,0,QGPU_FRAGMENT_BUFFER_BASE)) {
                result = kIOReturnBadArgument; break;
            }
            fragment=&Adreno::selectConstantWindowProgram(*fragment,bindings,
                job.scratch?job.scratch->address:0,job.scratch?UInt32(job.scratch->length):0);
            if(renderPass && !Adreno::gmemPrograms(*vertex,*fragment)){result=kIOReturnUnsupported;break;}
            if(renderPass && !i) {
                encodingBindings.renderTargets[0]=target;
                for(unsigned n=1;n<QGPU_COLOR_TARGET_COUNT;++n)encodingBindings.renderTargets[n]=additionalTargets[n-1];
            }
            if(!GPU::Shader::graphicsTextures(*vertex,d.vertexTextures,d.vertexSamplers) ||
                !GPU::Shader::graphicsTextures(*fragment,d.textures,d.samplers)) {result=kIOReturnBadArgument;break;}
            const GPU::Shader::Texture *stageTextures[]={d.vertexTextures,d.textures};
            const GPU::Shader::Program *programs[]={vertex,fragment};
            UInt64 *stageAddresses[]={sampled.vertex,sampled.fragment};
            for(UInt32 stage=0;stage<2;++stage)for(UInt32 slot=0;slot<QGPU_STAGE_TEXTURE_COUNT;++slot)
                if(GPU::Shader::graphicsTextureMask(*programs[stage])&(1u<<slot)) {
                    stageAddresses[stage][slot]=resolveTexture(stageTextures[stage][slot]);
                    if(!stageAddresses[stage][slot])result=kIOReturnBadArgument;
                }
            if(result!=kIOReturnSuccess)break;
            drawPhase(job.drawBindingsTicks);
            const GPU::Shader::Binding *stages[]={d.buffers,d.fragmentBuffers};
            const UInt32 writeMasks[]={vertex->writeMask,fragment->writeMask};
            // Resource IDs and IOVAs differ for imports of the same pages.
            // Check actual backing ranges before allowing render/read aliases.
            constexpr UInt32 attachmentCount=QGPU_COLOR_TARGET_COUNT+2;
            auto &planes=encodingBindings.planes;__builtin_memset(planes,0,sizeof(planes));
            for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)planes[i]=GPU::Shader::colorTarget(d,i);
            planes[QGPU_COLOR_TARGET_COUNT]=d.depth;planes[QGPU_COLOR_TARGET_COUNT+1]=d.stencil;
            for(UInt32 stage=0;stage<2;++stage)for(UInt32 slot=0;slot<QGPU_STAGE_TEXTURE_COUNT;++slot)
                if(GPU::Shader::graphicsReadMask(*programs[stage])&(1u<<slot))
                    planes[attachmentCount+stage*QGPU_STAGE_TEXTURE_COUNT+slot]=stageTextures[stage][slot];
            constexpr UInt32 planeCount=attachmentCount+2*QGPU_STAGE_TEXTURE_COUNT;
            for (UInt32 p=0;p<planeCount && result==kIOReturnSuccess;++p) if (planes[p].resource) {
                auto *a=retainBuffer(planes[p].resource);GPU::StridedRange planeRange;
                if (!a || !textureRanges(planes[p],planeRange)) {result=kIOReturnBadArgument;break;}
                if(renderPass && p<QGPU_COLOR_TARGET_COUNT && d.indexSize) {
                    auto *index=retainBuffer(d.indices.resource);
                    if(!index || a->overlapsRanges(index,planeRange,{d.indices.offset,d.indices.bytes,0,1})) {
                        result=kIOReturnBadArgument;break;
                    }
                }
                for (UInt32 q=p+1;q<planeCount;++q) if (planes[q].resource && (p<attachmentCount || q<attachmentCount)) {
                    auto *b=retainBuffer(planes[q].resource);
                    GPU::StridedRange otherRange;
                    if (!b || !textureRanges(planes[q],otherRange) || a->overlapsRanges(b,planeRange,otherRange))
                        result=kIOReturnBadArgument;
                }
                for (UInt32 stage=0;stage<2;++stage) for (UInt32 slot=0;slot<GPU::Shader::MaxBindings;++slot) if (stages[stage][slot].resource &&
                    (p<attachmentCount || (writeMasks[stage]&(1u<<slot)))) {
                    const auto &binding=stages[stage][slot];
                    auto *b=retainBuffer(binding.resource);
                    if (!b || a->overlapsRanges(b,planeRange,{binding.offset,binding.bytes,0,1})) result=kIOReturnBadArgument;
                }
            }
            if (result != kIOReturnSuccess) break;
            drawPhase(job.drawAliasesTicks);
            const GPU::Shader::Program *snapshotProgram=nullptr;
            if(fragment->framebufferMask && !renderPass) {
                for(const auto &program:GPUPrograms::Programs)if(!strcmp(program.name,"framebuffer_snapshot")){snapshotProgram=&program;break;}
                if(!snapshotProgram){result=kIOReturnUnsupported;break;}
            }
            const bool drawTrace=fSubmitTraceEnabled && fSubmitTraceCount<fSubmitTraceLimit && !fragment->framebufferMask;
            if(drawTrace) {
                ++fSubmitTraceCount;
                kprintf("QcomGPU: render trace vs=%s fs=%s ib=%llx target=%llx size=%u,%u pitch=%u count=%u instances=%u position=%u vertex-id=%u color=%u\n",
                    vertex->name,fragment->name,job.ib->address,target,width,height,d.target.pitch,d.count,d.instances,
                    vertex->position,vertex->vertexID,fragment->colorRegisters[0]);
                kprintf("QcomGPU: render viewport=%x,%x,%x,%x depth=%x,%x scissor=%u,%u,%u,%u\n",
                    d.state.viewport[0],d.state.viewport[1],d.state.viewport[2],d.state.viewport[3],d.state.depthRange[0],d.state.depthRange[1],
                    d.scissor[0],d.scissor[1],d.scissor[2],d.scissor[3]);
            }
            encoded = pipeline.operation(job.commandBuffers?&allocator:nullptr,fCompactIndirectBuffers,
                [&](Adreno::IndirectBufferAllocator *chunks) {
                    return pipeline.draw(d,*vertex,*fragment,bindings,target,sampled,d.indexSize?job.indices->address+indexOffsets[i]:0,depth,stencil,additionalTargets,
                        snapshotProgram,d.indexSize?static_cast<const UInt8 *>(snapshot.indices->getBytesNoCopy())+indexOffsets[i]:nullptr,false,fragment->framebufferMask?chunks:nullptr);
                });
            if(operation) {
                auto &info=operation->description;encodingBindings.programDescriptions.describe(info.first,*vertex);
                encodingBindings.programDescriptions.describe(info.second,*fragment);
                info.firstID=d.vertex;info.secondID=d.fragment;info.count=d.count;info.instances=d.instances;
                info.primitive=d.primitive;info.indexSize=d.indexSize;info.width=width;info.height=height;
                for(unsigned n=0;n<4;++n)info.scissor[n]=d.scissor[n];
            }
            if(encoded==GPU::Result::Success && (!recordBindings(*vertex,bindings,vertexBindings) ||
                !recordBindings(*fragment,bindings,0,QGPU_FRAGMENT_BUFFER_BASE)))encoded=GPU::Result::Invalid;
            drawPhase(job.drawEmitTicks);
            ibAddress=pipeline.address();
            if(drawTrace) {
                const auto *words=static_cast<const UInt32 *>(job.ib->bytes());
                const auto limit=encoder.size()<2048?encoder.size():2048;
                for(UInt32 i=0;i<limit;i+=8) {
                    UInt32 value[8]{};for(UInt32 j=0;j<8 && i+j<limit;++j)value[j]=words[i+j];
                    kprintf("QcomGPU: render pm4 offset=%u words=%x,%x,%x,%x,%x,%x,%x,%x\n",i,
                        value[0],value[1],value[2],value[3],value[4],value[5],value[6],value[7]);
                }
                kprintf("QcomGPU: render pm4 result=%u words=%u captured=%u\n",unsigned(encoded),encoder.size(),limit);
            }
            if(framebuffer)job.progressLimit=pipeline.sequence();
            job.copiedFramebufferBytes=pipeline.copiedFramebufferBytes();job.framebufferPrimitives=pipeline.framebufferPrimitives();
            job.copiedFramebuffer2DBytes=pipeline.copiedFramebuffer2DBytes();
            job.uncroppedFramebufferBytes=pipeline.uncroppedFramebufferBytes();
            job.inplaceFramebufferPrimitives=pipeline.inplaceFramebufferPrimitives();
        } else { result = kIOReturnUnsupported; break; }
        if(operation) {
            if(pipeline.lastOperationTimed())operation->flags|=GPU::OperationActivity::Timed;
            if(pipeline.lastOperationCounters()){operation->flags|=GPU::ShaderCounters::RecordFlag;}
            auto &info=operation->description;
            info.constantWindowState=encoded==GPU::Result::Success?pipeline.lastConstantWindowState():0;
            info.fullSetups=pipeline.fullRasterSetups()-beforeFull;info.reused=pipeline.reusedRasterPrimitives()-beforeReused;
            info.inplace=pipeline.inplaceFramebufferPrimitives()-beforeInplace;info.snapshots=pipeline.framebufferPrimitives()-beforeSnapshot;
            info.flushes=pipeline.sequence()-beforeFlush;info.copyBytes=pipeline.copiedFramebufferBytes()-beforeCopied;
        }
        if(encoded==GPU::Result::Success && shaderWrites && i+1<header.count)
            encoded=pipeline.operation(job.commandBuffers?&allocator:nullptr,fCompactIndirectBuffers,
                [&](Adreno::IndirectBufferAllocator *) {return pipeline.flushShaderWrites();});
        ibAddress=pipeline.address();
        job.progressLimit=pipeline.sequence();
        job.shaderObjectHits=pipeline.reusedShaderObjects();job.shaderObjectSavedBytes=pipeline.avoidedShaderObjectBytes();
        job.rasterFullSetups=pipeline.fullRasterSetups();job.rasterReusedPrimitives=pipeline.reusedRasterPrimitives();
        job.rasterDriverUpdates=pipeline.updatedVertexDriverConstants();
        if (encoded != GPU::Result::Success) result = encoded == GPU::Result::NoResources ?
            kIOReturnNoResources : kIOReturnBadArgument;
        if(result==kIOReturnSuccess) {
            ticket->commandKinds|=GPU::Diagnostics::commandKind(command.opcode);
            if(command.opcode==GPU::ShaderDraw || command.opcode==GPU::ShaderDispatch)++job.shaderOperations;
        }
    }
    if(result==kIOReturnSuccess) {
        encoder.reserveTail(0);
        if(encoder.barrier()!=GPU::Result::Success)result=kIOReturnNoResources;
        if(result==kIOReturnSuccess && job.commandBuffers) {
            auto *last=OSDynamicCast(QcomGPUBuffer,job.commandBuffers->getObject(job.commandBuffers->getCount()-1));
            Adreno::IndirectBuffer item{last?last->address:0,encoder.size(),0};
            if(!last || !job.commandList->appendBytes(&item,sizeof(item)) ||
                job.commandList->getLength()!=job.commandBuffers->getCount()*sizeof(item))result=kIOReturnNoResources;
        }
    }
    if(result==kIOReturnSuccess && renderPass) {
        QcomGPUBuffer *root=nullptr;
        if(job.commandBuffers->getCount()>=Adreno::MaxIndirectBuffers ||
            !GPU::Native::admissionFits(fFramebufferIBBytes,Adreno::IBDwords*4,GPU::Native::FramebufferIndirectLimit))result=kIOReturnNoResources;
        else if(!(root=acquireIndirectBuffer()))result=kIOReturnNoMemory;
        if(root) {
            Adreno::Encoder rootCommands(static_cast<UInt32 *>(root->bytes()),Adreno::IBDwords);
            job.gmemTiles=encodingBindings.gmem.columns*encodingBindings.gmem.rows;
            job.gmemSamples=fJobHistory?GPU::RenderPass::timingTiles(encodingBindings.gmem):0;
            Adreno::GMEMEncoder tiles(rootCommands,job.scratch->address,job.gmemSamples!=0);
            auto encoded=tiles.encode(*renderPass,encodingBindings.gmem,encodingBindings.renderTargets,
                static_cast<const Adreno::IndirectBuffer *>(job.commandList->getBytesNoCopy()),
                job.commandList->getLength()/sizeof(Adreno::IndirectBuffer));
            if(encoded==GPU::Result::Success)encoded=rootCommands.barrier();
            const Adreno::IndirectBuffer item{root->address,rootCommands.size(),0};
            auto *list=encoded==GPU::Result::Success?OSData::withBytes(&item,sizeof(item)):nullptr;
            if(!list || !job.commandBuffers->setObject(root))result=kIOReturnNoResources;
            else {
                job.commandList->release();job.commandList=list;list=nullptr;
                job.framebufferIBBytes+=root->length;fFramebufferIBBytes+=root->length;
                job.progressLimit=tiles.sequence();encoder=rootCommands;
            }
            if(list)list->release();root->release();
        }
    }
    trace.mark(GPU::Diagnostics::Encoding);
    job.encodingTicks=mach_absolute_time()-encodingBegin;
    // Whole-resource cache preparation belongs immediately before dispatch,
    // after overlapping GPU users have retired. This also fixes the CPU-only
    // admission mode: encoding a second job must not clean an active writer.
    if (result==kIOReturnSuccess) {
        const auto addressBegin=mach_absolute_time();
        auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
        job.addressSpace=mapper?mapper->createAddressSpace(fRing,job.ib,job.indices,job.scratch,job.buffers,job.bufferCount,
            job.commandBuffers,ticket->compositor,&encodingBindings.mappingPlan):nullptr;
        if(!job.addressSpace)result=kIOReturnNoResources;
        job.addressSpaceTicks=mach_absolute_time()-addressBegin;
        trace.mark(GPU::Diagnostics::AddressSpace);
    }
    const auto fenceBegin=mach_absolute_time();
    if(result==kIOReturnSuccess && fNativeSurfaceFences) {
        UInt32 deferred=0;result=ticket->prepareSurfaceFences(nativeAcceleratorIdentifier(),deferred);
        GPU::Diagnostics::add(fSurfaceFencesCreated,ticket->surfaceFenceCount);
        GPU::Diagnostics::add(fSurfaceFencesDeferred,deferred);
    }
    if (result==kIOReturnSuccess) {result=prepareNativeCompletion(ticket);trace.mark(GPU::Diagnostics::FencePrepare);}
    job.fenceTicks=mach_absolute_time()-fenceBegin;
    if (result != kIOReturnSuccess) {
        finishJob(job, false); ticket->release();
        if (result==kIOReturnIOError) faultHardware();
        return outcome(result);
    }
    job.native = ticket; ticket->retain();job.commandWords=encoder.size();
    if(!platformReady()){finishJob(job,false);faultHardware();ticket->release();return outcome(kIOReturnIOError);}
    ticket->commandKinds=GPU::Diagnostics::normalizeKinds(ticket->commandKinds);
    job.admitted=mach_absolute_time();
    job.admissionDepth=fJobCount+1;
    ++fJobCount;
    bool armed=armNativeCompletion(ticket);
    if (!armed) {ticket->release();faultHardware();return outcome(kIOReturnIOError);}
    ticket->setCompletionEvent();
    *scheduled=ticket; // transfer the initial reference out of the driver gate
    fTimer->setTimeoutMS(10);
    trace.mark(GPU::Diagnostics::AdmissionDispatch);
    return outcome(kIOReturnSuccess); // admission only; end token follows real retirement
}
