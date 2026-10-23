// SPDX-License-Identifier: BSD-2-Clause
#include "GPUConnection.h"
#include <GPUDiagnostics.h>
#include <GPUHardwarePhases.h>
#include <GPUMappingBounds.h>
#include <GPUJobActivity.h>
#include <GPUOperationActivity.h>
#include <GPUShaderCounters.h>
#include <GPUNativeMemory.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOReturn.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include "CPUStatus.h"

static int shellInfo()
{
    if(geteuid())return 1;
    auto parent=reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT,"getppid"));
    auto priority=reinterpret_cast<int (*)(int,unsigned)>(dlsym(RTLD_DEFAULT,"getpriority"));
    // Native public PROC_PIDTASKINFO layout from sys/proc_info.h.
    struct TaskInfo {
        uint64_t virtualBytes,residentBytes,user,system,threadsUser,threadsSystem;
        int32_t policy,faults,pageins,cow,messagesSent,messagesReceived,machCalls,unixCalls,
            switches,threads,running,priority;
    } info{};
    static_assert(sizeof(TaskInfo)==96,"native proc_taskinfo ABI");
    void *library=dlopen("/usr/lib/libproc.dylib",RTLD_NOW|RTLD_LOCAL);
    auto query=library?reinterpret_cast<int (*)(int,int,uint64_t,void *,int)>(dlsym(library,"proc_pidinfo")):nullptr;
    const int pid=parent?parent():-1;errno=0;
    const int nice=pid>0 && priority?priority(0,unsigned(pid)):0,niceError=errno;
    const int bytes=pid>0 && query?query(pid,4,0,&info,sizeof(info)):0;
    mach_timebase_info_data_t timebase{};mach_timebase_info(&timebase);
    printf("GPU_SHELL_INFO pid=%d nice=%d nice_valid=%u task_bytes=%d priority=%d policy=%d threads=%d running=%d pageins=%d faults=%d user_ticks=%llu system_ticks=%llu timebase=%u/%u startup_sample=0\n",
        pid,nice,pid>0 && priority && !niceError,bytes,info.priority,info.policy,info.threads,info.running,info.pageins,info.faults,
        static_cast<unsigned long long>(info.user),static_cast<unsigned long long>(info.system),timebase.numer,timebase.denom);
    auto sysctl=reinterpret_cast<int (*)(const char *,void *,size_t *,const void *,size_t)>(dlsym(RTLD_DEFAULT,"sysctlbyname"));
    char uuid[64]={};size_t uuidBytes=sizeof(uuid);errno=0;
    int bootStatus=sysctl?sysctl("kern.bootsessionuuid",uuid,&uuidBytes,nullptr,0):-1;
    bool bootValid=!bootStatus && uuidBytes>0 && uuidBytes<=sizeof(uuid) && !uuid[uuidBytes-1] && strlen(uuid)==36;
    printf("GPU_BOOT_INFO status=%d errno=%d uuid=%s uptime_ticks=%llu\n",bootStatus,bootStatus?errno:0,
        bootValid?uuid:"(unavailable)",static_cast<unsigned long long>(mach_absolute_time()));
    if(library)dlclose(library);return bytes==sizeof(info)?0:1;
}

static void faultSnapshot(CFTypeRef value)
{
    if(CFGetTypeID(value)!=CFDataGetTypeID())return;
    const auto count=CFDataGetLength(static_cast<CFDataRef>(value));
    const auto *bytes=CFDataGetBytePtr(static_cast<CFDataRef>(value));
    if(count<=0 || count>8192 || (count&3))return;
    uint32_t hash=2166136261u;
    for(CFIndex i=0;i<count;++i)hash=(hash^bytes[i])*16777619u;
    printf("GPU_HLSQ_BEGIN bytes=%ld fnv=%08x\n",static_cast<long>(count),hash);
    for(CFIndex i=0;i<count;i+=32) {
        printf("GPU_HLSQ_DATA offset=%ld words=",static_cast<long>(i/4));
        for(CFIndex j=i;j<count && j<i+32;j+=4) {
            uint32_t word=0;memcpy(&word,bytes+j,sizeof(word));printf("%s%08x",j==i?"":",",word);
        }
        putchar('\n');
    }
    printf("GPU_HLSQ_END bytes=%ld fnv=%08x\n",static_cast<long>(count),hash);
}

static int kernelLog(bool display=false)
{
    if(geteuid())return 1;
    // libproc provides the non-consuming snapshot used by native dmesg;
    // kern.msgbuf is only the buffer-size sysctl on this kernel.
    void *library=dlopen("/usr/lib/libproc.dylib",RTLD_NOW|RTLD_LOCAL);
    using ReadLog=int (*)(void *,uint32_t);
    auto readLog=library?reinterpret_cast<ReadLog>(dlsym(library,"proc_kmsgbuf")):nullptr;
    constexpr uint32_t capacity=2*1024*1024;
    char *buffer=readLog?static_cast<char *>(calloc(capacity+1,1)):nullptr;
    if(!buffer){if(library)dlclose(library);fputs("GPU_KERNEL_LOG unavailable\n",stderr);return 1;}
    const int bytes=readLog(buffer,capacity);
    uint32_t matched=0,emitted=0,nonzero=0;
    if(bytes>0 && uint32_t(bytes)<=capacity) {
        buffer[bytes]=0;
        for(int i=0;i<bytes;++i)if(buffer[i])++nonzero;
        // Inspect the complete returned snapshot, including any unused NUL
        // spans. Never consume /dev/klog or synthesize missing log records.
        for(char *line=buffer,*limit=buffer+bytes;line<limit;) {
            if(!*line || *line=='\n'){++line;continue;}
            char *end=line;while(end<limit && *end && *end!='\n')++end;
            if(end<limit)*end=0;
            const bool relevant=display ? strstr(line,"QcomDisplay") || strstr(line,"IOMobileFramebuffer") :
                strstr(line,"QcomGPU") || strstr(line,"SMMU");
            if(relevant) {
                ++matched;
                if(emitted<256){printf("%s_KERNEL_LOG %s\n",display?"DISPLAY":"GPU",line);++emitted;}
            }
            line=end+1;
        }
    }
    printf("GPU_KERNEL_LOG bytes=%d\n",bytes);
    printf("GPU_KERNEL_LOG_STATS scope=%s bytes=%d nonzero=%u matched=%u emitted=%u truncated=%u\n",
        display?"display":"gpu",bytes,nonzero,matched,emitted,matched>emitted);
    free(buffer);dlclose(library);return bytes>0 && uint32_t(bytes)<=capacity?0:1;
}

// Restore only the diagnostic file previously used to bootstrap this driver.
// Native VFS performs the replacement; no old APFS extent or raw disk offset
// is reused. Both installed and original contents must match their receipts.
static int restoreBootstrap()
{
    constexpr size_t bytes=131072;
    constexpr char original[]="c193a3d42999f51673e33e077c9723a1084fbc89d015661ddc4e407ac78e1ba2";
    constexpr char installed[]="4a324a7c3253788cff87a65086422f5cd4b4a4cc299de10368b4d6c2f15c0b0d";
    const char *source="/System/Library/Extensions/displayctl.original";
    const char *destination="/private/var/displayctl",*temporary="/private/var/displayctl.restore";
    if(geteuid())return 1;
    auto *data=static_cast<unsigned char *>(malloc(bytes));if(!data)return 1;
    auto digest=[&](int fd,char (&hex)[65]) {
        struct stat s{};if(fstat(fd,&s) || !S_ISREG(s.st_mode) || s.st_size!=off_t(bytes) || s.st_uid || s.st_nlink!=1)return false;
        size_t done=0;
        while(done<bytes){auto n=pread(fd,data+done,bytes-done,off_t(done));if(n<0 && errno==EINTR)continue;if(n<=0)return false;done+=size_t(n);}
        unsigned char hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data,CC_LONG(bytes),hash);
        constexpr char digits[]="0123456789abcdef";
        for(unsigned i=0;i<sizeof(hash);++i){hex[i*2]=digits[hash[i]>>4];hex[i*2+1]=digits[hash[i]&15];}hex[64]=0;return true;
    };
    int src=open(source,O_RDONLY|O_NOFOLLOW|O_CLOEXEC),dst=-1,tmp=-1;char hash[65]{};bool created=false;int result=1;
    do {
        if(src<0 || !digest(src,hash) || strcmp(hash,original)){puts("GPU_BOOTSTRAP_RESTORE original backup validation failed");break;}
        dst=open(destination,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat before{},current{};
        if(dst<0 || fstat(dst,&before) || (before.st_mode&07777)!=0700 || !digest(dst,hash))break;
        if(!strcmp(hash,original)){puts("GPU_BOOTSTRAP_RESTORE original already present; readback verified");result=0;break;}
        if(strcmp(hash,installed)){printf("GPU_BOOTSTRAP_RESTORE unexpected installed sha256=%s; retained\n",hash);break;}
        if(!digest(src,hash) || strcmp(hash,original))break;
        tmp=open(temporary,O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0700);if(tmp<0)break;created=true;
        size_t done=0;
        while(done<bytes){auto n=pwrite(tmp,data+done,bytes-done,off_t(done));if(n<0 && errno==EINTR)continue;if(n<=0)break;done+=size_t(n);}
        if(done!=bytes || fsync(tmp) || !digest(tmp,hash) || strcmp(hash,original))break;
        if(lstat(destination,&current) || current.st_dev!=before.st_dev || current.st_ino!=before.st_ino ||
            current.st_mode!=before.st_mode || current.st_uid!=before.st_uid || current.st_nlink!=1 ||
            !digest(dst,hash) || strcmp(hash,installed))break;
        if(rename(temporary,destination))break;created=false;
        close(dst);dst=open(destination,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(dst<0 || !digest(dst,hash) || strcmp(hash,original))break;
        sync();puts("GPU_BOOTSTRAP_RESTORE vfs=1 bytes=131072 original readback verified");result=0;
    } while(false);
    if(created)unlink(temporary);
    if(tmp>=0)close(tmp);if(dst>=0)close(dst);if(src>=0)close(src);free(data);
    if(result)perror("GPU_BOOTSTRAP_RESTORE");return result;
}

static void addressSpaceStatistics(CFTypeRef value)
{
    using namespace GPU::Diagnostics::AddressSpaces;
    Snapshot snapshot{};
    if(CFGetTypeID(value)!=CFDataGetTypeID() || CFDataGetLength(static_cast<CFDataRef>(value))!=CFIndex(sizeof(snapshot))) {
        puts("GPU_AS_STATISTICS status=unsupported-data");return;
    }
    memcpy(&snapshot,CFDataGetBytePtr(static_cast<CFDataRef>(value)),sizeof(snapshot));
    if(snapshot.version!=Version || snapshot.bytes!=sizeof(snapshot) || snapshot.phaseCount!=PhaseCount ||
        snapshot.detailPeriod!=DetailPeriod || snapshot.enabled>1 || snapshot.capacity!=64 || snapshot.cached>snapshot.capacity) {
        puts("GPU_AS_STATISTICS status=unsupported-version");return;
    }
    const double usPerTick=snapshot.timebaseSpanTicks?double(snapshot.timebaseSpanNanoseconds)/snapshot.timebaseSpanTicks/1000.0:0;
    printf("GPU_AS_STATISTICS version=%u clock=cpu-observation timebase_ticks=%llu timebase_ns=%llu detail_period=%u details=nested\n",
        snapshot.version,(unsigned long long)snapshot.timebaseSpanTicks,(unsigned long long)snapshot.timebaseSpanNanoseconds,snapshot.detailPeriod);
    printf("GPU_AS_POOL enabled=%llu hits=%llu allocations=%llu cached=%llu capacity=%llu validation_failures=%llu\n",
        (unsigned long long)snapshot.enabled,(unsigned long long)snapshot.hits,(unsigned long long)snapshot.allocations,
        (unsigned long long)snapshot.cached,(unsigned long long)snapshot.capacity,(unsigned long long)snapshot.validationFailures);
    for(unsigned scope=0;scope<2;++scope)for(unsigned i=0;i<PhaseCount;++i) {
        const auto &timing=(scope?snapshot.compositor:snapshot.native).timing[i];
        printf("GPU_AS_TIMING scope=%s phase=%s sampled=%u nested=%u samples=%llu last_ticks=%llu max_ticks=%llu total_ticks=%llu invalid=%llu mean_us=%.3f\n",
            scope?"compositor":"native",phaseName(i),detail(Phase(i)),detail(Phase(i)),
            (unsigned long long)timing.samples,(unsigned long long)timing.lastTicks,(unsigned long long)timing.maxTicks,
            (unsigned long long)timing.totalTicks,(unsigned long long)timing.invalidDeltas,
            timing.samples?double(timing.totalTicks)/timing.samples*usPerTick:0);
    }
}

static void statistics(CFTypeRef value,int selected=-1)
{
    using namespace GPU::Diagnostics;
    Snapshot snapshot{};
    if(CFGetTypeID(value)!=CFDataGetTypeID() || CFDataGetLength(static_cast<CFDataRef>(value))!=CFIndex(sizeof(snapshot))) {
        puts("GPU_STATISTICS status=unsupported-data");return;
    }
    memcpy(&snapshot,CFDataGetBytePtr(static_cast<CFDataRef>(value)),sizeof(snapshot));
    if(snapshot.version!=Version || snapshot.bytes!=sizeof(snapshot) || snapshot.kindCount!=KindCount ||
        snapshot.stageCount!=StageCount || snapshot.native.reserved || snapshot.compositor.reserved) {
        printf("GPU_STATISTICS status=unsupported-version version=%u bytes=%u\n",snapshot.version,snapshot.bytes);return;
    }
    const double usPerTick=snapshot.timebaseSpanTicks?double(snapshot.timebaseSpanNanoseconds)/snapshot.timebaseSpanTicks/1000.0:0;
    printf("GPU_STATISTICS version=%u clock=cpu-observation timebase_ticks=%llu timebase_ns=%llu kind_membership=overlapping\n",
        snapshot.version,(unsigned long long)snapshot.timebaseSpanTicks,(unsigned long long)snapshot.timebaseSpanNanoseconds);
    printf("GPU_IB_POOL hits=%llu allocations=%llu cached=%llu capacity=%llu\n",(unsigned long long)snapshot.poolHits,
        (unsigned long long)snapshot.poolAllocations,(unsigned long long)snapshot.poolCached,(unsigned long long)snapshot.poolCapacity);
    for(unsigned scope=0;scope<2;++scope) {
        if(selected>=0 && scope!=unsigned(selected))continue;
        const auto &metrics=scope?snapshot.compositor:snapshot.native;const char *name=scope?"compositor":"native";
        const auto &core=scope?snapshot.compositorCore:snapshot.nativeCore;
        printf("GPU_CORE_INTERVAL scope=%s samples=%llu alwayson_ticks=%llu core_cycles=%llu mean_hz=%.3f min_hz=%llu max_hz=%llu invalid=%llu reference_hz=19200000 counter_writes=0\n",name,
            (unsigned long long)core.samples,(unsigned long long)core.alwaysonTicks,(unsigned long long)core.cycles,
            core.alwaysonTicks?double(core.cycles)*19200000.0/core.alwaysonTicks:0,
            (unsigned long long)core.minHz,(unsigned long long)core.maxHz,(unsigned long long)core.invalid);
        const auto &busy=scope?snapshot.compositorBusy:snapshot.nativeBusy;
        printf("GPU_BUSY_INTERVAL scope=%s samples=%llu alwayson_ticks=%llu busy_cycles=%llu mean_cycles_per_second=%.3f invalid=%llu selector=6 reference_hz=19200000 counter_writes=0 frequency_measurement=0\n",name,
            (unsigned long long)busy.samples,(unsigned long long)busy.alwaysonTicks,(unsigned long long)busy.cycles,
            busy.alwaysonTicks?double(busy.cycles)*19200000.0/busy.alwaysonTicks:0,(unsigned long long)busy.invalid);
        printf("GPU_ADMISSION scope=%s accepted=%llu rejected=%llu last_result=0x%x\n",name,
            (unsigned long long)metrics.accepted,(unsigned long long)metrics.rejected,metrics.lastResult);
        for(unsigned i=0;i<KindCount;++i)printf("GPU_KIND scope=%s kind=%s submitted=%llu completed=%llu failed=%llu\n",name,kindName(i),
            (unsigned long long)metrics.kinds.submitted[i],(unsigned long long)metrics.kinds.completed[i],(unsigned long long)metrics.kinds.failed[i]);
        for(unsigned i=0;i<StageCount;++i) {
            const auto &t=metrics.timing[i];
            printf("GPU_TIMING scope=%s phase=%s samples=%llu last_us=%.3f max_us=%.3f total_ticks=%llu invalid_deltas=%llu\n",name,stageName(i),
                (unsigned long long)t.samples,double(t.lastTicks)*usPerTick,double(t.maxTicks)*usPerTick,
                (unsigned long long)t.totalTicks,(unsigned long long)t.invalidDeltas);
        }
    }
}

static CFTypeRef copyGPUProperty(CFStringRef key)
{
    mach_port_t iterator=0,service=0;
    if(IOServiceGetMatchingServices(0,IOServiceMatching("QcomGPU"),&iterator)==KERN_SUCCESS) {
        service=IOIteratorNext(iterator);auto extra=IOIteratorNext(iterator);IOObjectRelease(iterator);
        if(extra){IOObjectRelease(extra);if(service)IOObjectRelease(service);service=0;}
    }
    CFTypeRef value=service?IORegistryEntryCreateCFProperty(service,key,nullptr,0):nullptr;
    if(service)IOObjectRelease(service);
    return value;
}
static void hardwarePhases(uint64_t expectedSequence)
{
    using namespace GPU::HardwarePhases;
    CFTypeRef value=copyGPUProperty(CFSTR("GPUHardwarePhases"));
    Snapshot snapshot;
    const bool valid=value && CFGetTypeID(value)==CFDataGetTypeID() && CFDataGetLength(static_cast<CFDataRef>(value))==CFIndex(sizeof(snapshot));
    if(valid)memcpy(&snapshot,CFDataGetBytePtr(static_cast<CFDataRef>(value)),sizeof(snapshot));
    if(value)CFRelease(value);
    if(!valid || snapshot.version!=Version || snapshot.bytes!=sizeof(snapshot) || snapshot.reserved || snapshot.enabled>1 || snapshot.referenceHz!=ReferenceHz) {
        puts("GPU_HW_PHASES status=unavailable");return;
    }
    printf("GPU_HW_PHASES version=%u sequence=%llu matched=%u enabled=%u clock=gpu reference_hz=%u\n",snapshot.version,
        (unsigned long long)snapshot.sequence,unsigned(snapshot.sequence==expectedSequence),snapshot.enabled,snapshot.referenceHz);
    const Group *groups[]={&snapshot.native,&snapshot.compositor,&snapshot.compositorRender};
    const char *names[]={"native","compositor","compositor-render"},*phases[]={"enter","commands","leave"};
    const double usPerTick=1000000.0/snapshot.referenceHz;
    for(unsigned scope=0;scope<3;++scope) {
        const auto &g=*groups[scope];const GPU::Diagnostics::Timing *timings[]={&g.enter,&g.commands,&g.leave};
        for(unsigned phase=0;phase<3;++phase) {
            const auto &t=*timings[phase];
            printf("GPU_HW_PHASE scope=%s phase=%s samples=%llu total_ticks=%llu last_us=%.3f max_us=%.3f mean_us=%.3f skipped=%llu invalid=%llu\n",
                names[scope],phases[phase],(unsigned long long)t.samples,(unsigned long long)t.totalTicks,
                t.lastTicks*usPerTick,t.maxTicks*usPerTick,t.samples?t.totalTicks*usPerTick/t.samples:0,
                (unsigned long long)g.skipped,(unsigned long long)g.invalid);
        }
    }
}

static void mappingWork(uint64_t expectedSequence)
{
    using namespace GPU::MappingBounds;
    auto value=copyGPUProperty(CFSTR("GPUJobMappingWork"));Snapshot snapshot;
    const bool valid=value && CFGetTypeID(value)==CFDataGetTypeID() && CFDataGetLength(static_cast<CFDataRef>(value))==CFIndex(sizeof(snapshot));
    if(valid)memcpy(&snapshot,CFDataGetBytePtr(static_cast<CFDataRef>(value)),sizeof(snapshot));
    if(value)CFRelease(value);
    if(!valid || snapshot.version!=Version || snapshot.bytes!=sizeof(snapshot)) {puts("GPU_MAPPING_WORK status=unavailable");return;}
    printf("GPU_MAPPING_WORK version=%u sequence=%llu matched=%u resource_pages_only=1\n",snapshot.version,
        (unsigned long long)snapshot.sequence,unsigned(snapshot.sequence==expectedSequence));
    const Work *work[]={&snapshot.native,&snapshot.compositor};
    for(unsigned i=0;i<2;++i) {
        const auto &v=*work[i];
        if(v.mappedPages>v.fullPages || v.fallbackRoots>v.roots || v.overflowRoots>v.fallbackRoots){puts("GPU_MAPPING_WORK status=invalid-counters");return;}
        printf("GPU_MAPPING scope=%s jobs=%llu roots=%llu full_pages=%llu mapped_pages=%llu fallback_roots=%llu overflow_roots=%llu page_bytes=4096\n",i?"compositor":"native",
            (unsigned long long)v.jobs,(unsigned long long)v.roots,(unsigned long long)v.fullPages,
            (unsigned long long)v.mappedPages,(unsigned long long)v.fallbackRoots,(unsigned long long)v.overflowRoots);
    }
}

static void resourceActivity(uint64_t sequence)
{
    using namespace GPU::JobActivity;
    auto value=copyGPUProperty(CFSTR("GPUResourceActivity"));
    const auto *s=value && CFGetTypeID(value)==CFDataGetTypeID() && CFDataGetLength((CFDataRef)value)==sizeof(Resources)?
        reinterpret_cast<const Resources *>(CFDataGetBytePtr((CFDataRef)value)):nullptr;
    if(!s || s->version!=Version || s->bytes!=sizeof(Resources) || s->types!=ResourceTypes || s->classes!=SizeClasses || !s->timebaseTicks) {
        puts("GPU_RESOURCES status=unavailable");if(value)CFRelease(value);return;
    }
    printf("GPU_RESOURCES version=%u sequence=%llu matched=%u timebase_ticks=%llu timebase_ns=%llu counts=factory_calls\n",s->version,
        (unsigned long long)s->sequence,unsigned(s->sequence==sequence),(unsigned long long)s->timebaseTicks,(unsigned long long)s->timebaseNs);
    const ResourceMetrics *metrics[]={&s->native,&s->compositor};
    const char *types[]={"buffer","client","surface"};
    for(unsigned scope=0;scope<2;++scope) {
        printf("GPU_RESOURCE_INVALID scope=%s count=%llu\n",scope?"compositor":"native",(unsigned long long)metrics[scope]->invalid);
        for(unsigned type=0;type<ResourceTypes;++type)for(unsigned size=0;size<SizeClasses;++size) {
            const auto &bin=metrics[scope]->bins[type][size];
            printf("GPU_RESOURCE scope=%s type=%s size_class=%u calls=%llu failures=%llu bytes=%llu total_ticks=%llu\n",scope?"compositor":"native",types[type],size,
                (unsigned long long)bin.calls,(unsigned long long)bin.failures,(unsigned long long)bin.bytes,(unsigned long long)bin.ticks);
        }
    }
    CFRelease(value);
}

static void shaderObjectActivity(uint64_t sequence)
{
    using namespace GPU::JobActivity;
    auto value=copyGPUProperty(CFSTR("GPUShaderObjectActivity"));
    const auto *s=value && CFGetTypeID(value)==CFDataGetTypeID() && CFDataGetLength((CFDataRef)value)==sizeof(ShaderObjects)?
        reinterpret_cast<const ShaderObjects *>(CFDataGetBytePtr((CFDataRef)value)):nullptr;
    if(!s || s->version!=Version || s->bytes!=sizeof(ShaderObjects)) {
        puts("GPU_SHADER_OBJECTS status=unavailable");if(value)CFRelease(value);return;
    }
    const ShaderObjectMetrics *metrics[]={&s->native,&s->compositor};
    for(unsigned i=0;i<2;++i)printf("GPU_SHADER_OBJECTS version=%u sequence=%llu matched=%u scope=%s reused=%llu avoided_bytes=%llu\n",
        s->version,(unsigned long long)s->sequence,unsigned(s->sequence==sequence),i?"compositor":"native",
        (unsigned long long)metrics[i]->reused,(unsigned long long)metrics[i]->avoidedBytes);
    CFRelease(value);
}

static void rasterStateActivity(uint64_t sequence)
{
    using namespace GPU::JobActivity;
    auto value=copyGPUProperty(CFSTR("GPURasterStateActivity"));
    const auto *s=value && CFGetTypeID(value)==CFDataGetTypeID() && CFDataGetLength((CFDataRef)value)==sizeof(RasterStates)?
        reinterpret_cast<const RasterStates *>(CFDataGetBytePtr((CFDataRef)value)):nullptr;
    if(!s || s->version!=Version || s->bytes!=sizeof(RasterStates)) {
        puts("GPU_RASTER_STATES status=unavailable");if(value)CFRelease(value);return;
    }
    const RasterStateMetrics *metrics[]={&s->native,&s->compositor};
    for(unsigned i=0;i<2;++i)printf("GPU_RASTER_STATES version=%u sequence=%llu matched=%u scope=%s full_setups=%llu reused_primitives=%llu driver_updates=%llu\n",
        s->version,(unsigned long long)s->sequence,unsigned(s->sequence==sequence),i?"compositor":"native",
        (unsigned long long)metrics[i]->fullSetups,(unsigned long long)metrics[i]->reusedPrimitives,(unsigned long long)metrics[i]->driverUpdates);
    CFRelease(value);
}

static int operationActivity(uint64_t sequence,bool examples=false,uint64_t after=0)
{
    using namespace GPU::OperationActivity;
    auto value=copyGPUProperty(CFSTR("GPUOperationActivity"));
    const auto *s=value && CFGetTypeID(value)==CFDataGetTypeID() && CFDataGetLength((CFDataRef)value)==sizeof(Snapshot)?
        reinterpret_cast<const Snapshot *>(CFDataGetBytePtr((CFDataRef)value)):nullptr;
    if(!s || s->version!=Version || s->bytes!=sizeof(Snapshot) || s->count>BucketCapacity ||
        s->slowCount>SlowCapacity || s->slowNext>=SlowCapacity || s->topCount>TopCapacity || s->reserved) {
        puts("GPU_OPERATIONS status=unavailable");if(value)CFRelease(value);return 1;
    }
    printf("GPU_OPERATIONS version=%u sequence=%llu matched=%u enabled=%u buckets=%u total=%llu overflow=%llu failed_jobs=%llu allocation_skips=%llu scratch_skips=%llu slow_count=%u slow_total=%llu top_count=%u reference_hz=19200000 boundary=cp_not_unconditional_shader_coherence\n",
        s->version,(unsigned long long)s->sequence,unsigned(sequence==s->sequence),s->enabled,s->count,
        (unsigned long long)s->total,(unsigned long long)s->overflow,(unsigned long long)s->failedJobs,(unsigned long long)s->allocationSkips,
        (unsigned long long)s->scratchSkips,s->slowCount,(unsigned long long)s->slowTotal,s->topCount);
    for(unsigned i=0;i<s->count;++i) {
        const auto &b=s->buckets[i];
        const Counts *counts[]={&b.native,&b.compositor};
        for(unsigned scope=0;scope<2;++scope) {
            const auto &c=*counts[scope];
            printf("GPU_OP_BUCKET index=%u scope=%s opcode=%u hash0=%llx hash1=%llx samples=%llu ticks=%llu max_ticks=%llu invalid=%llu skipped=%llu raster_calls=%llu flushes=%llu copy_bytes=%llu\n",
                i,scope?"compositor":"native",b.first.opcode,(unsigned long long)b.first.first.hash,(unsigned long long)b.first.second.hash,
                (unsigned long long)c.samples,(unsigned long long)c.ticks,(unsigned long long)c.maximum,
                (unsigned long long)c.invalid,(unsigned long long)c.skipped,(unsigned long long)c.primitives,
                (unsigned long long)c.flushes,(unsigned long long)c.copyBytes);
        }
        const Program *programs[]={&b.first.first,&b.first.second};
        for(unsigned n=0;n<2;++n)if(programs[n]->hash) {
            const auto &a=*programs[n];
            printf("GPU_OP_PROGRAM bucket=%u slot=%u hash=%llx name=%.*s stage=%u words=%u instrlen=%u constlen=%u full_regs=%u half_regs=%u flags=0x%x framebuffer_mask=0x%x\n",
                i,n,(unsigned long long)a.hash,int(sizeof(a.name)),a.name,a.stage,a.words,a.instructions,a.constants,
                a.fullRegs,a.halfRegs,a.flags,a.framebufferMask);
        }
    }
    unsigned emitted[2]{};
    if(examples)for(unsigned group=0;group<2;++group)for(unsigned i=0;i<(group?s->topCount:s->slowCount);++i) {
        const auto &r=group?s->top[i]:s->oldest(i);const auto &d=r.description;
        if(r.retired<=after)continue;
        ++emitted[group];
        printf("%s ordinal=%llu sequence=%u stamp=%u op=%u flags=0x%x opcode=%u hash0=%llx hash1=%llx begin=%llu end=%llu retired=%llu count=%u instances=%u primitive=%u index_size=%u width=%u height=%u scissor=%u,%u,%u,%u full=%llu reused=%llu inplace=%llu snapshots=%llu flushes=%llu copy_bytes=%llu constant_windows=%u window_mask=0x%x groups=%u,%u,%u local=%u,%u,%u grid=%u,%u,%u blit_kind=%u blit_source_bytes=%u blit_destination_bytes=%u blit_parameters=%u,%u,%u,%u,%u,%u,%u,%u\n",
            group?"GPU_OP_TOP":"GPU_OP_SLOW",(unsigned long long)(group?i+1:s->slowTotal-s->slowCount+i+1),r.sequence,r.stamp,r.index,r.flags,d.opcode,
            (unsigned long long)d.first.hash,(unsigned long long)d.second.hash,(unsigned long long)r.begin,(unsigned long long)r.end,
            (unsigned long long)r.retired,d.count,d.instances,d.primitive,d.indexSize,d.width,d.height,
            d.scissor[0],d.scissor[1],d.scissor[2],d.scissor[3],(unsigned long long)d.fullSetups,(unsigned long long)d.reused,
            (unsigned long long)d.inplace,(unsigned long long)d.snapshots,(unsigned long long)d.flushes,(unsigned long long)d.copyBytes,
            (d.constantWindowState>>8)&255,d.constantWindowState&255,
            d.groups[0],d.groups[1],d.groups[2],d.local[0],d.local[1],d.local[2],d.grid[0],d.grid[1],d.grid[2],
            d.blitKind,d.blitSourceBytes,d.blitDestinationBytes,d.blitParameters[0],d.blitParameters[1],d.blitParameters[2],d.blitParameters[3],
            d.blitParameters[4],d.blitParameters[5],d.blitParameters[6],d.blitParameters[7]);
    }
    printf("GPU_OPERATIONS_END sequence=%llu slow_records=%u top_records=%u after=%llu\n",(unsigned long long)s->sequence,emitted[0],emitted[1],(unsigned long long)after);
    CFRelease(value);return 0;
}

static int metalTiming(uint64_t after=0) {
    GPU::NativeConnection c;uint64_t stamp=0;auto status=c.open();if(!status)status=c.metalTimingSnapshot(stamp);
    auto value=status?nullptr:copyGPUProperty(CFSTR("GPUMetalTiming"));
    const auto *pointer=value && CFGetTypeID(value)==CFDataGetTypeID() && CFDataGetLength((CFDataRef)value)==sizeof(GPU::MetalTiming::History)?
        reinterpret_cast<const GPU::MetalTiming::History *>(CFDataGetBytePtr((CFDataRef)value)):nullptr;
    if(!pointer || pointer->version!=GPU::MetalTiming::Version || pointer->bytes!=sizeof(*pointer) || pointer->now!=stamp ||
        pointer->count>GPU::MetalTiming::Capacity || pointer->next>=GPU::MetalTiming::Capacity) {
        printf("METAL_TIMING status=0x%x valid=0\n",status);if(value)CFRelease(value);return 1;
    }
    const auto &h=*pointer;
    unsigned emitted=0;for(uint32_t i=0;i<h.count;++i)
        if(h.samples[(h.next+GPU::MetalTiming::Capacity-h.count+i)%GPU::MetalTiming::Capacity].received>after)++emitted;
    printf("METAL_TIMING version=%u count=%u total=%llu now=%llu timebase_ticks=%llu timebase_ns=%llu history_count=%u after=%llu\n",h.version,emitted,h.total,h.now,h.timebaseTicks,h.timebaseNS,h.count,after);
    for(uint32_t i=0;i<h.count;++i) {
        const auto &s=h.samples[(h.next+GPU::MetalTiming::Capacity-h.count+i)%GPU::MetalTiming::Capacity];const auto &r=s.record;
        if(s.received<=after)continue;
        printf("METAL_EVENT index=%u pid=%u compositor=%u stage=%u begin=%llu end=%llu identity=%llx detail=%llx argument=%llx received=%llu\n",i,s.pid,s.compositor,r.stage,r.begin,r.end,r.identity,r.detail,r.argument,s.received);
    }
    CFRelease(value);return 0;
}

static int jobHistory(uint64_t after=0)
{
    using namespace GPU::JobActivity;
    auto value=copyGPUProperty(CFSTR("GPUJobHistory"));
    const auto *s=value && CFGetTypeID(value)==CFDataGetTypeID() && CFDataGetLength((CFDataRef)value)==sizeof(History)?
        reinterpret_cast<const History *>(CFDataGetBytePtr((CFDataRef)value)):nullptr;
    if(!s || s->version!=Version || s->bytes!=sizeof(History) || s->capacity!=Capacity || s->count>Capacity || s->next>=Capacity ||
        s->total<s->count || (s->count<Capacity && s->count!=s->total) || s->reserved || !s->timebaseTicks) {
        puts("GPU_JOBS status=unavailable");if(value)CFRelease(value);return 1;
    }
    for(unsigned i=0;i<s->count;++i) {
        const auto &r=s->oldest(i);
        if(r.ordinal!=s->total-s->count+i+1 || r.reserved || (r.flags&~31u) || !(r.flags&Admitted) ||
            ((r.flags&HardwareInterval) && (!r.gpuBegin || r.gpuEnd<r.gpuBegin)) ||
            r.prepareBegin>r.admitted || r.retireEnd<r.retireBegin || r.retireBegin<r.admitted ||
            r.indexBytes>GPU::Native::IndexSnapshotLimit || r.indexSnapshotTicks>r.encodingTicks || r.indexAcquireTicks>r.encodingTicks ||
            ((r.flags&Successful) && (!r.sequence || !r.submitted || r.observed<r.submitted))) {
            puts("GPU_JOBS status=invalid-record");CFRelease(value);return 1;
        }
    }
    unsigned emitted=0;for(unsigned i=0;i<s->count;++i)if(s->oldest(i).retireEnd>after)++emitted;
    printf("GPU_JOBS version=%u sequence=%llu count=%u total=%llu capacity=%u cpu_timebase_ticks=%llu cpu_timebase_ns=%llu gpu_reference_hz=19200000 history_count=%u after=%llu\n",
        s->version,(unsigned long long)s->sequence,emitted,(unsigned long long)s->total,s->capacity,
        (unsigned long long)s->timebaseTicks,(unsigned long long)s->timebaseNs,s->count,(unsigned long long)after);
    for(unsigned i=0;i<s->count;++i) {
        const auto &r=s->oldest(i);
        if(r.retireEnd<=after)continue;
        printf("GPU_JOB ordinal=%llu seq=%u stamp=%u kinds=0x%x flags=0x%x ops=%u depth=%u prepare=%llu admitted=%llu submitted=%llu observed=%llu retire_begin=%llu retire_end=%llu gpu_begin=%llu gpu_end=%llu surfaces=%u,%u,%u,%u surface_count=%u pid=%u index_bytes=%u preflight_ticks=%llu index_acquire_ticks=%llu index_snapshot_ticks=%llu encoding_ticks=%llu address_space_ticks=%llu fence_ticks=%llu draw_attachments_ticks=%llu draw_indices_ticks=%llu draw_bindings_ticks=%llu draw_aliases_ticks=%llu draw_emit_ticks=%llu gmem_tiles=%u gmem_samples=%u gmem_load_ticks=%llu gmem_draw_ticks=%llu gmem_store_ticks=%llu\n",
            (unsigned long long)r.ordinal,r.sequence,r.stamp,r.kinds,r.flags,r.shaderOperations,r.ringDepth,
            (unsigned long long)r.prepareBegin,(unsigned long long)r.admitted,(unsigned long long)r.submitted,(unsigned long long)r.observed,
            (unsigned long long)r.retireBegin,(unsigned long long)r.retireEnd,(unsigned long long)r.gpuBegin,(unsigned long long)r.gpuEnd,
            r.writtenSurfaces[0],r.writtenSurfaces[1],r.writtenSurfaces[2],r.writtenSurfaces[3],r.writtenSurfaceCount,r.pid,r.indexBytes,
            (unsigned long long)r.preflightTicks,(unsigned long long)r.indexAcquireTicks,(unsigned long long)r.indexSnapshotTicks,
            (unsigned long long)r.encodingTicks,(unsigned long long)r.addressSpaceTicks,(unsigned long long)r.fenceTicks,
            (unsigned long long)r.drawAttachmentsTicks,(unsigned long long)r.drawIndicesTicks,(unsigned long long)r.drawBindingsTicks,
            (unsigned long long)r.drawAliasesTicks,(unsigned long long)r.drawEmitTicks,r.gmemTiles,r.gmemSamples,
            (unsigned long long)r.gmemLoadTicks,(unsigned long long)r.gmemDrawTicks,(unsigned long long)r.gmemStoreTicks);
    }
    printf("GPU_JOBS_END sequence=%llu count=%u\n",(unsigned long long)s->sequence,emitted);
    CFRelease(value);return 0;
}

static int liveStatistics(unsigned scope=1,bool operationExamples=false,uint64_t after=0)
{
    GPU::NativeConnection connection;GPU::Diagnostics::LiveSnapshot s{};
    auto status=connection.open();if(!status)status=connection.liveStatistics(scope,s);
    if(status){printf("GPU_LIVE status=0x%x\n",status);return 1;}
    if(s.version!=GPU::Diagnostics::LiveVersion || s.bytes!=sizeof(s) || s.scope!=scope || s.reserved || !s.sequence || s.endTicks<s.beginTicks ||
        s.addressSpaces.version!=GPU::Diagnostics::AddressSpaces::Version || s.addressSpaces.bytes!=sizeof(s.addressSpaces)) {
        puts("GPU_LIVE status=invalid-snapshot");return 1;
    }
    printf("GPU_LIVE_BEGIN version=%u bytes=%u scope=%s sequence=%llu begin_ticks=%llu end_ticks=%llu forced_retirement=0\n",
        s.version,s.bytes,scope?"compositor":"native",(unsigned long long)s.sequence,(unsigned long long)s.beginTicks,(unsigned long long)s.endTicks);
    for(unsigned i=0;i<2;++i) {
        const auto &w=i?s.compositor:s.native;
        printf("GPU_LIVE_WORK scope=%s submitted=%llu completed=%llu failed=%llu shader_operations=%llu copy_bytes=%llu inplace_primitives=%llu scratch_bytes=%llu scratch_avoided_bytes=%llu ib_count=%llu ib_bytes=%llu ib_command_words=%llu\n",i?"compositor":"native",
            (unsigned long long)w.submitted,(unsigned long long)w.completed,(unsigned long long)w.failed,(unsigned long long)w.shaderOperations,
            (unsigned long long)w.framebufferCopyBytes,(unsigned long long)w.inplacePrimitives,(unsigned long long)w.scratchBytes,(unsigned long long)w.scratchAvoidedBytes,
            (unsigned long long)w.indirectBuffers,(unsigned long long)w.indirectBytes,(unsigned long long)w.indirectCommandWords);
    }
    GPU::Diagnostics::Snapshot metrics{};metrics.bytes=sizeof(metrics);
    metrics.timebaseSpanTicks=s.timebaseSpanTicks;metrics.timebaseSpanNanoseconds=s.timebaseSpanNanoseconds;
    metrics.poolHits=s.poolHits;metrics.poolAllocations=s.poolAllocations;metrics.poolCached=s.poolCached;metrics.poolCapacity=s.poolCapacity;
    (scope?metrics.compositor:metrics.native)=s.metrics;
    (scope?metrics.compositorCore:metrics.nativeCore)=s.core;
    (scope?metrics.compositorBusy:metrics.nativeBusy)=s.busy;
    auto gpu=CFDataCreate(kCFAllocatorDefault,reinterpret_cast<const UInt8 *>(&metrics),sizeof(metrics));
    auto as=CFDataCreate(kCFAllocatorDefault,reinterpret_cast<const UInt8 *>(&s.addressSpaces),sizeof(s.addressSpaces));
    if(gpu)statistics(gpu,int(scope));if(as)addressSpaceStatistics(as);
    if(gpu)CFRelease(gpu);if(as)CFRelease(as);
    if(!gpu || !as){puts("GPU_LIVE status=no-memory");return 1;}
    printf("GPU_LIVE_END sequence=%llu\n",(unsigned long long)s.sequence);
    hardwarePhases(s.sequence);mappingWork(s.sequence);resourceActivity(s.sequence);shaderObjectActivity(s.sequence);rasterStateActivity(s.sequence);const auto operationStatus=operationActivity(s.sequence,operationExamples,after);return operationExamples?operationStatus:0;
}

static void memoryStatistics(CFTypeRef value)
{
    GPU::Native::Memory::Snapshot s;
    if(CFGetTypeID(value)!=CFDataGetTypeID() || CFDataGetLength((CFDataRef)value)!=CFIndex(sizeof(s))) {
        puts("GPU_MEMORY status=unsupported-data");return;
    }
    memcpy(&s,CFDataGetBytePtr((CFDataRef)value),sizeof(s));
    if(s.version!=GPU::Native::Memory::Version || s.bytes!=sizeof(s) || s.reserved) {puts("GPU_MEMORY status=unsupported-version");return;}
    printf("GPU_MEMORY version=%u limits_valid=%u flags=0x%x clients=%llu resources=%llu native_bytes=%llu mapped=%llu mapped_bytes=%llu mapped_span=%llu table_pages=%llu\n",
        s.version,unsigned(GPU::Native::Memory::validLimits(s.limits)),s.flags,(unsigned long long)s.native.clients,
        (unsigned long long)s.native.resources,(unsigned long long)s.native.bytes,(unsigned long long)s.mappings.count,
        (unsigned long long)s.mappings.bytes,(unsigned long long)s.mappings.spanBytes,(unsigned long long)s.mappings.tablePages);
    printf("GPU_MEMORY_LIMITS resources_per_client=%u resources_per_job=%u global_resources=%u resource_bytes=%llu client_bytes=%llu global_bytes=%llu aperture_bytes=%llu mappings=%u tables=%u\n",
        s.limits.resourcesPerClient,s.limits.resourcesPerJob,s.limits.globalResources,(unsigned long long)s.limits.resourceBytes,
        (unsigned long long)s.limits.clientBytes,(unsigned long long)s.limits.globalBytes,(unsigned long long)s.limits.apertureBytes,
        s.limits.mappingSlots,s.limits.tablePages);
}

static void responsibleTaskStatistics(CFTypeRef value)
{
    GPU::Native::ResponsibleTaskInfo s;
    if(CFGetTypeID(value)!=CFDataGetTypeID() || CFDataGetLength((CFDataRef)value)!=CFIndex(sizeof(s))) {
        puts("GPU_TASK_IDS status=unsupported-data");return;
    }
    memcpy(&s,CFDataGetBytePtr((CFDataRef)value),sizeof(s));
    if(s.version!=GPU::Native::ResponsibleTaskVersion || s.count>GPU::Native::MaxResponsibleTaskIDs) {
        puts("GPU_TASK_IDS status=unsupported-version");return;
    }
    for(uint32_t i=s.count;i<GPU::Native::MaxResponsibleTaskIDs;++i)if(s.ids[i]) {
        puts("GPU_TASK_IDS status=invalid-padding");return;
    }
    printf("GPU_TASK_IDS scope=last-completed-pixel-job version=%u count=%u energy_accounting=0 ids=",s.version,s.count);
    for(uint32_t i=0;i<s.count;++i)printf("%s%u",i?",":"",s.ids[i]);
    putchar('\n');
}

static int shaderCounters()
{
    using namespace GPU::ShaderCounters;
    auto value=copyGPUProperty(CFSTR("GPUShaderCounters"));
    const auto *s=value && CFGetTypeID(value)==CFDataGetTypeID() && CFDataGetLength((CFDataRef)value)==sizeof(Snapshot)?
        reinterpret_cast<const Snapshot *>(CFDataGetBytePtr((CFDataRef)value)):nullptr;
    if(!s || s->version!=1 || s->count>Capacity || s->next>=Capacity){if(value)CFRelease(value);return 1;}
    puts("GPU_COUNTER_LAYOUT SP=busy,alu,efu,stall_tp,stall_uche,stall_rb,long_sync,short_sync TP=busy,stall_uche,l1_requests,l1_misses UCHE=busy,stall_vbif,read_beats_tp,read_beats_sp");
    for(unsigned i=0;i<s->count;++i) {
        const auto &r=s->records[(s->next+Capacity-s->count+i)%Capacity];
        printf("GPU_COUNTER retired=%llu sequence=%u index=%u fs=%016llx ticks=%llu values=",(unsigned long long)r.retired,r.sequence,r.index,(unsigned long long)r.hash,(unsigned long long)r.ticks);
        for(unsigned n=0;n<Count;++n)printf("%s%llu",n?",":"",(unsigned long long)r.values[n]);
        putchar('\n');
    }
    CFRelease(value);return 0;
}

static int registryInfo(bool statisticsOnly=false,bool failuresOnly=false)
{
    const char *classes[]={"QcomGPU","AppleARMIODevice","QcomRPM","QcomSPMI","QcomGCC","QcomGPUCC","QcomOSM","QcomSCM","QualcommWatchdogTimer"};
    const char *keys[]={"GPUStage","GPUStartupResult","GPUInitializationStage","GPUHardwareReady",
        "GPUFeatures","GPUBlockingRequirements","GPUPlatformLease","GPUNoCReady","GPUZAPReady",
        "GPUPlatformController","GPUPlatformOperation","GPUPlatformResult",
        "GPUAddressSpaceSelfTestResult","GPUAddressSpaceSelfTestPassed","IOGPUNativeSubmission",
        "GPUNativeJobsSubmitted","GPUNativeJobsCompleted","GPUNativeTicketsFailed",
        "GPUNativeShaderOperations","GPUCompositorShaderOperations","IOGPUNativeShaderBatchVersion",
        "IOGPUNativeMixedShaderBatchVersion",
        "GPUTranslationCursor","GPUFramebuffer2DMode","GPUSecondaryIBCache","GPUNativeFramebufferCopyBytes","GPUCompositorFramebufferCopyBytes",
        "GPUNativeFramebuffer2DBytes","GPUCompositorFramebuffer2DBytes",
        "GPUFramebufferXCrop","GPUNativeFramebufferUncroppedBytes","GPUCompositorFramebufferUncroppedBytes",
        "GPUFramebufferWord32",
        "GPUFramebufferFetchMode","GPUNativeFramebufferInplacePrimitives","GPUCompositorFramebufferInplacePrimitives",
        "GPUPipelinedAdmission","IOGPUNativePipelineVersion","IOGPUNativeCompletionNotificationVersion","IOGPUNativeSurfaceFenceVersion",
        "GPUSurfaceWaitSnapshot","GPUNativeSurfaceFencesCreated","GPUNativeSurfaceFencesDeferred","GPUNativeSurfaceFencesCompleted","GPUNativeSurfaceFencesFailed","GPUNativeQueuedAdmissionJobs","GPUCompositorQueuedAdmissionJobs",
        "GPUNativeAdmissionMaxDepth","GPUCompositorAdmissionMaxDepth",
        "GPUCPUReadDeferrals","GPUPipelineCapacityDeferrals",
        "GPURingPipeline","IOGPUNativeRingPipelineVersion","GPUNativeQueuedRingJobs","GPUCompositorQueuedRingJobs",
        "GPUNativeRingMaxDepth","GPUCompositorRingMaxDepth",
        "GPUNativeFramebufferScratchBytes","GPUCompositorFramebufferScratchBytes",
        "GPUNativeFramebufferScratchAvoidedBytes","GPUCompositorFramebufferScratchAvoidedBytes",
        "GPUFramebufferCompactScratch","GPUCompactIndirectBuffers",
        "GPUNativeFramebufferPrimitives","GPUCompositorFramebufferPrimitives",
        "GPUCompositorJobsSubmitted","GPUCompositorJobsCompleted","GPUCompositorTicketsFailed","GPUIRQCallbacks","GPUNativeAdmissionDeferred",
        "IOGPUNativeImmutableBlitParameters","IOGPUNativeBlitLayoutVersion","IOGPUNativeMaxPendingJobs","IOGPUNativeAdmissionBackpressureVersion",
        "GPUStatistics","GPUAddressSpaceStatistics","GPUPrivateTableCache","GPUMemoryStatistics","GPUCompletedResponsibleTaskIDs",
        "GPUMetalDiagnosticVersion","GPUMetalTimingVersion","GPUMetalShaderCaptureVersion","GPUMetalFailureCount","GPUMetalFailureReports",
        "SecureOSMInitialized","CPUAutomaticPerformanceAvailable","CPUAutomaticPerformanceStartStatus",
        "CPUPerformancePolicy","CPUPerformanceSamplingPeriodUS","CPUPerformanceLoweringDelayUS","CPUPerformanceQoSHoldUS",
        "FrequencyObservation","CPUVoltageOwner",
        "GPURecoveryAttempts","GPURecoveryCount","GPURecoveryStage","GPURecoveryResult",
        "GPUFaultStatus","GPUFaultIRQs","GPUFaultCPError","GPUFaultRptr","GPUFaultWptr",
        "GPUFaultSnapshotControl","GPUFaultSnapshotVersion","GPUFaultHLSQSnapshot",
        "GPUMapperFaultReason","GPUMapperFaultFSR","GPUMapperFaultFAR","GPUMapperFaultSyndrome",
        "WatchdogEnabled","WatchdogCaptureSeconds","WatchdogCaptureExpired","PanicWatchdogEnabled",
        "MetalPluginName","MetalPluginClassName","SecureFirmwareCapabilities","SecureFirmwareConvention",
        "qcom,gpu-handoff","qcom,gpu-gfx-handoff"};
    for(auto name:classes) {
        if((statisticsOnly || failuresOnly) && strcmp(name,"QcomGPU"))continue;
        mach_port_t iterator=0;unsigned found=0;
        auto status=IOServiceGetMatchingServices(0,IOServiceMatching(name),&iterator);
        if(status){printf("GPU_REGISTRY class=%s status=0x%x\n",name,status);continue;}
        for(unsigned count=0;count<256;++count) {
            const auto service=IOIteratorNext(iterator);if(!service)break;
            bool selected=strcmp(name,"AppleARMIODevice")!=0;
            if(!selected) {
                auto value=IORegistryEntryCreateCFProperty(service,CFSTR("compatible"),kCFAllocatorDefault,0);
                if(value && CFGetTypeID(value)==CFDataGetTypeID()) {
                    const auto data=CFDataGetBytePtr(static_cast<CFDataRef>(value));const auto size=CFDataGetLength(static_cast<CFDataRef>(value));
                    constexpr char gpu[]="qcom,adreno-540.1";
                    for(CFIndex offset=0;offset+CFIndex(sizeof(gpu))<=size;++offset)
                        if(!memcmp(data+offset,gpu,sizeof(gpu)))selected=true;
                }
                if(value)CFRelease(value);
            }
            if(selected) {
                printf("GPU_REGISTRY class=%s index=%u\n",name,found++);fflush(stdout);
                for(auto key:keys) {
                    if(failuresOnly && strncmp(key,"GPUMetalFailure",15) && strncmp(key,"GPUFault",8) &&
                        strncmp(key,"GPURecovery",11) && strcmp(key,"GPUNativeJobsSubmitted") &&
                        strcmp(key,"GPUNativeJobsCompleted") && strcmp(key,"GPUCompositorJobsSubmitted") &&
                        strcmp(key,"GPUCompositorJobsCompleted") && strcmp(key,"GPUNativeTicketsFailed") &&
                        strcmp(key,"GPUCompositorTicketsFailed"))continue;
                    if(statisticsOnly && strcmp(key,"GPUStatistics") && strcmp(key,"GPUAddressSpaceStatistics") &&
                        strcmp(key,"GPUPrivateTableCache") && strcmp(key,"GPUMemoryStatistics") &&
                        strcmp(key,"GPUCompletedResponsibleTaskIDs") && strncmp(key,"GPUMetal",8) &&
                        strcmp(key,"GPUTranslationCursor") && !strstr(key,"ShaderOperations") && !strstr(key,"Framebuffer") &&
                        strcmp(key,"IOGPUNativeShaderBatchVersion"))continue;
                    auto property=CFStringCreateWithCString(kCFAllocatorDefault,key,kCFStringEncodingUTF8);
                    auto value=IORegistryEntryCreateCFProperty(service,property,kCFAllocatorDefault,0);CFRelease(property);
                    if(value){if(!strcmp(key,"GPUStatistics"))statistics(value);
                        else if(!strcmp(key,"GPUAddressSpaceStatistics"))addressSpaceStatistics(value);
                        else if(!strcmp(key,"GPUMemoryStatistics"))memoryStatistics(value);
                        else if(!strcmp(key,"GPUCompletedResponsibleTaskIDs"))responsibleTaskStatistics(value);
                        else if(!strcmp(key,"GPUFaultHLSQSnapshot"))faultSnapshot(value);
                        else {printf("GPU_PROPERTY %s=",key);fflush(stdout);CFShow(value);}CFRelease(value);}
                }
            }
            IOObjectRelease(service);
        }
        IOObjectRelease(iterator);printf("GPU_REGISTRY class=%s count=%u\n",name,found);
    }
    return 0;
}

// Compiled here for later, explicit target use. The build never executes it.
static kern_return_t nativeFillTest(GPU::NativeConnection &gpu,bool batch=false,bool events=false)
{
    GPU::Native::QueueData queue{};
    GPU::Native::ResourceData resources[GPU::Native::Memory::ResourcesPerJob]{};
    const unsigned resourceCount=batch?GPU::Native::Memory::ResourcesPerJob:1;
    GPU::Native::ShmemData shmem{};
    GPU::Native::NotificationData notifications{};
    mach_port_t notificationPort=MACH_PORT_NULL;
    unsigned wakeups=0;
    auto r = gpu.newQueue("GPU bring-up", queue);
    if (!r) r = gpu.newNotificationQueue(64, notifications);
    if (!r) r = gpu.setNotificationQueue(queue.id, notifications.id);
    if (!r && events) {
        notificationPort=IODataQueueAllocateNotificationPort();
        if(!notificationPort)r=kIOReturnNoResources;
        if(!r && gpu.setNotificationPort(UINT32_MAX,notificationPort)!=kIOReturnBadArgument)r=kIOReturnIOError;
        if(!r)r=gpu.setNotificationPort(notifications.id,notificationPort);
        if(!r && gpu.setNotificationPort(notifications.id,notificationPort)!=kIOReturnInternalError)r=kIOReturnIOError;
    }
    for(unsigned i=0;i<resourceCount && !r;++i)r=gpu.newResource(GPU::PageBytes,resources[i]);
    if (!r) r = gpu.newShmem(GPU::PageBytes, shmem);
    GPU::Command commands[GPU::Native::Memory::ResourcesPerJob]{};
    for(unsigned i=0;i<resourceCount;++i)commands[i]={GPU::Fill32,0,resources[i].id,0,256,0x12345678u+i};
    if(!r)r=gpu.writeCommands(shmem,commands,resourceCount);
    const uint64_t submitted = !r ? mach_absolute_time() : 0;
    uint64_t submitTicks = 0;
    GPU::Native::CompletionWindow completions;
    auto submit = [&](uint64_t start, uint64_t end) {
        const auto begin = mach_absolute_time();
        auto result = gpu.submit(queue.id, shmem.id, start, end);
        if(!result && !completions.admit(start,end))result=kIOReturnIOError;
        submitTicks += mach_absolute_time() - begin;
        return result;
    };
    if (!r) r = submit(1, 2);
    if (!r && batch) {
        for(unsigned i=0;i<resourceCount;++i){commands[i].offset=256;commands[i].value=0x87654321u+i;}
        r=gpu.writeCommands(shmem,commands,resourceCount);
        if (!r) r = submit(3, 4);
    }
    const auto waitBegin = !r ? mach_absolute_time() : 0;
    if(!r && events) {
        mach_timebase_info_data_t clock{};r=mach_timebase_info(&clock);
        if(!r && !clock.denom)r=kIOReturnIOError;
        while(!r && completions.pending()) {
            const auto elapsedNS=(mach_absolute_time()-waitBegin)*clock.numer/clock.denom;
            if(elapsedNS>=2000000000ULL){r=kIOReturnTimeout;break;}
            bool received=false;
            r=GPU::NativeConnection::receiveNotification(notificationPort,uint32_t((2000000000ULL-elapsedNS)/1000000+1),received);
            if(!r && !received)r=kIOReturnTimeout;
            if(!r){++wakeups;r=gpu.pollCompletions(notifications,completions);}
        }
    } else {
        if (!r) r = gpu.waitCompletion(notifications,completions,2);
        if (!r && batch) r=gpu.waitCompletion(notifications,completions,4);
    }
    const uint64_t elapsed = !r ? mach_absolute_time()-submitted : 0;
    const uint64_t waitTicks = !r ? mach_absolute_time()-waitBegin : 0;
    for(unsigned n=0;n<resourceCount && !r;++n) {
        if(!resources[n].cpuAddress){r=kIOReturnNotReady;break;}
        uint32_t values[128];__atomic_thread_fence(__ATOMIC_ACQUIRE);
        memcpy(values,reinterpret_cast<const void *>(uintptr_t(resources[n].cpuAddress)),sizeof(values));
        for(unsigned i=0;i<(batch?128u:64u);++i)
            if(values[i]!=(i<64?0x12345678u+n:0x87654321u+n)){r=kIOReturnIOError;break;}
    }
    if (queue.id) { auto status = gpu.deleteQueue(queue.id); if (!r) r = status; }
    if (notifications.id) { auto status = gpu.deleteNotificationQueue(notifications.id); if (!r) r = status; }
    if(events && !r && gpu.setNotificationPort(notifications.id,notificationPort)!=kIOReturnBadArgument)r=kIOReturnIOError;
    if(notificationPort)mach_port_mod_refs(mach_task_self(),notificationPort,MACH_PORT_RIGHT_RECEIVE,-1);
    if (shmem.id) { auto status = gpu.deleteShmem(shmem.id); if (!r) r = status; }
    for(auto &resource:resources)if(resource.id){auto status=gpu.deleteResource(resource.id);if(!r)r=status;}
    if (!r) puts(batch?"Native 64 resource job admissions, immutable command snapshots and two hardware end tokens verified.":
        "Native IOGPU resource/queue submission and CP fence verified.");
    if(events)printf("GPU_NOTIFICATION_TEST status=0x%x wakeups=%u event_only=1 token_retired=%llu invalid_id_duplicate_deleted_checked=1\n",
        r,wakeups,(unsigned long long)completions.retired());
    if(!r) {
        mach_timebase_info_data_t timebase{};
        if(mach_timebase_info(&timebase)==KERN_SUCCESS && timebase.denom)
            printf("GPU_COMPLETION jobs=%u submit_to_retire_ms=%.3f submit_calls_ms=%.3f notification_wait_ms=%.3f resources=%u\n",batch?2u:1u,
                double(elapsed)*timebase.numer/timebase.denom/1000000.0,
                double(submitTicks)*timebase.numer/timebase.denom/1000000.0,
                double(waitTicks)*timebase.numer/timebase.denom/1000000.0,resourceCount);
    }
    return r;
}

static kern_return_t fillTest(GPU::Connection &gpu)
{
    GPU::Info info{}; auto r = gpu.info(info);
    if (r) return r;
    if (!(info.features & GPU::HardwareSubmit)) return kIOReturnNotReady;
    uint64_t context = 0, queue = 0, target = 0, commandBuffer = 0, allocated = 0, fence = 0;
    r = gpu.createContext(context);
    if (!r) r = gpu.createQueue(context, queue);
    if (!r) r = gpu.createBuffer(context, GPU::PageBytes, target, allocated);
    if (!r) r = gpu.createBuffer(context, GPU::PageBytes, commandBuffer, allocated);
    GPU::Command command{GPU::Fill32,0,target,0,256,0x12345678};
    if (!r) r = gpu.write(commandBuffer, 0, &command, sizeof(command));
    if (!r) r = gpu.submit(queue, commandBuffer, 0, sizeof(command), fence);
    bool complete = false;
    if (!r) for (unsigned i = 0; i < 100; ++i) {
        r = gpu.fence(queue, fence, complete);
        if (r || complete) break;
        usleep(10000);
    }
    if (!r && !complete) r = kIOReturnTimeout;
    uint32_t result[64] = {};
    if (!r) r = gpu.read(target, 0, result, sizeof(result));
    if (!r) for (auto value : result) if (value != command.value) { r = kIOReturnIOError; break; }
    if (commandBuffer) { auto cleanup = gpu.destroyBuffer(commandBuffer); if (!r) r = cleanup; }
    if (target) { auto cleanup = gpu.destroyBuffer(target); if (!r) r = cleanup; }
    if (queue) { auto cleanup = gpu.destroyQueue(queue); if (!r) r = cleanup; }
    if (context) { auto cleanup = gpu.destroyContext(context); if (!r) r = cleanup; }
    if (!r) puts("GPU CP fill and hardware fence verified.");
    return r;
}

static kern_return_t resources(GPU::Connection &gpu)
{
    uint64_t context = 0, queue = 0, buffer = 0, allocated = 0;
    auto r = gpu.createContext(context);
    if (!r) r = gpu.createQueue(context, queue);
    if (!r) r = gpu.createBuffer(context, GPU::PageBytes, buffer, allocated);
    const char pattern[] = "GPU host resource round trip";
    char copied[sizeof(pattern)] = {};
    if (!r) r = gpu.write(buffer, 4, pattern, sizeof(pattern));
    if (!r) r = gpu.read(buffer, 4, copied, sizeof(copied));
    if (!r && memcmp(pattern, copied, sizeof(pattern))) r = KERN_FAILURE;
    // Always release the full graph, even if a previous operation failed.
    if (buffer) { auto cleanup = gpu.destroyBuffer(buffer); if (!r) r = cleanup; }
    if (queue) { auto cleanup = gpu.destroyQueue(queue); if (!r) r = cleanup; }
    if (context) { auto cleanup = gpu.destroyContext(context); if (!r) r = cleanup; }
    if (!r) puts("Host context/queue/buffer round trip passed; no GPU commands issued.");
    return r;
}
int main(int argc, char **argv)
{
    if(argc==2 && !strcmp(argv[1],"cpu-status"))return CPUStatus::run(0);
    if(argc==3 && !strcmp(argv[1],"cpu-sample")) {
        char *end=nullptr;errno=0;const auto seconds=strtoul(argv[2],&end,10);
        if(errno || !argv[2][0] || argv[2][0]=='-' || !end || *end || !seconds || seconds>60)return 2;
        return CPUStatus::run(unsigned(seconds));
    }
    if(argc==4 && !strcmp(argv[2],"--after-ticks")) {
        char *end=nullptr;errno=0;const auto after=strtoull(argv[3],&end,10);
        if(errno || !argv[3][0] || argv[3][0]=='-' || !end || *end)return 2;
        if(!strcmp(argv[1],"job-history"))return jobHistory(after);
        if(!strcmp(argv[1],"metal-timing"))return metalTiming(after);
        if(!strcmp(argv[1],"operation-profile"))return liveStatistics(1,true,after);
        return 2;
    }
    if(argc==2 && !strcmp(argv[1],"restore-bootstrap"))return restoreBootstrap();
    if(argc==2 && !strcmp(argv[1],"kernel-log"))return kernelLog();
    if(argc==3 && !strcmp(argv[1],"kernel-log") && !strcmp(argv[2],"display"))return kernelLog(true);
    if(argc==2 && !strcmp(argv[1],"shell-info"))return shellInfo();
    if(argc==2 && !strcmp(argv[1],"registry"))return registryInfo();
    if(argc==2 && !strcmp(argv[1],"failures"))return registryInfo(false,true);
    if(argc==2 && !strcmp(argv[1],"stats"))return registryInfo(true);
    if(argc==2 && !strcmp(argv[1],"stats-live"))return liveStatistics();
    if(argc==2 && !strcmp(argv[1],"metal-timing"))return metalTiming();
    if(argc==2 && !strcmp(argv[1],"job-history"))return jobHistory();
    if(argc==2 && !strcmp(argv[1],"operation-profile"))return liveStatistics(1,true);
    if(argc==2 && !strcmp(argv[1],"shader-counters"))return shaderCounters();
    if(argc==3 && !strcmp(argv[1],"stats-live") && !strcmp(argv[2],"native"))return liveStatistics(0);
    if (argc == 2 && !strcmp(argv[1], "reboot")) {
        auto request = reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "reboot"));
        if (getuid() || !request) return 1;
        puts("GPU_REBOOT requesting native orderly reboot"); fflush(stdout); sync();
        int result = request(0); if (result) perror("GPU_REBOOT"); return 1;
    }
    if (argc == 2 && (!strcmp(argv[1], "native-fill-test") || !strcmp(argv[1],"native-async-test") || !strcmp(argv[1],"native-notify-test"))) {
        GPU::NativeConnection gpu; auto r = gpu.open();
        if (!r) r = nativeFillTest(gpu,strcmp(argv[1],"native-fill-test")!=0,!strcmp(argv[1],"native-notify-test"));
        if (r) fprintf(stderr, "Native IOGPU submission failed: 0x%x\n", r);
        return r ? 1 : 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "native-info") || !strcmp(argv[1], "diagnose"))) {
        if(!strcmp(argv[1],"diagnose")) {registryInfo();fflush(stdout);}
        GPU::NativeConnection gpu; auto r = gpu.open();
        GPU::NativeInfo info{};
        if (!r) r = gpu.info(info);
        if (!r) printf("IOGPUFamily device: %s; stamp channels=%u\n",
            info.name, info.stampCount);
        else fprintf(stderr, "Native IOGPU query failed: 0x%x\n", r);
        return r ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--schema")) {
        puts("protocol=2; connection=0x47505502; Submit accepts Command[1..64] with Fill32 or Barrier; GPU addresses and raw PM4 are rejected; hardware submission requires the retained-firmware/IOMapper handoff");
        return 0;
    }
    if (argc != 2 || (strcmp(argv[1], "info") && strcmp(argv[1], "resources") && strcmp(argv[1], "fill-test"))) {
        fputs("usage: gpuctl --schema | registry | stats | stats-live | metal-timing | job-history | operation-profile | diagnose | shell-info | cpu-status | cpu-sample SECONDS | kernel-log [display] | restore-bootstrap | native-info | native-fill-test | native-async-test | native-notify-test | info | resources | fill-test | reboot\n", stderr); return 2;
    }
    GPU::Connection gpu; auto r = gpu.open();
    if (!r && !strcmp(argv[1], "fill-test")) r = fillTest(gpu);
    else if (!r && !strcmp(argv[1], "resources")) r = resources(gpu);
    else if (!r) {
        GPU::Info info{}; r = gpu.info(info);
        if (!r) printf("protocol=%llu chip=0x%llx GMEM=%llu features=0x%llx state=%llu blockers=0x%llx\n",
            info.version, info.chip, info.gmemBytes, info.features, info.state, info.blockers);
    }
    if (r) fprintf(stderr, "GPU request failed: 0x%x\n", r);
    return r ? 1 : 0;
}
