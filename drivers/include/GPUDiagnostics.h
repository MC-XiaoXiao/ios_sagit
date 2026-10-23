// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "GPUUserProtocol.h"

namespace GPU { namespace Diagnostics {
// Optional vendor diagnostics on the original device connection. These report
// userspace failures before native admission; they never submit GPU work.
constexpr uint32_t MetalFailureSelector=0x1002, MetalFailureVersion=1, MaxMetalFailures=32;
// Read-only policy query on the caller's existing native device connection.
// Input is the registry observation (0/1), output is the enabled version (0/1).
constexpr uint32_t MetalConfigurationSelector=0x1004;
// Expensive AIR reconstruction and shader file export are separately opt-in.
// The error-reporting configuration alone must never enable them.
constexpr uint32_t MetalShaderCaptureVersion=1;
enum MetalStage : uint32_t { Library=1, Function, RenderPipeline, ComputePipeline, Encoder, CommandBuffer, Resource };
struct MetalFailure {
    uint32_t version=MetalFailureVersion,stage=0,code=0,reserved=0;
    char message[1024]{};
};
inline bool validMetalFailure(const MetalFailure &f) {
    return f.version==MetalFailureVersion && f.stage>=Library && f.stage<=Resource &&
        !f.reserved && f.message[0] && !f.message[sizeof(f.message)-1];
}
static_assert(sizeof(MetalFailure)==1040,"bounded Metal diagnostic ABI");
constexpr uint32_t Version=5, KindCount=4;
enum Kind : uint32_t { Marker=1, Transfer=2, Compute=4, Render=8 };
enum Stage : uint32_t {
    Preflight, IBAcquire, Encoding, CachePrepare, AddressSpace, FencePrepare, AdmissionDispatch,
    AdmissionTotal, QueueWait, Kick, PollValidation, RunObserved, RetireCleanup, NativePublish,
    RetireAddressSpace, RetireCoherence, RetireSurfacePublish, RetireResources, RetirePools,
    SnapshotMap, SnapshotCopy, SnapshotValidate, PreflightRetire, PreflightTicket,
    IndexAcquire, IndexSnapshot, RetireIndices, RetireCommands,
    WrapperSubmit, WrapperResourceCreate, WrapperResourceDelete, WrapperShmemCreate, WrapperProgram, NativeResourceFree, StageCount
};
inline const char *kindName(unsigned i) {
    constexpr const char *names[]={"marker","transfer","compute","render"};return i<KindCount?names[i]:"invalid";
}
inline const char *stageName(unsigned i) {
    constexpr const char *names[]={"preflight","ib_acquire","encoding","cache_prepare","address_space","fence_prepare","admission_dispatch",
        "admission_total","queue_wait","kick","poll_validation","run_observed","retire_cleanup","native_publish",
        "retire_address_space","retire_coherence","retire_surface_publish","retire_resources","retire_pools",
        "snapshot_map","snapshot_copy","snapshot_validate","preflight_retire","preflight_ticket",
        "index_acquire","index_snapshot","retire_indices","retire_commands",
        "wrapper_submit","wrapper_resource_create","wrapper_resource_delete","wrapper_shmem_create","wrapper_program","native_resource_free"};
    return i<StageCount?names[i]:"invalid";
}
inline uint32_t commandKind(uint32_t opcode) {
    switch(opcode) {
    case Barrier:return Marker;
    case Fill32:case Copy32:return Transfer;
    case ShaderDispatch:return Compute;
    case ShaderDraw:return Render;
    default:return 0;
    }
}
inline uint32_t normalizeKinds(uint32_t mask) {return mask&(Transfer|Compute|Render)?mask&(Transfer|Compute|Render):mask&Marker;}
inline void add(uint64_t &value,uint64_t amount=1) {value=amount>UINT64_MAX-value?UINT64_MAX:value+amount;}
struct Timing {
    uint64_t samples=0,lastTicks=0,maxTicks=0,totalTicks=0,invalidDeltas=0;
    bool sample(uint64_t begin,uint64_t end) {
        if(end<begin){add(invalidDeltas);return false;}
        lastTicks=end-begin;if(lastTicks>maxTicks)maxTicks=lastTicks;add(totalTicks,lastTicks);add(samples);return true;
    }
};
struct Kinds {
    uint64_t submitted[KindCount]{},completed[KindCount]{},failed[KindCount]{};
    void post(uint32_t mask) {mask=normalizeKinds(mask);for(unsigned i=0;i<KindCount;++i)if(mask&(1u<<i))add(submitted[i]);}
    void finish(uint32_t mask,bool success) {mask=normalizeKinds(mask);for(unsigned i=0;i<KindCount;++i)if(mask&(1u<<i))add(success?completed[i]:failed[i]);}
};
struct Metrics {
    Kinds kinds{};
    uint64_t accepted=0,rejected=0;
    uint32_t lastResult=0,reserved=0;
    Timing timing[StageCount]{};
    void admission(uint32_t result,uint64_t begin,uint64_t end) {
        add(result?rejected:accepted);lastResult=result;timing[AdmissionTotal].sample(begin,end);
    }
};
// CP_ALWAYS_COUNT is already selected by the retained startup. Paired reads
// do not enable/program counters. The ratio measures effective CP cycles per
// always-on interval; clock gating can make it lower than the configured PLL.
struct CoreClock {
    uint64_t samples=0,alwaysonTicks=0,cycles=0,minHz=0,maxHz=0,invalid=0;
    bool sample(uint64_t reference,uint64_t count,bool allowZero=false) {
        constexpr uint64_t referenceHz=19200000;
        if(!reference || (!count && !allowZero) || count>UINT64_MAX/referenceHz ||
            reference>UINT64_MAX-alwaysonTicks || count>UINT64_MAX-cycles) {
            add(invalid);return false;
        }
        const auto hz=count*referenceHz/reference;
        if(!samples || hz<minHz)minHz=hz;
        if(hz>maxHz)maxHz=hz;
        alwaysonTicks+=reference;cycles+=count;add(samples);return true;
    }
};
// Read-only IORegistry data without pointers or physical addresses. Metrics
// timings use CPU mach_absolute_time; CoreClock keeps its separate CP/XO units
// and never claims CPU frequency or a configured GPU PLL rate.
struct Snapshot {
    uint32_t version=Version,bytes=0,kindCount=KindCount,stageCount=StageCount;
    uint64_t timebaseSpanTicks=1000000,timebaseSpanNanoseconds=0;
    uint64_t poolHits=0,poolAllocations=0,poolCached=0,poolCapacity=MaxJobs;
    Metrics native{},compositor{};
    CoreClock nativeCore{},compositorCore{};
    // RBBM STATUS_MASKED counts busy cycles, not unconditional clock cycles.
    // Keep raw counts separate; a cycles/reference ratio includes idle time.
    CoreClock nativeBusy{},compositorBusy{};
};
static_assert(sizeof(Timing)==40 && sizeof(Metrics)==1480 && sizeof(CoreClock)==48 && sizeof(Snapshot)==3216,"GPU diagnostic data ABI");
// Separate optional property: the original GPUStatistics layout stays v5.
// Allocate/Zero/Source are nested, sampled details of mapping, not additional
// wall time. All other phases observe each attempted build/retirement.
namespace AddressSpaces {
constexpr uint32_t Version=1, DetailPeriod=64;
enum Phase : uint32_t { BuildWait, BuildHealth, Allocate, Zero, Source, FirmwareMap,
    BufferMap, Publish, BuildTLB, RetireWait, RetireHealth, RetireTLB, Release, PhaseCount };
inline const char *phaseName(unsigned i) {
    constexpr const char *names[]={"build_lock_wait","build_health","table_allocate","table_zero","physical_source",
        "firmware_map","buffer_map","publish","build_tlb","retire_lock_wait","retire_health","retire_tlb","table_release"};
    return i<PhaseCount?names[i]:"invalid";
}
inline bool detail(Phase phase){return phase==Allocate || phase==Zero || phase==Source;}
struct Metrics {Timing timing[PhaseCount]{};};
struct Snapshot {
    uint32_t version=Version,bytes=0,phaseCount=PhaseCount,detailPeriod=DetailPeriod;
    uint64_t timebaseSpanTicks=1000000,timebaseSpanNanoseconds=0;
    uint64_t enabled=0,hits=0,allocations=0,cached=0,capacity=64,validationFailures=0;
    Metrics native{},compositor{};
};
struct Trace {
    Metrics *all=nullptr,*compositor=nullptr;
    uint64_t (*clock)()=nullptr;
    bool detailed=false;
    uint64_t begin(Phase phase) const {return clock && (!detail(phase) || detailed)?clock():0;}
    void sample(Phase phase,uint64_t begin,uint64_t end) const {
        if(all)all->timing[phase].sample(begin,end);
        if(compositor)compositor->timing[phase].sample(begin,end);
    }
    void end(Phase phase,uint64_t begin) const {if(clock && (!detail(phase) || detailed))sample(phase,begin,clock());}
};
class Span {
    Trace *trace;Phase phase;uint64_t start;
public:
    Span(Trace *t,Phase p):trace(t),phase(p),start(t?t->begin(p):0){}
    ~Span(){if(trace)trace->end(phase,start);}
    Span(const Span &)=delete;Span &operator=(const Span &)=delete;
};
static_assert(sizeof(Snapshot)==1120,"private address-space diagnostic ABI");
}
// A single gated CPU-observed snapshot, independent of timer publication.
// No pointers, MMIO reads, forced retirement, GPU submission or registry writes.
constexpr uint32_t LiveSelector=0x1003,LiveVersion=1;
struct WorkCounts {
    uint64_t submitted=0,completed=0,failed=0,shaderOperations=0;
    uint64_t framebufferCopyBytes=0,inplacePrimitives=0,scratchBytes=0,scratchAvoidedBytes=0;
    uint64_t indirectBuffers=0,indirectBytes=0,indirectCommandWords=0;
};
struct LiveSnapshot {
    uint32_t version=LiveVersion,bytes=0,scope=0,reserved=0;
    uint64_t sequence=0,beginTicks=0,endTicks=0;
    uint64_t timebaseSpanTicks=1000000,timebaseSpanNanoseconds=0;
    uint64_t poolHits=0,poolAllocations=0,poolCached=0,poolCapacity=MaxJobs;
    Metrics metrics{};
    CoreClock core{},busy{};
    AddressSpaces::Snapshot addressSpaces{};
    WorkCounts native{},compositor{};
};
// Stay below the 4096-byte inline IOKit method-output limit; scope0 is all
// native jobs, scope1 compositor. Each request is its own timestamped sample.
static_assert(sizeof(WorkCounts)==88 && sizeof(LiveSnapshot)==2960,"live GPU diagnostic ABI");
template<class Clock> class AdmissionTrace {
    Metrics &all_;
    Metrics *compositor_=nullptr;
    Clock clock_;
    uint64_t begin_,cursor_;
    bool done_=false;
public:
    AdmissionTrace(Metrics &all,Clock clock):all_(all),clock_(clock),begin_(clock_()),cursor_(begin_){}
    ~AdmissionTrace(){if(!done_)finish(UINT32_MAX);}
    void compositor(Metrics *metrics){compositor_=metrics;}
    // Detail intervals can be nested in an existing stage. They do not move
    // its cursor and must not be added to the parent as disjoint wall time.
    void sample(Stage stage,uint64_t begin,uint64_t end) {
        all_.timing[stage].sample(begin,end);
        if(compositor_)compositor_->timing[stage].sample(begin,end);
    }
    void mark(Stage stage) {
        auto now=clock_();all_.timing[stage].sample(cursor_,now);
        if(compositor_)compositor_->timing[stage].sample(cursor_,now);
        cursor_=now;
    }
    uint32_t finish(uint32_t result) {
        if(!done_){auto now=clock_();all_.admission(result,begin_,now);if(compositor_)compositor_->admission(result,begin_,now);done_=true;}
        return result;
    }
};
template<class T,unsigned Capacity> class Cache {
    T slots_[Capacity]{};
    unsigned count_=0;
public:
    unsigned count() const{return count_;}
    T pop(){if(!count_)return {};T result=slots_[--count_];slots_[count_]={};return result;}
    bool push(T value){if(!value || count_==Capacity)return false;slots_[count_++]=value;return true;}
};
// Holds only retired, caller-owned buffers. Exact sizes preserve the original
// shader-visible allocation bounds; the caller must clear data before reuse.
template<class T,unsigned Capacity,uint64_t MaxBytes> class SizedCache {
    T slots_[Capacity]{};
    unsigned count_=0;
    uint64_t bytes_=0;
    T remove(unsigned index) {
        T value=slots_[index];bytes_-=value->length;
        for(unsigned i=index+1;i<count_;++i)slots_[i-1]=slots_[i];
        slots_[--count_]={};return value;
    }
public:
    unsigned count() const{return count_;}
    uint64_t bytes() const{return bytes_;}
    T take(uint64_t length){for(unsigned i=0;i<count_;++i)if(slots_[i]->length==length)return remove(i);return {};}
    T pop(){return count_?remove(0):T{};}
    bool push(T value) {
        if(!value || !value->length || count_==Capacity || value->length>MaxBytes-bytes_)return false;
        slots_[count_++]=value;bytes_+=value->length;return true;
    }
};
} }
