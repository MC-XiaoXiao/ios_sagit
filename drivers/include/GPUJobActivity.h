// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUNativeProtocol.h>
#include <GPUDiagnostics.h>
namespace GPU { namespace JobActivity {
constexpr char HistoryProperty[]="GPUJobHistory",ResourceProperty[]="GPUResourceActivity";
constexpr char ShaderObjectProperty[]="GPUShaderObjectActivity";
constexpr char RasterStateProperty[]="GPURasterStateActivity";
constexpr uint32_t Version=4,Capacity=2048,SizeClasses=6,ResourceTypes=3;
enum Flags : uint32_t { Compositor=1,Successful=2,HardwareInterval=4,SurfaceOverflow=8,Admitted=16 };
struct Record {
    uint64_t ordinal=0,prepareBegin=0,admitted=0,submitted=0,observed=0,retireBegin=0,retireEnd=0;
    uint64_t gpuBegin=0,gpuEnd=0;
    uint32_t sequence=0,stamp=0,kinds=0,flags=0,shaderOperations=0,ringDepth=0;
    uint32_t writtenSurfaceCount=0,writtenSurfaces[4]{};
    uint32_t reserved=0;
    // CPU preparation intervals. Index details are nested in encoding, so
    // they must not be added to it when reconstructing a critical path.
    uint64_t preflightTicks=0,indexAcquireTicks=0,indexSnapshotTicks=0,encodingTicks=0,addressSpaceTicks=0,fenceTicks=0;
    // Draw subintervals are nested in encodingTicks.
    uint64_t drawAttachmentsTicks=0,drawIndicesTicks=0,drawBindingsTicks=0,drawAliasesTicks=0,drawEmitTicks=0;
    uint32_t pid=0,indexBytes=0;
    uint32_t gmemTiles=0,gmemSamples=0;
    uint64_t gmemLoadTicks=0,gmemDrawTicks=0,gmemStoreTicks=0;
};
struct History {
    uint32_t version=Version,bytes=sizeof(History),capacity=Capacity,count=0,next=0,reserved=0;
    uint64_t sequence=0,total=0,timebaseTicks=1000000,timebaseNs=0;
    Record records[Capacity]{};
    void append(Record record) {
        if(total==UINT64_MAX)return;
        record.ordinal=++total;records[next]=record;next=(next+1)%Capacity;
        if(count<Capacity)++count;
    }
    const Record &oldest(unsigned index) const {return records[(next+Capacity-count+index)%Capacity];}
};
// Successful native factory calls grouped by the actual returned allocation
// size, plus failed requests by requested size. Not unique backing lifetime
// counts: repeated IOSurface imports may refer to the same cached mapping.
struct ResourceBin {uint64_t calls=0,failures=0,bytes=0,ticks=0;};
inline unsigned sizeClass(uint64_t bytes) {
    constexpr uint64_t ends[]={4096,16384,65536,262144,1048576};
    for(unsigned i=0;i<SizeClasses-1;++i)if(bytes<=ends[i])return i;
    return SizeClasses-1;
}
inline unsigned resourceType(uint32_t type) {
    return type==Native::Buffer?0:type==Native::ClientBuffer?1:type==Native::IOSurface?2:ResourceTypes;
}
struct ResourceMetrics {
    ResourceBin bins[ResourceTypes][SizeClasses]{};
    uint64_t invalid=0;
    void sample(uint32_t type,uint64_t bytes,uint64_t begin,uint64_t end,bool success) {
        const auto group=resourceType(type);
        if(group>=ResourceTypes || end<begin){Diagnostics::add(invalid);return;}
        auto &bin=bins[group][sizeClass(bytes)];Diagnostics::add(bin.calls);
        Diagnostics::add(bin.ticks,end-begin);
        if(success)Diagnostics::add(bin.bytes,bytes);else Diagnostics::add(bin.failures);
    }
};
struct Resources {
    uint32_t version=Version,bytes=sizeof(Resources),types=ResourceTypes,classes=SizeClasses;
    uint64_t sequence=0,timebaseTicks=1000000,timebaseNs=0;
    ResourceMetrics native{},compositor{};
    void sample(bool ui,uint32_t type,uint64_t bytes,uint64_t begin,uint64_t end,bool success) {
        native.sample(type,bytes,begin,end,success);
        if(ui)compositor.sample(type,bytes,begin,end,success);
    }
};
// Committed ISA copies avoided in physically successful retired jobs.
// Does not count GPU instruction-cache hits or direct LOAD_STATE payloads.
struct ShaderObjectMetrics {uint64_t reused=0,avoidedBytes=0;};
struct ShaderObjects {
    uint32_t version=Version,bytes=sizeof(ShaderObjects);
    uint64_t sequence=0;
    ShaderObjectMetrics native{},compositor{};
};
static_assert(sizeof(ShaderObjects)==48,"shader object optional property ABI");
// Successful physical retirements only. Counts actual full state setups,
// primitives retaining that state in the same draw/IB, and partial VS loads.
struct RasterStateMetrics {uint64_t fullSetups=0,reusedPrimitives=0,driverUpdates=0;};
struct RasterStates {
    uint32_t version=Version,bytes=sizeof(RasterStates);
    uint64_t sequence=0;
    RasterStateMetrics native{},compositor{};
};
static_assert(sizeof(RasterStates)==64,"raster state optional property ABI");
static_assert(sizeof(Record)==248 && sizeof(History)==507960 && sizeof(Resources)==1208,"job activity optional properties ABI");
} }
