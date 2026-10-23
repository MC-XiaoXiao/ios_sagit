// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/graphics/IOGPU.h>
#include <GPUNativeProtocol.h>
#include <GPUSurfaceProtocol.h>
#include <GPUNativeMemory.h>
#include <GPUSurfaceAccess.h>
#include "QcomGPUBuffer.h"
class QcomGPU;
class IOFence;
class OSArray;
constexpr UInt32 QcomGPUEventChannels=GPU::MaxClients*GPU::MaxQueues;
static_assert(QcomGPUEventChannels<=64,"event channel allocation mask capacity");
struct QcomGPUSurfaceWaitLog {
    OSArray *records=nullptr;
    unsigned count=0;
    void append(const char *,...);
};
class QcomGPUMemoryMap : public IOGPUMemoryMap {
    OSDeclareDefaultStructors(QcomGPUMemoryMap);
    QcomGPUBuffer *fBuffer = nullptr;
protected:
    void nativeSlot15() APPLE_KEXT_OVERRIDE;
    UInt64 address() APPLE_KEXT_OVERRIDE;
    UInt64 gpuAddress() APPLE_KEXT_OVERRIDE;
    UInt64 length() APPLE_KEXT_OVERRIDE;
    UInt64 mappedLength() APPLE_KEXT_OVERRIDE;
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE;
    void nativeSlot27() APPLE_KEXT_OVERRIDE;
public:
    bool initWithBuffer(QcomGPUBuffer *);
    void free() APPLE_KEXT_OVERRIDE;
    QcomGPUBuffer *buffer() const { return fBuffer; }
};
class QcomGPUResource : public IOGPUResource {
    OSDeclareDefaultStructors(QcomGPUResource);
    QcomGPU *fGPU = nullptr;
    IOGPUDevice *fOwner = nullptr; // namespace identity; no ownership cycle
    UInt64 fCharge = 0;
    IOVirtualAddress fClientAddress = 0;
    bool fNativeInitialized = false;
    bool fDiagnosticCompositor = false; // attribution only, never resource authority
    UInt32 fType = 0;
    IOSurface *fSurface = nullptr;
    GPU::Surface::Info fSurfaceInfo{};
    QcomGPUBuffer *fSurfaceView = nullptr;
protected:
    UInt64 mappingOptions(const void *, UInt64) APPLE_KEXT_OVERRIDE;
    bool map(UInt64, UInt32) APPLE_KEXT_OVERRIDE;
    bool mapAtAddressLength(UInt64, UInt64, UInt64) APPLE_KEXT_OVERRIDE;
    void nativeSlot27() APPLE_KEXT_OVERRIDE;
    UInt64 nativeSlot28() APPLE_KEXT_OVERRIDE;
public:
    bool initWithGPU(IOGPU *, IOGPUDevice *, UInt32) APPLE_KEXT_OVERRIDE;
    bool initialize(IOGPUDevice *, const void *, UInt64) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool ownedBy(IOGPUDevice *owner) const { return fOwner == owner; }
    UInt64 chargedBytes() const { return fCharge; }
    QcomGPUBuffer *buffer() const;
    const GPU::Surface::Info *surfaceInfo() const { return fSurface ? &fSurfaceInfo : nullptr; }
    IOSurface *surface() const { return fSurface; } // borrowed while resource retained
    void markSurfaceWritten();
    IOVirtualAddress cpuAddress() const { return fClientAddress ? fClientAddress : nativeClientMap ? nativeClientMap->getVirtualAddress() : 0; }
};
class QcomGPUScheduler : public IOGPUScheduler {
    OSDeclareDefaultStructors(QcomGPUScheduler);
};
class QcomGPUWorkQueue : public IOGPUWorkQueue {
    OSDeclareDefaultStructors(QcomGPUWorkQueue);
public:
    void submit(IOGPUCommandDescriptor *) APPLE_KEXT_OVERRIDE;
    void restart() APPLE_KEXT_OVERRIDE;
};
class QcomGPUNativeSubmission;
class QcomGPUCommandQueue : public IOGPUCommandQueue {
    OSDeclareDefaultStructors(QcomGPUCommandQueue);
    QcomGPU *fGPU = nullptr;
    IOGPUDevice *fOwner = nullptr;
    IOReturn fResult = kIOReturnNotReady;
    UInt64 fLastToken = 0;
    UInt32 fPending = 0;
    UInt32 fPriority = 0;
    bool fNativeInitialized = false,fFaulted=false;
    QcomGPUWorkQueue *fWorkQueue = nullptr;
    UInt32 fEventChannel = UINT32_MAX;
    QcomGPUNativeSubmission *fScheduledSubmission = nullptr;
protected:
    void nativeSlot22() APPLE_KEXT_OVERRIDE; // native effective-priority change, no arguments
    void submitCommandBuffer(const void *) APPLE_KEXT_OVERRIDE;
public:
    bool initWithGPU(IOGPU *, IOGPUDevice *, const void *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    QcomGPUWorkQueue *workQueue() const {return fWorkQueue;}
    UInt32 eventChannel() const {return fEventChannel;}
    QcomGPUNativeSubmission *takeScheduledSubmission() {auto *result=fScheduledSubmission;fScheduledSubmission=nullptr;return result;}
    IOReturn result() const { return fResult; }
    bool reserveTokens(const GPU::Native::SubmitArgs &);
    void fail() {fFaulted=true;}
    UInt32 pending() const { return fPending; }
    UInt32 priority() const { return __atomic_load_n(&fPriority,__ATOMIC_ACQUIRE); }
    void beginSubmission() { ++fPending; }
    void endSubmission() { if (fPending) --fPending; }
    IOGPUEventFence *copyCurrentFence();
    bool eventStateValid() const;
    void addNativeEvent(IOGPUEventMachine *events) { events->nativeAddEvent(fEventChannel,&nativeCurrentEvents,false); }
    void clearNativeEvents() { nativeCurrentEvents.clear();nativePriorEvents.clear(); }
};
class QcomGPUNativeSubmission : public IOGPUCommandDescriptor {
    OSDeclareDefaultStructors(QcomGPUNativeSubmission);
public:
    bool initSubmission(QcomGPU *, QcomGPUCommandQueue *);
    IOReturn prepare(UInt32) APPLE_KEXT_OVERRIDE;
    void cancel() APPLE_KEXT_OVERRIDE;
    void setCompletionEvent();
    bool done = false;
    IOReturn result = kIOReturnNotReady;
    QcomGPUCommandQueue *queue = nullptr;
    IOGPUDevice *device = nullptr;
    IOGPUEventFence *fence = nullptr;
    UInt32 stamp = 0;
    bool armed = false;
    bool compositor = false;
    UInt32 commandKinds=0;
    GPU::Native::ResponsibleTaskInfo responsibleTasks{};
    IOGPUResource *resources[GPU::Native::Memory::ResourcesPerJob] = {};
    UInt32 resourceCount = 0;
    UInt64 surfaceWrites = 0;
    IOFence *surfaceFences[GPU::Native::Memory::ResourcesPerJob] = {};
    UInt32 surfaceFenceCount=0;
    UInt64 surfaceInitiallyReady=0;
    bool surfaceFencesFinished=false;
    IOReturn prepareSurfaceFences(UInt32 accelerator,UInt32 &deferred);
    GPU::Native::SurfaceAccessObservation surfaceAccess() const;
    bool finishSurfaceFences(IOReturn status);
    void logSurfaceFenceWait(unsigned jobOffset,QcomGPUSurfaceWaitLog &) const;
    void free() APPLE_KEXT_OVERRIDE;
};
