// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/graphics/IOGPU.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include "AdrenoSubmit.h"
#include "AdrenoStartup.h"
#include "QcomGPUBuffer.h"
#include <GPUNativeProtocol.h>
#include <GPUNativeCompletion.h>
#include <GPUDiagnostics.h>
#include <GPUMetalTiming.h>
#include <GPUMetalCapabilities.h>
#include <GPUHardwarePhases.h>
#include <GPUMappingBounds.h>
#include <GPUJobActivity.h>
#include <GPUOperationActivity.h>
#include <GPUShaderCounters.h>
#include <GPUNativeMemory.h>
class QcomGPUUserClient;
class QcomGPUMapper;
class QcomGPUAddressSpace;
class QcomGPUPlatform;
class QcomGPUNoC;
class QcomGPUSecure;
class QcomGPUCommandQueue;
class QcomGPUWorkQueue;
class QcomGPUNativeSubmission;
namespace QcomPower {class OSMController;}
class QcomGPU : public IOGPU {
    OSDeclareDefaultStructors(QcomGPU);
    IOWorkLoop *fLoop = nullptr;
    IOBufferMemoryDescriptor *fStampMemory = nullptr;
    bool fNativeStarting = false;
    IOCommandGate *fGate = nullptr;
    QcomGPUUserClient *fClients[GPU::MaxClients] = {};
    struct Registers {
        volatile uint8_t *base = nullptr;
        uint32_t read(uint32_t);
        void write(uint32_t, uint32_t);
        void publish();
        void acquire();
        void delay(unsigned);
    } fRegisters;
    Adreno::Engine<Registers> fEngine{fRegisters};
    IOMapper *fMapper = nullptr;
    QcomGPUPlatform *fPlatform = nullptr;
    QcomGPUNoC *fNoC = nullptr;
    QcomGPUSecure *fSecure = nullptr;
    IOMemoryMap *fRegisterMap = nullptr;
    QcomGPUBuffer *fRing = nullptr, *fFence = nullptr;
    QcomGPUBuffer *fFaultSnapshot = nullptr;
    bool fFaultSnapshotAttempted = false;
    IOInterruptEventSource *fInterrupt = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    struct Job {
        QcomGPUUserClient *client = nullptr;
        QcomGPUNativeSubmission *native = nullptr;
        QcomGPUBuffer *ib = nullptr, *indices = nullptr, *scratch = nullptr, *buffers[GPU::Native::Memory::ResourcesPerJob] = {};
        OSArray *commandBuffers=nullptr;
        OSData *commandList=nullptr;
        OSData *operationProfile=nullptr; // CPU-only trusted metadata; no client/program pointers
        QcomGPUAddressSpace *addressSpace = nullptr;
        uint32_t queue = 0, sequence = 0, bufferCount = 0,commandWords=0;
        Adreno::ExecutionToken execution;
        uint32_t shaderOperations=0;
        uint32_t admissionDepth=0;
        uint32_t ringDepth=0;
        uint64_t copiedFramebufferBytes=0,framebufferPrimitives=0,copiedFramebuffer2DBytes=0,uncroppedFramebufferBytes=0,inplaceFramebufferPrimitives=0;
        bool hardwareDone=false,accessFailed=false,hardwarePhases=false;
        uint64_t fence = 0, submitted = 0;
        uint64_t admitted=0,retireStarted=0;
        uint64_t prepareBegin=0,observed=0,gpuBegin=0,gpuEnd=0;
        uint64_t preflightTicks=0,indexAcquireTicks=0,indexSnapshotTicks=0,encodingTicks=0,addressSpaceTicks=0,fenceTicks=0;
        uint64_t drawAttachmentsTicks=0,drawIndicesTicks=0,drawBindingsTicks=0,drawAliasesTicks=0,drawEmitTicks=0;
        uint32_t diagnosticPID=0,indexBytes=0,gmemTiles=0,gmemSamples=0;
        uint64_t shaderObjectHits=0,shaderObjectSavedBytes=0;
        uint64_t rasterFullSetups=0,rasterReusedPrimitives=0,rasterDriverUpdates=0;
        uint64_t framebufferBytes = 0;
        uint64_t framebufferFullBytes = 0;
        uint64_t framebufferIBBytes=0;
        uint32_t progressLimit=0;
        Adreno::ProgressWatchdog progress;
    } fJobs[GPU::MaxJobs] = {};
    uint32_t fJobHead = 0, fJobCount = 0;
    uint32_t fActiveJob=GPU::MaxJobs;
    QcomGPUNativeSubmission *fPreparedSubmission=nullptr; // borrowed while its native prepare owns hardware admission
    GPU::Diagnostics::Cache<QcomGPUBuffer *,GPU::MaxJobs> fIndirectBuffers;
    UInt64 fIndirectBufferHits=0,fIndirectBufferAllocations=0;
    static constexpr uint64_t ScratchCacheBytes=32*1024*1024;
    GPU::Diagnostics::SizedCache<QcomGPUBuffer *,4,ScratchCacheBytes> fScratchBuffers;
    UInt64 fScratchHits=0,fScratchAllocations=0,fScratchEvictions=0;
    GPU::Diagnostics::Timing fScratchClearTime{},fScratchAllocateTime{};
    IOMemoryMap *fPerformanceClockMap=nullptr;
    IOMemoryMap *fPerformanceGPUCCMap=nullptr;
    IOMemoryMap *fPerformanceAPCS[2]={};
    QcomPower::OSMController *fPerformanceCPUClocks=nullptr;
    uint64_t fPerformanceClockDeadline=0;
    uint32_t fPerformanceClockSamples=0;
    bool fPerformanceClockAttempted=false;
    bool fExecutionProfile=false;
    GPU::Diagnostics::Timing fExecutionNative{},fExecutionCompositor{};
    GPU::HardwarePhases::Snapshot fHardwarePhases;
    OSData *fOperationActivity=nullptr;
    UInt32 fShaderCounterProfile=0; // One-based operation index; zero disables counters.
    GPU::ShaderCounters::Snapshot fShaderCounters;
    OSData *fMetalTiming=nullptr;
    OSData *fJobHistory=nullptr; // CPU-only bounded history, never a GPU mapping or stack local
    GPU::JobActivity::Resources fResourceActivity;
    GPU::JobActivity::ShaderObjects fShaderObjectActivity;
    GPU::JobActivity::RasterStates fRasterStateActivity;
    UInt64 fExecutionInvalid=0;
    void observePerformanceClocks();
    static constexpr uint64_t IndexSnapshotLimit=GPU::Native::IndexSnapshotLimit;
    uint64_t fIndexSnapshotBytes = 0; // live plus cached private index copies stay within16MiB
    GPU::Diagnostics::SizedCache<QcomGPUBuffer *,4,IndexSnapshotLimit> fIndexBuffers;
    UInt64 fIndexHits=0,fIndexAllocations=0,fIndexEvictions=0;
    bool fIndexCache=true;
    bool fStagedAdmissionChecks=true;
    uint64_t fFramebufferSnapshotBytes = 0; // at most 64 MiB across live native jobs
    uint64_t fFramebufferIBBytes=0; // at most64 MiB across live framebuffer batches
    bool fPrivateAddressSpacesVerified = false;
    uint64_t fTimeoutTicks = 0;
    uint64_t fBootstrapDeadline = 0;
    bool fBootstrapPending=false, fAdmissionOn=true;
    Adreno::PlatformContract fFirmwareContract{};
    Adreno::FirmwareHandoff fFirmwareHandoff{};
    uint64_t fRecoveryDrainDeadline=0;
    uint32_t fRecoveryAttempts=0,fRecoveryCount=0,fRecoveryStage=0;
    IOReturn fRecoveryResult=kIOReturnNotReady;
    bool fFirmwareRetained=false,fRecoveryPending=false,fRecoveryFailed=false;
    bool fInterruptAdded = false, fTimerAdded = false, fProviderOpened = false, fQuarantined = false;
    GPU::State fState = GPU::Stopped;
    bool fAdded = false, fPM = false, fStarted = false;
    GPU::Native::Memory::Usage fNativeMemoryUsage{};
    bool fNativeAccountingFault=false;
    IOGPUDeviceUserClient *fNativeClientRegistry[GPU::MaxClients] = {}; // gate-protected weak entries
    UInt32 fNativeCalls = 0;
    UInt64 fNativeSubmitted=0,fNativeCompleted=0,fNativeFailed=0;
    UInt64 fNativeShaderOperations=0,fCompositorShaderOperations=0;
    UInt64 fNativeFramebufferBytes=0,fCompositorFramebufferBytes=0;
    UInt64 fNativeFramebuffer2DBytes=0,fCompositorFramebuffer2DBytes=0;
    UInt64 fNativeFramebufferUncroppedBytes=0,fCompositorFramebufferUncroppedBytes=0;
    UInt64 fNativeFramebufferPrimitives=0,fCompositorFramebufferPrimitives=0;
    UInt64 fNativeFramebufferInplacePrimitives=0,fCompositorFramebufferInplacePrimitives=0;
    UInt64 fNativeQueuedAdmissionJobs=0,fCompositorQueuedAdmissionJobs=0;
    UInt32 fNativeAdmissionMaxDepth=0,fCompositorAdmissionMaxDepth=0;
    UInt64 fCPUReadDeferrals=0,fPipelineCapacityDeferrals=0;
    GPU::Native::ResponsibleTaskInfo fLastResponsibleTasks{};
    UInt64 fCompositorSubmitted=0,fCompositorCompleted=0,fCompositorFailed=0,fIRQCallbacks=0;
    UInt64 fNativeBackpressure=0;
    UInt64 fStatisticsDeadline=0;
    GPU::Diagnostics::Snapshot fDiagnostics{};
    bool fSubmitTraceEnabled=false;
    bool fDirectShaderUploads=false;
    UInt32 fFramebuffer2DMode=0;
    bool fFramebufferXCrop=true;
    bool fFramebufferWord32=false; // Keep the unverified word32 candidate separate from in-place feedback.
    UInt32 fFramebufferFetchMode=0;
    bool fPipelinedAdmission=false;
    bool fRingPipeline=false;
    bool fCompactIndirectBuffers=false;
    bool fCompletionNotifications=false;
    bool fNativeSurfaceFences=false;
    UInt64 fSurfaceFencesCreated=0,fSurfaceFencesDeferred=0,fSurfaceFencesCompleted=0,fSurfaceFencesFailed=0;
    UInt64 fLiveStatisticsSequence=0;
    UInt64 fNativeIndirectBuffers=0,fCompositorIndirectBuffers=0;
    UInt64 fNativeIndirectBytes=0,fCompositorIndirectBytes=0;
    UInt64 fNativeIndirectWords=0,fCompositorIndirectWords=0;
    UInt64 fNativeQueuedRingJobs=0,fCompositorQueuedRingJobs=0;
    UInt32 fNativeRingMaxDepth=0,fCompositorRingMaxDepth=0;
    UInt64 fNativeFramebufferScratchBytes=0,fCompositorFramebufferScratchBytes=0;
    UInt64 fNativeFramebufferScratchAvoidedBytes=0,fCompositorFramebufferScratchAvoidedBytes=0;
    bool fSecondaryIBCache=true;
    bool fDomainResumeEnabled=false;
    UInt32 fSubmitTraceCount=0,fSubmitTraceLimit=0;
    void sampleNative(GPU::Diagnostics::Stage,UInt64,UInt64,bool);
    static IOReturn action(OSObject *, void *, void *, void *, void *);
    static IOReturn startEngineAction(OSObject *,void *,void *,void *,void *);
    static IOReturn bootstrapAction(OSObject *,void *,void *,void *,void *);
    static IOReturn mapperFaultAction(OSObject *,void *,void *,void *,void *);
    static IOReturn recoveryAction(OSObject *,void *,void *,void *,void *);
    static IOReturn nativeQueryAction(OSObject *, void *, void *, void *, void *);
    static IOReturn scheduledAction(OSObject *,void *,void *,void *,void *);
    static IOReturn nativeOpenAction(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeAccountingAction(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeDrainAction(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeRegistryAction(OSObject *, void *, void *, void *, void *);
    void closeNativeClients();
    IOReturn prepareNativeCompletion(QcomGPUNativeSubmission *);
    bool armNativeCompletion(QcomGPUNativeSubmission *);
    IOReturn perform(QcomGPUUserClient *, uint32_t, IOExternalMethodArguments *);
    IOReturn request(QcomGPUUserClient *, uint32_t, IOExternalMethodArguments * = nullptr);
    IOReturn configureHardware(IOService *);
    IOReturn verifyAddressSpaces();
    bool publishHardwareState();
    void publishStatistics();
    bool stopHardware();
    void releaseHardware();
    bool finishJob(Job &, bool);
    bool cancelFailedSurfaceAccess();
    QcomGPUBuffer *acquireIndirectBuffer();
    void recycleIndirectBuffer(QcomGPUBuffer *&);
    QcomGPUBuffer *acquireScratch(uint64_t);
    void recycleScratch(QcomGPUBuffer *&,bool);
    QcomGPUBuffer *acquireIndexSnapshot(uint64_t);
    void recycleIndexSnapshot(QcomGPUBuffer *&,bool);
    void faultHardware(bool recoverable=false);
    void captureFaultState();
    bool recoveryAvailable() const;
    IOReturn recoverHardware();
    void quarantineHardware();
    void retireJobs();
    bool dispatchNextJob(QcomGPUNativeSubmission *selected=nullptr);
    bool dispatchBlocked(unsigned) const;
    IOReturn submit(QcomGPUUserClient *, IOExternalMethodArguments *);
    uint64_t features() const;
    uint64_t blockers() const;
    static void interruptAction(OSObject *, IOInterruptEventSource *, int);
    static void timerAction(OSObject *, IOTimerEventSource *);
    friend class QcomGPUUserClient;
protected:
    IOMemoryDescriptor *getStampMemory(UInt32 *) APPLE_KEXT_OVERRIDE;
    IOGPUEventMachine *allocEventMachine() APPLE_KEXT_OVERRIDE;
    IOGPUScheduler *allocScheduler() APPLE_KEXT_OVERRIDE;
    IOGPUTask *createGPUTask(task_t) APPLE_KEXT_OVERRIDE;
    IOGPUTask *createKernelGPUTask() APPLE_KEXT_OVERRIDE;
    void getGPUConfiguration(IOGPUConfiguration *) APPLE_KEXT_OVERRIDE;
    bool configureDevice(IOService *) APPLE_KEXT_OVERRIDE;
    void nativeSlot196() APPLE_KEXT_OVERRIDE;
    IOGPUDeviceUserClient *allocDeviceUserClient() APPLE_KEXT_OVERRIDE;
    IOGPUSysMemory *allocSystemMemory() APPLE_KEXT_OVERRIDE;
    IOGPUResource *allocResource() APPLE_KEXT_OVERRIDE;
    OSObject *allocWorkQueue() APPLE_KEXT_OVERRIDE;
    IOGPUCommandQueue *allocCommandQueue() APPLE_KEXT_OVERRIDE;
    IOBufferMemoryDescriptor *createBufferMemoryDescriptor(task_t, IOOptionBits, vm_size_t, vm_size_t) APPLE_KEXT_OVERRIDE;
public:
    IOReturn prepareScheduled(QcomGPUNativeSubmission *);
    void submitScheduled(QcomGPUNativeSubmission *);
    void cancelScheduled(QcomGPUNativeSubmission *);
    void restartScheduled();
    QcomGPUWorkQueue *createNativeWorkQueue(QcomGPUCommandQueue *);
    UInt32 reserveNativeEventChannel();
    void releaseNativeEventChannel(UInt32);
    GPU::Native::Memory::Limits nativeMemoryLimits() const;
    bool nativeAdmissionBlocked(bool indexed,bool framebuffer,const GPU::Native::AdmissionMemory &,bool cpuReadHazard);
    bool nativeCPUReadBlocked(const QcomGPUBuffer *,UInt32 ownUses=0) const;
    bool nativePipelinedAdmission() const {return fPipelinedAdmission;}
    bool nativeFramebufferInplace(task_t) const;
    bool nativeFramebufferCompact() const {return fFramebufferFetchMode!=3;}
    bool nativeSubmissionReady() const;
    bool nativeAddressSpacesReady() const;
    bool nativeCPUPreparationReady() const; // local state only; never authorizes MMIO or DMA
    bool platformReady() const;
    bool powerReady() const;
    void mapperFault();
    void beginNativeCall() { ++fNativeCalls; }
    void endNativeCall() { --fNativeCalls; fGate->commandWakeup(&fNativeCalls); }
    bool reserveNative(UInt64);
    void releaseNative(UInt64,UInt64 releaseBegin=0,bool compositor=false);
    bool registerNativeClient(IOGPUDeviceUserClient *);
    void unregisterNativeClient(IOGPUDeviceUserClient *);
    QcomGPUBuffer *mapNativeMemory(IOMemoryDescriptor *, bool shared = false);
    IOReturn submitNative(QcomGPUCommandQueue *, IOGPUDevice *, const GPU::Native::SubmitArgs &,QcomGPUNativeSubmission **);
    bool completeNative(QcomGPUNativeSubmission *, bool);
    IOReturn nativeQuery(IOGPUDeviceUserClient *, UInt32, IOExternalMethodArguments *);
    IOReturn copyLiveStatistics(UInt32,GPU::Diagnostics::LiveSnapshot &);
    IOReturn recordMetalTiming(const GPU::MetalTiming::Record &,uint32_t,bool);
    IOReturn publishMetalTiming(uint64_t &);
    static bool nativeShaderTask(task_t);
    static bool nativeCompositorTask(task_t);
    IOMemoryDescriptor *stampMemory() const { return fStampMemory; }
    IOService *probe(IOService *, SInt32 *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    void systemWillShutdown(IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
};
