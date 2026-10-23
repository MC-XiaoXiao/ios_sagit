// SPDX-License-Identifier: BSD-2-Clause
#include "QcomGPU.h"
#include "QcomGPUMapper.h"
#include "QcomGPUPlatform.h"
#include "QcomGPUNoC.h"
#include "QcomGPUSecure.h"
#include "QcomGPUUserClient.h"
#include "QcomGPUNative.h"
#include "AdrenoRecovery.h"
#include "../../clock/QcomOSM/OSMController.h"
#include <GPUBlit.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/mach_time.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <A5xxRegs.h>

uint32_t QcomGPU::Registers::read(uint32_t offset) {
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + offset);
    __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld" : "+r"(value) :: "memory"); return value;
}
void QcomGPU::Registers::write(uint32_t offset, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
void QcomGPU::Registers::publish() { __asm__ volatile("dsb oshst" ::: "memory"); }
void QcomGPU::Registers::acquire() { __asm__ volatile("dmb oshld" ::: "memory"); }
void QcomGPU::Registers::delay(unsigned us) { IODelay(us); }
bool QcomGPU::powerReady() const {return fPlatform && fPlatform->healthy();}
bool QcomGPU::platformReady() const {
    static unsigned measured=0;
    if(!__atomic_load_n(&measured,__ATOMIC_RELAXED) && fNoC && fSecure) {
        int timing=0;PE_parse_boot_argn("gpu-platform-timing",&timing,sizeof(timing));
        if(timing) {
            const auto start=mach_absolute_time();const bool power=powerReady();
            const auto powered=mach_absolute_time();const bool noc=power && fNoC->healthy();
            const auto connected=mach_absolute_time();const bool secure=noc && fSecure->healthy();
            const auto end=mach_absolute_time();
            if(secure) {
                __atomic_store_n(&measured,1,__ATOMIC_RELAXED);uint64_t ns[3]{};
                if(powered>=start)absolutetime_to_nanoseconds(powered-start,&ns[0]);
                if(connected>=powered)absolutetime_to_nanoseconds(connected-powered,&ns[1]);
                if(end>=connected)absolutetime_to_nanoseconds(end-connected,&ns[2]);
                kprintf("QcomGPU: platform validation power-ns=%llu noc-ns=%llu secure-ns=%llu\n",ns[0],ns[1],ns[2]);
            }
            return secure;
        }
        __atomic_store_n(&measured,1,__ATOMIC_RELAXED);
    }
    return powerReady() && (!fNoC || fNoC->healthy()) && (!fSecure || fSecure->healthy());
}
bool QcomGPU::nativeSubmissionReady() const {
    auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
    // Mapper health checks platform accessibility before its first MMIO read.
    // Do not repeat the same controller/NoC validation immediately before it.
    return fStarted && fState==GPU::FrameworkReady && fEngine.ready() && fNoC && fSecure &&
        (mapper ? mapper->healthy() : platformReady());
}
bool QcomGPU::nativeAddressSpacesReady() const {
    auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
    return fPrivateAddressSpacesVerified && nativeSubmissionReady() && mapper && mapper->privateAddressSpacesReady();
}
bool QcomGPU::nativeCPUPreparationReady() const {
    auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
    // A local-state guard, never evidence of healthy hardware. It permits
    // CPU-only resource preparation and snapshot/encoding. The mapper checks
    // actual platform/context/stream/firmware state before changing retained
    // mappings and again before their TLBSYNC. createAddressSpace, retireJobs
    // and dispatch keep their hardware guards. Resource handles may reuse an
    // existing map; their creation does not authorize GPU execution.
    // Re-evaluate after every Family entry/wait; carry no health result across
    // a gate release. The outer Submit hardware check is retained.
    return fPrivateAddressSpacesVerified && fStarted && fState==GPU::FrameworkReady &&
        !fQuarantined && fEngine.ready() && fNoC && fSecure && mapper && mapper->privateAddressSpacesReady();
}
IOReturn QcomGPU::startEngineAction(OSObject *object,void *handoff,void *,void *,void *) {
    auto *gpu=static_cast<QcomGPU *>(object);
    if(gpu->fState!=GPU::FrameworkReady)return kIOReturnNotReady;
    if(!gpu->fNoC || !gpu->fSecure || !gpu->platformReady())return kIOReturnNotReady;
    auto *mapper=OSDynamicCast(QcomGPUMapper,gpu->fMapper);if(mapper && !mapper->healthy())return kIOReturnNotReady;
    GPU::Result status;
    if(gpu->fFirmwareRetained) {
        // Moving from EFI to the kernel ring requires CP reinitialization;
        // RB_CNTL alone resets RPTR underneath the retained PFP fetch state.
        struct Guard {
            QcomGPU *owner;QcomGPUMapper *mapper;QcomGPUSecure *secure;Registers *registers;
            bool accessible(){return owner->platformReady();}
            bool healthy(){return accessible() && mapper->healthy();}
            bool isolated(){return accessible() && (registers->read(Adreno::MEControl)&Adreno::Halt)==Adreno::Halt &&
                (registers->read(Adreno::VBIFHalted)&15)==15;}
            bool synchronize(){return mapper->synchronizeForRecovery();}
            bool restoreZAP(){return secure->restore()==kIOReturnSuccess;}
        } guard{gpu,mapper,gpu->fSecure,&gpu->fRegisters};
        const auto initialized=Adreno::recover(gpu->fFirmwareContract,gpu->fFirmwareHandoff,gpu->fRegisters,guard,gpu->fEngine,
            static_cast<uint32_t *>(gpu->fRing->bytes()),gpu->fRing->address,
            static_cast<uint32_t *>(gpu->fFence->bytes()),gpu->fFence->address);
        status=initialized.result;
        kprintf("QcomGPU: kernel ring initialization stage=%u result=%u dma-stopped=%u\n",
            unsigned(initialized.stage),unsigned(status),initialized.dmaStopped);
        gpu->setProperty("GPUInitializationStage",uint64_t(initialized.stage),32);
        if(status!=GPU::Result::Success && !initialized.dmaStopped)gpu->quarantineHardware();
    } else {
        status=gpu->fEngine.start(*static_cast<Adreno::PlatformContract *>(handoff),static_cast<uint32_t *>(gpu->fRing->bytes()),
            gpu->fRing->address,static_cast<uint32_t *>(gpu->fFence->bytes()),gpu->fFence->address,{true,true});
    }
    if(status!=GPU::Result::Success || !gpu->platformReady() || (mapper && !mapper->healthy())) {
        IOLog("QcomGPU: retained CP/SMMU handoff self-test failed (%u)\n",unsigned(status));return kIOReturnIOError;
    }
    const auto verified=gpu->verifyAddressSpaces();
    kprintf("QcomGPU: private address-space self-test result=%x passed=%u\n",verified,gpu->fPrivateAddressSpacesVerified);
    return verified;
}
IOReturn QcomGPU::mapperFaultAction(OSObject *object,void *,void *,void *,void *) {
    auto *gpu=static_cast<QcomGPU *>(object);
    if (gpu->fState!=GPU::Faulted && gpu->fState!=GPU::Stopped) gpu->faultHardware();
    return kIOReturnSuccess;
}
void QcomGPU::mapperFault() {if(fGate)fGate->runAction(mapperFaultAction);}
uint64_t QcomGPU::features() const {
    return GPU::FrameworkFeatures | (nativeSubmissionReady() ? GPU::HardwareSubmit | GPU::GPUVirtualMemory : 0) |
        (nativeAddressSpacesReady() ? GPU::NativeSubmission | GPU::SharedSurfaces | GPU::AsynchronousSubmit | GPU::IsolatedSubmit : 0);
}
uint64_t QcomGPU::blockers() const {
    return (nativeSubmissionReady() ? 0 : GPU::HardwareRequirements) | GPU::MetalBackend;
}
bool QcomGPU::publishHardwareState()
{
    int batch=1;PE_parse_boot_argn("gpu-native-batch",&batch,sizeof(batch));
    setProperty("IOGPUNativeShaderBatchVersion",batch?GPU::Native::BatchCommandVersion:0,32);
    int mixed=1;PE_parse_boot_argn("gpu-mixed-batch",&mixed,sizeof(mixed));
    setProperty("IOGPUNativeMixedShaderBatchVersion",batch && mixed?1:0,32);
    setProperty("IOGPUNativePipelineVersion",fPipelinedAdmission?1:0,32);
    setProperty("IOGPUNativeRingPipelineVersion",fRingPipeline?1:0,32);
    setProperty("IOGPUNativeCompletionNotificationVersion",fCompletionNotifications?1:0,32);
    setProperty("IOGPUNativeSurfaceFenceVersion",fNativeSurfaceFences?1:0,32);
    publishStatistics();
    const bool ready=nativeSubmissionReady();
    return setProperty("GPUProtocolVersion",GPU::ProtocolVersion,32) && setProperty("GPUChipID",GPU::ChipID,32) &&
        setProperty("GPUFeatures",features(),64) &&
        setProperty("GPUBlockingRequirements",(ready?0:GPU::HardwareRequirements)|GPU::MetalBackend,64) &&
        setProperty("GPUStage",fBootstrapPending?"resource-wait":fRecoveryPending?"recovery-wait":fState==GPU::Faulted?"faulted":ready?"hardware-submit":"framework") &&
        setProperty("GPUHardwareReady",ready) && setProperty("IOGPUNativeSubmission",nativeAddressSpacesReady()) &&
        setProperty("IOGPUNativeIOSurfaceImport",nativeAddressSpacesReady()) &&
        setProperty("IOGPUNativeAsyncSubmission",nativeAddressSpacesReady()) &&
        setProperty("IOGPUNativeImmutableBlitParameters",nativeAddressSpacesReady()) &&
        setProperty("IOGPUNativeBlitLayoutVersion",GPU::Blit::NativeLayoutVersion,32) &&
        setProperty("IOGPUNativeMaxPendingJobs",GPU::MaxJobs,32) &&
        setProperty("IOGPUNativeAdmissionBackpressureVersion",1,32) &&
        setProperty("IOGPUNativeAddressSpaces",nativeAddressSpacesReady()) &&
        setProperty("GPUAddressSpaceSelfTestPassed",fPrivateAddressSpacesVerified) &&
        setProperty("GPUAddressSpaceMode",nativeAddressSpacesReady()?"private-per-submission":"kernel-diagnostics") &&
        setProperty("GPUStartupPending",fBootstrapPending) &&
        setProperty("GPURuntimeRecoveryAvailable",recoveryAvailable()) && setProperty("GPURecoveryPending",fRecoveryPending) &&
        setProperty("GPURecoveryAttempts",fRecoveryAttempts,32) && setProperty("GPURecoveryCount",fRecoveryCount,32) &&
        setProperty("GPURecoveryStage",fRecoveryStage,32) && setProperty("GPURecoveryResult",uint64_t(uint32_t(fRecoveryResult)),32);
}
void QcomGPU::observePerformanceClocks()
{
    uint32_t enabled=0;
    PE_parse_boot_argn("gpu-performance-observe",&enabled,sizeof(enabled));
    if(!enabled || fPerformanceClockSamples>=64)return;
    fExecutionProfile=true;
    const auto now=mach_absolute_time();if(now<fPerformanceClockDeadline)return;
    uint64_t interval=0;nanoseconds_to_absolutetime(5000000000ULL,&interval);
    fPerformanceClockDeadline=now+interval;
    // Diagnostic opt-in only. Reuse the existing MSM8998 OSM validator with
    // NO write/wait callbacks and a CPU read-only mapping. Do not start the
    // OSM driver, enable counters, change a P-state, voltage or PMU state.
    if(!fPerformanceClockAttempted) {
        fPerformanceClockAttempted=true;
        auto *memory=IODeviceMemory::withRange(0x179c0000,0x4000);
        fPerformanceClockMap=memory?memory->map(kIOMapInhibitCache|kIOMapReadOnly):nullptr;
        OSSafeReleaseNULL(memory);
        // Exact MSM8998 GPUCC range, read-only diagnostic mapping. Neither
        // debug muxes nor clock/CRC/PLL control registers are programmed.
        auto *gpuccMemory=IODeviceMemory::withRange(0x05065000,0x9000);
        fPerformanceGPUCCMap=gpuccMemory?gpuccMemory->map(kIOMapInhibitCache|kIOMapReadOnly):nullptr;
        OSSafeReleaseNULL(gpuccMemory);
        // Exact PLL/RCGR ranges from this device's vendor DT. Read-only
        // diagnostics: no PLL, mux, OSM, debug counter or PMU write.
        const uint64_t apcsBases[]={0x17910000,0x17810000};
        for(unsigned cluster=0;cluster<2;++cluster) {
            auto *resource=IODeviceMemory::withRange(apcsBases[cluster],0x8000);
            fPerformanceAPCS[cluster]=resource?resource->map(kIOMapInhibitCache|kIOMapReadOnly):nullptr;
            OSSafeReleaseNULL(resource);
        }
        if(fPerformanceClockMap && fPerformanceClockMap->getLength()==0x4000) {
            fPerformanceCPUClocks=new QcomPower::OSMController;
            if(fPerformanceCPUClocks) {
                QcomPower::IO io{fPerformanceClockMap,[](void *context,unsigned,uint32_t offset) {
                    auto *mapping=static_cast<IOMemoryMap *>(context);
                    auto value=*reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()+offset);
                    __asm__ volatile("dmb oshld" ::: "memory");return value;
                },nullptr,nullptr,nullptr};
                const auto status=fPerformanceCPUClocks->initialize(io);
                kprintf("QcomGPU: cpu-clock-observer init=%u mode=read-only-desired-pstate-not-measured-frequency\n",unsigned(status));
                if(status!=QcomPower::OK){delete fPerformanceCPUClocks;fPerformanceCPUClocks=nullptr;}
            }
        }
    }
    uint64_t gpuHz=0;IOReturn status=kIOReturnNotAttached;
    auto *bus=getProvider()?getProvider()->getProvider():nullptr;
    auto *function=OSSymbol::withCStringNoCopy(ClockController::Function);
    auto request=ClockController::targeted(ClockController::GPUCC,ClockController::GetRate,7);
    if(bus && function)status=bus->callPlatformFunction(function,false,&request.request,nullptr,nullptr,nullptr);
    if(status==kIOReturnSuccess && ClockController::valid(request.request))gpuHz=request.request.value;
    OSSafeReleaseNULL(function);
    uint32_t raw[2][4]{};
    if(fPerformanceClockMap && fPerformanceClockMap->getLength()==0x4000)for(unsigned cluster=0;cluster<2;++cluster) {
        const uint32_t offsets[]={0x1004,0x1f10,0x1f00,0x1f04};
        for(unsigned i=0;i<4;++i)raw[cluster][i]=*reinterpret_cast<volatile uint32_t *>(
            fPerformanceClockMap->getVirtualAddress()+cluster*0x2000+offsets[i]);
    }
    for(unsigned cluster=0;cluster<2;++cluster)if(fPerformanceAPCS[cluster] && fPerformanceAPCS[cluster]->getLength()==0x8000) {
        const uint32_t offsets[]={0x1050,0x1054,0x6000,0x6004,0x6008,0x600c,0x602c};
        uint32_t before[7]{},after[7]{};
        auto base=fPerformanceAPCS[cluster]->getVirtualAddress();
        for(unsigned i=0;i<7;++i)before[i]=*reinterpret_cast<volatile uint32_t *>(base+offsets[i]);
        __asm__ volatile("dmb oshld" ::: "memory");
        for(unsigned i=0;i<7;++i)after[i]=*reinterpret_cast<volatile uint32_t *>(base+offsets[i]);
        __asm__ volatile("dmb oshld" ::: "memory");
        bool stable=true;for(unsigned i=0;i<7;++i)if(before[i]!=after[i])stable=false;
        kprintf("QcomGPU: cpu-inherited-clock cluster=%u ticks=%llu stable=%u rcgr-cmd=%x rcgr-cfg=%x pll-mode=%x pll-l=%x pll-word8=%x pll-user=%x pll-status=%x decoded-hz=0 measured=0\n",
            cluster,now,stable,before[0],before[1],before[2],before[3],before[4],before[5],before[6]);
    }
    __asm__ volatile("dmb oshld" ::: "memory");
    kprintf("QcomGPU: clock-observation sample=%u ticks=%llu gpu-status=%x gpu-config-hz=%llu cpu-lut-valid=%u pwr-desired-hz=%u perf-desired-hz=%u pwr-enable=%x pwr-index=%u pwr-counter-control=%x pwr-counter=%x perf-enable=%x perf-index=%u perf-counter-control=%x perf-counter=%x irq=%llu compositor-completed=%llu\n",
        ++fPerformanceClockSamples,now,unsigned(status),gpuHz,fPerformanceCPUClocks!=nullptr,
        fPerformanceCPUClocks?fPerformanceCPUClocks->rate(0):0,fPerformanceCPUClocks?fPerformanceCPUClocks->rate(1):0,
        raw[0][0],raw[0][1],raw[0][2],raw[0][3],raw[1][0],raw[1][1],raw[1][2],raw[1][3],fIRQCallbacks,fCompositorCompleted);
    // Raw CP ticks are a separate clock domain from GPUStatistics CPU ticks.
    // GPUCC RBBMTIMER is pinned to XO19.2MHz by the retained power contract.
    const auto hi=fRegisters.read(0x4d3*4),lo=fRegisters.read(0x4d2*4),hiAfter=fRegisters.read(0x4d3*4);
    kprintf("QcomGPU: execution-observation ticks=%llu alwayson=%llu stable=%u counter-hz=19200000 native-samples=%llu native-total=%llu compositor-samples=%llu compositor-total=%llu compositor-max=%llu invalid=%llu\n",
        mach_absolute_time(),lo|(uint64_t(hiAfter)<<32),hi==hiAfter,fExecutionNative.samples,fExecutionNative.totalTicks,
        fExecutionCompositor.samples,fExecutionCompositor.totalTicks,fExecutionCompositor.maxTicks,fExecutionInvalid);
    const auto &core=fDiagnostics.compositorCore;
    kprintf("QcomGPU: core-clock-observation scope=compositor samples=%llu alwayson-ticks=%llu core-cycles=%llu min-hz=%llu max-hz=%llu invalid=%llu counter-writes=0\n",
        core.samples,core.alwaysonTicks,core.cycles,core.minHz,core.maxHz,core.invalid);
    const auto &busy=fDiagnostics.compositorBusy;
    kprintf("QcomGPU: rbbm-busy-observation scope=compositor samples=%llu alwayson-ticks=%llu busy-cycles=%llu invalid=%llu selector=6 counter-writes=0\n",
        busy.samples,busy.alwaysonTicks,busy.cycles,busy.invalid);
    if(fPerformanceGPUCCMap && fPerformanceGPUCCMap->getLength()==0x9000) {
        const uint32_t offsets[]={0,4,0xc,0x38,0x1070,0x1074,0x1098,0x10a0,0x10a4};uint32_t first[9]{},again[9]{};
        const auto base=fPerformanceGPUCCMap->getVirtualAddress();
        for(unsigned pass=0;pass<2;++pass)for(unsigned i=0;i<9;++i) {
            auto &v=pass?again[i]:first[i];v=*reinterpret_cast<volatile uint32_t *>(base+offsets[i]);
            __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld" : "+r"(v) :: "memory");
        }
        kprintf("QcomGPU: gpucc-clock-tuple stable=%u pll-mode=%x pll-l=%x pll-user=%x pll-alpha=%x rcgr-cmd=%x rcgr-cfg=%x gfx-gate=%x crc-sid=%x crc-mnd=%x throttle=%x throttle-force=%x sp-clock=%x writes=0\n",
            !memcmp(first,again,sizeof(first)),again[0],again[1],again[2],again[3],again[4],again[5],again[6],again[7],again[8],
            fRegisters.read(0xa8a3*4),fRegisters.read(0xa8a8*4),fRegisters.read(0xa880*4));
    }
    kprintf("QcomGPU: render-work-observation ticks=%llu native-copy-bytes=%llu native-primitives=%llu compositor-copy-bytes=%llu compositor-primitives=%llu native-uncropped-bytes=%llu compositor-uncropped-bytes=%llu\n",
        mach_absolute_time(),fNativeFramebufferBytes,fNativeFramebufferPrimitives,fCompositorFramebufferBytes,fCompositorFramebufferPrimitives,
        fNativeFramebufferUncroppedBytes,fCompositorFramebufferUncroppedBytes);
}
IOReturn QcomGPU::recordMetalTiming(const GPU::MetalTiming::Record &r,uint32_t pid,bool compositor) {
    if(r.version!=GPU::MetalTiming::Version || r.stage<GPU::MetalTiming::Library || r.stage>GPU::MetalTiming::PresentOptions || r.end<r.begin)return kIOReturnBadArgument;
    if(!fMetalTiming) {
        fMetalTiming=OSData::withCapacity(sizeof(GPU::MetalTiming::History));
        if(!fMetalTiming || !fMetalTiming->appendBytes(nullptr,sizeof(GPU::MetalTiming::History)))return kIOReturnNoMemory;
        auto &h=*static_cast<GPU::MetalTiming::History *>(const_cast<void *>(fMetalTiming->getBytesNoCopy()));
        h.version=GPU::MetalTiming::Version;h.bytes=sizeof(h);h.timebaseTicks=1000000;
    }
    auto &h=*static_cast<GPU::MetalTiming::History *>(const_cast<void *>(fMetalTiming->getBytesNoCopy()));
    h.append(r,mach_absolute_time(),pid,compositor);return kIOReturnSuccess;
}
IOReturn QcomGPU::publishMetalTiming(uint64_t &stamp) {
    if(!fMetalTiming)return kIOReturnNotReady;
    auto &h=*static_cast<GPU::MetalTiming::History *>(const_cast<void *>(fMetalTiming->getBytesNoCopy()));
    h.now=mach_absolute_time();absolutetime_to_nanoseconds(h.timebaseTicks,&h.timebaseNS);stamp=h.now;
    return setProperty("GPUMetalTiming",&h,sizeof(h))?kIOReturnSuccess:kIOReturnNoMemory;
}
IOReturn QcomGPU::copyLiveStatistics(UInt32 scope,GPU::Diagnostics::LiveSnapshot &out)
{
    // Called only by the native query gate, after connection lifecycle checks.
    auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
    if(!mapper || scope>1)return kIOReturnNotReady;
    bzero(&out,sizeof(out));out.version=GPU::Diagnostics::LiveVersion;out.bytes=sizeof(out);
    out.scope=scope;
    out.beginTicks=mach_absolute_time();GPU::Diagnostics::add(fLiveStatisticsSequence);out.sequence=fLiveStatisticsSequence;
    out.timebaseSpanTicks=fDiagnostics.timebaseSpanTicks;
    absolutetime_to_nanoseconds(out.timebaseSpanTicks,&out.timebaseSpanNanoseconds);
    out.poolHits=fIndirectBufferHits;out.poolAllocations=fIndirectBufferAllocations;
    out.poolCached=fIndirectBuffers.count();out.poolCapacity=GPU::MaxJobs;
    out.metrics=scope?fDiagnostics.compositor:fDiagnostics.native;
    out.core=scope?fDiagnostics.compositorCore:fDiagnostics.nativeCore;
    out.busy=scope?fDiagnostics.compositorBusy:fDiagnostics.nativeBusy;
    out.addressSpaces=mapper->addressSpaceStatistics();
    out.native={fNativeSubmitted,fNativeCompleted,fNativeFailed,fNativeShaderOperations,
        fNativeFramebufferBytes,fNativeFramebufferInplacePrimitives,fNativeFramebufferScratchBytes,fNativeFramebufferScratchAvoidedBytes,
        fNativeIndirectBuffers,fNativeIndirectBytes,fNativeIndirectWords};
    out.compositor={fCompositorSubmitted,fCompositorCompleted,fCompositorFailed,fCompositorShaderOperations,
        fCompositorFramebufferBytes,fCompositorFramebufferInplacePrimitives,fCompositorFramebufferScratchBytes,fCompositorFramebufferScratchAvoidedBytes,
        fCompositorIndirectBuffers,fCompositorIndirectBytes,fCompositorIndirectWords};
    fHardwarePhases.sequence=out.sequence;fHardwarePhases.enabled=fExecutionProfile;
    setProperty(GPU::HardwarePhases::Property,&fHardwarePhases,sizeof(fHardwarePhases));
    auto mappedWork=mapper->mappingWorkStatistics();mappedWork.sequence=out.sequence;
    setProperty(GPU::MappingBounds::Property,&mappedWork,sizeof(mappedWork));
    fResourceActivity.sequence=out.sequence;fResourceActivity.timebaseNs=out.timebaseSpanNanoseconds;
    setProperty(GPU::JobActivity::ResourceProperty,&fResourceActivity,sizeof(fResourceActivity));
    fShaderObjectActivity.sequence=out.sequence;
    setProperty(GPU::JobActivity::ShaderObjectProperty,&fShaderObjectActivity,sizeof(fShaderObjectActivity));
    fRasterStateActivity.sequence=out.sequence;
    setProperty(GPU::JobActivity::RasterStateProperty,&fRasterStateActivity,sizeof(fRasterStateActivity));
    if(fOperationActivity) {
        auto &profile=*static_cast<GPU::OperationActivity::Snapshot *>(const_cast<void *>(fOperationActivity->getBytesNoCopy()));
        profile.sequence=out.sequence;setProperty(GPU::OperationActivity::Property,&profile,sizeof(profile));
    }
    if(fShaderCounterProfile)setProperty("GPUShaderCounters",&fShaderCounters,sizeof(fShaderCounters));
    if(fJobHistory) {
        auto &history=*static_cast<GPU::JobActivity::History *>(const_cast<void *>(fJobHistory->getBytesNoCopy()));
        history.sequence=out.sequence;history.timebaseNs=out.timebaseSpanNanoseconds;
        // Registry readers run outside this gate. Publish an immutable copy,
        // never expose the live ring's OSData to concurrent readers.
        setProperty(GPU::JobActivity::HistoryProperty,&history,sizeof(history));
    }
    out.endTicks=mach_absolute_time();return kIOReturnSuccess;
}
void QcomGPU::publishStatistics()
{
    GPU::Native::Memory::Snapshot memory;
    memory.limits=nativeMemoryLimits();memory.native=fNativeMemoryUsage;
    memory.flags=fNativeAccountingFault?1:0;
    if(auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper))memory.mappings=mapper->memoryUsage();
    setProperty("IOGPUNativeMemoryLimits",&memory.limits,sizeof(memory.limits));
    setProperty("GPUMemoryStatistics",&memory,sizeof(memory));
    setProperty("GPUNativeJobsSubmitted",fNativeSubmitted,64);
    setProperty("GPUNativeJobsCompleted",fNativeCompleted,64);
    setProperty("GPUNativeTicketsFailed",fNativeFailed,64);
    setProperty("GPUCompositorJobsSubmitted",fCompositorSubmitted,64);
    setProperty("GPUCompositorJobsCompleted",fCompositorCompleted,64);
    setProperty("GPUCompositorTicketsFailed",fCompositorFailed,64);
    setProperty("GPUCompletedResponsibleTaskIDs",&fLastResponsibleTasks,sizeof(fLastResponsibleTasks));
    setProperty("GPUIRQCallbacks",fIRQCallbacks,64);
    setProperty("GPUNativeSurfaceFencesCreated",fSurfaceFencesCreated,64);
    setProperty("GPUNativeSurfaceFencesDeferred",fSurfaceFencesDeferred,64);
    setProperty("GPUNativeSurfaceFencesCompleted",fSurfaceFencesCompleted,64);
    setProperty("GPUNativeSurfaceFencesFailed",fSurfaceFencesFailed,64);
    setProperty("GPUNativeShaderOperations",fNativeShaderOperations,64);
    setProperty("GPUCompositorShaderOperations",fCompositorShaderOperations,64);
    setProperty("GPUNativeFramebufferCopyBytes",fNativeFramebufferBytes,64);
    setProperty("GPUNativeFramebuffer2DBytes",fNativeFramebuffer2DBytes,64);
    setProperty("GPUCompositorFramebuffer2DBytes",fCompositorFramebuffer2DBytes,64);
    setProperty("GPUCompositorFramebufferCopyBytes",fCompositorFramebufferBytes,64);
    setProperty("GPUNativeFramebufferUncroppedBytes",fNativeFramebufferUncroppedBytes,64);
    setProperty("GPUCompositorFramebufferUncroppedBytes",fCompositorFramebufferUncroppedBytes,64);
    setProperty("GPUNativeFramebufferPrimitives",fNativeFramebufferPrimitives,64);
    setProperty("GPUCompositorFramebufferPrimitives",fCompositorFramebufferPrimitives,64);
    setProperty("GPUNativeFramebufferInplacePrimitives",fNativeFramebufferInplacePrimitives,64);
    setProperty("GPUCompositorFramebufferInplacePrimitives",fCompositorFramebufferInplacePrimitives,64);
    setProperty("GPUNativeQueuedAdmissionJobs",fNativeQueuedAdmissionJobs,64);
    setProperty("GPUCompositorQueuedAdmissionJobs",fCompositorQueuedAdmissionJobs,64);
    setProperty("GPUNativeAdmissionMaxDepth",fNativeAdmissionMaxDepth,32);
    setProperty("GPUCompositorAdmissionMaxDepth",fCompositorAdmissionMaxDepth,32);
    setProperty("GPUCPUReadDeferrals",fCPUReadDeferrals,64);
    setProperty("GPUPipelineCapacityDeferrals",fPipelineCapacityDeferrals,64);
    setProperty("GPUNativeQueuedRingJobs",fNativeQueuedRingJobs,64);
    setProperty("GPUCompositorQueuedRingJobs",fCompositorQueuedRingJobs,64);
    setProperty("GPUNativeRingMaxDepth",fNativeRingMaxDepth,32);
    setProperty("GPUCompositorRingMaxDepth",fCompositorRingMaxDepth,32);
    setProperty("GPUNativeFramebufferScratchBytes",fNativeFramebufferScratchBytes,64);
    setProperty("GPUCompositorFramebufferScratchBytes",fCompositorFramebufferScratchBytes,64);
    setProperty("GPUNativeFramebufferScratchAvoidedBytes",fNativeFramebufferScratchAvoidedBytes,64);
    setProperty("GPUCompositorFramebufferScratchAvoidedBytes",fCompositorFramebufferScratchAvoidedBytes,64);
    setProperty("GPUNativeAdmissionDeferred",fNativeBackpressure,64);
    fDiagnostics.bytes=sizeof(fDiagnostics);
    absolutetime_to_nanoseconds(fDiagnostics.timebaseSpanTicks,&fDiagnostics.timebaseSpanNanoseconds);
    fDiagnostics.poolHits=fIndirectBufferHits;fDiagnostics.poolAllocations=fIndirectBufferAllocations;
    fDiagnostics.poolCached=fIndirectBuffers.count();
    setProperty("GPUStatistics",&fDiagnostics,sizeof(fDiagnostics));
    if(auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper)) {
        auto statistics=mapper->addressSpaceStatistics();
        setProperty("GPUAddressSpaceStatistics",&statistics,sizeof(statistics));
    }
    setProperty("GPUScratchCacheHits",fScratchHits,64);
    setProperty("GPUScratchCacheAllocations",fScratchAllocations,64);
    setProperty("GPUScratchCacheEvictions",fScratchEvictions,64);
    setProperty("GPUScratchCacheBytes",fScratchBuffers.bytes(),64);
    observePerformanceClocks();
}
void QcomGPU::sampleNative(GPU::Diagnostics::Stage stage,UInt64 begin,UInt64 end,bool compositor)
{
    fDiagnostics.native.timing[stage].sample(begin,end);
    if(compositor)fDiagnostics.compositor.timing[stage].sample(begin,end);
}
IOReturn QcomGPU::bootstrapAction(OSObject *object,void *,void *,void *,void *)
{
    auto *gpu=static_cast<QcomGPU *>(object);
    if(!gpu->fStarted || !gpu->fBootstrapPending)return kIOReturnNotReady;
    if(!gpu->fAdmissionOn){gpu->fTimer->setTimeoutMS(100);return kIOReturnNotReady;}
    // Admission remains closed between attempts. The Family gate excludes
    // clients and SMMU fault callbacks while the warm handoff is adopted.
    gpu->fState=GPU::FrameworkReady;
    auto status=gpu->configureHardware(gpu->getProvider());
    gpu->setProperty("GPUStartupResult",uint64_t(uint32_t(status)),32);
    if(status==kIOReturnSuccess) {
        gpu->fBootstrapPending=false;
        if(gpu->fInterrupt)gpu->fInterrupt->enable();
        gpu->fTimer->setTimeoutMS(100);
        if(gpu->publishHardwareState())return status;
        gpu->faultHardware();return kIOReturnNoMemory;
    }
    // Retry only before GPU/SMMU MMIO or DMA has begun. A timeout, bad
    // handoff, changed retained token or unknown ACK never restarts hardware.
    if(!gpu->fRegisterMap && !gpu->fQuarantined && QcomGPUPlatform::retryable(status) &&
        mach_absolute_time()<gpu->fBootstrapDeadline) {
        gpu->fState=GPU::Suspended;gpu->fTimer->setTimeoutMS(100);gpu->publishHardwareState();return kIOReturnNotReady;
    }
    if(QcomGPUPlatform::retryable(status) && mach_absolute_time()>=gpu->fBootstrapDeadline)status=kIOReturnTimeout;
    gpu->setProperty("GPUStartupResult",uint64_t(uint32_t(status)),32);
    gpu->fBootstrapPending=false;gpu->faultHardware();gpu->publishHardwareState();
    if(!gpu->fRegisterMap && gpu->fPlatform) {
        if(gpu->fPlatform->release()) {
            delete gpu->fPlatform;gpu->fPlatform=nullptr;
            gpu->setProperty("GPUPlatformLease",false);
            if(gpu->fProviderOpened){gpu->getProvider()->close(gpu);gpu->fProviderOpened=false;}
        } else gpu->quarantineHardware();
    }
    IOLog("QcomGPU: resource bootstrap failed (%x); hardware admission closed\n",status);
    UInt32 failfast=0;
    if(PE_parse_boot_argn("gpu-failfast",&failfast,sizeof(failfast)) && failfast)
        panic("QcomGPU: requested bring-up stop after bootstrap failure (%x)",status);
    return status;
}
IOReturn QcomGPU::configureHardware(IOService *provider)
{
    if(!fJobHistory) {
        fJobHistory=OSData::withCapacity(sizeof(GPU::JobActivity::History));
        if(!fJobHistory || !fJobHistory->appendBytes(nullptr,sizeof(GPU::JobActivity::History)))return kIOReturnNoMemory;
        auto &history=*static_cast<GPU::JobActivity::History *>(const_cast<void *>(fJobHistory->getBytesNoCopy()));
        history.version=GPU::JobActivity::Version;history.bytes=sizeof(history);
        history.capacity=GPU::JobActivity::Capacity;history.timebaseTicks=1000000;
    }
    UInt32 shaderCounters=0;PE_parse_boot_argn("gpu-oc",&shaderCounters,sizeof(shaderCounters));
    fShaderCounterProfile=shaderCounters<=GPU::MaxCommands?shaderCounters:0;
    UInt32 operationTiming=fShaderCounterProfile?1:0;PE_parse_boot_argn("gpu-op-timing",&operationTiming,sizeof(operationTiming));
    if(operationTiming && !fOperationActivity) {
        fOperationActivity=OSData::withCapacity(sizeof(GPU::OperationActivity::Snapshot));
        if(fOperationActivity && fOperationActivity->appendBytes(nullptr,sizeof(GPU::OperationActivity::Snapshot))) {
            auto &profile=*static_cast<GPU::OperationActivity::Snapshot *>(const_cast<void *>(fOperationActivity->getBytesNoCopy()));
            profile.version=GPU::OperationActivity::Version;profile.bytes=sizeof(profile);profile.enabled=1;
            fExecutionProfile=true;
        } else OSSafeReleaseNULL(fOperationActivity); // Diagnostic allocation failure must not disable the GPU.
    }
    UInt32 metalDiagnostics=0;
    PE_parse_boot_argn("gpu-metal-diagnostics",&metalDiagnostics,sizeof(metalDiagnostics));
    PE_parse_boot_argn("gpu-md",&metalDiagnostics,sizeof(metalDiagnostics)); // Compact alias for combined long-frame diagnostics.
    setProperty("GPUMetalTimingVersion",metalDiagnostics?GPU::MetalTiming::Version:0,32);
    setProperty("GPUMetalDiagnosticVersion",metalDiagnostics?GPU::Diagnostics::MetalFailureVersion:0,32);
    UInt32 shaderCapture=0;
    PE_parse_boot_argn("gpu-metal-shader-capture",&shaderCapture,sizeof(shaderCapture));
    setProperty("GPUMetalShaderCaptureVersion",metalDiagnostics && shaderCapture==1?GPU::Diagnostics::MetalShaderCaptureVersion:0,32);
    UInt32 trace=0;PE_parse_boot_argn("gpu-submit-trace",&trace,sizeof(trace));
    fSubmitTraceEnabled=trace!=0;
    fSubmitTraceLimit=trace>3?3:trace;
    UInt32 direct=0;PE_parse_boot_argn("gpu-shader-direct",&direct,sizeof(direct));
    fDirectShaderUploads=direct!=0;
    UInt32 snapshot2D=0;PE_parse_boot_argn("gpu-fb2d",&snapshot2D,sizeof(snapshot2D));
    fFramebuffer2DMode=snapshot2D<=2?snapshot2D:0;
    UInt32 snapshotXCrop=1;PE_parse_boot_argn("gpu-fb-xcrop",&snapshotXCrop,sizeof(snapshotXCrop));
    fFramebufferXCrop=snapshotXCrop!=0;setProperty("GPUFramebufferXCrop",fFramebufferXCrop);
    UInt32 snapshotWord32=0;PE_parse_boot_argn("gpu-fb2d-word32",&snapshotWord32,sizeof(snapshotWord32));
    fFramebufferWord32=snapshotWord32!=0;setProperty("GPUFramebufferWord32",fFramebufferWord32);
    setProperty("GPUFramebuffer2DMode",fFramebuffer2DMode,32);
    kprintf("QcomGPU: framebuffer-2d mode=%u (0=compute 1=noncompositor 2=all)\n",fFramebuffer2DMode);
    kprintf("QcomGPU: framebuffer-xcrop enabled=%u\n",fFramebufferXCrop);
    kprintf("QcomGPU: framebuffer-word32 enabled=%u\n",fFramebufferWord32);
    UInt32 fetchMode=0;PE_parse_boot_argn("gpu-fb-fetch",&fetchMode,sizeof(fetchMode));
    PE_parse_boot_argn("gpu-fbf",&fetchMode,sizeof(fetchMode)); // Short alias fits the native NVRAM command-line limit.
    fFramebufferFetchMode=fetchMode<=3?fetchMode:0;setProperty("GPUFramebufferFetchMode",fFramebufferFetchMode,32);
    setProperty("GPUFramebufferCompactScratch",nativeFramebufferCompact());
    kprintf("QcomGPU: framebuffer-fetch mode=%u (0=snapshot 1=privileged-noncompositor 2=all 3=all-fullscratch-control) experimental=1\n",fFramebufferFetchMode);
    UInt32 pipe=0;PE_parse_boot_argn("gpu-pipe",&pipe,sizeof(pipe));
    fPipelinedAdmission=pipe>=1 && pipe<=6;fRingPipeline=pipe>=2 && pipe<=6;
    fCompactIndirectBuffers=pipe>=4 && pipe<=6;setProperty("GPUCompactIndirectBuffers",fCompactIndirectBuffers);
    fCompletionNotifications=pipe==5 || pipe==6;fNativeSurfaceFences=pipe==6;
    setProperty("GPUPipelinedAdmission",fPipelinedAdmission);setProperty("GPURingPipeline",fRingPipeline);
    kprintf("QcomGPU: pipelined-admission enabled=%u producer-alias-check=1\n",fPipelinedAdmission);
    UInt32 secondaryCache=1;PE_parse_boot_argn("gpu-ib-cache",&secondaryCache,sizeof(secondaryCache));
    fSecondaryIBCache=secondaryCache!=0;setProperty("GPUSecondaryIBCache",fSecondaryIBCache);
    UInt32 indexCache=1;PE_parse_boot_argn("gpu-index-cache",&indexCache,sizeof(indexCache));
    fIndexCache=indexCache!=0;setProperty("GPUIndexSnapshotCache",fIndexCache);
    UInt32 stagedChecks=1;PE_parse_boot_argn("gpu-admit-checks",&stagedChecks,sizeof(stagedChecks));
    fStagedAdmissionChecks=stagedChecks!=0;setProperty("GPUStagedAdmissionChecks",fStagedAdmissionChecks);
    kprintf("QcomGPU: staged-admission-checks=%u actual-poll/as-publication/retirement-checks=retained\n",fStagedAdmissionChecks);
    UInt32 resume=0;PE_parse_boot_argn("gpu-domain-resume",&resume,sizeof(resume));
    fDomainResumeEnabled=resume!=0;
    auto *property = provider->getProperty("qcom,gpu-handoff");
    if (!property) return kIOReturnSuccess; // Native queries and host diagnostics remain available.
    auto *data = OSDynamicCast(OSData, property);
    Adreno::PlatformContract contract{};
    if (!data || data->getLength() != sizeof(contract)) return kIOReturnIOError;
    // Apple flattened DT stores fixed-width little-endian cells on arm64.
    memcpy(&contract, data->getBytesNoCopy(), sizeof(contract));
    if(!contract.valid())return kIOReturnBadArgument;
    if(!fProviderOpened) {if(!provider->open(this))return kIOReturnExclusiveAccess;fProviderOpened=true;}
    if(!fPlatform)fPlatform=new QcomGPUPlatform(provider);
    if(!fPlatform)return kIOReturnNoMemory;
    const auto acquired=fPlatform->acquire();if(acquired!=kIOReturnSuccess)return acquired;
    kprintf("QcomGPU: retained power/clock/regulator leases acquired\n");
    setProperty("GPUPlatformLease",true);
    const auto resources=provider->getResources();if(resources!=kIOReturnSuccess)return resources;
    fRegisterMap = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    if (!fRegisterMap || fRegisterMap->getLength() != Adreno::RegisterBytes) return kIOReturnIOError;
    fRegisters.base = reinterpret_cast<volatile uint8_t *>(fRegisterMap->getVirtualAddress());
    // Local BIMC QoS and bandwidth changes require the same actual CP/AXI
    // isolation proof as SMMU adoption, including the external mapper path.
    if(!powerReady() || (fRegisters.read(Adreno::MEControl)&Adreno::Halt)!=Adreno::Halt ||
        fRegisters.read(Adreno::TrustControl))return kIOReturnIOError;
    fRegisters.write(Adreno::VBIFHalt,15);
    unsigned waited=0;while((fRegisters.read(Adreno::VBIFHalted)&15)!=15 && waited<100000){IODelay(10);waited+=10;}
    if((fRegisters.read(Adreno::VBIFHalted)&15)!=15 ||
        !Adreno::haltedPipelineIdle(fRegisters.read(Adreno::Status)))return kIOReturnIOError;
    kprintf("QcomGPU: stopped CP and VBIF drain verified\n");
    fNoC=QcomGPUNoC::create(this,provider,fRegisterMap);if(!fNoC)return kIOReturnBadArgument;
    const auto interconnect=fNoC->acquire();if(interconnect!=kIOReturnSuccess)return interconnect;
    setProperty("GPUNoCReady",true);
    kprintf("QcomGPU: interconnect retained\n");
    // Restore the loader-authenticated ZAP through the real Qualcomm PAS-13
    // service. The GPU stays halted and the BIMC/RPM/clock leases stay held.
    fSecure=QcomGPUSecure::create(this);if(!fSecure)return kIOReturnNotReady;
    const auto secure=fSecure->resume();
    if(secure!=kIOReturnSuccess) {
        if(fSecure->quarantined())quarantineHardware();
        return secure;
    }
    if(!powerReady() || (fRegisters.read(Adreno::MEControl)&Adreno::Halt)!=Adreno::Halt ||
        (fRegisters.read(Adreno::VBIFHalted)&15)!=15 || !Adreno::haltedPipelineIdle(fRegisters.read(Adreno::Status)) ||
        fRegisters.read(Adreno::TrustControl)>1) {quarantineHardware();return kIOReturnIOError;}
    setProperty("GPUZAPReady",true);
    kprintf("QcomGPU: authenticated ZAP resumed through SCM\n");
    // Resolve an actual object without IOMapper's unbounded identifier wait.
    auto *object=provider->getProperty("iommu-parent");
    if (object) {
        if (provider->getProperty("qcom,adreno-smmu-handoff")) return kIOReturnIOError;
        fMapper=OSDynamicCast(IOMapper,object);if(fMapper)fMapper->retain();
    } else if (provider->getProperty("qcom,adreno-smmu-handoff")) {
        fMapper=QcomGPUMapper::create(this,provider,fRegisterMap);
    }
    if (!fMapper) {IOLog("QcomGPU: hardware submission requires a resolved device IOMapper or retained SMMU handoff\n");return kIOReturnIOError;}
    kprintf("QcomGPU: native IOMapper created\n");
    if(auto *handoff=provider->getProperty("qcom,gpu-firmware-handoff")) {
        auto *firmware=OSDynamicCast(OSData,handoff);auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
        if(!firmware || firmware->getLength()!=sizeof(fFirmwareHandoff) || !mapper)return kIOReturnBadArgument;
        memcpy(&fFirmwareHandoff,firmware->getBytesNoCopy(),sizeof(fFirmwareHandoff));
        if(!fFirmwareHandoff.valid(contract) || !mapper->retainFirmware(fFirmwareHandoff))return kIOReturnIOError;
        fFirmwareContract=contract;fFirmwareRetained=true;
        kprintf("QcomGPU: authenticated firmware backing retained\n");
    }
    fRing = QcomGPUBuffer::allocate(Adreno::RingDwords * 4, fMapper);
    fFence = QcomGPUBuffer::allocate(GPU::PageBytes, fMapper);
    if (!fRing || !fFence) return kIOReturnIOError;
    UInt32 snapshot=0;PE_parse_boot_argn("gpu-fault-snapshot",&snapshot,sizeof(snapshot));
    if(snapshot) {
        // Allocate while the mapper is healthy. This kernel-only backing is
        // never added to a client's private address space or freed on a fault.
        fFaultSnapshot=QcomGPUBuffer::allocate(2*GPU::PageBytes,fMapper);
        if(!fFaultSnapshot)return kIOReturnNoMemory;
    }
    kprintf("QcomGPU: kernel ring=%llx fence=%llx allocated\n",fRing->address,fFence->address);
    fInterrupt = IOInterruptEventSource::interruptEventSource(this, interruptAction, provider, 0);
    if (!fInterrupt || fLoop->addEventSource(fInterrupt) != kIOReturnSuccess) return kIOReturnIOError;
    fInterruptAdded = true;
    nanoseconds_to_absolutetime(500000000ULL, &fTimeoutTicks);
    // Startup polling and SMMU fault actions share the Family gate. A fault
    // cannot race the transition from stopped firmware to the new CP ring.
    if(fGate->runAction(startEngineAction,&contract)!=kIOReturnSuccess)return kIOReturnIOError;
    return kIOReturnSuccess;
}
IOReturn QcomGPU::verifyAddressSpaces()
{
    fPrivateAddressSpacesVerified=false;
    setProperty("GPUAddressSpaceSelfTestResult",UInt64(UInt32(kIOReturnNotReady)),32);
    auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
    if(!mapper || !mapper->privateAddressSpacesReady())return kIOReturnSuccess; // kernel diagnostics only
    if(fJobCount || !fEngine.ready() || fQuarantined)return kIOReturnNotReady;
    if(fRegisters.read(0x831*4))return kIOReturnIOError; // known CP_CNTL/APRIV baseline before the first switch
    auto &job=fJobs[fJobHead];
    job.ib=acquireIndirectBuffer();
    auto *probe=QcomGPUBuffer::allocate(GPU::PageBytes,fMapper);
    if(probe){job.buffers[job.bufferCount++]=probe;probe->beginGPUUse();}
    if(!job.ib || !probe){finishJob(job,false);return kIOReturnNoMemory;}
    Adreno::Encoder encoder(static_cast<UInt32 *>(job.ib->bytes()),Adreno::IBDwords);
    constexpr UInt32 pattern=0x41535054; // private address-space write, never CPU completion
    if(encoder.fill(probe->address,4,pattern)!=GPU::Result::Success || encoder.barrier()!=GPU::Result::Success) {
        finishJob(job,false);return kIOReturnNoResources;
    }
    job.addressSpace=mapper->createAddressSpace(fRing,job.ib,nullptr,nullptr,job.buffers,job.bufferCount);
    if(!job.addressSpace){finishJob(job,false);return kIOReturnNoResources;}
    const auto context=job.addressSpace->context();
    auto status=fEngine.post(job.ib->address,encoder.size(),job.sequence,&context);
    if(status!=GPU::Result::Success){finishJob(job,false);return kIOReturnIOError;}
    ++fJobCount;job.addressSpace->markSubmitted();job.submitted=mach_absolute_time();
    // Uses the same production FIFO/retirement graph as user jobs. A failed
    // switch never frees a private root or backing before halt/AXI drain.
    for(unsigned waited=0;waited<100000;waited+=10) {
        if(!platformReady()){faultHardware();return kIOReturnIOError;}
        UInt32 completed=0;
        if(fEngine.poll(completed)!=GPU::Result::Success){faultHardware();return kIOReturnIOError;}
        if(completed>=job.sequence) {
            if(!job.addressSpace->retire()){faultHardware();return kIOReturnIOError;}
            const bool correct=__atomic_load_n(static_cast<UInt32 *>(probe->bytes()),__ATOMIC_ACQUIRE)==pattern;
            const unsigned index=fJobHead;fJobHead=(fJobHead+1)%GPU::MaxJobs;--fJobCount;
            const bool clean=finishJob(fJobs[index],correct);
            fPrivateAddressSpacesVerified=correct && clean;
            setProperty("GPUAddressSpaceSelfTestResult",UInt64(UInt32(fPrivateAddressSpacesVerified?kIOReturnSuccess:kIOReturnIOError)),32);
            return fPrivateAddressSpacesVerified?kIOReturnSuccess:kIOReturnIOError;
        }
        IODelay(10);
    }
    faultHardware();return kIOReturnTimeout;
}
QcomGPUBuffer *QcomGPU::acquireIndirectBuffer()
{
    if(auto *buffer=fIndirectBuffers.pop()) {
        GPU::Diagnostics::add(fIndirectBufferHits);
        // No outstanding job/private page table can still reference a pooled
        // buffer. Clear old code/data before building the next immutable IB.
        bzero(buffer->bytes(),Adreno::IBDwords*4);return buffer;
    }
    auto *buffer=QcomGPUBuffer::allocate(Adreno::IBDwords*4,fMapper);
    if(buffer)GPU::Diagnostics::add(fIndirectBufferAllocations);
    return buffer;
}
void QcomGPU::recycleIndirectBuffer(QcomGPUBuffer *&buffer)
{
    if(!buffer)return;
    if(buffer->length==Adreno::IBDwords*4 && !fQuarantined && fIndirectBuffers.push(buffer)) {
        buffer=nullptr;return;
    }
    OSSafeReleaseNULL(buffer);
}
QcomGPUBuffer *QcomGPU::acquireScratch(uint64_t bytes)
{
    auto *buffer=fScratchBuffers.take(bytes);
    const auto begin=mach_absolute_time();
    if(buffer) {
        GPU::Diagnostics::add(fScratchHits);
        // Write-combined kernel mapping, no client alias. Preserve full
        // initialization, including progress/timestamp cells and padding.
        bzero(buffer->bytes(),buffer->length);
        fScratchClearTime.sample(begin,mach_absolute_time());
    } else {
        buffer=QcomGPUBuffer::allocate(bytes,fMapper);
        fScratchAllocateTime.sample(begin,mach_absolute_time());
        if(buffer)GPU::Diagnostics::add(fScratchAllocations);
    }
    const auto requests=fScratchHits+fScratchAllocations;
    if(requests<=8 || !(requests%256))
        kprintf("QcomGPU: scratch-cache requests=%llu hits=%llu allocations=%llu evictions=%llu cached-bytes=%llu clear-samples=%llu clear-ticks=%llu allocate-samples=%llu allocate-ticks=%llu\n",
            requests,fScratchHits,fScratchAllocations,fScratchEvictions,fScratchBuffers.bytes(),
            fScratchClearTime.samples,fScratchClearTime.totalTicks,fScratchAllocateTime.samples,fScratchAllocateTime.totalTicks);
    return buffer;
}
void QcomGPU::recycleScratch(QcomGPUBuffer *&buffer,bool clean)
{
    if(!buffer)return;
    if(clean && !fQuarantined && buffer->length<=ScratchCacheBytes) {
        while(true) {
            if(fScratchBuffers.push(buffer)){buffer=nullptr;return;}
            auto *old=fScratchBuffers.pop();
            if(!old)break;
            GPU::Diagnostics::add(fScratchEvictions);old->release();
        }
    }
    OSSafeReleaseNULL(buffer);
}
QcomGPUBuffer *QcomGPU::acquireIndexSnapshot(uint64_t bytes)
{
    if(!bytes || bytes>IndexSnapshotLimit-fIndexSnapshotBytes)return nullptr;
    auto *buffer=fIndexCache?fIndexBuffers.take(bytes):nullptr;
    if(buffer) {
        // Retired kernel-owned storage, no client alias. Clear padding too,
        // then the native encoder resnapshots and validates every index.
        bzero(buffer->bytes(),buffer->length);GPU::Diagnostics::add(fIndexHits);
    } else {
        // Cached pages share the original16MiB budget with all live jobs.
        // Evict before allocating, so peak retained memory never doubles.
        while(fIndexBuffers.bytes()>IndexSnapshotLimit-fIndexSnapshotBytes-bytes) {
            auto *old=fIndexBuffers.pop();if(!old)return nullptr;
            old->release();GPU::Diagnostics::add(fIndexEvictions);
        }
        buffer=QcomGPUBuffer::allocate(bytes,fMapper);
        if(buffer)GPU::Diagnostics::add(fIndexAllocations);
    }
    const auto requests=fIndexHits+fIndexAllocations;
    if(requests<=8 || !(requests%256))
        kprintf("QcomGPU: index-cache enabled=%u requests=%llu hits=%llu allocations=%llu evictions=%llu cached-bytes=%llu live-bytes=%llu\n",
            fIndexCache,requests,fIndexHits,fIndexAllocations,fIndexEvictions,fIndexBuffers.bytes(),fIndexSnapshotBytes);
    return buffer;
}
void QcomGPU::recycleIndexSnapshot(QcomGPUBuffer *&buffer,bool clean)
{
    if(!buffer)return;
    if(fIndexCache && clean && !fQuarantined && buffer->length<=IndexSnapshotLimit-fIndexSnapshotBytes) {
        while(true) {
            if(fIndexBuffers.bytes()<=IndexSnapshotLimit-fIndexSnapshotBytes-buffer->length &&
                fIndexBuffers.push(buffer)){buffer=nullptr;return;}
            auto *old=fIndexBuffers.pop();if(!old)break;
            old->release();GPU::Diagnostics::add(fIndexEvictions);
        }
    }
    OSSafeReleaseNULL(buffer);
}
bool QcomGPU::finishJob(Job &job, bool success)
{
    const auto retireBegin=job.retireStarted?job.retireStarted:mach_absolute_time();
    const auto addressBegin=mach_absolute_time();
    if (job.addressSpace && !job.addressSpace->retire(success && job.hardwareDone)) {
        quarantineHardware();return false;
    }
    auto *native = job.native;
    auto *client = job.client;
    const bool issued=job.sequence!=0;
    auto detailCursor=mach_absolute_time();
    // The normal FIFO path retires the AS before entering finishJob. Do not
    // count its idempotent second retire as a second address-space sample.
    if(native && issued && !job.retireStarted)
        sampleNative(GPU::Diagnostics::RetireAddressSpace,addressBegin,detailCursor,native->compositor);
    auto detail=[&](GPU::Diagnostics::Stage stage) {
        const auto now=mach_absolute_time();
        if(native && issued)sampleNative(stage,detailCursor,now,native->compositor);
        detailCursor=now;
    };
    bool coherent=true;
    // Called only after a hardware timestamp or a proved CP/AXI drain. Make
    // GPU writes visible before publishing a successful native fence/token.
    if (issued) for (unsigned i=0;i<job.bufferCount;++i)
        if (!job.buffers[i]->synchronize()) coherent=false;
    // Keep the private sample cell until real completion, ASID retirement
    // and CPU coherence. Failed jobs retain it until a proved stop/restart.
    if(issued && success && coherent && job.execution.sequence && !fEngine.retireExecution(job.execution))coherent=false;
    if (!coherent) {success=false;fRecoveryFailed=true;}
    detail(GPU::Diagnostics::RetireCoherence);
    if (issued && coherent && native) for (UInt32 i=0;i<native->resourceCount;++i)
        if (native->surfaceWrites&GPU::Native::Memory::writeBit(i)) if (auto *resource=OSDynamicCast(QcomGPUResource,native->resources[i]))
            resource->markSurfaceWritten();
    detail(GPU::Diagnostics::RetireSurfacePublish);
    if (client) if (auto *queue = client->fSession.queues.find(job.queue)) {
        if (queue->pending) --queue->pending;
        if (success) queue->completed = job.fence;
        else queue->faulted = true;
    }
    // Private translations and their ASID are retired before any original
    // DMA mapping or descriptor can be destroyed.
    OSSafeReleaseNULL(job.addressSpace);
    for (unsigned i = 0; i < job.bufferCount; ++i) {
        job.buffers[i]->endGPUUse();
        job.buffers[i]->release();
    }
    detail(GPU::Diagnostics::RetireResources);
    const auto indicesBegin=mach_absolute_time();
    if (job.indices) {
        fIndexSnapshotBytes -= job.indices->length;
        // finishJob has already proved private-AS retirement/TLBSYNC and
        // cache completion. Failed or never-issued jobs never enter the pool.
        recycleIndexSnapshot(job.indices,issued && success && coherent);
    }
    if(native && issued)sampleNative(GPU::Diagnostics::RetireIndices,indicesBegin,mach_absolute_time(),native->compositor);
    const auto commandsBegin=mach_absolute_time();
    if(job.framebufferBytes)fFramebufferSnapshotBytes-=job.framebufferBytes;
    if(job.framebufferIBBytes)fFramebufferIBBytes-=job.framebufferIBBytes;
    OSSafeReleaseNULL(job.commandList);
    if(fSecondaryIBCache && success && coherent && !fQuarantined && job.commandBuffers) {
        // A real timestamp and private-AS/ASID retirement precede this point.
        // Transfer each secondary IB's owned reference to the same bounded
        // pool as the primary. The array drops its separate reference below.
        // Cache admission never retains a failed/incoherent job's buffers.
        for(unsigned i=0;i<job.commandBuffers->getCount();++i) {
            auto *buffer=OSDynamicCast(QcomGPUBuffer,job.commandBuffers->getObject(i));
            if(buffer && buffer!=job.ib){buffer->retain();recycleIndirectBuffer(buffer);}
        }
    }
    OSSafeReleaseNULL(job.commandBuffers);
    if(native && issued)sampleNative(GPU::Diagnostics::RetireCommands,commandsBegin,mach_absolute_time(),native->compositor);
    // Only after retirement/TLBSYNC and cache completion. Failure paths keep
    // their original release/quarantine behavior, never populate this cache.
    // Read the private NC timestamp page only after physical completion,
    // ASID retirement and coherence, before its backing can be recycled.
    auto *operationProfile=job.operationProfile;job.operationProfile=nullptr;
    const auto operationJobBegin=job.gpuBegin,operationJobEnd=job.gpuEnd;
    if(operationProfile && issued && success && coherent && job.hardwareDone && job.scratch && job.scratch->bytes()) {
        auto *records=static_cast<GPU::OperationActivity::Record *>(const_cast<void *>(operationProfile->getBytesNoCopy()));
        const auto *cells=reinterpret_cast<const volatile UInt32 *>(static_cast<const UInt8 *>(job.scratch->bytes())+
            GPU::OperationActivity::ScratchOffset);
        __asm__ volatile("dmb oshld" ::: "memory");
        for(unsigned i=0;i<operationProfile->getLength()/sizeof(*records);++i) {
            auto &r=records[i];r.sequence=job.sequence;r.stamp=native?native->stamp:0;r.retired=mach_absolute_time();
            if(r.flags&GPU::OperationActivity::Timed) {
                r.begin=cells[4*i]|(UInt64(cells[4*i+1])<<32);r.end=cells[4*i+2]|(UInt64(cells[4*i+3])<<32);
                if((r.flags&GPU::ShaderCounters::RecordFlag) && r.begin>=operationJobBegin && r.end>=r.begin && r.end<=operationJobEnd) {
                    using namespace GPU::ShaderCounters;
                    const auto *counts=reinterpret_cast<const volatile UInt32 *>(static_cast<const UInt8 *>(job.scratch->bytes())+ScratchOffset);
                    Record record;record.retired=r.retired;record.hash=r.description.second.hash;record.ticks=r.end-r.begin;record.sequence=r.sequence;record.index=r.index;
                    for(unsigned n=0;n<Count;++n) {
                        const UInt64 a=counts[n*2]|(UInt64(counts[n*2+1])<<32),b=counts[(n+Count)*2]|(UInt64(counts[(n+Count)*2+1])<<32);
                        record.values[n]=b-a;
                    }
                    fShaderCounters.append(record);
                }
            }
        }
    }
    UInt64 gmemTicks[3]{};UInt32 gmemSamples=0;
    if(job.gmemSamples && success && coherent && job.hardwareDone && job.scratch && job.scratch->bytes()) {
        const auto *cells=reinterpret_cast<const volatile UInt32 *>(static_cast<const UInt8 *>(job.scratch->bytes())+
            GPU::RenderPass::TimingOffset);
        __asm__ volatile("dmb oshld" ::: "memory");
        for(UInt32 i=0;i<job.gmemSamples;++i) {
            UInt64 t[4];for(unsigned n=0;n<4;++n)t[n]=cells[i*8+n*2]|(UInt64(cells[i*8+n*2+1])<<32);
            if(t[0]<job.gpuBegin || t[3]>job.gpuEnd || t[0]>t[1] || t[1]>t[2] || t[2]>t[3])continue;
            for(unsigned n=0;n<3;++n)gmemTicks[n]+=t[n+1]-t[n];
            ++gmemSamples;
        }
    }
    recycleScratch(job.scratch,success && coherent);
    recycleIndirectBuffer(job.ib);
    detail(GPU::Diagnostics::RetirePools);
    if(native && issued)sampleNative(GPU::Diagnostics::RetireCleanup,retireBegin,mach_absolute_time(),native->compositor);
    const auto copiedBytes=job.copiedFramebufferBytes,primitives=job.framebufferPrimitives,copied2DBytes=job.copiedFramebuffer2DBytes,
        uncroppedBytes=job.uncroppedFramebufferBytes,inplacePrimitives=job.inplaceFramebufferPrimitives;
    const auto admissionDepth=job.admissionDepth;
    const auto ringDepth=job.ringDepth;
    const auto shaderObjectHits=job.shaderObjectHits,shaderObjectSavedBytes=job.shaderObjectSavedBytes;
    const auto rasterFullSetups=job.rasterFullSetups,rasterReusedPrimitives=job.rasterReusedPrimitives,rasterDriverUpdates=job.rasterDriverUpdates;
    const auto scratchBytes=job.framebufferBytes;
    const auto avoidedScratchBytes=job.framebufferFullBytes>=scratchBytes?job.framebufferFullBytes-scratchBytes:0;
    GPU::JobActivity::Record activity;
    if(native && job.admitted && fJobHistory) {
        activity.prepareBegin=job.prepareBegin;activity.admitted=job.admitted;activity.submitted=job.submitted;
        activity.observed=job.observed;activity.retireBegin=retireBegin;
        activity.gpuBegin=job.gpuBegin;activity.gpuEnd=job.gpuEnd;
        activity.sequence=job.sequence;activity.stamp=native->stamp;activity.kinds=native->commandKinds;
        activity.flags=GPU::JobActivity::Admitted|(native->compositor?GPU::JobActivity::Compositor:0)|
            (job.gpuBegin && job.gpuEnd>=job.gpuBegin?GPU::JobActivity::HardwareInterval:0);
        activity.shaderOperations=job.shaderOperations;activity.ringDepth=job.ringDepth;
        activity.preflightTicks=job.preflightTicks;activity.indexAcquireTicks=job.indexAcquireTicks;
        activity.indexSnapshotTicks=job.indexSnapshotTicks;activity.encodingTicks=job.encodingTicks;
        activity.addressSpaceTicks=job.addressSpaceTicks;activity.fenceTicks=job.fenceTicks;
        activity.drawAttachmentsTicks=job.drawAttachmentsTicks;activity.drawIndicesTicks=job.drawIndicesTicks;
        activity.drawBindingsTicks=job.drawBindingsTicks;activity.drawAliasesTicks=job.drawAliasesTicks;activity.drawEmitTicks=job.drawEmitTicks;
        activity.pid=job.diagnosticPID;activity.indexBytes=job.indexBytes;
        activity.gmemTiles=job.gmemTiles;activity.gmemSamples=gmemSamples;
        activity.gmemLoadTicks=gmemTicks[0];activity.gmemDrawTicks=gmemTicks[1];activity.gmemStoreTicks=gmemTicks[2];
        for(unsigned i=0;i<native->resourceCount;++i)if(native->surfaceWrites&GPU::Native::Memory::writeBit(i)) {
            const auto *resource=OSDynamicCast(QcomGPUResource,native->resources[i]);
            const auto *info=resource?resource->surfaceInfo():nullptr;
            if(!info)continue;
            // Multiple views may name the same native IOSurface.
            bool seen=false;for(unsigned n=0;n<activity.writtenSurfaceCount && n<4;++n)
                if(activity.writtenSurfaces[n]==info->id)seen=true;
            if(seen)continue;
            if(activity.writtenSurfaceCount<4)activity.writtenSurfaces[activity.writtenSurfaceCount++]=info->id;
            else activity.flags|=GPU::JobActivity::SurfaceOverflow;
        }
    }
    job = {}; // Clear the FIFO before releasing a client which may enter free.
    if (native) {
        if(!success && issued && native->queue)native->queue->fail();
        if (!completeNative(native,success)) coherent=false;
        if(activity.admitted && fJobHistory) {
            activity.retireEnd=mach_absolute_time();
            if(success && coherent && native->result==kIOReturnSuccess)activity.flags|=GPU::JobActivity::Successful;
            auto &history=*static_cast<GPU::JobActivity::History *>(const_cast<void *>(fJobHistory->getBytesNoCopy()));
            history.append(activity);
        }
        if(operationProfile && fOperationActivity) {
            auto &profile=*static_cast<GPU::OperationActivity::Snapshot *>(const_cast<void *>(fOperationActivity->getBytesNoCopy()));
            if(issued && success && coherent && native->result==kIOReturnSuccess) {
                const auto *records=static_cast<const GPU::OperationActivity::Record *>(operationProfile->getBytesNoCopy());
                for(unsigned i=0;i<operationProfile->getLength()/sizeof(*records);++i)profile.append(records[i],operationJobBegin,operationJobEnd);
            } else ++profile.failedJobs;
        }
        // Account only real, coherent, successfully published completions.
        // Payload bytes count one copy; read+write traffic is at least twice
        // this amount, not a measurement of external DDR transactions.
        if(issued && success && coherent && native->result==kIOReturnSuccess) {
            GPU::Diagnostics::add(fRasterStateActivity.native.fullSetups,rasterFullSetups);
            GPU::Diagnostics::add(fRasterStateActivity.native.reusedPrimitives,rasterReusedPrimitives);
            GPU::Diagnostics::add(fRasterStateActivity.native.driverUpdates,rasterDriverUpdates);
            if(native->compositor) {
                GPU::Diagnostics::add(fRasterStateActivity.compositor.fullSetups,rasterFullSetups);
                GPU::Diagnostics::add(fRasterStateActivity.compositor.reusedPrimitives,rasterReusedPrimitives);
                GPU::Diagnostics::add(fRasterStateActivity.compositor.driverUpdates,rasterDriverUpdates);
            }
            GPU::Diagnostics::add(fShaderObjectActivity.native.reused,shaderObjectHits);
            GPU::Diagnostics::add(fShaderObjectActivity.native.avoidedBytes,shaderObjectSavedBytes);
            if(native->compositor) {
                GPU::Diagnostics::add(fShaderObjectActivity.compositor.reused,shaderObjectHits);
                GPU::Diagnostics::add(fShaderObjectActivity.compositor.avoidedBytes,shaderObjectSavedBytes);
            }
            GPU::Diagnostics::add(fNativeFramebufferScratchBytes,scratchBytes);
            GPU::Diagnostics::add(fNativeFramebufferScratchAvoidedBytes,avoidedScratchBytes);
            if(native->compositor){GPU::Diagnostics::add(fCompositorFramebufferScratchBytes,scratchBytes);
                GPU::Diagnostics::add(fCompositorFramebufferScratchAvoidedBytes,avoidedScratchBytes);}
            fNativeFramebufferBytes+=copiedBytes;fNativeFramebufferPrimitives+=primitives;
            fNativeFramebuffer2DBytes+=copied2DBytes;
            fNativeFramebufferUncroppedBytes+=uncroppedBytes;
            fNativeFramebufferInplacePrimitives+=inplacePrimitives;
            if(admissionDepth>1)++fNativeQueuedAdmissionJobs;
            if(admissionDepth>fNativeAdmissionMaxDepth)fNativeAdmissionMaxDepth=admissionDepth;
            if(ringDepth>1)++fNativeQueuedRingJobs;
            if(ringDepth>fNativeRingMaxDepth)fNativeRingMaxDepth=ringDepth;
            if(native->compositor){fCompositorFramebufferBytes+=copiedBytes;fCompositorFramebufferPrimitives+=primitives;}
            if(native->compositor)fCompositorFramebuffer2DBytes+=copied2DBytes;
            if(native->compositor)fCompositorFramebufferUncroppedBytes+=uncroppedBytes;
            if(native->compositor)fCompositorFramebufferInplacePrimitives+=inplacePrimitives;
            if(native->compositor){if(admissionDepth>1)++fCompositorQueuedAdmissionJobs;
                if(admissionDepth>fCompositorAdmissionMaxDepth)fCompositorAdmissionMaxDepth=admissionDepth;}
            if(native->compositor){if(ringDepth>1)++fCompositorQueuedRingJobs;
                if(ringDepth>fCompositorRingMaxDepth)fCompositorRingMaxDepth=ringDepth;}
        }
        native->release();
    }
    if (client) client->release();
    OSSafeReleaseNULL(operationProfile);
    return coherent;
}
bool QcomGPU::stopHardware()
{
    if (fInterrupt) fInterrupt->disable();
    if (fTimer) fTimer->cancelTimeout();
    if(fQuarantined)return false;
    // Lost clocks/rails make even a halt-register access unsafe. Keep the
    // entire DMA graph alive without attempting MMIO or a reset sequence.
    if(fEngine.ownsHardware() && !platformReady()) {
        quarantineHardware();return false;
    }
    if (!fEngine.stop()) {
        // Never unmap/free a buffer still reachable by a wedged command
        // processor. Retain the service and the complete pending graph.
        quarantineHardware();
        IOLog("QcomGPU: DMA halt timed out; mappings quarantined until platform reset\n");
        return false;
    }
    if (auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper)) if (!mapper->restoreKernelContext()) {
        quarantineHardware();return false;
    }
    for (unsigned i=0;i<fJobCount;++i) {
        auto &job=fJobs[(fJobHead+i)%GPU::MaxJobs];
        if(job.addressSpace && !job.addressSpace->retire()){quarantineHardware();return false;}
    }
    bool coherent=true;
    while (fJobCount) {
        unsigned index = fJobHead;
        fJobHead = (fJobHead + 1) % GPU::MaxJobs; --fJobCount;
        if(!finishJob(fJobs[index], false)) {if(fQuarantined)return false;coherent=false;}
    }
    fActiveJob=GPU::MaxJobs;
    publishStatistics(); // The regular sampling timer is stopped now.
    return coherent;
}
void QcomGPU::quarantineHardware()
{
    if(!fQuarantined){fQuarantined=true;retain();}
    if(fInterrupt)fInterrupt->disable();
    if(fTimer)fTimer->cancelTimeout();
    // Wake synchronous callers without releasing buffers which the hardware
    // may still reach. Jobs retain their tickets, queues and native resources.
    for(UInt32 i=0;i<fJobCount;++i) {
        auto &job=fJobs[(fJobHead+i)%GPU::MaxJobs];
        if(job.native && !job.native->done) completeNative(job.native,false);
    }
    publishStatistics(); // Preserve failed-ticket evidence without touching DMA.
    IOLog("QcomGPU: DMA isolation cannot be proved; mappings and platform leases quarantined until reset\n");
}
void QcomGPU::releaseHardware()
{
    if (fQuarantined || fEngine.ownsHardware()) return;
    if((fMapper || fRegisterMap) && !powerReady()){quarantineHardware();return;}
    // Keep the pool throughout warm operation/recovery. Complete its original
    // DMA mappings only after a proved hardware stop, before mapper shutdown.
    while(auto *buffer=fIndirectBuffers.pop())buffer->release();
    while(auto *buffer=fScratchBuffers.pop())buffer->release();
    while(auto *buffer=fIndexBuffers.pop())buffer->release();
    delete fPerformanceCPUClocks;fPerformanceCPUClocks=nullptr;
    OSSafeReleaseNULL(fPerformanceClockMap);
    OSSafeReleaseNULL(fPerformanceGPUCCMap);
    for(auto &mapping:fPerformanceAPCS)OSSafeReleaseNULL(mapping);
    if (fTimerAdded) { fLoop->removeEventSource(fTimer); fTimerAdded = false; }
    if (fInterruptAdded) { fLoop->removeEventSource(fInterrupt); fInterruptAdded = false; }
    OSSafeReleaseNULL(fTimer); OSSafeReleaseNULL(fInterrupt);
    OSSafeReleaseNULL(fRing); OSSafeReleaseNULL(fFence); OSSafeReleaseNULL(fFaultSnapshot);
    OSSafeReleaseNULL(fOperationActivity);
    OSSafeReleaseNULL(fJobHistory);
    OSSafeReleaseNULL(fMetalTiming);
    if (auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper)) if(!mapper->shutdown()) {
        if(!fQuarantined){fQuarantined=true;retain();}
        IOLog("QcomGPU: SMMU mappings or failed TLB sync retain the platform lease\n");return;
    }
    if(fSecure) {
        if(!fSecure->release()){quarantineHardware();return;}
        delete fSecure;fSecure=nullptr;setProperty("GPUZAPReady",false);
    }
    if(fNoC) {
        if(!fNoC->shutdown()){quarantineHardware();return;}
        delete fNoC;fNoC=nullptr;setProperty("GPUNoCReady",false);
    }
    OSSafeReleaseNULL(fRegisterMap);
    auto *mapper=fMapper;fMapper=nullptr;if(mapper)mapper->release();
    fRegisters.base = nullptr;
    if(fPlatform) {
        if(!fPlatform->release()) {
            if(!fQuarantined){fQuarantined=true;retain();}
            IOLog("QcomGPU: platform lease cannot be verified for release\n");return;
        }
        delete fPlatform;fPlatform=nullptr;setProperty("GPUPlatformLease",false);
    }
    if (fProviderOpened) { getProvider()->close(this); fProviderOpened = false; }
}
bool QcomGPU::recoveryAvailable() const {
    return fStarted && fFirmwareRetained && !fQuarantined && !fRecoveryFailed && fRecoveryAttempts<3 &&
        fRing && fFence && fRegisterMap && fPlatform && fNoC && fSecure && OSDynamicCast(QcomGPUMapper,fMapper);
}
IOReturn QcomGPU::recoverHardware() {
    auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
    if(!recoveryAvailable() || !mapper || fQuarantined || fJobCount || fNativeCalls || fState!=GPU::Faulted)return kIOReturnNotReady;
    ++fRecoveryAttempts;
    struct Guard {
        QcomGPU *owner;QcomGPUMapper *mapper;QcomGPUSecure *secure;Registers *registers;
        bool accessible(){return owner->platformReady();}
        bool healthy(){return accessible() && mapper->healthy();}
        bool isolated(){return accessible() && (registers->read(Adreno::MEControl)&Adreno::Halt)==Adreno::Halt &&
            (registers->read(Adreno::VBIFHalted)&15)==15;}
        bool synchronize(){return mapper->synchronizeForRecovery();}
        bool restoreZAP(){return secure->restore()==kIOReturnSuccess;}
    } guard{this,mapper,fSecure,&fRegisters};
    const auto restored=Adreno::recover(fFirmwareContract,fFirmwareHandoff,fRegisters,guard,fEngine,
        static_cast<uint32_t *>(fRing->bytes()),fRing->address,static_cast<uint32_t *>(fFence->bytes()),fFence->address);
    fRecoveryStage=uint32_t(restored.stage);
    if(restored.result!=GPU::Result::Success) {
        fRecoveryFailed=true;
        if(!restored.dmaStopped)quarantineHardware();
        IOLog("QcomGPU: recovery failed at stage %u; failed jobs were not replayed\n",fRecoveryStage);
        return restored.result==GPU::Result::Timeout?kIOReturnTimeout:kIOReturnIOError;
    }
    const auto privateTest=verifyAddressSpaces();
    if(privateTest!=kIOReturnSuccess){fRecoveryFailed=true;faultHardware();return privateTest;}
    ++fRecoveryCount;fState=GPU::FrameworkReady;
    if(fInterrupt)fInterrupt->enable();
    IOLog("QcomGPU: CP/GPMU restored after reset, fresh non-secure timestamp verified (recovery %u)\n",fRecoveryCount);
    return kIOReturnSuccess;
}
IOReturn QcomGPU::recoveryAction(OSObject *object,void *,void *,void *,void *) {
    auto *gpu=static_cast<QcomGPU *>(object);
    if(!gpu->fStarted || !gpu->fRecoveryPending)return kIOReturnNotReady;
    if(!gpu->fAdmissionOn){gpu->fTimer->setTimeoutMS(100);return kIOReturnNotReady;}
    // Old native callers must publish their failed Family fence/notifications
    // before the hardware sequence counter is restarted. Do not wait while
    // holding their command gate.
    if(gpu->fNativeCalls && mach_absolute_time()<gpu->fRecoveryDrainDeadline) {
        gpu->fTimer->setTimeoutMS(10);return kIOReturnNotReady;
    }
    gpu->fRecoveryPending=false;
    if(gpu->fNativeCalls){gpu->fRecoveryFailed=true;gpu->fRecoveryResult=kIOReturnTimeout;}
    else gpu->fRecoveryResult=gpu->recoverHardware();
    if(!gpu->publishHardwareState()) {gpu->fRecoveryFailed=true;gpu->faultHardware();return kIOReturnNoMemory;}
    if(gpu->fRecoveryResult==kIOReturnSuccess)gpu->fTimer->setTimeoutMS(100);
    return gpu->fRecoveryResult;
}
void QcomGPU::captureFaultState()
{
    auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
    if(!fFaultSnapshot || fFaultSnapshotAttempted || !mapper || !fRegisterMap || !platformReady())return;
    // A5xx's CP crashdumper can reach the HLSQ aperture on secure platforms.
    // CPU reads of the nominal SP/HLSQ register addresses cannot. Require an
    // empty, idle CP in the kernel tree; never switch out of a faulted job to
    // obtain a snapshot. A stalled SMMU must not launch another DMA engine.
    if(fRegisters.read(Adreno::RBRead)!=fRegisters.read(Adreno::RBWrite) ||
        (fRegisters.read(Adreno::Status)&~1u) || (fRegisters.read(Adreno::Status3)&(1u<<24)) ||
        fRegisters.read(Adreno::VBIFHalt) || fRegisters.read(Adreno::MEControl) ||
        fRegisters.read(Adreno::TrustControl) ||
        !mapper->kernelContextSelected())return;
    fFaultSnapshotAttempted=true;
    static constexpr struct {UInt32 type,base,count;} banks[]={
        {0x35,0xe00,0x32},{0x3f,0xec0,0x40},
        {0x32,0xe780,0x62},{0x34,0xef80,0x62},
        {0x3e,0xe580,0x170},{0x3c,0xed80,0x170}
    };
    constexpr UInt32 dataOffset=1024;
    auto *script=static_cast<UInt64 *>(fFaultSnapshot->bytes());
    auto *data=reinterpret_cast<UInt32 *>(static_cast<UInt8 *>(fFaultSnapshot->bytes())+dataOffset);
    UInt32 words=0;
    for(const auto &bank:banks) {
        *script++=UInt64(bank.type)<<8;
        *script++=(UInt64(REG_A5XX_HLSQ_DBG_READ_SEL)<<44)|(1u<<21)|1;
        *script++=fFaultSnapshot->address+dataOffset+words*4;
        *script++=(UInt64(REG_A5XX_HLSQ_DBG_AHB_READ_APERTURE)<<44)|bank.count;
        words+=bank.count;
    }
    *script++=0;*script++=0;
    if(dataOffset+words*4>fFaultSnapshot->length)return;
    for(UInt32 i=0;i<words;++i)data[i]=0xcdcdcdcd;
    fRegisters.publish();
    Adreno::Startup<Registers> startup(fRegisters);startup.hardwareClockGating(false);
    fRegisters.write(REG_A5XX_CP_CRASH_SCRIPT_BASE_LO*4,UInt32(fFaultSnapshot->address));
    fRegisters.write(REG_A5XX_CP_CRASH_SCRIPT_BASE_HI*4,UInt32(fFaultSnapshot->address>>32));
    fRegisters.write(REG_A5XX_CP_CRASH_DUMP_CNTL*4,1);
    UInt32 control=0,elapsed=0;
    for(;elapsed<10000;elapsed+=100) {
        control=fRegisters.read(REG_A5XX_CP_CRASH_DUMP_CNTL*4);
        if(control&4)break;
        fRegisters.delay(100);
    }
    fRegisters.acquire();
    setProperty("GPUFaultSnapshotControl",control,32);
    kprintf("QcomGPU: HLSQ snapshot control=%x elapsed-us=%u words=%u complete=%u\n",control,elapsed,words,(control&4)!=0);
    if(!(control&4)) {
        // stopHardware() must prove the following CP/AXI drain. Keep the
        // script and destination retained if completion cannot be proved.
        fRecoveryFailed=true;return;
    }
    startup.hardwareClockGating(true);
    setProperty("GPUFaultSnapshotVersion",UInt64(1),32);
    setProperty("GPUFaultHLSQSnapshot",data,words*4);
    UInt32 offset=0;
    for(const auto &bank:banks) {
        // Keep UART small: all banks survive in the property, while the
        // Compute configuration and object addresses are printed immediately.
        for(UInt32 i=0;i<bank.count;++i) {
            const auto reg=bank.base+i;
            const auto canonical=reg>=0xed80?reg-0x800:reg;
            if(canonical==0xe05 || canonical==0xec1 || canonical==0xe589 ||
                (canonical>=0xe5f0 && canonical<=0xe5f8) || canonical==0xe784 ||
                canonical==0xe790 || canonical==0xe796 ||
                (canonical>=0xe7b0 && canonical<=0xe7bb) || canonical==0xe7dc || canonical==0xe7dd)
                kprintf("QcomGPU: HLSQ bank=%x reg=%x value=%x\n",bank.type,reg,data[offset+i]);
        }
        offset+=bank.count;
    }
}
void QcomGPU::faultHardware(bool recoverable)
{
    fPreparedSubmission=nullptr;
    if(fState!=GPU::Faulted && fRegisterMap && platformReady()) {
        // Capture before halt/reset changes the evidence. Never touch MMIO
        // after losing the platform leases, even for a diagnostic snapshot.
        const auto status=fRegisters.read(Adreno::Status),irq=fRegisters.read(Adreno::InterruptStatus);
        const auto cp=fRegisters.read(Adreno::CPInterruptStatus);
        const auto read=fRegisters.read(Adreno::RBRead),write=fRegisters.read(Adreno::RBWrite);
        kprintf("QcomGPU: fault recoverable=%u jobs=%u active=%u status=%x irq=%x cp-error=%x rptr=%x wptr=%x\n",
            recoverable,fJobCount,fActiveJob,status,irq,cp,read,write);
        setProperty("GPUFaultStatus",status,32);setProperty("GPUFaultIRQs",irq,32);
        setProperty("GPUFaultCPError",cp,32);setProperty("GPUFaultRptr",read,32);setProperty("GPUFaultWptr",write,32);
        // SP/HLSQ context state is behind the HLSQ debug aperture. It is
        // not nominal CPU MMIO on this platform; keep direct reads limited
        // to the RBBM/CP registers above.
        captureFaultState();
    }
    const bool wasRunning=fState==GPU::FrameworkReady || fState==GPU::Suspended;
    fRecoveryPending=false;
    fPrivateAddressSpacesVerified=false;
    fState = GPU::Faulted;
    for (auto *client : fClients) if (client) for (unsigned i = 0; i < GPU::MaxQueues; ++i) {
        auto *queue = client->fSession.queues.at(i);
        if (queue->handle) queue->faulted = true;
    }
    const bool stopped=stopHardware();
    if(!recoverable || !stopped)fRecoveryFailed=true;
    if(recoverable && wasRunning && stopped && !fBootstrapPending && !fQuarantined && recoveryAvailable() &&
        platformReady() && OSDynamicCast(QcomGPUMapper,fMapper)->healthy()) {
        uint64_t interval=0;nanoseconds_to_absolutetime(1000000000ULL,&interval);
        fRecoveryDrainDeadline=mach_absolute_time()+interval;fRecoveryPending=true;fTimer->setTimeoutMS(1);
    }
    setProperty("GPUHardwareReady", false);
    setProperty("IOGPUNativeSubmission", false);
    setProperty("GPUFeatures", features(), 64);
    setProperty("GPUBlockingRequirements", blockers(), 64);
    publishHardwareState();
    publishStatistics();
}
bool QcomGPU::dispatchBlocked(unsigned candidate) const
{
    auto jobAt=[&](unsigned i)->const Job &{return fJobs[(fJobHead+i)%GPU::MaxJobs];};
    auto view=[&](unsigned i){Adreno::AdmissionCandidate c;c.issued=jobAt(i).sequence!=0;c.cancelled=jobAt(i).accessFailed;return c;};
    auto conflicts=[&](unsigned a,unsigned b) {
        const auto &left=jobAt(a),&right=jobAt(b);
        if((left.native && left.native->resourceCount!=left.bufferCount) ||
            (right.native && right.native->resourceCount!=right.bufferCount))return true;
        for(unsigned i=0;i<left.bufferCount;++i)for(unsigned j=0;j<right.bufferCount;++j) {
            if(!left.buffers[i] || !right.buffers[j])return true;
            const bool writes=!left.native || !right.native ||
                (left.native->surfaceWrites&GPU::Native::Memory::writeBit(i)) ||
                (right.native->surfaceWrites&GPU::Native::Memory::writeBit(j));
            if(!writes)continue;
            const auto *x=left.buffers[i]->backing(),*y=right.buffers[j]->backing();
            if(x->overlaps(y,0,x->length,0,y->length))return true;
        }
        return false;
    };
    return Adreno::dispatchDependencyBlocked(candidate,fJobCount,view,conflicts);
}
bool QcomGPU::cancelFailedSurfaceAccess()
{
    using GPU::Native::SurfaceAccessState;
    for(unsigned i=0;i<fJobCount;++i) {
        auto &job=fJobs[(fJobHead+i)%GPU::MaxJobs];
        if(!job.native || job.sequence || job.accessFailed)continue;
        const auto access=job.native->surfaceAccess();
        if(access.state!=SurfaceAccessState::Failed)continue;
        auto *queue=job.native->queue;queue->fail();
        // No cancelled slot has reached the ring. Release its surface access
        // with an error now, while retaining resources and reserved timeline
        // stamps until FIFO retirement. Already-posted DMA is never cancelled
        // or released by this path, and other queues remain operational.
        for(unsigned n=0;n<fJobCount;++n) {
            auto &pending=fJobs[(fJobHead+n)%GPU::MaxJobs];
            if(!pending.native || pending.native->queue!=queue || pending.sequence || pending.accessFailed)continue;
            pending.accessFailed=true;
            if(!pending.native->finishSurfaceFences(access.status?IOReturn(access.status):kIOReturnAborted)) {
                faultHardware();return false;
            }
        }
    }
    return true;
}
namespace {
enum ScheduledOperation {PrepareScheduled,SubmitScheduled,CancelScheduled,RestartScheduled};
}
IOReturn QcomGPU::prepareScheduled(QcomGPUNativeSubmission *ticket) {
    return fGate->runAction(scheduledAction,ticket,reinterpret_cast<void *>(PrepareScheduled));
}
void QcomGPU::submitScheduled(QcomGPUNativeSubmission *ticket) {
    fGate->runAction(scheduledAction,ticket,reinterpret_cast<void *>(SubmitScheduled));
}
void QcomGPU::cancelScheduled(QcomGPUNativeSubmission *ticket) {
    fGate->runAction(scheduledAction,ticket,reinterpret_cast<void *>(CancelScheduled));
}
void QcomGPU::restartScheduled() {
    fGate->runAction(scheduledAction,nullptr,reinterpret_cast<void *>(RestartScheduled));
}
IOReturn QcomGPU::scheduledAction(OSObject *owner,void *object,void *operation,void *,void *) {
    auto *gpu=static_cast<QcomGPU *>(owner);auto *ticket=static_cast<QcomGPUNativeSubmission *>(object);
    const auto action=uintptr_t(operation);
    if(action==RestartScheduled){gpu->faultHardware(true);return kIOReturnSuccess;}
    if(action==CancelScheduled) {
        if(gpu->fPreparedSubmission==ticket)gpu->fPreparedSubmission=nullptr;
        if(!ticket->done)gpu->faultHardware();
        return kIOReturnSuccess;
    }
    if(ticket->done) {
        if(gpu->fPreparedSubmission==ticket)gpu->fPreparedSubmission=nullptr;
        return kIOReturnSuccess;
    }
    if(action==PrepareScheduled) {
        if(gpu->fPreparedSubmission && gpu->fPreparedSubmission!=ticket)return kIOReturnNotReady;
        if(!gpu->nativeCPUPreparationReady())return kIOReturnNoMemory;
        unsigned offset=0;
        while(offset<gpu->fJobCount && gpu->fJobs[(gpu->fJobHead+offset)%GPU::MaxJobs].native!=ticket)++offset;
        if(offset==gpu->fJobCount)return kIOReturnNoMemory;
        auto &job=gpu->fJobs[(gpu->fJobHead+offset)%GPU::MaxJobs];
        if(job.accessFailed)return kIOReturnNoMemory;
        if((!gpu->fRingPipeline && gpu->fActiveJob<GPU::MaxJobs) || gpu->dispatchBlocked(offset) ||
            ticket->surfaceAccess().state!=GPU::Native::SurfaceAccessState::Ready)return kIOReturnNotReady;
        const auto count=job.commandList?job.commandList->getLength()/sizeof(Adreno::IndirectBuffer):1;
        if(!gpu->fEngine.submissionSpace(count,gpu->fExecutionProfile || gpu->fRingPipeline))return kIOReturnNotReady;
        gpu->fPreparedSubmission=ticket;
        return kIOReturnSuccess;
    }
    if(action==SubmitScheduled) {
        const bool posted=gpu->fPreparedSubmission==ticket && gpu->dispatchNextJob(ticket);
        gpu->fPreparedSubmission=nullptr;
        if(!posted)gpu->faultHardware();
        return posted?kIOReturnSuccess:kIOReturnIOError;
    }
    return kIOReturnBadArgument;
}

bool QcomGPU::dispatchNextJob(QcomGPUNativeSubmission *selected)
{
    if(fPreparedSubmission && selected!=fPreparedSubmission)return true;
    if((!fRingPipeline && fActiveJob<GPU::MaxJobs) || !fJobCount)return selected==nullptr;
    if(!cancelFailedSurfaceAccess())return false;
    for(unsigned posted=0;posted<GPU::MaxJobs;++posted) {
        auto view=[&](unsigned offset) {
            const auto &job=fJobs[(fJobHead+offset)%GPU::MaxJobs];
            Adreno::AdmissionCandidate candidate;
            candidate.owner=job.native?static_cast<const void *>(job.native->queue):job.client;
            candidate.queue=job.native?0:job.queue;
            candidate.priority=job.native?job.native->queue->priority():1;
            candidate.ready=!job.native && !job.sequence && !job.accessFailed &&
                (!fRingPipeline || !dispatchBlocked(offset));
            candidate.complete=job.hardwareDone;candidate.issued=job.sequence!=0;candidate.cancelled=job.accessFailed;return candidate;
        };
        unsigned offset=0;
        if(selected) {
            while(offset<fJobCount && fJobs[(fJobHead+offset)%GPU::MaxJobs].native!=selected)++offset;
        } else offset=Adreno::nextAdmission(fJobCount,view,fRingPipeline);
        if(offset==fJobCount)return selected==nullptr;
        unsigned index=(fJobHead+offset)%GPU::MaxJobs;auto &job=fJobs[index];
        const auto kickBegin=mach_absolute_time();
        if(!platformReady()){faultHardware();return false;}
        // Explicit engineering opt-in until the GPMU transition is verified.
        if(fDomainResumeEnabled && fActiveJob==GPU::MaxJobs) {
            // Domain resume belongs in a proved CP/AXI quiescent window, as in
            // a5xx_pm_resume. An empty software queue alone is not a hardware halt.
            const auto spBefore=fRegisters.read(0xa88b*4),rbBefore=fRegisters.read(0xa88d*4);
            GPU::Result resumed=GPU::Result::Success;
            if(spBefore==UINT32_MAX || rbBefore==UINT32_MAX)resumed=GPU::Result::Fault;
            else if(!(spBefore&(1u<<20)) || !(rbBefore&(1u<<20))) {
                resumed=fEngine.idlePowerTransition([&]() {
                    if(fFaultSnapshot)kprintf("QcomGPU: core resume quiesced me=%x axi=%x rptr=%x wptr=%x\n",
                        fRegisters.read(Adreno::MEControl),fRegisters.read(Adreno::VBIFHalted),
                        fRegisters.read(Adreno::RBRead),fRegisters.read(Adreno::RBWrite));
                    auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
                    return platformReady() && mapper && mapper->healthy() &&
                        mapper->kernelContextSelected() && Adreno::enableCoreBlocks(fRegisters) && platformReady();
                });
            }
            if(resumed!=GPU::Result::Success) {
                kprintf("QcomGPU: core-domain resume failed sp=%x rbccu=%x\n",spBefore,rbBefore);
                faultHardware();return false;
            }
            if(fFaultSnapshot)kprintf("QcomGPU: core domains before-kick sp=%x rbccu=%x after-sp=%x after-rbccu=%x sp-control=%x rbccu-control=%x\n",
                spBefore,rbBefore,fRegisters.read(0xa88b*4),fRegisters.read(0xa88d*4),
                fRegisters.read(0xa881*4),fRegisters.read(0xa887*4));
        }
        auto context=job.addressSpace?job.addressSpace->context():Adreno::PageTableContext{};
        const auto *addressSpace=job.addressSpace?&context:nullptr;
        if(job.native) {
            const auto cacheBegin=mach_absolute_time();
            for(unsigned i=0;i<job.bufferCount;++i)if(!job.buffers[i]->synchronize()){faultHardware();return false;}
            sampleNative(GPU::Diagnostics::CachePrepare,cacheBegin,mach_absolute_time(),job.native->compositor);
        }
        const auto depth=fEngine.lastIssued()-fEngine.lastCompleted()+1;
        const bool profile=fExecutionProfile || fRingPipeline;
        const uint64_t phaseVA=fExecutionProfile && job.native && job.addressSpace && job.scratch &&
            job.scratch->bytes() && job.scratch->length>=128 ? job.scratch->address+GPU::HardwarePhases::ScratchOffset:0;
        auto status=job.commandList?fEngine.postBatch(static_cast<const Adreno::IndirectBuffer *>(job.commandList->getBytesNoCopy()),
            job.commandList->getLength()/sizeof(Adreno::IndirectBuffer),job.sequence,addressSpace,profile?&job.execution:nullptr,phaseVA):
            fEngine.post(job.ib->address,job.commandWords,job.sequence,addressSpace,profile?&job.execution:nullptr,phaseVA);
        if(status==GPU::Result::Busy && !selected && fRingPipeline && fActiveJob<GPU::MaxJobs)return true;
        if(status!=GPU::Result::Success) {
            // Accepted native tickets already own timeline stamps. Fail/drain
            // every ticket in admission order if the hardware cannot accept it.
            faultHardware(status==GPU::Result::Fault);return false;
        }
        job.hardwarePhases=phaseVA!=0;
        if(job.addressSpace)job.addressSpace->markSubmitted();
        if(job.native) {
            sampleNative(GPU::Diagnostics::QueueWait,job.admitted,kickBegin,job.native->compositor);
            ++fNativeSubmitted;if(job.native->compositor)++fCompositorSubmitted;
            fNativeShaderOperations+=job.shaderOperations;
            if(job.native->compositor)fCompositorShaderOperations+=job.shaderOperations;
            const auto count=job.commandList?job.commandList->getLength()/sizeof(Adreno::IndirectBuffer):1;
            UInt64 words=job.commandWords;
            if(job.commandList) {
                words=0;const auto *list=static_cast<const Adreno::IndirectBuffer *>(job.commandList->getBytesNoCopy());
                for(unsigned i=0;i<count;++i)words+=list[i].words;
            }
            const UInt64 bytes=job.commandBuffers?job.framebufferIBBytes:job.ib->length;
            GPU::Diagnostics::add(fNativeIndirectBuffers,count);GPU::Diagnostics::add(fNativeIndirectBytes,bytes);GPU::Diagnostics::add(fNativeIndirectWords,words);
            if(job.native->compositor) {
                GPU::Diagnostics::add(fCompositorIndirectBuffers,count);GPU::Diagnostics::add(fCompositorIndirectBytes,bytes);GPU::Diagnostics::add(fCompositorIndirectWords,words);
            }
            fDiagnostics.native.kinds.post(job.native->commandKinds);
            if(job.native->compositor)fDiagnostics.compositor.kinds.post(job.native->commandKinds);
        }
        job.submitted=mach_absolute_time();
        job.ringDepth=depth;
        if(job.native)sampleNative(GPU::Diagnostics::Kick,kickBegin,job.submitted,job.native->compositor);
        if(fActiveJob==GPU::MaxJobs)fActiveJob=index;
        if(selected)return true;
        if(!fRingPipeline)return true;
    }
    return true;
}
void QcomGPU::retireJobs()
{
    if (fQuarantined || !fEngine.ready()) return;
    const auto pollBegin=mach_absolute_time();
    const bool activeNative=fActiveJob<GPU::MaxJobs && fJobs[fActiveJob].native;
    const bool compositor=activeNative && fJobs[fActiveJob].native->compositor;
    // Preserve the validation and fault path; measure it separately from the
    // observed execution interval, which also includes IRQ/timer scheduling.
    auto sampledPoll=[&]() {
        const auto now=mach_absolute_time();
        if(activeNative)sampleNative(GPU::Diagnostics::PollValidation,pollBegin,now,compositor);
        return now;
    };
    // healthy() acquires the mapper lock, then checks actual platform access
    // BEFORE context/stream/firmware MMIO. An outer platform scan duplicates
    // that work and can go stale while waiting for the mapper lock. Keep the
    // direct check only for the early bootstrap path without a mapper.
    auto *mapper=OSDynamicCast(QcomGPUMapper,fMapper);
    if(!(mapper?mapper->healthy():platformReady())) {
        sampledPoll();if(fState!=GPU::Faulted)faultHardware();return;
    }
    uint32_t completed = 0;
    if (fEngine.poll(completed) != GPU::Result::Success) {sampledPoll();faultHardware(true);return;}
    const auto observed=sampledPoll();
    for(unsigned i=0;i<fJobCount;++i) {
        auto &job=fJobs[(fJobHead+i)%GPU::MaxJobs];
        if(!job.sequence || job.hardwareDone || job.sequence>completed)continue;
        const bool jobNative=job.native!=nullptr,jobCompositor=jobNative && job.native->compositor;
        if(fExecutionProfile && jobNative) {
            uint64_t begin=0,end=0;
            if(fEngine.executionInterval(job.execution,begin,end)) {
                job.gpuBegin=begin;job.gpuEnd=end;
                fExecutionNative.sample(begin,end);
                if(jobCompositor)fExecutionCompositor.sample(begin,end);
                const bool render=(job.native->commandKinds&GPU::Diagnostics::Render)!=0;
                if(job.hardwarePhases && job.scratch && job.scratch->bytes()) {
                    const auto *phase=reinterpret_cast<const volatile UInt32 *>(
                        static_cast<const UInt8 *>(job.scratch->bytes())+GPU::HardwarePhases::ScratchOffset);
                    // The verified global completion is already observed;
                    // this CPU-NC, job-owned page cannot be recycled yet.
                    const UInt64 entered=phase[0]|(UInt64(phase[1])<<32),drained=phase[2]|(UInt64(phase[3])<<32);
                    fHardwarePhases.sample(jobCompositor,render,begin,entered,drained,end);
                } else fHardwarePhases.skip(jobCompositor,render);
                uint64_t coreBegin=0,coreEnd=0;
                if(fEngine.executionCoreInterval(job.execution,coreBegin,coreEnd)) {
                    fDiagnostics.nativeCore.sample(end-begin,coreEnd-coreBegin);
                    if(jobCompositor)fDiagnostics.compositorCore.sample(end-begin,coreEnd-coreBegin);
                } else {
                    GPU::Diagnostics::add(fDiagnostics.nativeCore.invalid);
                    if(jobCompositor)GPU::Diagnostics::add(fDiagnostics.compositorCore.invalid);
                }
                uint64_t busyBegin=0,busyEnd=0;
                if(fEngine.executionBusyInterval(job.execution,busyBegin,busyEnd)) {
                    fDiagnostics.nativeBusy.sample(end-begin,busyEnd-busyBegin,true);
                    if(jobCompositor)fDiagnostics.compositorBusy.sample(end-begin,busyEnd-busyBegin,true);
                } else {
                    GPU::Diagnostics::add(fDiagnostics.nativeBusy.invalid);
                    if(jobCompositor)GPU::Diagnostics::add(fDiagnostics.compositorBusy.invalid);
                }
            } else ++fExecutionInvalid;
        }
        job.observed=observed;
        if(jobNative)sampleNative(GPU::Diagnostics::RunObserved,job.submitted,observed,jobCompositor);
        job.hardwareDone=true;
    }
    // Native queues have independent completion channels. Retire actual GPU
    // completions without holding a finished UI job behind an unrelated
    // queue which has not submitted yet. Physical aliases still serialize at
    // dispatch; same-queue order comes from the original IOGPUWorkQueue.
    if(!cancelFailedSurfaceAccess())return;
    fActiveJob=GPU::MaxJobs;
    for(unsigned offset=0;offset<fJobCount;) {
        const auto index=(fJobHead+offset)%GPU::MaxJobs;auto &job=fJobs[index];
        if(!job.hardwareDone && !job.accessFailed){++offset;continue;}
        // A failed, unsubmitted descriptor cannot cover an earlier event in
        // its own queue. Other queues can continue to retire independently.
        bool prior=false;
        if(job.native)for(unsigned n=0;n<offset;++n) {
            const auto *other=fJobs[(fJobHead+n)%GPU::MaxJobs].native;
            if(other && other->queue==job.native->queue){prior=true;break;}
        }
        if(prior){++offset;continue;}
        const bool success=!job.accessFailed;
        if(success && fRingPipeline && dispatchBlocked(offset)){faultHardware();return;}
        job.retireStarted=mach_absolute_time();
        const bool retired=!job.addressSpace || job.addressSpace->retire(success);
        if(job.native)sampleNative(GPU::Diagnostics::RetireAddressSpace,
            job.retireStarted,mach_absolute_time(),job.native->compositor);
        if(!retired){faultHardware();return;}
        // Keep ownership in the fixed job storage while removing this entry
        // from admission order. finishJob clears the detached tail slot.
        Job finished=job;
        for(unsigned n=offset;n+1<fJobCount;++n)
            fJobs[(fJobHead+n)%GPU::MaxJobs]=fJobs[(fJobHead+n+1)%GPU::MaxJobs];
        const auto tail=(fJobHead+--fJobCount)%GPU::MaxJobs;fJobs[tail]=finished;
        if(!finishJob(fJobs[tail],success)){faultHardware();return;}
    }
    // Compaction changes indices; the watchdog follows actual execution.
    for(unsigned i=0;i<fJobCount;++i) {
        const auto index=(fJobHead+i)%GPU::MaxJobs;const auto &job=fJobs[index];
        if(job.sequence && !job.hardwareDone && (fActiveJob==GPU::MaxJobs || job.sequence<fJobs[fActiveJob].sequence))fActiveJob=index;
    }
    if(!dispatchNextJob())return;
    if(fJobCount) {
        // Native memory unprepare marks this before completion processing.
        // Our A540 maps and IOSurface readiness live outside that resource
        // set; report their retry opportunity through the same Family hook.
        // A completion IRQ alone does not wake prepare while another queue
        // remains in flight, even when a prior hardware dependency retired.
        nativeSchedulerObject()->nativeResourcesAvailable();
        nativeNotifyCompletion();
    }
    if (fActiveJob<GPU::MaxJobs) {
        auto &job=fJobs[fActiveJob];uint32_t progress=0;
        if(job.progressLimit && job.scratch)progress=static_cast<volatile uint32_t *>(job.scratch->bytes())[4];
        auto status=job.progress.poll(mach_absolute_time(),progress,job.progressLimit,fTimeoutTicks);
        if(status!=GPU::Result::Success) {
            IOLog("QcomGPU: submission watchdog stalled or invalid progress\n");faultHardware(true);return;
        }
        fTimer->setTimeoutMS(10);
    } else if(fJobCount) {
        // Native display/read fences can be the only remaining dependency.
        // Never leave an accepted surface waiter on the100ms idle cadence.
        const auto now=mach_absolute_time();
        for(unsigned i=0;i<fJobCount;++i) {
            const auto &job=fJobs[(fJobHead+i)%GPU::MaxJobs];
            if(job.native && !job.sequence && !job.accessFailed &&
                job.native->surfaceAccess().state==GPU::Native::SurfaceAccessState::Pending &&
                now>=job.admitted && now-job.admitted>=10*fTimeoutTicks) {
                if(fExecutionProfile) {
                    auto *records=OSArray::withCapacity(32);QcomGPUSurfaceWaitLog log{records};
                    log.append("QcomGPU: surface-wait-timeout jobs=%u accelerator=%u now=%llu",fJobCount,nativeAcceleratorIdentifier(),now);
                    for(unsigned n=0;n<fJobCount;++n) {
                        const auto &pending=fJobs[(fJobHead+n)%GPU::MaxJobs];
                        log.append("QcomGPU: surface-wait-job index=%u queue=%p sequence=%u stamp=%u done=%u admitted=%llu",n,
                            pending.native?pending.native->queue:nullptr,pending.sequence,pending.native?pending.native->stamp:0,pending.hardwareDone,pending.admitted);
                        if(pending.native)pending.native->logSurfaceFenceWait(n,log);
                    }
                    if(records){setProperty("GPUSurfaceWaitSnapshot",records);records->release();}
                }
                faultHardware();return;
            }
        }
        fTimer->setTimeoutMS(1);
    } else fTimer->setTimeoutMS(100); // validate retained platform state while idle too
}
void QcomGPU::interruptAction(OSObject *owner, IOInterruptEventSource *, int) {
    auto *gpu=static_cast<QcomGPU *>(owner);++gpu->fIRQCallbacks;gpu->retireJobs();
}
void QcomGPU::timerAction(OSObject *owner, IOTimerEventSource *) {
    auto *gpu=static_cast<QcomGPU *>(owner);
    if(gpu->fBootstrapPending)gpu->fGate->runAction(bootstrapAction);
    else if(gpu->fRecoveryPending)gpu->fGate->runAction(recoveryAction);
    else gpu->retireJobs();
    // Publish sampled counters without allocating registry numbers on every
    // frame. Compositor ownership comes from the actual task entitlement.
    const auto now=mach_absolute_time();
    if(gpu->fTimeoutTicks && now>=gpu->fStatisticsDeadline) {
        gpu->fStatisticsDeadline=now+gpu->fTimeoutTicks*2;
        gpu->publishStatistics();
    }
}
IOReturn QcomGPU::submit(QcomGPUUserClient *client, IOExternalMethodArguments *a)
{
    if (!nativeSubmissionReady()) return kIOReturnNotReady;
    // Resolve any already-written hardware fence before checking FIFO capacity.
    retireJobs();
    if (!nativeSubmissionReady()) return kIOReturnIOError;
    const uint64_t *in = a->scalarInput;
    if (in[3] % sizeof(GPU::Command) || in[3] > sizeof(GPU::Command) * GPU::MaxCommands) return kIOReturnBadArgument;
    auto *queue = client->fSession.queues.find(in[0]);
    auto *source = client->fMemory[in[1] & 0xff];
    if (!queue || !source || !source->address) return kIOReturnBadArgument;
    if (source->pending) return kIOReturnBusy;
    if (queue->faulted) return kIOReturnIOError;
    if (fJobCount == GPU::MaxJobs || queue->issued == UINT64_MAX) return kIOReturnNoResources;
    auto &job = fJobs[(fJobHead + fJobCount) % GPU::MaxJobs];
    job.ib = acquireIndirectBuffer();
    if (!job.ib) return kIOReturnNoMemory;
    // The command descriptors are copied into kernel-generated PM4. The source
    // buffer may subsequently change without changing this private immutable IB.
    Adreno::Encoder encoder(static_cast<uint32_t *>(job.ib->bytes()), Adreno::IBDwords);
    auto *bytes = static_cast<const uint8_t *>(source->bytes()) + in[2];
    GPU::Command commands[GPU::MaxCommands];
    memcpy(commands, bytes, in[3]);
    uint64_t addresses[GPU::MaxBuffers] = {};
    for (unsigned i = 0; i < GPU::MaxBuffers; ++i) if (client->fMemory[i]) addresses[i] = client->fMemory[i]->address;
    uint32_t destinations = 0;
    auto status = Adreno::encodeCommands(client->fSession, queue->parent, commands,
        static_cast<uint32_t>(in[3] / sizeof(GPU::Command)), addresses, encoder, destinations);
    if (status == GPU::Result::Success) status = encoder.barrier();
    if (status != GPU::Result::Success) {
        finishJob(job, false);
        return status == GPU::Result::NoResources ? kIOReturnNoResources :
            status == GPU::Result::Unsupported ? kIOReturnUnsupported : kIOReturnBadArgument;
    }
    for (unsigned i = 0; i < GPU::MaxBuffers; ++i) if (destinations & (1u << i)) {
        auto *target = client->fMemory[i]; target->retain(); target->beginGPUUse();
        job.buffers[job.bufferCount++] = target;
    }
    job.client = client; client->retain(); job.queue = queue->handle;
    job.fence = queue->issued + 1;job.commandWords=encoder.size();
    if(!platformReady()){finishJob(job,false);faultHardware();return kIOReturnIOError;}
    ++fJobCount; ++queue->pending; queue->issued = job.fence;
    a->scalarOutput[0] = job.fence;
    if(!dispatchNextJob())return kIOReturnIOError;
    fTimer->setTimeoutMS(10); return kIOReturnSuccess;
}
