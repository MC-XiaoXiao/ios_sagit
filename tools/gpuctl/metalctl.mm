// SPDX-License-Identifier: BSD-2-Clause
#import "QcomMTL.h"
#import "NativeMetal.h"
#import <IOSurface/IOSurfaceRef.h>
#import <QuartzCore/CARenderer.h>
#import <QuartzCore/CALayer.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMediaTiming.h>
#include "IOKitUser.h"
#include "GPUDiagnostics.h"
#include "GPUNativeProtocol.h"
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string>
#include <initializer_list>
#include <cmath>
#include <sys/mman.h>
#include <unistd.h>
#import <objc/runtime.h>
#include <mach-o/loader.h>

@interface NSObject (NativeFluidityAssets)
- (id)initWithDevice:(id<MTLDevice>)device;
- (id<MTLTexture>)newTextureWithContentsOfURL:(NSURL *)url options:(NSDictionary *)options error:(NSError **)error;
@end

@protocol QcomRetiredCleanupDiagnostics
- (NSDictionary *)qcomRetiredCleanupStatistics;
- (void)qcomWaitForRetiredCleanup;
@end
@protocol QcomMetalDiagnosticConfiguration
- (BOOL)qcomMetalDiagnosticsEnabled;
@end

static bool commandReferences(id<MTLDevice> device);

static bool waitForStatus(id<MTLCommandBuffer> buffer,MTLCommandBufferStatus status) {
    for(unsigned n=0;n<5000;++n) {
        if(buffer.status>=status)return buffer.status!=MTLCommandBufferStatusError;
        usleep(1000);
    }
    fprintf(stderr,"Retirement status timeout: got=%lu wanted=%lu\n",(unsigned long)buffer.status,(unsigned long)status);return false;
}

static int retiredCleanup(id<MTLDevice> device) {
    auto queue=[device newCommandQueueWithMaxCommandBufferCount:32];
    if(![(id)queue respondsToSelector:@selector(qcomRetiredCleanupStatistics)])return 1;
    auto diagnostics=(id<QcomRetiredCleanupDiagnostics>)queue;
    auto entered=dispatch_semaphore_create(0),release=dispatch_semaphore_create(0),callback=dispatch_semaphore_create(0);
    auto counts=[NSLock new];__block unsigned freed=0,handlers=0;__block BOOL wrongOrder=NO,releaseTimedOut=NO;
    auto output=[device newBufferWithLength:256 options:MTLResourceStorageModeShared];if(!output.contents)return 1;
    memset(output.contents,0xa5,output.length);
    id<MTLCommandBuffer> commands[10];commands[0]=[queue commandBuffer];
    @autoreleasepool {
        const NSUInteger length=NSUInteger(getpagesize());
        void *mapping=mmap(nullptr,length,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
        if(mapping==MAP_FAILED)return 1;memset(mapping,0x37,length);
        auto source=[device newBufferWithBytesNoCopy:mapping length:length options:MTLResourceStorageModeShared
            deallocator:^(void *bytes,NSUInteger size) {
                dispatch_semaphore_signal(entered);
                BOOL timeout=dispatch_semaphore_wait(release,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC))!=0;
                munmap(bytes,size);[counts lock];++freed;releaseTimedOut=timeout;[counts unlock];
            }];
        if(!source){munmap(mapping,length);return 1;}
        auto encoder=[commands[0] blitCommandEncoder];if(!encoder){dispatch_semaphore_signal(release);return 1;}
        [encoder copyFromBuffer:source sourceOffset:0 toBuffer:output destinationOffset:0 size:16];[encoder endEncoding];
        [commands[0] addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            [counts lock];wrongOrder|=handlers!=0 || cb.status!=MTLCommandBufferStatusCompleted;++handlers;[counts unlock];
            dispatch_semaphore_signal(callback);
        }];
        [commands[0] commit];
    }
    bool ok=false;
    @try {
        if(dispatch_semaphore_wait(entered,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC)))return 1;
        // The first destructor is deliberately held after native unmapping.
        // Completion and later physical GPU writes must continue independently.
        if(!waitForStatus(commands[0],MTLCommandBufferStatusCompleted) ||
            dispatch_semaphore_wait(callback,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC)))return 1;
        for(unsigned i=0;i<16;++i)if(static_cast<uint8_t *>(output.contents)[i]!=0x37)return 1;
        for(unsigned index=1;index<10;++index) @autoreleasepool {
            commands[index]=[queue commandBuffer];if(!commands[index])return 1;
            auto encoder=[commands[index] blitCommandEncoder];if(!encoder)return 1;
            [encoder fillBuffer:output range:NSMakeRange(index*16,16) value:uint8_t(index)];[encoder endEncoding];
            [commands[index] addCompletedHandler:^(id<MTLCommandBuffer> cb) {
                [counts lock];wrongOrder|=handlers!=index || cb.status!=MTLCommandBufferStatusCompleted;++handlers;[counts unlock];
            }];
            [commands[index] commit];
        }
        if(!waitForStatus(commands[7],MTLCommandBufferStatusCompleted))return 1;
        NSDictionary *full=nil;
        for(unsigned n=0;n<5000;++n) {
            full=[diagnostics qcomRetiredCleanupStatistics];
            if([full[@"budget_waits"] unsignedLongLongValue])break;
            usleep(1000);
        }
        if([full[@"pending"] unsignedIntValue]!=8 || ![full[@"budget_waits"] unsignedLongLongValue] ||
            commands[8].status>=MTLCommandBufferStatusCompleted || commands[9].status>=MTLCommandBufferStatusCompleted)return 1;
        printf("METAL_CLEANUP phase=blocked pending=%u bytes=%llu budget_waits=%llu later_completions=7\n",
            [full[@"pending"] unsignedIntValue],[full[@"pending_bytes"] unsignedLongLongValue],[full[@"budget_waits"] unsignedLongLongValue]);
        dispatch_semaphore_signal(release);
        for(auto command:commands)if(!waitForStatus(command,MTLCommandBufferStatusCompleted))return 1;
        for(auto command:commands)[command waitUntilCompleted];
        [diagnostics qcomWaitForRetiredCleanup];
        for(unsigned index=1;index<10;++index)for(unsigned i=0;i<16;++i)
            if(static_cast<uint8_t *>(output.contents)[index*16+i]!=index)return 1;
        NSDictionary *after=[diagnostics qcomRetiredCleanupStatistics];
        [counts lock];ok=freed==1 && handlers==10 && !wrongOrder && !releaseTimedOut;[counts unlock];
        if([after[@"pending"] unsignedIntValue] || [after[@"pending_bytes"] unsignedLongLongValue] || [after[@"exclusive"] boolValue])ok=false;
        printf("METAL_CLEANUP phase=drained runs=%llu peak=%u peak_bytes=%llu release_ns=%llu max_release_ns=%llu queue_ns=%llu callbacks=%u deallocators=%u ok=%u\n",
            [after[@"runs"] unsignedLongLongValue],[after[@"peak_pending"] unsignedIntValue],[after[@"peak_bytes"] unsignedLongLongValue],
            [after[@"release_ns"] unsignedLongLongValue],[after[@"max_release_ns"] unsignedLongLongValue],[after[@"queue_ns"] unsignedLongLongValue],handlers,freed,ok);
    } @finally {dispatch_semaphore_signal(release);}
    if(!ok || !commandReferences(device))return 1;
    puts("Metal cleanup: real GPU readback, independent completion, bounded backpressure, FIFO handlers and exactly-once client release passed");return 0;
}

static NSString *driverBundlePath() {
    NSString *executable = NSBundle.mainBundle.executablePath ?: NSProcessInfo.processInfo.arguments[0];
    NSString *adjacent = [executable.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"QcomMTLDriver.bundle"];
    if ([NSFileManager.defaultManager fileExistsAtPath:[adjacent stringByAppendingPathComponent:@"QcomMTLDriver"]])
        return adjacent;
    return @"/System/Library/Extensions/QcomMTLDriver.bundle";
}
static id<MTLLibrary> diagnosticLibrary(id<MTLDevice> device) {
    NSError *error=nil;
    auto url=[NSURL fileURLWithPath:[driverBundlePath() stringByAppendingPathComponent:@"default.qmetallib"]];
    auto library=[device newLibraryWithURL:url error:&error];
    if(!library)fprintf(stderr,"Diagnostic library: %s\n",error.description.UTF8String);
    return library;
}

// Keep the system discovery path free of an LC_LOAD_DYLIB for our bundle.
// The explicit factory remains available when the bundle is beside this tool.
static id<MTLDevice> explicitDevice(NSError **error) {
    NSString *path = [driverBundlePath() stringByAppendingPathComponent:@"QcomMTLDriver"];
    void *module = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
    typedef id<MTLDevice> (*Factory)(NSError **) NS_RETURNS_RETAINED;
    auto factory = module ? reinterpret_cast<Factory>(dlsym(module, "QcomMTLCreateSystemDefaultDevice")) : nullptr;
    if (!factory) {
        const char *message = dlerror();
        if (error) *error = [NSError errorWithDomain:@"org.ios2arm.graphics.QcomMTLDriver" code:1
            userInfo:@{NSLocalizedDescriptionKey:message ? [NSString stringWithUTF8String:message] : @"Metal factory is missing"}];
        return nil;
    }
    // ObjC classes and returned resources may outlive the factory call.
    return factory(error);
}

static bool completed(id<MTLCommandBuffer> commands) {
    if (!commands) return false;
    [commands commit]; [commands waitUntilCompleted];
    if (commands.status == MTLCommandBufferStatusCompleted) return true;
    fprintf(stderr, "%s\n", commands.error.localizedDescription.UTF8String); return false;
}

#include "AIRNIROutputDiagnostics.h"
#include "RenderClearDiagnostics.h"
#include "GMEMDiagnostics.h"

// Separate shader launch from SSBO access before the larger pipeline exercise.
// Both stages use the public Metal JIT and real command-buffer completion.
static int shaderProbe(id<MTLDevice> device, unsigned begin=0, unsigned end=2) {
    NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void probe_empty() {}\n"
        "kernel void probe_store(device uint *output [[buffer(0)]]) { output[0] = 0x1234abcdu; }\n";
    auto library=[device newLibraryWithSource:source options:nil error:&error];
    if(!library) {fprintf(stderr,"GPU_SHADER_PROBE compile failed: %s\n",error.localizedDescription.UTF8String);return 1;}
    auto queue=[device newCommandQueue];if(!queue)return 1;
    for(unsigned phase=begin;phase<end;++phase) {
        const char *name=phase?"probe_store":"probe_empty";
        auto function=[library newFunctionWithName:[NSString stringWithUTF8String:name]];
        auto pipeline=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
        if(!pipeline) {fprintf(stderr,"GPU_SHADER_PROBE phase=%s pipeline failed: %s\n",name,error.localizedDescription.UTF8String);return 1;}
        id<MTLBuffer> output=phase?[device newBufferWithLength:64 options:MTLResourceStorageModeShared]:nil;
        if(phase && (!output || !output.contents))return 1;
        if(output)memset(output.contents,0xa5,64);
        auto commands=[queue commandBuffer];
        auto encoder=[commands computeCommandEncoder];if(!encoder)return 1;
        [encoder setComputePipelineState:pipeline];
        if(output)[encoder setBuffer:output offset:0 atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        [encoder endEncoding];
        printf("GPU_SHADER_PROBE phase=%s submitted\n",name);fflush(stdout);
        if(!completed(commands)) {printf("GPU_SHADER_PROBE phase=%s failed\n",name);return 1;}
        if(output)for(unsigned i=0;i<16;++i) {
            const uint32_t expected=i?0xa5a5a5a5u:0x1234abcdu;
            if(static_cast<const uint32_t *>(output.contents)[i]!=expected) {
                fprintf(stderr,"GPU_SHADER_PROBE phase=%s readback mismatch at %u\n",name,i);return 1;
            }
        }
        printf("GPU_SHADER_PROBE phase=%s completed readback=%u\n",name,phase);fflush(stdout);
    }
    return 0;
}

static int renderProbe(id<MTLDevice> device) {
    NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 probe_vertex(uint i [[vertex_id]]) { if (i == 0u) return float4(-1.0,-1.0,0.0,1.0); if (i == 1u) return float4(3.0,-1.0,0.0,1.0); return float4(-1.0,3.0,0.0,1.0); }\n"
        "fragment float4 probe_fragment() { return float4(1.0,0.0,0.0,1.0); }\n";
    auto library=[device newLibraryWithSource:source options:nil error:&error];
    if(!library) {fprintf(stderr,"GPU_RENDER_PROBE compile failed: %s\n",error.localizedDescription.UTF8String);return 1;}
    auto vertex=[library newFunctionWithName:@"probe_vertex"],fragment=[library newFunctionWithName:@"probe_fragment"];
    if(!vertex || !fragment)return 1;
    auto descriptor=[MTLRenderPipelineDescriptor new];descriptor.vertexFunction=vertex;descriptor.fragmentFunction=fragment;
    descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    auto pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if(!pipeline) {fprintf(stderr,"GPU_RENDER_PROBE pipeline failed: %s\n",error.localizedDescription.UTF8String);return 1;}
    auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
    auto texture=[device newTextureWithDescriptor:td];auto queue=[device newCommandQueue];if(!texture || !queue)return 1;
    uint8_t pixels[64*64*4];memset(pixels,0xa5,sizeof(pixels));
    [texture replaceRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0 withBytes:pixels bytesPerRow:256];
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
    pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    auto commands=[queue commandBuffer];auto encoder=[commands renderCommandEncoderWithDescriptor:pass];if(!encoder)return 1;
    [encoder setRenderPipelineState:pipeline];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
    puts("GPU_RENDER_PROBE submitted ssbo=0 load=DontCare");fflush(stdout);
    if(!completed(commands))return 1;
    [texture getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
    unsigned red=0,unchanged=0,other=0,firstBad=64*64;
    for(unsigned i=0;i<64*64;++i) {
        bool expected=pixels[i*4]==255 && !pixels[i*4+1] && !pixels[i*4+2] && pixels[i*4+3]==255;
        bool sentinel=pixels[i*4]==0xa5 && pixels[i*4+1]==0xa5 && pixels[i*4+2]==0xa5 && pixels[i*4+3]==0xa5;
        if(expected)++red;else {if(firstBad==64*64)firstBad=i;if(sentinel)++unchanged;else ++other;}
    }
    printf("GPU_RENDER_PROBE pixels red=%u unchanged=%u other=%u\n",red,unchanged,other);
    if(firstBad!=64*64) {
        auto i=firstBad;fprintf(stderr,"GPU_RENDER_PROBE mismatch pixel=%u rgba=%u,%u,%u,%u\n",i,pixels[i*4],pixels[i*4+1],pixels[i*4+2],pixels[i*4+3]);return 1;
    }
    puts("GPU_RENDER_PROBE completed readback_pixels=4096 rgba=255,0,0,255");return 0;
}

struct RenderCounts { uint64_t submitted=0,completed=0,failed=0; };
static bool nativeRenderCounts(id<MTLDevice> device,RenderCounts &counts,
    GPU::Diagnostics::Snapshot *diagnostic=nullptr,bool *mixed=nullptr,bool includeCompute=false) {
    mach_port_t iterator=MACH_PORT_NULL;
    if(IOServiceGetMatchingServices(MACH_PORT_NULL,IOServiceMatching("QcomGPU"),&iterator))return false;
    bool found=false;mach_port_t service;
    while((service=IOIteratorNext(iterator))) {
        uint64_t registry=0;
        if(!IORegistryEntryGetRegistryEntryID(service,&registry) && registry==device.registryID) {
            if(includeCompute) {
                mach_port_t connection=MACH_PORT_NULL;GPU::Diagnostics::LiveSnapshot live{};
                uint64_t scope=0;uint32_t scalarCount=0;size_t size=sizeof(live);
                auto status=IOServiceOpen(service,mach_task_self(),1,&connection);
                if(!status)status=IOConnectCallMethod(connection,GPU::Diagnostics::LiveSelector,&scope,1,nullptr,0,
                    nullptr,&scalarCount,&live,&size);
                if(connection)IOServiceClose(connection);
                found=!status && !scalarCount && size==sizeof(live) && live.version==GPU::Diagnostics::LiveVersion &&
                    live.bytes==sizeof(live) && !live.scope && !live.reserved;
                if(found) {
                    const auto &k=live.metrics.kinds;
                    counts={k.submitted[3]+k.submitted[2],k.completed[3]+k.completed[2],k.failed[3]+k.failed[2]};
                }
                IOObjectRelease(service);break;
            }
            CFTypeRef value=IORegistryEntryCreateCFProperty(service,CFSTR("GPUStatistics"),nullptr,0);
            GPU::Diagnostics::Snapshot snapshot{};
            if(value && CFGetTypeID(value)==CFDataGetTypeID() &&
                    CFDataGetLength(static_cast<CFDataRef>(value))==CFIndex(sizeof(snapshot))) {
                memcpy(&snapshot,CFDataGetBytePtr(static_cast<CFDataRef>(value)),sizeof(snapshot));
                found=snapshot.version==GPU::Diagnostics::Version && snapshot.bytes==sizeof(snapshot) &&
                    snapshot.kindCount==GPU::Diagnostics::KindCount && snapshot.stageCount==GPU::Diagnostics::StageCount &&
                    !snapshot.native.reserved && !snapshot.compositor.reserved;
                if(found) {
                    counts={snapshot.native.kinds.submitted[3],snapshot.native.kinds.completed[3],snapshot.native.kinds.failed[3]};
                    if(includeCompute){counts.submitted+=snapshot.native.kinds.submitted[2];counts.completed+=snapshot.native.kinds.completed[2];counts.failed+=snapshot.native.kinds.failed[2];}
                    if(diagnostic)*diagnostic=snapshot;
                    if(mixed) {
                        auto v=IORegistryEntryCreateCFProperty(service,CFSTR("IOGPUNativeMixedShaderBatchVersion"),nullptr,0);int32_t version=0;
                        *mixed=v && CFGetTypeID(v)==CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v,kCFNumberSInt32Type,&version) && version==1;
                        if(v)CFRelease(v);
                    }
                }
            }
            if(value)CFRelease(value);
        }
        IOObjectRelease(service);if(found)break;
    }
    IOObjectRelease(iterator);return found;
}

// Prove compute->compute->fragment visibility and that the native driver
// actually admits one mixed job. A later CPU-snapshotted index draw stays
// behind the real GPU producer's completion, in its own job.
static int mixedBatch(id<MTLDevice> device) {
    NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void mixed_add(device const uint *a [[buffer(0)]],device uint *b [[buffer(1)]],uint i [[thread_position_in_grid]]) { b[i]=a[i]+7u; }\n"
        "vertex float4 mixed_position(device const float4 *v [[buffer(0)]],uint i [[vertex_id]]) { return v[i]; }\n"
        "fragment float4 mixed_color(device const uint *v [[buffer(0)]],float4 p [[position]]) { uint i=(uint(p.y)*64u+uint(p.x))*4u; return float4(float(v[i]),float(v[i+1u]),float(v[i+2u]),255.0)/255.0; }\n";
    auto library=[device newLibraryWithSource:source options:nil error:&error];auto queue=[device newCommandQueue];
    auto kernel=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"mixed_add"] error:&error];
    auto descriptor=[MTLRenderPipelineDescriptor new];descriptor.vertexFunction=[library newFunctionWithName:@"mixed_position"];
    descriptor.fragmentFunction=[library newFunctionWithName:@"mixed_color"];descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    auto render=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if(!library || !queue || !kernel || !render){fprintf(stderr,"Mixed shader setup: %s\n",error.localizedDescription.UTF8String);return 1;}
    constexpr NSUInteger count=64*64*4,bytes=count*4,pixelBytes=64*64*4,guard=256;
    auto input=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    auto intermediate=[device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    auto output=[device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    auto readback=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    auto backing=[device newBufferWithLength:pixelBytes+2*guard options:MTLResourceStorageModeShared];
    const float points[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};
    auto vertices=[device newBufferWithBytes:points length:sizeof(points) options:MTLResourceStorageModeShared];
    auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
    auto texture=[backing newTextureWithDescriptor:td offset:guard bytesPerRow:256];
    if(!input || !intermediate || !output || !readback || !texture || !vertices)return 1;
    for(NSUInteger i=0;i<count;++i)static_cast<uint32_t *>(input.contents)[i]=uint32_t((i*13+i/17)%241);
    memset(backing.contents,0xa5,backing.length);
    auto recordCompute=[&](id<MTLCommandBuffer> cb,id<MTLBuffer> from,id<MTLBuffer> to,NSUInteger elements) {
        auto encoder=[cb computeCommandEncoder];[encoder setComputePipelineState:kernel];
        [encoder setBuffer:from offset:0 atIndex:0];[encoder setBuffer:to offset:0 atIndex:1];
        [encoder dispatchThreadgroups:MTLSizeMake(elements/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
        [encoder endEncoding];
    };
    auto recordDraw=[&](id<MTLCommandBuffer> cb,id<MTLBuffer> indices,bool clear=false) {
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
        pass.colorAttachments[0].loadAction=clear?MTLLoadActionClear:MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        auto encoder=[cb renderCommandEncoderWithDescriptor:pass];[encoder setRenderPipelineState:render];
        [encoder setVertexBuffer:vertices offset:0 atIndex:0];[encoder setFragmentBuffer:output offset:0 atIndex:0];
        if(indices)[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt32 indexBuffer:indices indexBufferOffset:0];
        else [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
    };
    auto pixelsCorrect=[&]() {
        const auto *p=static_cast<const uint8_t *>(backing.contents);const auto *v=static_cast<const uint32_t *>(input.contents);
        for(NSUInteger i=0;i<backing.length;++i) {
            const uint8_t expected=i<guard || i>=guard+pixelBytes?0xa5:(i-guard)%4==3?255:uint8_t(v[i-guard]+14);
            if(p[i]!=expected){fprintf(stderr,"Mixed pixels byte=%lu actual=%u expected=%u\n",(unsigned long)i,p[i],expected);return false;}
        }
        return true;
    };
    auto counts=[&](GPU::Diagnostics::Snapshot &snapshot,bool &mixed) {RenderCounts unused;return nativeRenderCounts(device,unused,&snapshot,&mixed);};
    auto difference=[](uint64_t all,uint64_t compositor) {return all-compositor;};
    auto sampledAfter=[&](const GPU::Diagnostics::Snapshot &before,GPU::Diagnostics::Snapshot &after,
        bool &mixed,uint64_t jobs,uint64_t computes,uint64_t renders) {
        // GPUStatistics is sampled. Real completion/pixel validation is
        // already established; let that registry view catch up as in the
        // existing CARenderer probe, without submitting extra GPU work.
        for(unsigned attempt=0;attempt<=120;++attempt) {
            if(!counts(after,mixed))return false;
            const auto admitted=difference(after.native.accepted,after.compositor.accepted)-difference(before.native.accepted,before.compositor.accepted);
            const auto cs=difference(after.native.kinds.completed[2],after.compositor.kinds.completed[2])-difference(before.native.kinds.completed[2],before.compositor.kinds.completed[2]);
            const auto fs=difference(after.native.kinds.completed[3],after.compositor.kinds.completed[3])-difference(before.native.kinds.completed[3],before.compositor.kinds.completed[3]);
            if(admitted>=jobs && cs>=computes && fs>=renders)return true;
            if(attempt!=120)[NSThread sleepForTimeInterval:0.05];
        }
        return false;
    };
    auto verifyCounts=[&](const GPU::Diagnostics::Snapshot &before,const GPU::Diagnostics::Snapshot &after,
        uint64_t expectedJobs,uint64_t computes,uint64_t renders,const char *phase,bool mixed) {
        auto delta=[&](uint64_t a,uint64_t ac,uint64_t b,uint64_t bc){return difference(b,bc)-difference(a,ac);};
        const auto jobs=delta(before.native.accepted,before.compositor.accepted,after.native.accepted,after.compositor.accepted);
        const auto cs=delta(before.native.kinds.completed[2],before.compositor.kinds.completed[2],after.native.kinds.completed[2],after.compositor.kinds.completed[2]);
        const auto fs=delta(before.native.kinds.completed[3],before.compositor.kinds.completed[3],after.native.kinds.completed[3],after.compositor.kinds.completed[3]);
        printf("GPU_MIXED_BATCH phase=%s mixed=%u admitted_jobs=%llu compute_kinds=%llu render_kinds=%llu expected_jobs=%llu\n",phase,mixed,jobs,cs,fs,expectedJobs);
        return jobs==expectedJobs && cs==computes && fs==renders;
    };
    GPU::Diagnostics::Snapshot before{},after{};bool mixed=false,afterMixed=false;
    if(!counts(before,mixed))return 1;
    auto cb=[queue commandBuffer];recordCompute(cb,input,intermediate,count);recordCompute(cb,intermediate,output,count);recordDraw(cb,nil);
    if(!completed(cb) || !pixelsCorrect() || !sampledAfter(before,after,afterMixed,mixed?1:3,mixed?1:2,1) || mixed!=afterMixed ||
        !verifyCounts(before,after,mixed?1:3,mixed?1:2,1,"compute-compute-render",mixed))return 1;
    before=after;cb=[queue commandBuffer];recordCompute(cb,input,intermediate,count);recordCompute(cb,intermediate,readback,count);
    if(!completed(cb) || !sampledAfter(before,after,afterMixed,mixed?1:2,mixed?1:2,0) || !verifyCounts(before,after,mixed?1:2,mixed?1:2,0,"compute-only",mixed))return 1;
    for(NSUInteger i=0;i<count;++i)if(static_cast<const uint32_t *>(readback.contents)[i]!=static_cast<const uint32_t *>(input.contents)[i]+14)return 1;
    // Repeated resources must not split a full dispatch batch. Verify the
    // actual native job count and all values after dependent GPU ping-pong.
    before=after;cb=[queue commandBuffer];
    constexpr unsigned dispatchCount=GPU::Native::MaxBatchCommands;
    for(unsigned i=0;i<dispatchCount;++i)recordCompute(cb,i?((i&1)?intermediate:readback):input,(i&1)?readback:intermediate,count);
    if(!completed(cb) || !sampledAfter(before,after,afterMixed,mixed?1:dispatchCount,mixed?1:dispatchCount,0) ||
        !verifyCounts(before,after,mixed?1:dispatchCount,mixed?1:dispatchCount,0,"maximum-dispatch-resource-reuse",mixed))return 1;
    for(NSUInteger i=0;i<count;++i)if(static_cast<const uint32_t *>(readback.contents)[i]!=static_cast<const uint32_t *>(input.contents)[i]+7*dispatchCount)return 1;
    auto indexInput=[device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    auto indexOutput=[device newBufferWithLength:256 options:MTLResourceStorageModePrivate];if(!indexInput || !indexOutput)return 1;
    for(unsigned i=0;i<64;++i)static_cast<uint32_t *>(indexInput.contents)[i]=uint32_t(i)-7u;
    before=after;cb=[queue commandBuffer];recordCompute(cb,indexInput,indexOutput,64);recordDraw(cb,indexOutput);
    if(!completed(cb) || !pixelsCorrect() || !sampledAfter(before,after,afterMixed,2,1,1) || !verifyCounts(before,after,2,1,1,"GPU-produced-indices-boundary",mixed))return 1;
    // QuartzCore imports CPU indices while clearing private pass attachments.
    // Exercise that combination and verify both one native job and GPU pixels.
    void *clientMemory=nullptr;if(posix_memalign(&clientMemory,16384,16384))return 1;
    memset(clientMemory,0xa5,16384);auto *clientWords=static_cast<uint32_t *>(clientMemory);
    clientWords[0]=0;clientWords[1]=1;clientWords[2]=2;
    auto clientIndices=[device newBufferWithBytesNoCopy:clientMemory length:16384 options:MTLResourceStorageModeShared
        deallocator:^(void *memory,NSUInteger length){(void)length;free(memory);}];
    if(!clientIndices){free(clientMemory);return 1;}
    auto sharedTexture=texture;MTLTextureDescriptor *privateDescriptor=[td copy];privateDescriptor.storageMode=MTLStorageModePrivate;
    texture=[device newTextureWithDescriptor:privateDescriptor];if(!texture)return 1;
    before=after;cb=[queue commandBuffer];recordDraw(cb,clientIndices,true);
    if(!completed(cb) || !sampledAfter(before,after,afterMixed,mixed?1:2,1,1) ||
        !verifyCounts(before,after,mixed?1:2,1,1,"private-clear-client-indices",mixed))return 1;
    cb=[queue commandBuffer];auto download=[cb blitCommandEncoder];
    [download copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1)
        toTexture:sharedTexture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [download endEncoding];texture=sharedTexture;
    if(!completed(cb) || !pixelsCorrect())return 1;
    puts("GPU_MIXED_BATCH verified nonuniform4096pixels/exactguards,16384uintcomputevalues,andGPUindexproducer ordering");return 0;
}

// Exercise Apple's own Metal renderer and its native shader library. The
// supplied queue orders the GPU readback after CARenderer's committed work.
// This is offscreen evidence; it never substitutes for a system UI frame.
static int nativeRendererProbe(id<MTLDevice> device,NSUInteger width=64,NSUInteger height=64,bool surfaceBacked=false) {
    const char *phase="allocation";
    @try {
        const NSUInteger pitch=(width*4+255)&~NSUInteger(255),bytes=pitch*height,count=width*height;
        if(bytes>device.maxBufferLength)return 1;
        printf("GPU_CA_RENDERER size=%lux%lu row=%lu backing=%s\n",(unsigned long)width,(unsigned long)height,
            (unsigned long)pitch,surfaceBacked?"IOSurface":"buffer");
        auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
        td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        id<MTLTexture> target=nil;
        if(surfaceBacked) {
            IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)@{
                (__bridge NSString *)kIOSurfaceWidth:@(width),(__bridge NSString *)kIOSurfaceHeight:@(height),
                (__bridge NSString *)kIOSurfaceBytesPerElement:@4,(__bridge NSString *)kIOSurfaceBytesPerRow:@(pitch),
                (__bridge NSString *)kIOSurfaceAllocSize:@(bytes),(__bridge NSString *)kIOSurfacePixelFormat:@(0x42475241u)});
            if(surface){target=[device newTextureWithDescriptor:td iosurface:surface plane:0];CFRelease(surface);}
        } else target=[device newTextureWithDescriptor:td];
        auto queue=[device newCommandQueue];
        auto readback=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if(!target || !queue || !readback || !readback.contents)return 1;
        // Replay the resource-state calls made by QuartzCore's idle collector,
        // then restore valid backing before rendering and GPU readback.
        phase="purgeable";
        if ([readback setPurgeableState:MTLPurgeableStateVolatile]!=MTLPurgeableStateNonVolatile ||
            [readback setPurgeableState:MTLPurgeableStateKeepCurrent]!=MTLPurgeableStateVolatile ||
            [readback setPurgeableState:MTLPurgeableStateNonVolatile]!=MTLPurgeableStateVolatile)return 1;
        if(surfaceBacked) {
            if([target setPurgeableState:MTLPurgeableStateEmpty]!=MTLPurgeableStateNonVolatile ||
               [target setPurgeableState:MTLPurgeableStateKeepCurrent]!=MTLPurgeableStateNonVolatile)return 1;
        } else if ([target setPurgeableState:MTLPurgeableStateEmpty]!=MTLPurgeableStateNonVolatile ||
            [target setPurgeableState:MTLPurgeableStateVolatile]!=MTLPurgeableStateEmpty ||
            [target setPurgeableState:MTLPurgeableStateNonVolatile]!=MTLPurgeableStateEmpty ||
            [target setPurgeableState:MTLPurgeableStateKeepCurrent]!=MTLPurgeableStateNonVolatile)return 1;
        printf("GPU_CA_RENDERER purgeable=buffer-volatile-retained,texture-%s backing_restored=1\n",surfaceBacked?"iosurface-owned":"empty-observed");
        auto sentinel=[NSMutableData dataWithLength:bytes];if(!sentinel || !sentinel.mutableBytes)return 1;
        memset(sentinel.mutableBytes,0xa5,bytes);
        [target replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:sentinel.bytes bytesPerRow:pitch];
        memset(readback.contents,0x3c,readback.length);
        RenderCounts before{},after{};
        if(!nativeRenderCounts(device,before,nullptr,nullptr,true)){puts("GPU_CA_RENDERER statistics unavailable");return 1;}
        phase="create";printf("GPU_CA_RENDERER phase=%s api=rendererWithMTLTexture:options: offscreen=1\n",phase);fflush(stdout);
        auto renderer=[CARenderer rendererWithMTLTexture:target options:@{kCARendererMetalCommandQueue:queue}];
        if(!renderer){puts("GPU_CA_RENDERER renderer unavailable");return 1;}
        CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
        const CGFloat red[]={1,0,0,1},green[]={0,1,0,1};
        CGColorRef background=space?CGColorCreate(space,red):nullptr;
        CGColorRef foreground=space?CGColorCreate(space,green):nullptr;
        if(space)CGColorSpaceRelease(space);
        if(!background || !foreground){if(background)CGColorRelease(background);if(foreground)CGColorRelease(foreground);return 1;}
        auto root=[CALayer layer],child=[CALayer layer];
        [CATransaction begin];[CATransaction setDisableActions:YES];
        root.frame=CGRectMake(0,0,width,height);root.opaque=YES;root.backgroundColor=background;
        child.frame=CGRectMake(width/4,height/4,width/2,height/2);child.opaque=YES;child.backgroundColor=foreground;
        [root addSublayer:child];
        renderer.layer=root;renderer.bounds=CGRectMake(0,0,width,height);
        [CATransaction commit];
        CGColorRelease(background);CGColorRelease(foreground);
        // The command-line process has no application run loop to flush an
        // implicit transaction after attaching the tree to this renderer.
        phase="flush";printf("GPU_CA_RENDERER phase=%s\n",phase);fflush(stdout);
        [CATransaction flush];
        // Reuse the same native target across opaque, translucent, moved and
        // removed content. A single solid frame cannot reveal stale damage
        // or incorrect destination reads during the Welcome transition.
        const NSUInteger imageWidth=width/2,imageHeight=height/2;
        auto imageBytes=[NSMutableData dataWithLength:imageWidth*imageHeight*4];
        auto *rgba=static_cast<uint8_t *>(imageBytes.mutableBytes);
        for(NSUInteger y=0;y<imageHeight;++y)for(NSUInteger x=0;x<imageWidth;++x) {
            unsigned alpha=x<imageWidth/4?0:x<3*imageWidth/4?128:255;
            NSUInteger at=(y*imageWidth+x)*4;rgba[at]=rgba[at+2]=0;rgba[at+1]=rgba[at+3]=uint8_t(alpha);
        }
        auto provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)imageBytes);
        auto imageSpace=CGColorSpaceCreateDeviceRGB();
        auto image=provider && imageSpace?CGImageCreate(imageWidth,imageHeight,8,32,imageWidth*4,imageSpace,
            kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big,provider,nullptr,false,kCGRenderingIntentDefault):nullptr;
        if(provider)CGDataProviderRelease(provider);if(imageSpace)CGColorSpaceRelease(imageSpace);
        if(!image)return 1;
        id content=(__bridge_transfer id)image;
        for(unsigned frame=0;frame<8;++frame) {
        const float opacity[]={1.0f,0.5f,0.25f,0.0f,1.0f,1.0f,0.5f,0.0f};
        const NSUInteger left=frame==1 || frame==2?width/2:width/4;
        const NSUInteger right=3*width/4;
        [CATransaction begin];[CATransaction setDisableActions:YES];
        child.frame=CGRectMake(left,height/4,right-left,height/2);child.opacity=opacity[frame];
        if(frame>=5) {
            child.backgroundColor=nullptr;child.opaque=NO;child.contents=frame==7?nil:content;
            child.minificationFilter=kCAFilterNearest;child.magnificationFilter=kCAFilterNearest;
        }
        [CATransaction commit];[CATransaction flush];
        phase="begin";printf("GPU_CA_RENDERER frame=%u opacity=%.2f phase=%s\n",frame,double(opacity[frame]),phase);fflush(stdout);
        [renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:nullptr];
        [renderer addUpdateRect:frame?CGRectMake(width/4,height/4,width/2,height/2):renderer.bounds];
        CGRect update=renderer.updateBounds;
        printf("GPU_CA_RENDERER update_bounds=%.0f,%.0f,%.0f,%.0f\n",
            double(update.origin.x),double(update.origin.y),double(update.size.width),double(update.size.height));
        phase="render";printf("GPU_CA_RENDERER phase=%s\n",phase);fflush(stdout);
        [renderer render];
        phase="end";printf("GPU_CA_RENDERER phase=%s\n",phase);fflush(stdout);
        [renderer endFrame];
        phase="readback";printf("GPU_CA_RENDERER phase=%s\n",phase);fflush(stdout);
        auto commands=[queue commandBuffer];auto blit=[commands blitCommandEncoder];if(!blit)return 1;
        [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1)
            toBuffer:readback destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:bytes];
        [blit endEncoding];if(!completed(commands))return 1;
        // The driver publishes registry statistics about once per second.
        // Queue retirement is already proved above; allow its sampled view
        // to catch up before rejecting an otherwise valid rendered frame.
        for(unsigned attempt=0;attempt<=60;++attempt) {
            if(!nativeRenderCounts(device,after,nullptr,nullptr,true))return 1;
            if(after.failed!=before.failed ||
                    (after.submitted>before.submitted && after.completed>before.completed))break;
            if(attempt!=60)[NSThread sleepForTimeInterval:0.05];
        }
        printf("GPU_CA_RENDERER global_shader_before=%llu,%llu,%llu global_shader_after=%llu,%llu,%llu\n",
            (unsigned long long)before.submitted,(unsigned long long)before.completed,(unsigned long long)before.failed,
            (unsigned long long)after.submitted,(unsigned long long)after.completed,(unsigned long long)after.failed);
        const auto *pixels=static_cast<const uint8_t *>(readback.contents);NSUInteger correct=0,unchanged=0,firstBad=count;
        for(NSUInteger y=0;y<height;++y)for(NSUInteger x=0;x<width;++x) {
            NSUInteger i=y*width+x,at=y*pitch+x*4;bool center=x>=left && x<right && y>=height/4 && y<3*height/4;
            unsigned alpha=255;
            if(frame>=5 && center){NSUInteger column=x-left;alpha=column<imageWidth/4?0:column<3*imageWidth/4?128:255;}
            unsigned green=center?unsigned(opacity[frame]*alpha+0.5f):0;
            const uint8_t expected[]={0,uint8_t(green),uint8_t(255-green),255};
            uint32_t pixel;memcpy(&pixel,pixels+at,sizeof(pixel));
            bool valid=true;for(unsigned c=0;c<4;++c) {
                int difference=int((pixel>>(c*8))&255)-int(expected[c]);
                if(difference < -1 || difference > 1)valid=false;
            }
            if(valid)++correct;else if(firstBad==count)firstBad=i;
            if(pixel==0xa5a5a5a5u)++unchanged;
        }
        printf("GPU_CA_RENDERER frame=%u pixels_correct=%lu unchanged=%lu expected=%lu\n",frame,(unsigned long)correct,(unsigned long)unchanged,(unsigned long)count);
        if(firstBad!=count) {
            const auto *pixel=pixels+(firstBad/width)*pitch+(firstBad%width)*4;uint8_t direct[4]={};
            [target getBytes:direct bytesPerRow:4 fromRegion:MTLRegionMake2D(firstBad%width,firstBad/width,1,1) mipmapLevel:0];
            printf("GPU_CA_RENDERER frame=%u mismatch_pixel=%lu bgra=%u,%u,%u,%u direct_bgra=%u,%u,%u,%u\n",frame,(unsigned long)firstBad,
                pixel[0],pixel[1],pixel[2],pixel[3],direct[0],direct[1],direct[2],direct[3]);return 1;
        }
        if(after.submitted<=before.submitted || after.completed<=before.completed || after.failed!=before.failed) {
            puts("GPU_CA_RENDERER GPU shader work completion unverified");return 1;
        }
        before=after;
        }
        printf("GPU_CA_RENDERER completed frames=8 native_layers=2 pixels_per_frame=%lu system_ui_presentation_unverified=1\n",(unsigned long)count);return 0;
    } @catch(NSException *exception) {
        fprintf(stderr,"GPU_CA_RENDERER phase=%s exception=%s reason=%s\n",phase,exception.name.UTF8String,exception.reason.UTF8String);return 1;
    }
}

// Keep the existing blit-test as the target entry point. These failures are
// detected before any resource-bearing native submission, including when an
// encoder object itself survives endEncoding.
static bool commandReferences(id<MTLDevice> device) {
    auto queue=[device newCommandQueue];if(!queue)return false;
    __attribute__((objc_precise_lifetime)) id<MTLBuffer> readback=[device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    if(!readback)return false;
    auto retained=[queue commandBuffer];__weak id<MTLBuffer> kept=nil;
    @autoreleasepool {
        auto source=[device newBufferWithLength:64 options:MTLResourceStorageModeShared];if(!source)return false;
        memset(source.contents,0x2b,64);kept=source;
        auto encoder=[retained blitCommandEncoder];if(!encoder)return false;
        [encoder copyFromBuffer:source sourceOffset:0 toBuffer:readback destinationOffset:0 size:64];[encoder endEncoding];
    }
    if(!retained.retainedReferences || !kept || !completed(retained))return false;
    for(unsigned i=0;i<64;++i)if(static_cast<const uint8_t *>(readback.contents)[i]!=0x2b)return false;
    for(unsigned api=0;api<2;++api) {
        auto descriptor=[MTLCommandBufferDescriptor new];descriptor.retainedReferences=NO;
        auto borrowed=api?[queue commandBufferWithDescriptor:descriptor]:[queue commandBufferWithUnretainedReferences];
        if(!borrowed || borrowed.retainedReferences)return false;
        __weak id<MTLBuffer> expired=nil;id<MTLBlitCommandEncoder> encoder=nil;
        @autoreleasepool {
            auto temporary=[device newBufferWithLength:64 options:MTLResourceStorageModeShared];if(!temporary)return false;
            expired=temporary;encoder=[borrowed blitCommandEncoder];if(!encoder)return false;
            [encoder fillBuffer:temporary range:NSMakeRange(0,64) value:0x7e];[encoder endEncoding];
        }
        if(expired || !encoder)return false;
        [borrowed commit];[borrowed waitUntilCompleted];
        if(borrowed.status!=MTLCommandBufferStatusError || ![borrowed.error.domain isEqualToString:MTLCommandBufferErrorDomain] ||
            borrowed.error.code!=MTLCommandBufferErrorInvalidResource)return false;
    }
    puts("Command buffer references: retained resource survived; both borrowed APIs rejected expired resources");return true;
}

static bool bufferTextures(id<MTLDevice> device,bool heaps=false) {
    // A nonzero base and padded rows distinguish a real storage alias from a
    // separately allocated texture or a view which forgot its buffer offset.
    const NSUInteger length=1024,width=heaps?17:13,height=5,pitch=128,sourceOffset=64,targetOffset=128;
    __attribute__((objc_precise_lifetime)) id<MTLHeap> sharedHeap=nil,privateHeap=nil;
    if(heaps) {
        auto hd=[MTLHeapDescriptor new];hd.size=16384;hd.type=MTLHeapTypePlacement;hd.storageMode=MTLStorageModeShared;
        hd.hazardTrackingMode=MTLHazardTrackingModeTracked;sharedHeap=[device newHeapWithDescriptor:hd];
        hd.storageMode=MTLStorageModePrivate;privateHeap=[device newHeapWithDescriptor:hd];
        if(!sharedHeap || !privateHeap)return false;
    }
    __attribute__((objc_precise_lifetime)) id<MTLBuffer> source=heaps?[sharedHeap newBufferWithLength:length options:MTLResourceStorageModeShared offset:256]:[device newBufferWithLength:length options:MTLResourceStorageModeShared];
    __attribute__((objc_precise_lifetime)) id<MTLBuffer> target=heaps?[privateHeap newBufferWithLength:length options:MTLResourceStorageModePrivate offset:4096]:[device newBufferWithLength:length options:MTLResourceStorageModePrivate];
    __attribute__((objc_precise_lifetime)) id<MTLBuffer> readback=heaps?[sharedHeap newBufferWithLength:length options:MTLResourceStorageModeShared offset:2048]:[device newBufferWithLength:length options:MTLResourceStorageModeShared];
    __attribute__((objc_precise_lifetime)) id<MTLBuffer> whole=heaps?[privateHeap newBufferWithLength:privateHeap.size options:MTLResourceStorageModePrivate offset:0]:nil;
    if(heaps && (!whole || source.heap!=sharedHeap || source.heapOffset!=256 || target.heap!=privateHeap || target.heapOffset!=4096))return false;
    if(!source || !target || !readback || [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA8Unorm]!=64)return false;
    memset(source.contents,0x3c,length);
    auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
    auto allocated=device.currentAllocatedSize;
    __attribute__((objc_precise_lifetime)) id<MTLTexture> upload=[source newTextureWithDescriptor:d offset:sourceOffset bytesPerRow:pitch];
    d.storageMode=MTLStorageModePrivate;
    __attribute__((objc_precise_lifetime)) id<MTLTexture> output=[target newTextureWithDescriptor:d offset:targetOffset bytesPerRow:pitch];
    __attribute__((objc_precise_lifetime)) id<MTLTexture> view=[output newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
    if(!upload || !output || !view || allocated!=device.currentAllocatedSize || upload.buffer!=source || output.buffer!=target ||
        upload.bufferOffset!=sourceOffset || output.bufferOffset!=targetOffset || upload.bufferBytesPerRow!=pitch ||
        output.bufferBytesPerRow!=pitch || target.contents || view.parentTexture!=output || view.buffer ||
        view.bufferOffset || view.bufferBytesPerRow || view.gpuResourceID._impl!=output.gpuResourceID._impl)return false;
    __attribute__((objc_precise_lifetime)) id<MTLTexture> heapTarget=heaps?[privateHeap newTextureWithDescriptor:d offset:4096+targetOffset]:nil;
    if(heaps && (!heapTarget || heapTarget.heap!=privateHeap || heapTarget.heapOffset!=4096+targetOffset || heapTarget.buffer || allocated!=device.currentAllocatedSize))return false;
    uint8_t pixels[17*5*4];
    for(NSUInteger i=0;i<sizeof(pixels);i+=4){pixels[i]=17;pixels[i+1]=34;pixels[i+2]=51;pixels[i+3]=255;}
    [upload replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:pixels bytesPerRow:width*4];
    auto *cpu=static_cast<const uint8_t *>(source.contents);
    auto check=[&](const uint8_t *bytes,NSUInteger offset,uint8_t guard,const uint8_t *color) {
        for(NSUInteger i=0;i<length;++i) {
            bool pixel=i>=offset && i-offset<pitch*height && (i-offset)%pitch<width*4;
            uint8_t expected=pixel?color[(i-offset)%4]:guard;
            if(bytes[i]!=expected){fprintf(stderr,"Buffer texture mismatch at %lu\n",(unsigned long)i);return false;}
        }
        return true;
    };
    const uint8_t original[]={17,34,51,255},transformed[]={238,34,51,255};
    if(!check(cpu,sourceOffset,0x3c,original))return false;
    // Exercise the same original VM-backed import used by UI image data.
    // Protect before importing so IOGPU observes a genuinely read-only range.
    __attribute__((objc_precise_lifetime)) id<MTLBuffer> immutable=nil;
    if(!heaps) {
        const NSUInteger page=getpagesize();
        void *mapping=mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        if(mapping==MAP_FAILED)return false;
        memcpy(mapping,cpu,length);
        if(mprotect(mapping,page,PROT_READ)){munmap(mapping,page);return false;}
        immutable=[device newBufferWithBytesNoCopy:mapping length:page options:MTLResourceStorageModeShared
            deallocator:^(void *bytes,NSUInteger size){munmap(bytes,size);}];
        if(!immutable){munmap(mapping,page);return false;}
        d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
        upload=[immutable newTextureWithDescriptor:d offset:sourceOffset bytesPerRow:pitch];
        d.storageMode=MTLStorageModePrivate;
        if(!upload || upload.buffer!=immutable || immutable.contents!=mapping)return false;
    }
    auto queue=[device newCommandQueue];auto commands=(heaps?[queue commandBufferWithUnretainedReferences]:[queue commandBuffer]);auto blit=[commands blitCommandEncoder];
    if(!queue || !commands || !blit)return false;
    if(whole)[blit fillBuffer:whole range:NSMakeRange(0,whole.length) value:0x7e];
    [blit fillBuffer:target range:NSMakeRange(0,length) value:0x5a];[blit copyFromTexture:upload toTexture:output];[blit endEncoding];
    auto download=[&](id<MTLCommandBuffer> batch) {
        auto copy=[batch blitCommandEncoder];if(!copy)return false;
        [copy copyFromBuffer:target sourceOffset:0 toBuffer:readback destinationOffset:0 size:length];[copy endEncoding];
        return completed(batch);
    };
    if(!download(commands) || !check(static_cast<const uint8_t *>(readback.contents),targetOffset,0x5a,original))return false;
    const uint8_t cleared[]={64,128,191,255};
    auto clearPass=[MTLRenderPassDescriptor renderPassDescriptor];clearPass.colorAttachments[0].texture=heaps?heapTarget:output;
    clearPass.colorAttachments[0].loadAction=MTLLoadActionClear;clearPass.colorAttachments[0].storeAction=MTLStoreActionStore;
    clearPass.colorAttachments[0].clearColor=MTLClearColorMake(0.25,0.5,0.75,1);
    auto cd=[MTLCommandBufferDescriptor new];cd.retainedReferences=!heaps;
    commands=[queue commandBufferWithDescriptor:cd];if(!commands || commands.retainedReferences==heaps)return false;auto clearEncoder=[commands renderCommandEncoderWithDescriptor:clearPass];if(!clearEncoder)return false;
    [clearEncoder endEncoding];
    if(!download(commands) || !check(static_cast<const uint8_t *>(readback.contents),targetOffset,0x5a,cleared))return false;
    __attribute__((objc_precise_lifetime)) id<MTLLibrary> library=diagnosticLibrary(device);NSError *error=nil;
    __attribute__((objc_precise_lifetime)) id<MTLComputePipelineState> compute=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"image_transform"] error:&error];
    if(!library || !compute){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return false;}
    commands=(heaps?[queue commandBufferWithUnretainedReferences]:[queue commandBuffer]);auto encoder=[commands computeCommandEncoder];if(!encoder)return false;
    uint32_t level=0;[encoder setComputePipelineState:compute];[encoder setTexture:upload atIndex:0];[encoder setTexture:view atIndex:7];
    [encoder setBytes:&level length:sizeof(level) atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake((width+7)/8,1,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];[encoder endEncoding];
    if(!download(commands) || !check(static_cast<const uint8_t *>(readback.contents),targetOffset,0x5a,transformed))return false;
    auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"position"];
    pd.fragmentFunction=[library newFunctionWithName:@"textured"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    __attribute__((objc_precise_lifetime)) id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    const float triangle[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};
    __attribute__((objc_precise_lifetime)) id<MTLBuffer> vertices=heaps?[sharedHeap newBufferWithLength:sizeof(triangle) options:MTLResourceStorageModeShared offset:8192]:[device newBufferWithBytes:triangle length:sizeof(triangle) options:MTLResourceStorageModeShared];
    if(heaps && vertices)memcpy(vertices.contents,triangle,sizeof(triangle));
    auto sd=[MTLSamplerDescriptor new];__attribute__((objc_precise_lifetime)) id<MTLSamplerState> sampler=[device newSamplerStateWithDescriptor:sd];
    if(!pipeline || !vertices || !sampler)return false;
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=heaps?heapTarget:output;
    pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    commands=(heaps?[queue commandBufferWithUnretainedReferences]:[queue commandBuffer]);auto render=[commands renderCommandEncoderWithDescriptor:pass];if(!render)return false;
    [render setRenderPipelineState:pipeline];[render setVertexBuffer:vertices offset:0 atIndex:0];
    [render setFragmentTexture:upload atIndex:0];[render setFragmentSamplerState:sampler atIndex:0];
    [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[render endEncoding];
    if(!download(commands) || !check(static_cast<const uint8_t *>(readback.contents),targetOffset,0x5a,original))return false;
    if(heaps) {
        auto completeHeap=[device newBufferWithLength:whole.length options:MTLResourceStorageModeShared];
        commands=(heaps?[queue commandBufferWithUnretainedReferences]:[queue commandBuffer]);blit=[commands blitCommandEncoder];if(!completeHeap || !blit)return false;
        [blit copyFromBuffer:whole sourceOffset:0 toBuffer:completeHeap destinationOffset:0 size:whole.length];[blit endEncoding];
        if(!completed(commands))return false;
        auto *bytes=static_cast<const uint8_t *>(completeHeap.contents);
        for(NSUInteger i=0;i<whole.length;++i) {
            bool resource=i>=4096 && i-4096<length,pixel=i>=4096+targetOffset && i-(4096+targetOffset)<pitch*height && (i-(4096+targetOffset))%pitch<width*4;
            uint8_t expected=pixel?original[(i-(4096+targetOffset))%4]:resource?0x5a:0x7e;
            if(bytes[i]!=expected){fprintf(stderr,"Heap guard mismatch at %lu\n",(unsigned long)i);return false;}
        }
        auto hd=[MTLHeapDescriptor new];hd.size=16384;auto automatic=[device newHeapWithDescriptor:hd];
        id<MTLBuffer> first=[automatic newBufferWithLength:65 options:MTLResourceStorageModePrivate];
        __attribute__((objc_precise_lifetime)) id<MTLBuffer> second=[automatic newBufferWithLength:65 options:MTLResourceStorageModePrivate];
        auto requirements=[device heapBufferSizeAndAlignWithLength:65 options:MTLResourceStorageModePrivate];
        if(!first || !second || requirements.size!=128 || requirements.align!=64 || automatic.usedSize!=256)return false;
        auto address=first.gpuAddress;[first makeAliasable];
        __attribute__((objc_precise_lifetime)) id<MTLBuffer> reused=[automatic newBufferWithLength:65 options:MTLResourceStorageModePrivate];
        if(!first.isAliasable || !reused || reused.gpuAddress!=address || automatic.usedSize!=256)return false;
        first=nil;if(automatic.usedSize!=256)return false;
        __attribute__((objc_precise_lifetime)) id<MTLTexture> texture=[automatic newTextureWithDescriptor:d];
        __attribute__((objc_precise_lifetime)) id<MTLTexture> child=[texture newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
        if(!texture || !child)return false;[texture makeAliasable];
        __attribute__((objc_precise_lifetime)) id<MTLBuffer> replacement=[automatic newBufferWithLength:texture.allocatedSize options:MTLResourceStorageModePrivate];
        if(!texture.isAliasable || !child.isAliasable || !replacement)return false;
        auto used=automatic.usedSize;texture=nil;child=nil;if(automatic.usedSize!=used)return false;
        puts("Metal heaps: placement offsets, heap textures, rebased GPU access, full backing guards and automatic alias lifetime verified");
    } else {
        auto rejected=[[device newCommandQueue] commandBuffer];auto write=[rejected blitCommandEncoder];
        if(!rejected || !write)return false;
        [write fillBuffer:immutable range:NSMakeRange(0,length) value:0xff];[write endEncoding];
        [rejected commit];[rejected waitUntilCompleted];
        if(rejected.status!=MTLCommandBufferStatusError ||
            !check(static_cast<const uint8_t *>(immutable.contents),sourceOffset,0x3c,original))return false;
        puts("Metal buffer textures: read-only client import, GPU blit/compute/sample reads, rejected GPU write and guard readback verified");
    }
    return true;
}

static bool packedClearGuards(id<MTLDevice> device) {
    auto queue=[device newCommandQueue];if(!queue)return false;
    const MTLPixelFormat formats[]={MTLPixelFormatR8Unorm,MTLPixelFormatRG8Unorm};
    for(unsigned index=0;index<2;++index) {
        const NSUInteger cpp=index+1,pitch=64,offset=64,width=3,height=5,length=1024;
        const NSUInteger clearWidth=index?2:width,clearHeight=index?3:height;
        auto storage=[device newBufferWithLength:length options:MTLResourceStorageModeShared];if(!storage)return false;
        memset(storage.contents,0x5a,length);
        auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[index] width:width height:height mipmapped:NO];
        d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;
        auto texture=[storage newTextureWithDescriptor:d offset:offset bytesPerRow:pitch];if(!texture)return false;
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
        pass.renderTargetWidth=clearWidth;pass.renderTargetHeight=clearHeight;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(0.25,0.5,0.75,1);
        auto commands=[queue commandBuffer];auto encoder=[commands renderCommandEncoderWithDescriptor:pass];if(!encoder)return false;
        [encoder endEncoding];if(!completed(commands))return false;
        auto *bytes=static_cast<const uint8_t *>(storage.contents);
        for(NSUInteger i=0;i<length;++i) {
            bool pixel=i>=offset && i-offset<pitch*clearHeight && (i-offset)%pitch<clearWidth*cpp;
            uint8_t expected=pixel?((i-offset)%cpp?128:64):0x5a;
            if(bytes[i]!=expected){fprintf(stderr,"Packed clear format=%u guard mismatch at %lu\n",index,(unsigned long)i);return false;}
        }
    }
    puts("Metal clear: pitched RGBA, packed R8/RG8 and constrained render extents with padding/outer guards verified");return true;
}

@protocol QcomMTLSubmissionControl
- (void)setSubmissionQueue:(dispatch_queue_t)queue;
@end
@protocol QcomMTLSubmissionWait
- (BOOL)commitAndWaitUntilSubmitted;
@end

static int fences(id<MTLDevice> device) {
    id<MTLCommandQueue> producer=[device newCommandQueue],consumer=[device newCommandQueue];
    id<MTLFence> first=[device newFence],second=[device newFence];id<MTLEvent> event=[device newEvent];
    auto target=dispatch_queue_create("metalctl.submission",DISPATCH_QUEUE_SERIAL);
    if(!producer || !consumer || !first || !second || !event ||
        ![producer respondsToSelector:@selector(setSubmissionQueue:)] || ![consumer respondsToSelector:@selector(setSubmissionQueue:)])return 1;
    [(id<QcomMTLSubmissionControl>)producer setSubmissionQueue:target];[(id<QcomMTLSubmissionControl>)consumer setSubmissionQueue:target];
    // A real first fill must not schedule a buffer whose later operations
    // still wait on a future event. QuartzCore may recycle encoded inputs
    // from the scheduled callback, before the completed callback runs.
    auto reached=[device newSharedEvent],gate=[device newSharedEvent];
    auto probe=[producer commandBuffer];auto probeBytes=[device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    if(!reached || !gate || !probe || !probeBytes)return 1;
    __block NSUInteger scheduled=0;[probe addScheduledHandler:^(id<MTLCommandBuffer> commands){(void)commands;++scheduled;}];
    auto firstFill=[probe blitCommandEncoder];[firstFill fillBuffer:probeBytes range:NSMakeRange(0,64) value:0x11];[firstFill endEncoding];
    [probe encodeSignalEvent:reached value:1];[probe encodeWaitForEvent:gate value:1];
    auto lastFill=[probe blitCommandEncoder];[lastFill fillBuffer:probeBytes range:NSMakeRange(0,64) value:0x22];[lastFill endEncoding];
    [probe commit];
    double deadline=CACurrentMediaTime()+3;
    while(reached.signaledValue<1 && CACurrentMediaTime()<deadline)[NSThread sleepForTimeInterval:0.005];
    bool deferred=reached.signaledValue==1 && probe.status==MTLCommandBufferStatusCommitted;
    gate.signaledValue=1;[probe waitUntilCompleted];
    if(!deferred || scheduled!=1 || probe.status!=MTLCommandBufferStatusCompleted)return 1;
    for(unsigned i=0;i<64;++i)if(static_cast<const uint8_t *>(probeBytes.contents)[i]!=0x22)return 1;
    puts("Metal scheduling: real first fill retired; future event kept later operations and Scheduled callback pending");
    // Native submission returns NO behind an uncommitted enqueue. Exercise
    // this on the same serial target: waiting here would prevent its caller
    // from ever committing the predecessor.
    auto reserved=[producer commandBuffer],following=[producer commandBuffer];
    if(!reserved || !following)return 1;
    auto reservedFill=[reserved blitCommandEncoder];
    if(!reservedFill)return 1;
    [reservedFill fillBuffer:probeBytes range:NSMakeRange(0,64) value:0x33];[reservedFill endEncoding];
    auto followingFill=[following blitCommandEncoder];
    if(!followingFill)return 1;
    [followingFill fillBuffer:probeBytes range:NSMakeRange(0,64) value:0x44];[followingFill endEncoding];
    [reserved enqueue];[following enqueue];
    __block BOOL reservedSubmitted=YES,reservedPremature=YES;
    dispatch_sync(target,^{
        reservedSubmitted=[(id<QcomMTLSubmissionWait>)following commitAndWaitUntilSubmitted];
        reservedPremature=following.status!=MTLCommandBufferStatusCommitted;
        [reserved commit];
    });
    [reserved waitUntilCompleted];[following waitUntilCompleted];
    if(reservedSubmitted || reservedPremature || reserved.status!=MTLCommandBufferStatusCompleted ||
       following.status!=MTLCommandBufferStatusCompleted)return 1;
    for(unsigned i=0;i<64;++i)if(static_cast<const uint8_t *>(probeBytes.contents)[i]!=0x44)return 1;
    puts("Metal scheduling: uncommitted predecessor returned NO; reverse commit preserved FIFO and real readback");
    const NSUInteger bytes=1024;
    id<MTLBuffer> source=[device newBufferWithLength:bytes options:MTLResourceStorageModePrivate|MTLResourceHazardTrackingModeUntracked];
    id<MTLBuffer> output=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared|MTLResourceHazardTrackingModeUntracked];
    id<MTLBuffer> readback=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;id<MTLTexture> color=[device newTextureWithDescriptor:td];
    id<MTLCommandBuffer> a=[producer commandBuffer],b=[consumer commandBuffer],c=[producer commandBuffer];
    if(!source || !output || !readback || !color || !a || !b || !c || ![b respondsToSelector:@selector(commitAndWaitUntilSubmitted)])return 1;
    auto upload=[a blitCommandEncoder];if(!upload)return 1;
    [upload fillBuffer:source range:NSMakeRange(0,bytes) value:0x5a];[upload updateFence:first];[upload endEncoding];
    [a encodeWaitForEvent:event value:1];
    auto copy=[b blitCommandEncoder];if(!copy)return 1;
    [copy waitForFence:first];[copy copyFromBuffer:source sourceOffset:0 toBuffer:output destinationOffset:0 size:bytes];
    [copy updateFence:second];[copy endEncoding];
    auto compute=[b computeCommandEncoder];if(!compute)return 1;
    [compute waitForFence:second];id<MTLResource> resources[]={output};
    [compute memoryBarrierWithResources:resources count:1];[compute updateFence:first];[compute endEncoding];
    [b encodeSignalEvent:event value:1];
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=color;
    pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.colorAttachments[0].clearColor=MTLClearColorMake(0.25,0.5,0.75,1);
    auto render=[c renderCommandEncoderWithDescriptor:pass];if(!render)return 1;
    [render waitForFence:first beforeStages:MTLRenderStageFragment];
    [render updateFence:second afterStages:MTLRenderStageVertex|MTLRenderStageFragment];[render endEncoding];
    auto download=[c blitCommandEncoder];if(!download)return 1;
    [download waitForFence:second];[download copyFromBuffer:output sourceOffset:0 toBuffer:readback destinationOffset:0 size:bytes];[download endEncoding];
    __block BOOL submitted=NO,premature=NO,callbackOrder=YES;__block NSUInteger callbacks=0;
    [a addCompletedHandler:^(id<MTLCommandBuffer> completed){(void)completed;callbackOrder&=callbacks++==0;}];
    [c addCompletedHandler:^(id<MTLCommandBuffer> completed){(void)completed;callbackOrder&=callbacks++==1;}];
    // One shared serial target forces dependency waits to yield. The private
    // submission helper must return NO for deferred cleanup without draining
    // a's future event wait or claiming that b's pixels were submitted.
    dispatch_sync(target,^{[a commit];submitted=[(id<QcomMTLSubmissionWait>)b commitAndWaitUntilSubmitted];
        premature=b.status!=MTLCommandBufferStatusCommitted;[c commit];});
    for(id<MTLCommandBuffer> commands in @[a,b,c]) {
        [commands waitUntilCompleted];
        if(commands.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"%s\n",commands.error.localizedDescription.UTF8String);return 1;}
    }
    if(submitted || premature || !callbackOrder || callbacks!=2 || !readback.contents)return 1;
    for(NSUInteger i=0;i<bytes;++i)if(((const uint8_t *)readback.contents)[i]!=0x5a)return 1;
    uint8_t pixels[64];[color getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
    const uint8_t expected[]={64,128,191,255};
    for(unsigned i=0;i<sizeof(pixels);++i)if(pixels[i]!=expected[i%4])return 1;
    puts("Metal fences: two queues/shared serial target, blit/compute/render clear, reused updates, future event resume and actual GPU readback verified");
    return 0;
}

// One optional end-to-end exercise for the initial AOT profile. Not run by make.
static id<MTLTexture> surfaceTexture(id<MTLDevice> device,MTLTextureDescriptor *d) {
    size_t pitch=IOSurfaceAlignProperty(kIOSurfaceBytesPerRow,d.width*4+64);
    size_t offset=IOSurfaceAlignProperty(kIOSurfaceOffset,4096);
    NSDictionary *properties=@{(__bridge NSString *)kIOSurfaceWidth:@(d.width),
        (__bridge NSString *)kIOSurfaceHeight:@(d.height),(__bridge NSString *)kIOSurfaceBytesPerElement:@4,
        (__bridge NSString *)kIOSurfaceBytesPerRow:@(pitch),(__bridge NSString *)kIOSurfaceOffset:@(offset),
        (__bridge NSString *)kIOSurfaceAllocSize:@((offset+pitch*d.height+16383)&~size_t(16383)),
        (__bridge NSString *)kIOSurfacePixelFormat:@(0x52474241u)};
    IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (!surface) return nil;
    id<MTLTexture> texture=[device newTextureWithDescriptor:d iosurface:surface plane:0];
    CFRelease(surface);return texture;
}

// Records CPU present invocations only; no fake display completion or time.
@interface MetalctlDrawable : NSObject <MTLDrawable>
@property(nonatomic,strong) id<MTLTexture> texture;
@property(nonatomic,strong) NSMutableArray *calls;
@property(nonatomic,strong) NSLock *lock;
@property(nonatomic) NSUInteger drawableID;
@end
@implementation MetalctlDrawable
- (void)present {[_lock lock];[_calls addObject:@(_drawableID)];[_lock unlock];}
- (void)presentAtTime:(CFTimeInterval)time {(void)time;[self present];}
- (void)presentAfterMinimumDuration:(CFTimeInterval)time {(void)time;[self present];}
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)handler {(void)handler;}
- (CFTimeInterval)presentedTime {return 0;}
@end

static int inlineConstants(id<MTLDevice> device) {
    NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void inline_compute(device uint *o [[buffer(0)]],constant uint2 &p [[buffer(1)]]) {o[p.x]=p.y;}\n"
        "vertex float4 inline_vertex(device const float4 *v [[buffer(0)]],constant float4 &p [[buffer(1)]],uint i [[vertex_id]]) {return v[i]+p;}\n"
        "fragment float4 inline_fragment(constant float4 &c [[buffer(0)]]) {return c;}\n";
    auto library=[device newLibraryWithSource:source options:nil error:&error];
    auto compute=library?[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"inline_compute"] error:&error]:nil;
    auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"inline_vertex"];
    pd.fragmentFunction=[library newFunctionWithName:@"inline_fragment"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    auto render=compute?[device newRenderPipelineStateWithDescriptor:pd error:&error]:nil;
    if(!render){fprintf(stderr,"Inline constants compile: %s\n",error.localizedDescription.UTF8String);return 1;}
    auto queue=[device newCommandQueue];
    auto output=[device newBufferWithLength:128*4 options:MTLResourceStorageModeShared];
    const float positions[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};
    auto vertices=[device newBufferWithBytes:positions length:sizeof(positions) options:MTLResourceStorageModeShared];
    auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
    auto target=[device newTextureWithDescriptor:td];
    if(!queue || !output.contents || !vertices || !target)return 1;
    memset(output.contents,0xa5,output.length);
    id<MTLCommandBuffer> commands[2];
    uint32_t expected[128];for(auto &v:expected)v=0xa5a5a5a5;
    for(unsigned phase=0;phase<2;++phase) {
        commands[phase]=[queue commandBufferWithUnretainedReferences];
        @autoreleasepool {
            if(!phase) {
                auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=target;
                pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
                auto graphics=[commands[phase] renderCommandEncoderWithDescriptor:pass];if(!graphics)return 1;
                [graphics setRenderPipelineState:render];[graphics setVertexBuffer:vertices offset:0 atIndex:0];
                for(unsigned i=0;i<8;++i) {
                    float shift[4]={0,0,0,0},color[4]={float(i)/8,0.5f,0.25f,1};
                    [graphics setVertexBytes:shift length:sizeof(shift) atIndex:1];
                    [graphics setFragmentBytes:color length:sizeof(color) atIndex:0];
                    memset(shift,0xff,sizeof(shift));memset(color,0,sizeof(color));
                    [graphics setScissorRect:MTLScissorRect{i*8,0,8,64}];
                    [graphics drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                }
                [graphics endEncoding];
            }
            auto encoder=[commands[phase] computeCommandEncoder];if(!encoder)return 1;
            [encoder setComputePipelineState:compute];[encoder setBuffer:output offset:0 atIndex:0];
            // Small snapshots roll over pages, then full-page snapshots
            // exercise the bounded packing limit and standalone fallback.
            for(unsigned i=0;i<60;++i) {
                uint32_t data[1024];memset(data,0x7b,sizeof(data));
                const unsigned index=phase*64+i;const uint32_t value=0x12340000+index*7919;
                const bool offset=i<40 && (i&1);const unsigned prefix=offset?1:0;
                data[prefix]=index;data[prefix+1]=value;expected[index]=value;
                const NSUInteger length=i>=40?4096:offset?13:8;
                [encoder setBytes:data length:length atIndex:1];
                if(offset)[encoder setBufferOffset:4 atIndex:1];
                // Callers may immediately overwrite or release their source.
                memset(data,0xcc,sizeof(data));
                [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
            }
            [encoder endEncoding];

        }
    }
    // Encode both buffers before committing either: their snapshots must
    // remain independent, even with unretained user-resource references.
    [commands[0] commit];[commands[1] commit];
    for(auto command:commands) {
        [command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) {
            fprintf(stderr,"Inline constants completion: %s\n",command.error.localizedDescription.UTF8String);return 1;
        }
    }
    if([(id)queue respondsToSelector:@selector(qcomWaitForRetiredCleanup)])
        [(id<QcomRetiredCleanupDiagnostics>)queue qcomWaitForRetiredCleanup];
    auto values=static_cast<const uint32_t *>(output.contents);
    for(unsigned i=0;i<128;++i)if(values[i]!=expected[i]) {
        fprintf(stderr,"Inline snapshot mismatch index=%u got=%x expected=%x\n",i,values[i],expected[i]);return 1;
    }
    uint8_t pixels[64*64*4];[target getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        const int wanted[]={int((x/8)*255.0/8+0.5),128,64,255};
        for(unsigned c=0;c<4;++c)if(abs(int(pixels[(y*64+x)*4+c])-wanted[c])>1) {
            fprintf(stderr,"Inline graphics mismatch x=%u y=%u channel=%u got=%u expected=%d\n",x,y,c,pixels[(y*64+x)*4+c],wanted[c]);return 1;
        }
    }
    // A warm-up CB runs after the original retirement tails, including their
    // standalone overflow allocations. Then encode TWO new buffers before
    // committing either: each needs its own snapshot even on a reused page.
    // currentAllocatedSize includes cached native resource.bytes, so obtaining
    // these private pages from the existing pool must be allocation-neutral.
    // This verifies reuse through the real adapter and GPU, not a mock cache.
    NSUInteger pooledBytes=0;
    for(unsigned epoch=0;epoch<7;++epoch) {
        const unsigned count=epoch?2:1;
        id<MTLCommandBuffer> reuse[2];
        for(unsigned n=0;n<count;++n) @autoreleasepool {
            reuse[n]=[queue commandBufferWithUnretainedReferences];
            auto encoder=[reuse[n] computeCommandEncoder];if(!encoder)return 1;
            [encoder setComputePipelineState:compute];[encoder setBuffer:output offset:0 atIndex:0];
            const unsigned index=epoch?(epoch-1)*2+n:0;
            uint32_t data[]={index,0x76540000+epoch*37+n*7919};expected[index]=data[1];
            [encoder setBytes:data length:sizeof(data) atIndex:1];memset(data,0xcc,sizeof(data));
            [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
            [encoder endEncoding];
        }
        if(epoch && device.currentAllocatedSize!=pooledBytes) {
            fprintf(stderr,"Inline reuse allocated new backing: epoch=%u before=%llu encoded=%llu\n",epoch,
                (unsigned long long)pooledBytes,(unsigned long long)device.currentAllocatedSize);return 1;
        }
        for(unsigned n=0;n<count;++n)[reuse[n] commit];
        for(unsigned n=0;n<count;++n) {
            [reuse[n] waitUntilCompleted];
            if(reuse[n].status!=MTLCommandBufferStatusCompleted) {
                fprintf(stderr,"Inline reuse completion: %s\n",reuse[n].error.localizedDescription.UTF8String);return 1;
            }
        }
        if([(id)queue respondsToSelector:@selector(qcomWaitForRetiredCleanup)])
            [(id<QcomRetiredCleanupDiagnostics>)queue qcomWaitForRetiredCleanup];
        if(!epoch)pooledBytes=device.currentAllocatedSize;
        else if(device.currentAllocatedSize!=pooledBytes) {
            fprintf(stderr,"Inline reuse allocation accounting changed: epoch=%u\n",epoch);return 1;
        }
        values=static_cast<const uint32_t *>(output.contents);
        for(unsigned i=0;i<128;++i)if(values[i]!=expected[i]) {
            fprintf(stderr,"Inline reuse mismatch epoch=%u index=%u got=%x expected=%x\n",epoch,i,values[i],expected[i]);return 1;
        }
    }
    printf("Metal inline reuse:13 GPU command buffers,6 paired unretained epochs,immediate source overwrite,128 outputs/guards per epoch,allocation-neutral bytes=%llu verified\n",(unsigned long long)pooledBytes);
    puts("Metal inline constants:120 original compute snapshots,source overwrite,odd lengths/view offsets,page rollover/fallback,2 original unretained command buffers,vertex/fragment snapshots and4096 RGBA pixels verified");
    return 0;
}

static int astcTextures(id<MTLDevice> device) {
    NSError *error=nil;auto library=diagnosticLibrary(device);auto queue=[device newCommandQueue];
    auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"position"];
    pd.fragmentFunction=[library newFunctionWithName:@"textured"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    auto pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    const float positions[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};
    auto vertices=[device newBufferWithBytes:positions length:sizeof(positions) options:MTLResourceStorageModeShared];
    auto sd=[MTLSamplerDescriptor new];sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterNearest;
    auto sampler=[device newSamplerStateWithDescriptor:sd];
    auto targetDescriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    targetDescriptor.storageMode=MTLStorageModeShared;targetDescriptor.usage=MTLTextureUsageRenderTarget;
    auto target=[device newTextureWithDescriptor:targetDescriptor];
    if(!pipeline || !vertices || !sampler || !target){fprintf(stderr,"ASTC pipeline: %s\n",error.localizedDescription.UTF8String);return 1;}
    // Khronos ASTC LDR constant-color void extents: UNORM16 RGBA, all-one
    // extent coordinates. Asymmetric colors detect channel swaps and sRGB.
    const uint16_t colors[4][4]={{0x4000,0x8000,0xc000,0xffff},{0xd000,0x1000,0x7000,0x8000},
        {0x2000,0xe000,0x6000,0xc000},{0xf000,0x5000,0x3000,0x4000}};
    uint8_t blocks[64];
    for(unsigned i=0;i<4;++i) {
        auto *block=blocks+i*16;memset(block,0xff,16);block[0]=0xfc;block[1]=0xfd;
        for(unsigned c=0;c<4;++c){block[8+c*2]=uint8_t(colors[i][c]);block[9+c*2]=uint8_t(colors[i][c]>>8);}
    }
    for(unsigned phase=0;phase<2;++phase) {
        auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:phase?MTLPixelFormatASTC_4x4_sRGB:MTLPixelFormatASTC_4x4_LDR width:7 height:5 mipmapped:NO];
        d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
        auto layout=[(_MTLDevice *)device newTextureLayoutWithDescriptor:d isHeapOrBufferBacked:NO];
        auto granularity=layout.copyGranularity;
        if(!layout || layout.size!=32784 || granularity.width!=4 || granularity.height!=4)return 1;
        void *memory=nullptr;if(posix_memalign(&memory,layout.alignment,layout.size))return 1;
        [layout initializeTextureMemory:memory];
        [layout copyFromLinearBytes:blocks linearOffset:0 linearBytesPerRow:32 linearBytesPerImage:64
            toTextureMemory:memory textureSlice:0 textureLevel:0 textureRegion:MTLRegionMake2D(0,0,7,5)];
        [layout finalizeTextureMemory:memory];
        auto backing=[device newBufferWithBytes:memory length:layout.size options:MTLResourceStorageModeShared];free(memory);
        auto texture=[backing newTextureWithDescriptor:d offset:0 bytesPerRow:1024];
        if(!texture)return 1;
        uint8_t bytes[80];memset(bytes,0xa5,sizeof(bytes));
        [texture getBytes:bytes bytesPerRow:40 fromRegion:MTLRegionMake2D(0,0,7,5) mipmapLevel:0];
        if(memcmp(bytes,blocks,32) || memcmp(bytes+40,blocks+32,32))return 1;
        for(unsigned i=0;i<80;++i)if(i%40>=32 && bytes[i]!=0xa5)return 1;
        auto commands=[queue commandBuffer];
        if(phase) {
            d.storageMode=MTLStorageModePrivate;auto copy=[device newTextureWithDescriptor:d];
            auto blit=[commands blitCommandEncoder];
            [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(7,5,1)
                toTexture:copy destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
            [blit endEncoding];texture=copy;if(!texture)return 1;
        }
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=target;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        auto draw=[commands renderCommandEncoderWithDescriptor:pass];if(!draw)return 1;
        [draw setRenderPipelineState:pipeline];[draw setVertexBuffer:vertices offset:0 atIndex:0];
        [draw setFragmentTexture:texture atIndex:0];[draw setFragmentSamplerState:sampler atIndex:0];
        [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[draw endEncoding];
        if(!completed(commands))return 1;
        uint8_t pixels[64*64*4];[target getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)for(unsigned c=0;c<4;++c) {
            unsigned tx=unsigned((x+0.5)*7/64),ty=unsigned((y+0.5)*5/64),block=(ty/4)*2+tx/4;
            double value=double(colors[block][c])/65535;
            if(phase && c<3)value=value<=0.04045?value/12.92:pow((value+0.055)/1.055,2.4);
            int wanted=int(lround(value*255));
            if(abs(int(pixels[(y*64+x)*4+c])-wanted)>2) {
                fprintf(stderr,"ASTC mismatch phase=%u x=%u y=%u c=%u got=%u expected=%d\n",phase,x,y,c,pixels[(y*64+x)*4+c],wanted);return 1;
            }
        }
        printf("ASTC_TEST phase=%u layout=pass block_copy=pass padding=pass sampled_pixels=4096 srgb=%u\n",phase,phase);
    }
    puts("Metal ASTC4x4: native layout and watermark, CPU block copies, buffer texture, private GPU copy and8192 sampled RGBA pixels verified");return 0;
}

static int presentationPipeline(id<MTLDevice> device) {
    NSError *error=nil;
    // A real bounded GPU workload gives subsequent CPU admission time to
    // overlap retirement; every output is checked with a closed-form LCG.
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void presentation_work(device uint *output [[buffer(0)]], constant uint &iterations [[buffer(1)]], uint i [[thread_position_in_grid]]) {"
        "uint x=i^0x13579bdu; for(uint n=0;n<iterations;++n) x=x*1664525u+1013904223u; output[i]=x;}\n";
    auto library=[device newLibraryWithSource:source options:nil error:&error];
    auto function=[library newFunctionWithName:@"presentation_work"];
    auto pipeline=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
    if(!pipeline){fprintf(stderr,"Presentation pipeline compile: %s\n",error.localizedDescription.UTF8String);return 1;}
    constexpr NSUInteger elements=262144;const uint32_t iterations=512;
    auto output=[device newBufferWithLength:elements*4 options:MTLResourceStorageModeShared];
    auto input=[device newBufferWithBytes:&iterations length:4 options:MTLResourceStorageModeShared];
    auto queue=[device newCommandQueue];
    auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
    auto firstTexture=surfaceTexture(device,td),secondTexture=surfaceTexture(device,td);
    auto first=[queue commandBuffer],second=[queue commandBuffer];
    if(!output || !input || !firstTexture || !secondTexture || !first || !second)return 1;
    auto calls=[NSMutableArray new];auto lock=[NSLock new];
    auto firstDrawable=[MetalctlDrawable new],secondDrawable=[MetalctlDrawable new];
    firstDrawable.texture=firstTexture;firstDrawable.calls=calls;firstDrawable.lock=lock;firstDrawable.drawableID=1;
    secondDrawable.texture=secondTexture;secondDrawable.calls=calls;secondDrawable.lock=lock;secondDrawable.drawableID=2;
    auto compute=[first computeCommandEncoder];if(!compute)return 1;
    [compute setComputePipelineState:pipeline];[compute setBuffer:output offset:0 atIndex:0];[compute setBuffer:input offset:0 atIndex:1];
    [compute dispatchThreadgroups:MTLSizeMake(elements/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];[compute endEncoding];
    auto clear=[&](id<MTLCommandBuffer> commands,id<MTLTexture> texture,MTLClearColor color) {
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=color;
        auto render=[commands renderCommandEncoderWithDescriptor:pass];if(!render)return false;[render endEncoding];return true;
    };
    if(!clear(first,firstTexture,MTLClearColorMake(1,0,0,1)) || !clear(second,secondTexture,MTLClearColorMake(0,1,0,1)))return 1;
    [first presentDrawable:firstDrawable];[second presentDrawable:secondDrawable];
    auto scheduled=dispatch_semaphore_create(0);
    __block BOOL overlapped=NO;__block NSUInteger completions=0;__block BOOL ordered=YES;
    [first addCompletedHandler:^(id<MTLCommandBuffer> commands){(void)commands;ordered&=completions++==0;}];
    [second addScheduledHandler:^(id<MTLCommandBuffer> commands){(void)commands;
        overlapped=first.status==MTLCommandBufferStatusScheduled;dispatch_semaphore_signal(scheduled);}];
    [second addCompletedHandler:^(id<MTLCommandBuffer> commands){(void)commands;ordered&=completions++==1;}];
    const auto begin=CACurrentMediaTime();[first commit];[second commit];
    const bool scheduledInTime=dispatch_semaphore_wait(scheduled,dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC))==0;
    [first waitUntilCompleted];[second waitUntilCompleted];
    const auto elapsed=CACurrentMediaTime()-begin;
    if(!scheduledInTime || first.status!=MTLCommandBufferStatusCompleted || second.status!=MTLCommandBufferStatusCompleted) {
        fprintf(stderr,"Presentation pipeline completion: first=%lu second=%lu errors=%s / %s\n",(unsigned long)first.status,(unsigned long)second.status,first.error.localizedDescription.UTF8String,second.error.localizedDescription.UTF8String);return 1;
    }
    [lock lock];BOOL presented=[calls isEqualToArray:@[@1,@2]];[lock unlock];
    if(!overlapped || !ordered || completions!=2 || !presented){fprintf(stderr,"Presentation pipeline overlap=%d order=%d callbacks=%lu presents=%s\n",overlapped,ordered,(unsigned long)completions,calls.description.UTF8String);return 1;}
    uint32_t a=1,b=0;
    for(uint32_t n=0;n<iterations;++n){a*=1664525u;b=b*1664525u+1013904223u;}
    const auto *values=static_cast<const uint32_t *>(output.contents);if(!values)return 1;
    for(NSUInteger i=0;i<elements;++i)if(values[i]!=a*(uint32_t(i)^0x13579bdu)+b){fprintf(stderr,"Presentation compute mismatch at %lu\n",(unsigned long)i);return 1;}
    uint8_t pixels[64*64*4];
    for(unsigned phase=0;phase<2;++phase) {
        auto texture=phase?secondTexture:firstTexture;[texture getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
        for(unsigned i=0;i<sizeof(pixels);++i)if(pixels[i]!=(i%4==(phase?1u:0u) || i%4==3?255:0))return 1;
    }
    printf("Metal presentation pipeline: later Scheduled before prior CPU completion; FIFO present/completion,262144 GPU outputs and8192 surface pixels verified; elapsed_ms=%.3f; optical_presentation_unverified=1\n",elapsed*1000);
    return 0;
}

@interface NSObject (MetalctlDrawableDescriptor)
- (void)setIsDrawable:(BOOL)value;
@end

static int directDrawableAdmission(id<MTLDevice> device) {
    auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
    [(id)d setIsDrawable:YES];
    auto texture=surfaceTexture(device,d);auto queue=[device newCommandQueue];
    auto view=[texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
    if(!texture || !view || !queue)return 1;
    auto target=dispatch_queue_create("metalctl.direct-drawable",DISPATCH_QUEUE_SERIAL);
    [(id<QcomMTLSubmissionControl>)queue setSubmissionQueue:target];
    auto encode=[&](id<MTLCommandBuffer> commands,unsigned n) {
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=(n&2)?view:texture;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake((n&1)?1:0,(n&1)?0:1,0,1);
        auto render=[commands renderCommandEncoderWithDescriptor:pass];if(!render)return false;
        [render endEncoding];return true;
    };
    // Occupy the configured serial target. Admission must use the real FIFO
    // executor, without waiting for a callback on this occupied target.
    __block BOOL valid=YES;
    dispatch_sync(target,^{
        for(unsigned n=0;n<8 && valid;++n) {
            auto commands=[queue commandBuffer];
            if(!encode(commands,n)){valid=NO;break;}
            [commands commit];
            valid=commands.status>=MTLCommandBufferStatusScheduled && commands.status!=MTLCommandBufferStatusError;
            if(!valid)break;
            [commands waitUntilCompleted];
            valid&=commands.status==MTLCommandBufferStatusCompleted;
        }
    });
    if(!valid){fprintf(stderr,"Direct drawable admission did not reach real Scheduled\n");return 1;}
    uint8_t pixels[64*64*4];[texture getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
    for(unsigned i=0;i<sizeof(pixels);++i)if(pixels[i]!=((i%4==0 || i%4==3)?255:0))return 1;
    // Reserved, uncommitted predecessors must retain their original FIFO;
    // committing the later drawable cannot wait for the caller's next commit.
    auto first=[queue commandBuffer],second=[queue commandBuffer];[first enqueue];[second enqueue];
    if(!encode(first,0) || !encode(second,1))return 1;
    [second commit];if(second.status!=MTLCommandBufferStatusCommitted)return 1;
    [first commit];[first waitUntilCompleted];[second waitUntilCompleted];
    if(first.status!=MTLCommandBufferStatusCompleted || second.status!=MTLCommandBufferStatusCompleted)return 1;
    // A future shared-event signal happens after commit returns. The new
    // direct-drawable path must not synchronously wait on that future signal.
    auto event=[device newSharedEvent];auto future=[queue commandBuffer];
    if(!event || !future)return 1;
    [future encodeWaitForEvent:event value:1];if(!encode(future,0))return 1;
    [future commit];if(future.status!=MTLCommandBufferStatusCommitted)return 1;
    event.signaledValue=1;[future waitUntilCompleted];
    if(future.status!=MTLCommandBufferStatusCompleted)return 1;
    puts("DIRECT_DRAWABLE_ADMISSION occupied_serial_target=pass real_scheduled=8 pixels=4096 reverse_commit_fifo=pass future_event_nonblocking=pass");
    return 0;
}

static bool archivedPipelines(id<MTLDevice> device,id<MTLLibrary> library,NSString *computeName,MTLRenderPipelineDescriptor *renderDescriptor,
    id<MTLComputePipelineState> *compute,id<MTLRenderPipelineState> *render,bool jit) {
    NSError *error=nil;auto descriptor=[MTLBinaryArchiveDescriptor new];
    auto archive=[device newBinaryArchiveWithDescriptor:descriptor error:&error];
    auto cd=[MTLComputePipelineDescriptor new];cd.computeFunction=[library newFunctionWithName:computeName];
    if(!archive || ![archive addComputePipelineFunctionsWithDescriptor:cd error:&error] ||
        ![archive addRenderPipelineFunctionsWithDescriptor:renderDescriptor error:&error])return false;
    NSURL *url=[NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]];
    @try {
        if(![archive serializeToURL:url error:&error])return false;
        descriptor.url=url;archive=[device newBinaryArchiveWithDescriptor:descriptor error:&error];
        if(!archive)return false;
        auto empty=[device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error];cd.binaryArchives=@[empty];
        if([device newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nullptr error:&error] || !error)return false;
        auto function=[&](NSString *name) {
            auto fd=[MTLFunctionDescriptor functionDescriptor];fd.name=name;fd.binaryArchives=@[archive];fd.options=MTLFunctionOptionFailOnBinaryArchiveMiss;
            return [library newFunctionWithDescriptor:fd error:&error];
        };
        auto kernel=function(computeName),vertex=function(renderDescriptor.vertexFunction.name),fragment=function(renderDescriptor.fragmentFunction.name);
        if(!kernel || !vertex || !fragment)return false;
        cd.computeFunction=kernel;cd.binaryArchives=@[archive];
        auto cachedCompute=[device newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nullptr error:&error];
        MTLRenderPipelineDescriptor *rd=[renderDescriptor copy];rd.vertexFunction=vertex;rd.fragmentFunction=fragment;rd.binaryArchives=@[archive];
        auto cachedRender=[device newRenderPipelineStateWithDescriptor:rd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nullptr error:&error];
        if(!cachedCompute || !cachedRender || (jit && cachedCompute.gpuResourceID._impl==(*compute).gpuResourceID._impl))return false;
        rd.rasterSampleCount=2;
        if([device newRenderPipelineStateWithDescriptor:rd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nullptr error:&error] || !error)return false;
        if(![device newRenderPipelineStateWithDescriptor:rd error:&error])return false; // optional miss falls back normally
        auto fd=[MTLFunctionDescriptor functionDescriptor];fd.name=computeName;fd.options=MTLFunctionOptionFailOnBinaryArchiveMiss;fd.binaryArchives=@[empty];
        if([library newFunctionWithDescriptor:fd error:&error] || !error)return false;
        NSMutableData *corrupt=[[NSData dataWithContentsOfURL:url] mutableCopy];if(!corrupt.length)return false;
        static_cast<uint8_t *>(corrupt.mutableBytes)[corrupt.length-1]^=1;
        if(![corrupt writeToURL:url options:NSDataWritingAtomic error:&error] || [device newBinaryArchiveWithDescriptor:descriptor error:&error] ||
            ![error.domain isEqualToString:MTLBinaryArchiveDomain] || error.code!=MTLBinaryArchiveErrorInvalidFile)return false;
        *compute=cachedCompute;*render=cachedRender;
        puts("Binary archives: persisted code restored, exact pipeline hit, state/empty misses and corrupt-file rejection");return true;
    } @finally {[NSFileManager.defaultManager removeItemAtURL:url error:nullptr];}
}

// Read-only framebuffer inputs keep exact HDR values and source contents.
// Exercise the actual AOT kernels with matching old/new operands. Every byte
// in the complete backing is checked, including padding and both guards.
static int clearKernels(id<MTLDevice> device,bool targetOnly=false,bool tailOnly=false) {
    auto library=diagnosticLibrary(device);auto queue=[device newCommandQueue];NSError *error=nil;
    if(!library || !queue)return 1;
    struct Layout {uint32_t width,rows,stride,offset;};
    const Layout layouts[]={{3,5,64,64},{6,7,64,64},{4,1,4,64},{8,5,64,64},{12,5,64,64},{16,1,16,64},{20,17,64,64},{24,17,64,64},{28,17,64,64},{4328,1922,4352,64},{3352,200,3392,64},{32,5,64,80},
        {1008,17,1040,64},{1024,5,1056,64},{1040,17,1088,64},{24,5,64,72},{8640,1920,8704,64},
        {4320,1920,4352,64},{8640,1920,8640,64},{2168,962,2176,64},{2184,744,2240,64},{4332,17,4352,64}};
    unsigned tests=0;uint64_t checked=0;
    const bool pairModes[]={false,true};
    for(bool pairs:pairModes)for(const auto &layout:layouts) @autoreleasepool {
        if(tailOnly && (pairs || !(layout.width==4328 || layout.width==2168 || layout.width==2184 || layout.width==4332)))continue;
        if(targetOnly && !(layout.width==1040 || (layout.rows==1920 && (layout.width==4320 || layout.stride==8640))))continue;
        const auto width=layout.width,rows=layout.rows,stride=rows==1?width:layout.stride,offset=layout.offset;
        if(pairs && ((width|stride|offset)&7))continue;
        const uint32_t low=0xbe123456,high=0x3fa12345;
        const NSUInteger total=NSUInteger(offset)+NSUInteger(rows-1)*stride+width+128;
        auto output=[device newBufferWithLength:total options:MTLResourceStorageModeShared];if(!output.contents)return 1;
        uint32_t values[8]={width,rows,stride,pairs?low:0,pairs?high:low,0,0,0};
        auto parameters=[device newBufferWithBytes:values length:sizeof(values) options:MTLResourceStorageModeShared];if(!parameters)return 1;
        const bool vectorFill=!(width&3) && !((offset|(rows>1?stride:0))&15);
        for(unsigned variant=0;variant<(vectorFill?2u:1u);++variant) {
            const char *name=pairs?(variant?((width&15)?"blit_fill_pairs_2d_tail":"blit_fill_pairs_2d"):"blit_fill_pairs"):
                (variant?((width&15)?"blit_fill_rows_2d_tail":"blit_fill_rows_2d"):"blit_fill_rows");
            auto function=[library newFunctionWithName:[NSString stringWithUTF8String:name]];
            auto pipeline=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
            if(!pipeline){fprintf(stderr,"Clear kernel %s: %s\n",name,error.localizedDescription.UTF8String);return 1;}
            memset(output.contents,0xa5,total);
            auto commands=[queue commandBuffer];auto encoder=[commands computeCommandEncoder];if(!encoder)return 1;
            [encoder setComputePipelineState:pipeline];[encoder setBuffer:output offset:offset atIndex:0];
            [encoder setBuffer:parameters offset:0 atIndex:1];
            const auto grid=variant?MTLSizeMake((width+15)/16,rows,1):MTLSizeMake(((width+3)/4)*rows,1,1);
            [encoder dispatchThreads:grid threadsPerThreadgroup:MTLSizeMake(64,1,1)];[encoder endEncoding];
            const auto begin=CACurrentMediaTime();if(!completed(commands))return 1;
            const auto elapsed=CACurrentMediaTime()-begin;
            const auto *actual=static_cast<const uint8_t *>(output.contents);
            for(NSUInteger i=0;i<total;++i) {
                uint8_t expected=0xa5;
                if(i>=offset) {
                    const auto relative=i-offset,y=relative/stride,x=relative%stride;
                    if(y<rows && x<width)expected=uint8_t((pairs && ((x/4)&1)?high:low)>>((x&3)*8));
                }
                if(actual[i]!=expected){fprintf(stderr,"Clear kernel %s byte=%lu actual=%u expected=%u\n",name,(unsigned long)i,actual[i],expected);return 1;}
            }
            const auto gpuBegin=commands.GPUStartTime,gpuEnd=commands.GPUEndTime;
            printf("CLEAR_KERNEL name=%s width_bytes=%u rows=%u stride=%u offset=%u checked_bytes=%lu elapsed_ms=%.3f gpu_ms=%.3f gpu_valid=%u guards=exact\n",
                name,width,rows,stride,offset,(unsigned long)total,elapsed*1000,(gpuEnd-gpuBegin)*1000,gpuBegin>0 && gpuEnd>=gpuBegin);
            ++tests;checked+=total;
        }
    }
    printf("CLEAR_KERNELS tests=%u checked_bytes=%llu old_new_same_layout=1\n",tests,(unsigned long long)checked);return 0;
}

static int framebufferReadOnly(id<MTLDevice> device) {
    const NSUInteger width=1040,height=67,pitch=(width*8+255)&~NSUInteger(255),offset=256,total=offset+pitch*height+256;
    NSError *error=nil;
    NSString *source=@R"SHADER(
struct FeedbackVertex { float4 position [[position]]; };
vertex FeedbackVertex readonly_vertex(device const float4 *points [[buffer(0)]],uint i [[vertex_id]]) {
 FeedbackVertex o; o.position=points[i]; return o;
}
fragment float4 readonly_fragment(float4 previous [[color(1)]]) { return previous; }
)SHADER";
    auto library=[device newLibraryWithSource:source options:nil error:&error];
    if(!library){fprintf(stderr,"Readonly feedback library: %s\n",error.localizedDescription.UTF8String);return 1;}
    auto queue=[device newCommandQueue];id<MTLBuffer> backing[2];id<MTLTexture> textures[2];
    NSMutableData *expected[2]={[NSMutableData dataWithLength:total],[NSMutableData dataWithLength:total]};
    for(unsigned plane=0;plane<2;++plane) {
        backing[plane]=[device newBufferWithLength:total options:MTLResourceStorageModeShared];
        auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:width height:height mipmapped:NO];
        td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        textures[plane]=[backing[plane] newTextureWithDescriptor:td offset:offset bytesPerRow:pitch];
        if(!backing[plane].contents || !textures[plane] || !expected[plane].mutableBytes)return 1;
        memset(expected[plane].mutableBytes,0xa5,total);
    }
    auto *input=static_cast<uint8_t *>(expected[1].mutableBytes);
    for(NSUInteger y=0;y<height;++y)for(NSUInteger x=0;x<width;++x) {
        const __fp16 pixel[]={__fp16(float((x+y)%16)*0.125f-1.0f),__fp16(float(x%8)*0.25f),__fp16(float(y%8)*0.25f),__fp16(1.0f)};
        memcpy(input+offset+y*pitch+x*8,pixel,8);
    }
    for(unsigned plane=0;plane<2;++plane)memcpy(backing[plane].contents,expected[plane].bytes,total);
    const float points[]={-1,-1,0,1, 1,-1,0,1, -1,1,0,1, -1,1,0,1, 1,-1,0,1, 1,1,0,1};
    auto vertices=[device newBufferWithBytes:points length:sizeof(points) options:MTLResourceStorageModeShared];
    auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"readonly_vertex"];
    pd.fragmentFunction=[library newFunctionWithName:@"readonly_fragment"];
    pd.colorAttachments[0].pixelFormat=pd.colorAttachments[1].pixelFormat=MTLPixelFormatRGBA16Float;
    // Leave the attachment write mask enabled: the shader has no output1.
    auto pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    if(!library || !queue || !vertices || !pipeline){fprintf(stderr,"Readonly feedback pipeline: %s\n",error.localizedDescription.UTF8String);return 1;}
    for(unsigned phase=0;phase<3;++phase) {
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        for(unsigned plane=0;plane<2;++plane) {
            pass.colorAttachments[plane].texture=textures[plane];
            pass.colorAttachments[plane].loadAction=phase && plane?MTLLoadActionClear:MTLLoadActionLoad;
            pass.colorAttachments[plane].storeAction=MTLStoreActionStore;
        }
        pass.colorAttachments[1].clearColor=phase==2?MTLClearColorMake(0,1,1,1):MTLClearColorMake(-0.25,1.5,0.375,0.75);
        auto commands=[queue commandBuffer];auto encoder=[commands renderCommandEncoderWithDescriptor:pass];
        if(!encoder)return 1;
        [encoder setRenderPipelineState:pipeline];[encoder setVertexBuffer:vertices offset:0 atIndex:0];
        [encoder setScissorRect:(MTLScissorRect){7,3,width-21,height-6}];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:2];[encoder endEncoding];
        if(!completed(commands))return 1;
        const __fp16 clear[]={__fp16(phase==2?0:-0.25f),__fp16(phase==2?1:1.5f),__fp16(phase==2?1:0.375f),__fp16(phase==2?1:0.75f)};
        if(phase)for(NSUInteger y=0;y<height;++y)for(NSUInteger x=0;x<width;++x)
            memcpy(input+offset+y*pitch+x*8,clear,8);
        auto *output=static_cast<uint8_t *>(expected[0].mutableBytes);
        for(NSUInteger y=3;y<height-3;++y)for(NSUInteger x=7;x<width-14;++x)
            memcpy(output+offset+y*pitch+x*8,input+offset+y*pitch+x*8,8);
        for(unsigned plane=0;plane<2;++plane) {
            const auto *actual=static_cast<const uint8_t *>(backing[plane].contents);
            const auto *reference=static_cast<const uint8_t *>(expected[plane].bytes);
            for(NSUInteger i=0;i<total;++i)if(actual[i]!=reference[i]) {
                fprintf(stderr,"Readonly feedback phase=%u plane=%u byte=%lu actual=%u expected=%u\n",phase,plane,(unsigned long)i,actual[i],reference[i]);return 1;
            }
        }
        printf("READONLY_FEEDBACK phase=%u pixels=%lu source=attachment1 output=attachment0 format=RGBA16Float instances=2 checked_bytes=%lu guards=exact source_unchanged=1 producer=%s\n",
            phase,(unsigned long)((width-21)*(height-6)),(unsigned long)(2*total),phase?"GPU_clear_same_command_buffer":"CPU_pattern");
    }
    return 0;
}
// Destination fetch also observes earlier overlapping primitives.
static int framebufferSnapshots(id<MTLDevice> device,NSUInteger width=1040,NSUInteger height=67) {
    const NSUInteger pitch=(width*4+255)&~NSUInteger(255),offset=256,bytes=pitch*height,total=bytes+512;
    const NSUInteger right=width-14,bottom=height-3;
    if(width<32 || height<16 || total>device.maxBufferLength)return 1;
    auto library=diagnosticLibrary(device);auto queue=[device newCommandQueue];NSError *error=nil;
    auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    auto backing=[device newBufferWithLength:total options:MTLResourceStorageModeShared];
    auto target=backing?[backing newTextureWithDescriptor:td offset:offset bytesPerRow:pitch]:nil;
    auto expected=[NSMutableData dataWithLength:total];
    if(!library || !queue || !target || !backing.contents || !expected.mutableBytes)return 1;
    auto *reference=static_cast<uint8_t *>(expected.mutableBytes);memset(reference,0xa5,total);
    for(NSUInteger y=0;y<height;++y)for(NSUInteger x=0;x<width;++x)for(unsigned c=0;c<4;++c)
        reference[offset+y*pitch+x*4+c]=uint8_t(x*17+y*31+c*67);
    memcpy(backing.contents,reference,total);
    const float points[]={-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    auto vertices=[device newBufferWithBytes:points length:sizeof(points) options:MTLResourceStorageModeShared];
    auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"position"];
    pd.fragmentFunction=[library newFunctionWithName:@"framebuffer_source_over"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    auto state=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    if(!vertices || !state){fprintf(stderr,"Framebuffer snapshot pipeline: %s\n",error.localizedDescription.UTF8String);return 1;}
    for(unsigned phase=0;phase<4;++phase) {
        const float color[]={phase?0.25f:0,phase?0.125f:0,phase?0.0625f:0,phase?0.5f:0};
        const NSUInteger left=phase<2?7:phase==2?width/3+1:width-19;
        const NSUInteger edge=phase<2?right:phase==2?left+13:width-1;
        auto uniform=[device newBufferWithBytes:color length:sizeof(color) options:MTLResourceStorageModeShared];
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=target;
        pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        auto commands=[queue commandBuffer];auto draw=[commands renderCommandEncoderWithDescriptor:pass];
        if(!uniform || !draw)return 1;
        [draw setRenderPipelineState:state];[draw setVertexBuffer:vertices offset:0 atIndex:0];[draw setFragmentBuffer:uniform offset:0 atIndex:0];
        if(phase)[draw setScissorRect:(MTLScissorRect){left,3,edge-left,bottom-3}];
        [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:phase?2:1];[draw endEncoding];
        if(!completed(commands))return 1;
        if(phase)for(NSUInteger y=3;y<bottom;++y)for(NSUInteger x=left;x<edge;++x)for(unsigned c=0;c<4;++c) {
            auto &value=reference[offset+y*pitch+x*4+c];
            for(unsigned primitive=0;primitive<2;++primitive)value=uint8_t(double(value)*0.5+double(color[c])*255+0.5);
        }
        const auto *actual=static_cast<const uint8_t *>(backing.contents);
        for(NSUInteger i=0;i<total;++i) {
            const NSUInteger x=i>=offset?(i-offset)%pitch/4:width,y=i>=offset?(i-offset)/pitch:height;
            const bool inside=phase && i>=offset && i<offset+bytes && y>=3 && y<bottom &&
                ((x>=7 && x<right) || (phase>=2 && x>=width/3+1 && x<width/3+14) ||
                 (phase>=3 && x>=width-19 && x<width-1));
            // Alpha UNORM ties may round differently. Identity, RGB,
            // untouched pixels, row padding and allocation guards are exact.
            const int tolerance=inside && (i-offset)%4==3?1:0,difference=int(actual[i])-reference[i];
            if(difference < -tolerance || difference > tolerance) {
                fprintf(stderr,"Framebuffer snapshot phase=%u byte=%lu actual=%u expected=%u tolerance=%d\n",
                    phase,(unsigned long)i,actual[i],reference[i],tolerance);
                const NSUInteger begin=i>16?i-16:0,end=i+32<total?i+32:total;
                for(NSUInteger at=begin;at<end;++at)fprintf(stderr," %lu:%u/%u",(unsigned long)at,actual[at],reference[at]);
                fputc('\n',stderr);return 1;
            }
        }
        printf("FRAMEBUFFER_SNAPSHOT phase=%u width=%lu height=%lu row=%lu checked_bytes=%lu instances=%u guards=exact identity=%u\n",
            phase,(unsigned long)width,(unsigned long)height,(unsigned long)pitch,(unsigned long)total,phase?2:1,!phase);
    }
    return 0;
}
// GPU-produced half payloads consumed in the same command buffer. Native
// completion/pixels validate dependency and rebinding; this is not an FPS test.
// Host compiler tests extract this exact literal to verify that the native
// dependency diagnostic really has BOTH a constant window and framebuffer
// fetch. White input makes its quarter-color contribution exactly half-safe.
static const char constantWindowFramebufferSource[]=R"QGPU(
; SPDX-License-Identifier: BSD-2-Clause
target datalayout = "e-p:64:64-i16:16-i32:32-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
define <4 x half> @constant_window_framebuffer(ptr addrspace(2) %input, <4 x half> %destination) {
 %p0 = getelementptr i8, ptr addrspace(2) %input, i64 176
 %p1 = getelementptr i8, ptr addrspace(2) %input, i64 184
 %p2 = getelementptr i8, ptr addrspace(2) %input, i64 192
 %p3 = getelementptr i8, ptr addrspace(2) %input, i64 200
 %p4 = getelementptr i8, ptr addrspace(2) %input, i64 208
 %v0 = load <4 x half>, ptr addrspace(2) %p0, align 2
 %v1 = load <4 x half>, ptr addrspace(2) %p1, align 2
 %v2 = load <4 x half>, ptr addrspace(2) %p2, align 2
 %v3 = load <4 x half>, ptr addrspace(2) %p3, align 2
 %v4 = load <4 x half>, ptr addrspace(2) %p4, align 2
 %s0 = fadd <4 x half> %v0, %v1
 %s1 = fadd <4 x half> %s0, %v2
 %s2 = fadd <4 x half> %s1, %v3
 %s3 = fadd <4 x half> %s2, %v4
 %quarter = fmul <4 x half> %destination, <half 0xH3400, half 0xH3400, half 0xH3400, half 0xH3400>
 %result = fadd <4 x half> %s3, %quarter
 ret <4 x half> %result
}
!air.version = !{!0}
!air.fragment = !{!1}
!0 = !{i32 2, i32 7, i32 0}
!1 = !{ptr @constant_window_framebuffer, !2, !3}
!2 = !{!4}
!3 = !{!5, !6}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"half4"}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 2, !"air.arg_type_align_size", i32 2, !"air.arg_type_name", !"half", !"air.arg_name", !"input"}
!6 = !{i32 1, !"air.render_target", i32 0, !"air.arg_type_name", !"half4"}
)QGPU";
static int constantWindowTest(id<MTLDevice> device) {
    NSError *error=nil;id<MTLLibrary> library=diagnosticLibrary(device);
    id<MTLCommandQueue> queue=[device newCommandQueue];
    id<MTLComputePipelineState> producer=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"constant_window_producer"] error:&error];
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
    pd.vertexFunction=[library newFunctionWithName:@"position"];pd.fragmentFunction=[library newFunctionWithName:@"constant_window_half"];
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> consumer=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    dispatch_data_t framebufferData=dispatch_data_create(constantWindowFramebufferSource,sizeof(constantWindowFramebufferSource)-1,dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{});
    id<MTLLibrary> framebufferLibrary=[device newLibraryWithData:framebufferData error:&error];
    pd.fragmentFunction=[framebufferLibrary newFunctionWithName:@"constant_window_framebuffer"];
    id<MTLRenderPipelineState> framebufferConsumer=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> target=[device newTextureWithDescriptor:td];
    const float vertices[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};
    id<MTLBuffer> vertex=[device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    id<MTLBuffer> full=[device newBufferWithLength:224 options:MTLResourceStorageModeShared];
    id<MTLBuffer> shortBuffer=[device newBufferWithLength:216 options:MTLResourceStorageModeShared];
    id<MTLBuffer> partial=[device newBufferWithLength:212 options:MTLResourceStorageModeShared];
    id<MTLBuffer> unaligned=[device newBufferWithLength:232 options:MTLResourceStorageModeShared];
    if(!library || !framebufferLibrary || !queue || !producer || !consumer || !framebufferConsumer || !target || !vertex || !full || !shortBuffer || !partial || !unaligned) {
        fprintf(stderr,"Constant-window setup failed: %s\n",error.localizedDescription.UTF8String);return 1;
    }
    id<MTLBuffer> inputs[]={full,shortBuffer,partial,unaligned,full};
    const NSUInteger offsets[]={0,0,0,4,0};const uint32_t phases[]={0,1,2,3,3};
    for(unsigned mode=0;mode<2;++mode)for(unsigned test=0;test<5;++test) {
        id<MTLBuffer> input=inputs[test];const NSUInteger offset=offsets[test],bytes=input.length-offset;
        memset(input.contents,0x5a,input.length);
        id<MTLCommandBuffer> commands=[queue commandBuffer];
        id<MTLComputeCommandEncoder> compute=[commands computeCommandEncoder];
        [compute setComputePipelineState:producer];[compute setBuffer:input offset:offset atIndex:0];
        [compute setBytes:&phases[test] length:sizeof(uint32_t) atIndex:1];
        [compute dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];[compute endEncoding];
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(mode?1:0,mode?1:0,mode?1:0,mode?1:0);
        id<MTLRenderCommandEncoder> render=[commands renderCommandEncoderWithDescriptor:pass];
        [render setRenderPipelineState:mode?framebufferConsumer:consumer];[render setVertexBuffer:vertex offset:0 atIndex:0];
        [render setFragmentBuffer:input offset:offset atIndex:1];
        [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[render endEncoding];
        if(!completed(commands))return 1;
        uint8_t expected[4];
        for(unsigned lane=0;lane<4;++lane) {
            unsigned units=mode?8:0;
            for(unsigned row=0;row<5;++row)if(176+8*row+2*lane+2<=bytes)units+=(row+lane+phases[test])%4+1;
            expected[lane]=uint8_t((units*255+16)/32);
        }
        uint8_t pixels[64*64*4];[target getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
        for(unsigned pixel=0;pixel<4096;++pixel)if(memcmp(pixels+pixel*4,expected,4)) {
            fprintf(stderr,"Constant-window test=%u pixel=%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",test,pixel,pixels[pixel*4],pixels[pixel*4+1],pixels[pixel*4+2],pixels[pixel*4+3],expected[0],expected[1],expected[2],expected[3]);return 1;
        }
        const auto *raw=static_cast<const uint8_t *>(input.contents);
        for(NSUInteger i=0;i<offset;++i)if(raw[i]!=0x5a)return 1;
        for(NSUInteger i=offset+224;i<input.length;++i)if(raw[i]!=0x5a)return 1;
        printf("CONSTANT_WINDOW test=%u framebuffer=%u bytes=%lu offset=%lu phase=%u pixels_correct=4096 producer=GPU same_command_buffer=1 rebind=%u expected=%u,%u,%u,%u\n",test,mode,(unsigned long)bytes,(unsigned long)offset,phases[test],test==4,expected[0],expected[1],expected[2],expected[3]);
    }
    return 0;
}
static const char wordWindowSource[]=R"QGPU(
#include <metal_stdlib>
using namespace metal;
kernel void word_producer(device float *output [[buffer(0)]], constant uint &phase [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    output[i]=float(((i*13u+phase*29u)&255u)+1u)*0.001953125f;
}
vertex float4 word_vertex(device const float4 *points [[buffer(0)]], uint i [[vertex_id]]) {return points[i];}
fragment float4 word_consumer(constant float4 *input [[buffer(1)]]) {
    return (input[0]+input[1]+input[2]+input[3]+input[4]+input[5]+input[6]+input[7]+
            input[8]+input[9]+input[10]+input[11]+input[12]+input[13]+input[14]+input[15])*0.0625f;
}
)QGPU";
static int wordWindowTest(id<MTLDevice> device) {
    NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithSource:[NSString stringWithUTF8String:wordWindowSource] options:nil error:&error];
    if(!library){fprintf(stderr,"Word-window library failed: %s\n",error.localizedDescription.UTF8String);return 1;}
    id<MTLCommandQueue> queue=[device newCommandQueue];
    id<MTLComputePipelineState> producer=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"word_producer"] error:&error];
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
    pd.vertexFunction=[library newFunctionWithName:@"word_vertex"];pd.fragmentFunction=[library newFunctionWithName:@"word_consumer"];
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> consumer=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    pd.rasterSampleCount=4;
    id<MTLRenderPipelineState> consumerMSAA=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> target=[device newTextureWithDescriptor:td];
    td.textureType=MTLTextureType2DMultisample;td.sampleCount=4;td.storageMode=MTLStorageModePrivate;
    id<MTLTexture> targetMSAA=[device newTextureWithDescriptor:td];
    const float points[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};
    id<MTLBuffer> vertex=[device newBufferWithBytes:points length:sizeof(points) options:MTLResourceStorageModeShared];
    // A float4 buffer binding must contain at least one full element. Twenty
    // bytes still exercises a partial second vector through bounded loads.
    const NSUInteger lengths[]={256,252,196,128,64,20,260,256,256},offsets[]={0,0,0,0,0,0,4,0,0};
    id<MTLBuffer> full=[device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if(!queue || !producer || !consumer || !consumerMSAA || !target || !targetMSAA || !vertex || !full){fprintf(stderr,"Word-window setup failed: %s\n",error.localizedDescription.UTF8String);return 1;}
    for(unsigned test=0;test<9;++test) {
        const bool multisample=test==8;
        id<MTLBuffer> input=test==0 || test>=7?full:[device newBufferWithLength:lengths[test] options:MTLResourceStorageModeShared];
        if(!input)return 1;
        memset(input.contents,0x5a,input.length);const NSUInteger offset=offsets[test],bytes=input.length-offset;
        uint32_t phase=test;
        id<MTLCommandBuffer> commands=[queue commandBuffer];
        id<MTLComputeCommandEncoder> compute=[commands computeCommandEncoder];
        [compute setComputePipelineState:producer];[compute setBuffer:input offset:offset atIndex:0];
        [compute setBytes:&phase length:sizeof(phase) atIndex:1];
        [compute dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];[compute endEncoding];
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=multisample?targetMSAA:target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=multisample?MTLStoreActionMultisampleResolve:MTLStoreActionStore;
        if(multisample)pass.colorAttachments[0].resolveTexture=target;
        id<MTLRenderCommandEncoder> render=[commands renderCommandEncoderWithDescriptor:pass];
        [render setRenderPipelineState:multisample?consumerMSAA:consumer];[render setVertexBuffer:vertex offset:0 atIndex:0];[render setFragmentBuffer:input offset:offset atIndex:1];
        [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[render endEncoding];
        if(!completed(commands))return 1;
        uint8_t expected[4],pixels[64*64*4];
        for(unsigned lane=0;lane<4;++lane) {
            unsigned sum=0;
            for(unsigned row=0;row<16;++row){unsigned i=4*row+lane;if(4*i+4<=bytes)sum+=((i*13+phase*29)&255)+1;}
            expected[lane]=uint8_t((sum*255+4096)/8192);
        }
        [target getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
        for(unsigned pixel=0;pixel<4096;++pixel)if(memcmp(pixels+4*pixel,expected,4)) {
            fprintf(stderr,"Word-window test=%u pixel=%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",test,pixel,pixels[4*pixel],pixels[4*pixel+1],pixels[4*pixel+2],pixels[4*pixel+3],expected[0],expected[1],expected[2],expected[3]);return 1;
        }
        const auto *raw=static_cast<const uint8_t *>(input.contents);
        for(NSUInteger i=0;i<offset;++i)if(raw[i]!=0x5a)return 1;
        printf("WORD_WINDOW test=%u bytes=%lu offset=%lu phase=%u pixels_correct=4096 producer=GPU same_command_buffer=1 rebind=%u samples=%u\n",test,(unsigned long)bytes,(unsigned long)offset,phase,test>=7,multisample?4:1);
    }
    return 0;
}
static int pipeline(id<MTLDevice> device, bool air = false, bool jit = false, bool states = false, bool indexed = false, bool varying = false, bool attributes = false, bool surfaces = false) {
    NSError *error = nil;
    NSString *airPath = [driverBundlePath() stringByAppendingPathComponent:@"Shaders.air"];
    id<MTLLibrary> library = air ? [device newLibraryWithURL:[NSURL fileURLWithPath:airPath] error:&error] : diagnosticLibrary(device);
    if (jit) {
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void add(device const uint *input [[buffer(0)]], device uint *output [[buffer(1)]], uint i [[thread_position_in_grid]]) { output[i] = input[i] + 19u; }\n"
            "vertex float4 position(device const float4 *vertices [[buffer(0)]], uint i [[vertex_id]]) { return vertices[i]; }\n"
            "fragment float4 color() { return float4(0.25,0.5,0.75,1.0); }\n"
            "fragment float4 textured(texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]], float4 pixel [[position]]) { return image.sample(filtering,pixel.xy/float2(64.0,64.0)); }";
        library = [device newLibraryWithSource:source options:nil error:&error];
    }
    if (!library) {fprintf(stderr,"Library allocation failed: %s\n",error.localizedDescription.UTF8String ?: "missing library");return 1;}
    NSString *prefix = air ? @"air_" : @"";
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLComputePipelineState> compute = [device newComputePipelineStateWithFunction:[library newFunctionWithName:[prefix stringByAppendingString:@"add"]] error:&error];
    id<MTLBuffer> input = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if (!library || !queue || !compute || !input || !output) { fprintf(stderr,"Pipeline allocation failed: %s\n",error.localizedDescription.UTF8String); return 1; }
    for (unsigned i=0;i<64;++i) ((uint32_t *)input.contents)[i] = i;
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    if (!encoder) return 1;
    [encoder setComputePipelineState:compute]; [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)]; [encoder endEncoding];
    if (!completed(commands)) return 1;
    for (unsigned i=0;i<64;++i) if (((uint32_t *)output.contents)[i] != i+(jit?19u:7u)) { fprintf(stderr,"Compute mismatch at %u\n",i); return 1; }
    const float vertices[] = {-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    id<MTLBuffer> vertex = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    id<MTLBuffer> vertexColors = nil, vertexCoordinates = nil;
    if (varying) {
        const float gradient[] = {0,1,0.25,1, 2,1,0.25,1, 0,-1,0.25,1};
        const float coordinates[] = {0,1,0,0, 2,1,0,0, 0,-1,0,0};
        vertexColors = [device newBufferWithBytes:gradient length:sizeof(gradient) options:MTLResourceStorageModeShared];
        vertexCoordinates = [device newBufferWithBytes:coordinates length:sizeof(coordinates) options:MTLResourceStorageModeShared];
        if (!vertexColors || !vertexCoordinates) return 1;
    }
    id<MTLBuffer> offsets = nil, index16 = nil, index32 = nil, privateIndices = nil;
    if (indexed) {
        const float biased[] = {100,100,0,1, -1,-1,0,1, 3,-1,0,1, -1,3,0,1};
        const float instances[] = {100,100,0,0, 0,0,0,0, 0,0,0,0};
        const uint16_t words16[] = {0xaaaa,0xbbbb,0,1,2,0};
        const uint32_t words32[] = {0xaaaaaaaa,0,1,2};
        vertex = [device newBufferWithBytes:biased length:sizeof(biased) options:MTLResourceStorageModeShared];
        offsets = [device newBufferWithBytes:instances length:sizeof(instances) options:MTLResourceStorageModeShared];
        index16 = [device newBufferWithBytes:words16 length:sizeof(words16) options:MTLResourceStorageModeShared];
        index32 = [device newBufferWithBytes:words32 length:sizeof(words32) options:MTLResourceStorageModeShared];
        privateIndices = [device newBufferWithLength:16 options:MTLResourceStorageModePrivate];
        if (!vertex || !offsets || !index16 || !index32 || !privateIndices) return 1;
    }
    if (attributes) {
        struct PackedVertex { float position[3];uint8_t color[4]; } packed[3]={};
        for (unsigned i=0;i<3;++i) {
            memcpy(packed[i].position,vertices+i*4,12);
            const uint8_t color[]={64,128,191,255};memcpy(packed[i].color,color,4);
        }
        const float steps[]={100,100, 100,100, 0,0, 100,100};
        vertex=[device newBufferWithBytes:packed length:sizeof(packed) options:MTLResourceStorageModeShared];
        offsets=[device newBufferWithBytes:steps length:sizeof(steps) options:MTLResourceStorageModeShared];
        if (!vertex || !offsets) return 1;
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    id<MTLTexture> target=surfaces?surfaceTexture(device,td):[device newTextureWithDescriptor:td];
    id<MTLTexture> upload=surfaces?surfaceTexture(device,td):[device newTextureWithDescriptor:td];
    id<MTLTexture> readback=surfaces && target?[device newTextureWithDescriptor:td iosurface:target.iosurface plane:0]:target;
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> sampled=surfaces?surfaceTexture(device,td):[device newTextureWithDescriptor:td];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
    if (!vertex || !target || !readback || !upload || !sampled || !sampler) return 1;
    if (surfaces && (!target.isShareable || readback.iosurface!=target.iosurface || target.iosurfacePlane)) return 1;
    uint8_t pixels[64*64*4];
    for (unsigned i=0;i<sizeof(pixels);i+=4) { pixels[i]=17; pixels[i+1]=34; pixels[i+2]=51; pixels[i+3]=255; }
    if (varying) for (unsigned i=0;i<sizeof(pixels);i+=4) {
        pixels[i]=uint8_t((i/4)%64*4);pixels[i+1]=uint8_t((i/4)/64*4);
    }
    [upload replaceRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0 withBytes:pixels bytesPerRow:256];
    commands = [queue commandBuffer]; id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (!blit) return 1;
    [blit copyFromTexture:upload toTexture:sampled]; [blit endEncoding]; if (!completed(commands)) return 1;
    for (NSString *name in @[@"color",@"textured"]) {
        if (attributes && ![name isEqualToString:@"color"]) continue;
        bool raster = states && [name isEqualToString:@"color"];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:[prefix stringByAppendingString:attributes ? @"attribute_position" : varying ? @"varying_position" : indexed ? @"instanced_position" : @"position"]];
        NSString *fragment = attributes ? @"attribute_color" : varying ? [@"varying_" stringByAppendingString:name] : name;
        pd.fragmentFunction = [library newFunctionWithName:[prefix stringByAppendingString:fragment]];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        if (raster) {
            auto color = pd.colorAttachments[0]; color.blendingEnabled = YES;
            color.writeMask = MTLColorWriteMaskRed|MTLColorWriteMaskAlpha;
            color.sourceRGBBlendFactor = color.sourceAlphaBlendFactor = MTLBlendFactorBlendAlpha;
            color.destinationRGBBlendFactor = color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusBlendAlpha;
        }
        if (indexed && [name isEqualToString:@"color"]) {
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
            pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
            pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        }
        if (attributes) {
            MTLVertexDescriptor *vd=[MTLVertexDescriptor vertexDescriptor];
            vd.attributes[0].format=MTLVertexFormatFloat3;vd.attributes[0].bufferIndex=0;vd.attributes[0].offset=0;
            vd.attributes[3].format=MTLVertexFormatUChar4Normalized;vd.attributes[3].bufferIndex=0;vd.attributes[3].offset=12;
            vd.attributes[7].format=MTLVertexFormatFloat2;vd.attributes[7].bufferIndex=1;vd.attributes[7].offset=0;
            vd.layouts[0].stride=16;vd.layouts[0].stepFunction=MTLVertexStepFunctionPerVertex;
            vd.layouts[1].stride=8;vd.layouts[1].stepFunction=MTLVertexStepFunctionPerInstance;vd.layouts[1].stepRate=2;
            pd.vertexDescriptor=vd;
        }
        id<MTLRenderPipelineState> render = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (attributes) pd.vertexDescriptor.attributes[0].format=MTLVertexFormatInvalid; // pipeline owns its snapshot
        if (!render) { fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 1; }
        if([name isEqualToString:@"color"] && (jit || (!air && !states && !indexed && !varying && !attributes && !surfaces))) {
            if(!archivedPipelines(device,library,[prefix stringByAppendingString:@"add"],pd,&compute,&render,jit))return 1;
            commands=[queue commandBuffer];encoder=[commands computeCommandEncoder];if(!encoder)return 1;
            [encoder setComputePipelineState:compute];[encoder setBuffer:input offset:0 atIndex:0];[encoder setBuffer:output offset:0 atIndex:1];
            [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];[encoder endEncoding];
            if(!completed(commands))return 1;
            for(unsigned i=0;i<64;++i)if(static_cast<const uint32_t *>(output.contents)[i]!=i+(jit?19u:7u))return 1;
        }
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore; pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0);
        if (raster) pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0.2,0.4,0);
        commands = [queue commandBuffer];
        if (indexed) {
            // Private index data is produced by an earlier GPU operation in
            // this command buffer; the kernel scans it only at draw execution.
            id<MTLBlitCommandEncoder> copy = [commands blitCommandEncoder];
            id<MTLBuffer> source = [name isEqualToString:@"color"] ? index16 : index32;
            [copy copyFromBuffer:source sourceOffset:0 toBuffer:privateIndices destinationOffset:0 size:source.length];
            [copy endEncoding];
        }
        id<MTLRenderCommandEncoder> draw = [commands renderCommandEncoderWithDescriptor:pass];
        if (!draw) return 1;
        [draw setRenderPipelineState:render]; [draw setVertexBuffer:vertex offset:0 atIndex:0];
        if (varying) {
            [draw setVertexBuffer:vertexColors offset:0 atIndex:1];
            [draw setVertexBuffer:vertexCoordinates offset:0 atIndex:2];
        }
        if (raster) {
            [draw setViewport:(MTLViewport){16,8,32,24,0,1}];
            [draw setBlendColorRed:0 green:0 blue:0 alpha:0.5];
            [draw setCullMode:MTLCullModeNone]; [draw setFrontFacingWinding:MTLWindingCounterClockwise];
        }
        if ([name isEqualToString:@"textured"]) { [draw setFragmentTexture:sampled atIndex:0]; [draw setFragmentSamplerState:sampler atIndex:0]; }
        if (attributes) {
            [draw setVertexBuffer:offsets offset:0 atIndex:1];
            [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:2 baseInstance:2];
        } else if (indexed) {
            [draw setVertexBuffer:offsets offset:0 atIndex:1];
            [draw drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3
                indexType:[name isEqualToString:@"color"] ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                indexBuffer:privateIndices indexBufferOffset:4 instanceCount:2 baseVertex:1 baseInstance:1];
        } else [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        if (raster) {
            // A later state change must not modify the recorded draw snapshot.
            [draw setViewport:(MTLViewport){0,0,64,64,0,1}]; [draw setCullMode:MTLCullModeFront];
            [draw setBlendColorRed:0 green:0 blue:0 alpha:0];
        }
        [draw endEncoding];
        uint32_t seed=surfaces?IOSurfaceGetSeed(target.iosurface):0;
        if (!completed(commands)) return 1;
        if (surfaces && seed==IOSurfaceGetSeed(target.iosurface)) {fprintf(stderr,"IOSurface modification seed did not advance\n");return 1;}
        [readback getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
        bool textured = [name isEqualToString:@"textured"];
        const uint8_t expected[] = {uint8_t(textured?17:indexed?128:64),uint8_t(textured?34:indexed?255:128),uint8_t(textured?51:indexed?255:191),255};
        for (unsigned i=0;i<sizeof(pixels);i+=4) {
            if (varying) {
                unsigned x=(i/4)%64,y=(i/4)/64;
                double r=(x+0.5)/64,g=(y+0.5)/64;
                const double expectedVarying[]={textured?x*4*r:r*255,textured?y*4*g:g*255,textured?12.75:63.75,255};
                for (unsigned c=0;c<4;++c) if (pixels[i+c]<expectedVarying[c]-1.5 || pixels[i+c]>expectedVarying[c]+1.5) {
                    fprintf(stderr,"Varying color/UV mismatch at %u,%u component %u\n",x,y,c);return 1;
                }
            } else if (raster) {
                unsigned x = (i/4)%64, y = (i/4)/64; bool inside = x >= 16 && x < 48 && y >= 8 && y < 32;
                int red = inside ? 32 : 0, alpha = inside ? 128 : 0;
                if (int(pixels[i])-red < -1 || int(pixels[i])-red > 1 || pixels[i+1] != 51 || pixels[i+2] != 102 ||
                    int(pixels[i+3])-alpha < -1 || int(pixels[i+3])-alpha > 1) {
                    fprintf(stderr,"Blend/mask/viewport mismatch at pixel %u,%u\n",x,y); return 1;
                }
            } else if (memcmp(pixels+i,expected,4)) {
                fprintf(stderr,"%s render mismatch at pixel %u\n",name.UTF8String,i/4); return 1;
            }
        }
    }
    puts(surfaces ? "Native IOSurface import, padded stride/offset, shared aliases, GPU render/blit/sample, seed and CPU readback verified" : attributes ? "Interleaved Float3/normalized UChar4, per-instance divisor/baseInstance and descriptor snapshots verified" : varying ? "Semantic-linked vertex colors and UVs, fragment interpolation and gradient texture sampling verified" :
        indexed ? "Private UInt16/UInt32 indexed draws, baseVertex, instance_id/baseInstance and additive instances verified" :
        states ? "Blend constants, component mask, partial viewport and per-draw state snapshots verified" :
        jit ? "JIT source -> A540 compute/render/texture submission verified" : "AOT compute, sysmem render, private texture upload/sampling and shared readback verified"); return 0;
}

// Optional device-side entry point. Building this program never runs it.
// Optional real-GPU depth/stencil exercise, compiled offline and never run by make.
static int depthStencil(id<MTLDevice> device) {
    NSError *error=nil;id<MTLLibrary> library=diagnosticLibrary(device);
    id<MTLCommandQueue> queue=[device newCommandQueue];
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> color=[device newTextureWithDescriptor:td];
    td.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;td.storageMode=MTLStorageModePrivate;
    id<MTLTexture> zs=[device newTextureWithDescriptor:td];
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
    pd.vertexFunction=[library newFunctionWithName:@"position"];pd.fragmentFunction=[library newFunctionWithName:@"color"];
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    pd.depthAttachmentPixelFormat=pd.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    MTLDepthStencilDescriptor *ds=[MTLDepthStencilDescriptor new];ds.depthCompareFunction=MTLCompareFunctionLess;ds.depthWriteEnabled=YES;
    MTLStencilDescriptor *face=[MTLStencilDescriptor new];face.stencilCompareFunction=MTLCompareFunctionAlways;
    face.depthStencilPassOperation=MTLStencilOperationReplace;face.depthFailureOperation=MTLStencilOperationIncrementWrap;
    ds.frontFaceStencil=ds.backFaceStencil=face;
    id<MTLDepthStencilState> state=[device newDepthStencilStateWithDescriptor:ds];
    const float vertices[]={-1,-1,0.25,1,3,-1,0.25,1,-1,3,0.25,1,
        -1,-1,0.75,1,3,-1,0.75,1,-1,3,0.75,1};
    id<MTLBuffer> vertex=[device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    id<MTLBuffer> depths=[device newBufferWithLength:16384 options:MTLResourceStorageModeShared];
    id<MTLBuffer> stencils=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    if (!library || !queue || !color || !zs || !pipeline || !state || !vertex || !depths || !stencils) {
        fprintf(stderr,"Depth/stencil allocation failed: %s\n",error.localizedDescription.UTF8String);return 1;
    }
    for (unsigned passIndex=0;passIndex<2;++passIndex) {
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.depthAttachment.texture=zs;pass.stencilAttachment.texture=zs;
        pass.depthAttachment.loadAction=pass.stencilAttachment.loadAction=passIndex?MTLLoadActionLoad:MTLLoadActionClear;
        pass.depthAttachment.storeAction=pass.stencilAttachment.storeAction=MTLStoreActionStore;
        pass.depthAttachment.clearDepth=1;pass.stencilAttachment.clearStencil=0;
        id<MTLCommandBuffer> commands=[queue commandBuffer];id<MTLRenderCommandEncoder> render=[commands renderCommandEncoderWithDescriptor:pass];
        if (!render) return 1;
        [render setRenderPipelineState:pipeline];[render setDepthStencilState:state];[render setStencilReferenceValue:58];
        [render setVertexBuffer:vertex offset:0 atIndex:0];
        [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:passIndex*3 vertexCount:3];[render endEncoding];
        if (passIndex) {
            id<MTLBlitCommandEncoder> copy=[commands blitCommandEncoder];if (!copy) return 1;
            [copy copyFromTexture:zs sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1)
                toBuffer:depths destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:0 options:MTLBlitOptionDepthFromDepthStencil];
            [copy copyFromTexture:zs sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1)
                toBuffer:stencils destinationOffset:0 destinationBytesPerRow:64 destinationBytesPerImage:0 options:MTLBlitOptionStencilFromDepthStencil];
            [copy endEncoding];
        }
        if (!completed(commands)) return 1;
    }
    uint8_t pixels[16384];[color getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
    for (unsigned i=0;i<4096;++i) if (((float *)depths.contents)[i]!=0.25f || ((uint8_t *)stencils.contents)[i]!=59 ||
        pixels[i*4] || pixels[i*4+1] || pixels[i*4+2] || pixels[i*4+3]) {
        fprintf(stderr,"Depth/stencil occlusion, store/load or aspect readback mismatch at %u\n",i);return 1;
    }
    puts("D32/S8 clear, depth rejection, stencil depth-fail, cross-pass retention and aspect readback verified");return 0;
}

static int fluidityAssets(id<MTLDevice> device) {
    @try {
        void *module=dlopen("/System/Library/Frameworks/MetalKit.framework/MetalKit",RTLD_NOW);
        if(!module){fprintf(stderr,"FLUIDITY_ASSET load_error=%s\n",dlerror());return 1;}
        auto key=static_cast<NSString * __unsafe_unretained *>(dlsym(module,"MTKTextureLoaderOptionTextureStorageMode"));
        id loader=[[NSClassFromString(@"MTKTextureLoader") alloc] initWithDevice:device];
        if(!key || !loader)return 1;
        NSString *base=@"/System/Library/ExtensionKit/Extensions/MercuryPosterExtension.appex";
        auto files=[NSFileManager.defaultManager contentsOfDirectoryAtPath:base error:nil];
        printf("FLUIDITY_ASSET directory_count=%lu\n",(unsigned long)files.count);fflush(stdout);
        NSError *shaderError=nil;
        auto library=[device newLibraryWithSource:@R"SHADER(struct U16Vertex { float4 position [[position]]; };
vertex U16Vertex u16_vertex(uint i [[vertex_id]]) {
 U16Vertex o; float x=i==1?3.0:-1.0;float y=i==2?3.0:-1.0;o.position=float4(x,y,0.0,1.0);return o;
}
fragment float4 u16_fragment(float4 pos [[position]],texture2d<float> image [[texture(0)]],constant uint2 &origin [[buffer(0)]]) {
 return image.read(uint2(pos.xy)+origin);
}
)SHADER" options:nil error:&shaderError];
        auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"u16_vertex"];
        pd.fragmentFunction=[library newFunctionWithName:@"u16_fragment"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
        auto pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&shaderError];auto queue=[device newCommandQueue];
        if(!pipeline || !queue){fprintf(stderr,"UNORM16 sampler pipeline failed: %s\n",shaderError.localizedDescription.UTF8String);return 1;}
        unsigned passed=0;
        for(NSString *name in @[@"maps.tif",@"filmic_lut.tif"]) {
            NSError *error=nil;printf("FLUIDITY_ASSET name=%s begin\n",name.UTF8String);fflush(stdout);
            auto texture=[loader newTextureWithContentsOfURL:[NSURL fileURLWithPath:[base stringByAppendingPathComponent:name]]
                options:@{*key:@(MTLStorageModePrivate)} error:&error];
            if(!texture){fprintf(stderr,"FLUIDITY_ASSET name=%s failed=%s\n",name.UTF8String,error.localizedDescription.UTF8String);return 1;}
            printf("FLUIDITY_ASSET name=%s format=%lu size=%lux%lu storage=%lu pass\n",name.UTF8String,
                (unsigned long)texture.pixelFormat,(unsigned long)texture.width,(unsigned long)texture.height,(unsigned long)texture.storageMode);
            fflush(stdout);
            if(texture.pixelFormat!=MTLPixelFormatRGBA16Unorm){fputs("Native TIFF loader changed its observed UNORM16 format\n",stderr);return 1;}
            auto targetDescriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:4 height:1 mipmapped:NO];
            targetDescriptor.storageMode=MTLStorageModeShared;targetDescriptor.usage=MTLTextureUsageRenderTarget;
            auto target=[device newTextureWithDescriptor:targetDescriptor];auto raw=[device newBufferWithLength:256 options:MTLResourceStorageModeShared];
            if(!target || !raw)return 1;
            const uint32_t origin[2]={uint32_t(texture.width/2),uint32_t(texture.height/2)};
            auto commands=[queue commandBuffer];auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
            auto render=[commands renderCommandEncoderWithDescriptor:pass];if(!render)return 1;
            [render setRenderPipelineState:pipeline];[render setFragmentTexture:texture atIndex:0];
            [render setFragmentBytes:origin length:sizeof(origin) atIndex:0];[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[render endEncoding];
            auto copy=[commands blitCommandEncoder];if(!copy)return 1;
            [copy copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(origin[0],origin[1],0) sourceSize:MTLSizeMake(4,1,1)
                toBuffer:raw destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:0];[copy endEncoding];
            if(!completed(commands))return 1;
            uint8_t pixels[16];[target getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,1) mipmapLevel:0];
            const auto *words=(const uint16_t *)raw.contents;
            for(unsigned n=0;n<4;++n)for(unsigned c=0;c<4;++c) {
                unsigned channel=c==0?2:c==2?0:c,expected=(uint32_t(words[n*4+channel])*255+32767)/65535;
                if(abs(int(pixels[n*4+c])-int(expected))>1) {
                    fprintf(stderr,"UNORM16 asset sample mismatch name=%s pixel=%u channel=%u raw=%u got=%u expected=%u\n",
                        name.UTF8String,n,c,words[n*4+channel],pixels[n*4+c],expected);return 1;
                }
            }
            printf("FLUIDITY_ASSET name=%s GPU_normalized_samples=4 raw16_readback=pass\n",name.UTF8String);fflush(stdout);++passed;
        }
        printf("FLUIDITY_ASSET completed=%u\n",passed);return 0;
    } @catch(NSException *exception) {
        fprintf(stderr,"FLUIDITY_ASSET exception=%s reason=%s\n%s\n",exception.name.UTF8String,exception.reason.UTF8String,
            [exception.callStackSymbols componentsJoinedByString:@"\n"].UTF8String);return 1;
    }
}

static int fluidityTest(id<MTLDevice> device) {
    @try {
        NSError *error=nil;
        puts("FLUIDITY_TEST phase=library begin");fflush(stdout);
        NSURL *url=[NSURL fileURLWithPath:@"/System/Library/ExtensionKit/Extensions/MercuryPosterExtension.appex/default.metallib"];
        auto library=[device newLibraryWithURL:url error:&error];
        if(!library){fprintf(stderr,"Fluidity library failed: %s\n",error.localizedDescription.UTF8String);return 1;}
        printf("FLUIDITY_TEST phase=library functions=%lu\n",(unsigned long)library.functionNames.count);fflush(stdout);
        NSString *names[]={@"fluidity::sdfVertexShader",@"fluidity::vertexShader",@"fluidity::sdfFragmentShader"};
        id<MTLFunction> functions[3];
        for(unsigned i=0;i<3;++i) {
            printf("FLUIDITY_TEST phase=function index=%u begin\n",i);fflush(stdout);
            functions[i]=[library newFunctionWithName:names[i]];
            if(!functions[i]){fprintf(stderr,"Fluidity function failed: %s\n",names[i].UTF8String);return 1;}
            printf("FLUIDITY_TEST phase=function index=%u pass\n",i);fflush(stdout);
        }
        for(unsigned i=0;i<9;++i) {
            id<MTLFunction> fragment=functions[2];
            if(i) {
                printf("FLUIDITY_TEST phase=specialize mask=%u begin\n",i-1);fflush(stdout);
                auto values=[MTLFunctionConstantValues new];
                for(unsigned b=0;b<3;++b){bool flag=((i-1)>>b)&1;[values setConstantValue:&flag type:MTLDataTypeBool atIndex:b];}
                fragment=[library newFunctionWithName:@"fluidity::fragmentShader" constantValues:values error:&error];
                if(!fragment){fprintf(stderr,"Fluidity specialization failed mask=%u: %s\n",i-1,error.localizedDescription.UTF8String);return 1;}
            }
            auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=functions[i?1:0];pd.fragmentFunction=fragment;
            pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;pd.colorAttachments[1].pixelFormat=MTLPixelFormatRG32Float;
            // Match Mercury's 0x10000874c descriptors, including the SDF
            // float blend and the main fragment's read-only attachment 1.
            // Merely accepting two attachment formats misses these states.
            pd.colorAttachments[i?1:0].writeMask=MTLColorWriteMaskNone;
            if(!i) {
                auto sdf=pd.colorAttachments[1];sdf.blendingEnabled=YES;
                sdf.rgbBlendOperation=sdf.alphaBlendOperation=MTLBlendOperationAdd;
                sdf.sourceRGBBlendFactor=sdf.sourceAlphaBlendFactor=MTLBlendFactorSourceAlpha;
                sdf.destinationRGBBlendFactor=MTLBlendFactorOne;
                sdf.destinationAlphaBlendFactor=MTLBlendFactorDestinationAlpha;
            }
            auto pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
            if(!pipeline){fprintf(stderr,"Fluidity pipeline failed case=%u: %s\n",i,error.localizedDescription.UTF8String);return 1;}
            printf("FLUIDITY_TEST phase=pipeline case=%u pass\n",i);fflush(stdout);
        }
        puts("FLUIDITY_TEST actual_native_library=pass functions=11 pipelines=9 mercury_descriptors=1");return 0;
    } @catch(NSException *exception) {
        fprintf(stderr,"FLUIDITY_TEST exception=%s reason=%s\n%s\n",exception.name.UTF8String,exception.reason.UTF8String,
            [exception.callStackSymbols componentsJoinedByString:@"\n"].UTF8String);return 1;
    }
}

static int uintTextures(id<MTLDevice> device) {
    NSError *error=nil;
    auto queue=[device newCommandQueue];
    auto air=[device newLibraryWithURL:[NSURL fileURLWithPath:[driverBundlePath() stringByAppendingPathComponent:@"IntegerTextures.air"]] error:&error];
    auto msl=[device newLibraryWithSource:@"kernel void copy_uint(texture2d<uint,access::read> input [[texture(0)]],texture2d<uint,access::write> output [[texture(7)]],uint2 id [[thread_position_in_grid]]) {uint4 v=input.read(id);output.write(v^uint4(2147483649u,0u,0u,0u),id);}\n"
        "kernel void read_uint(texture2d<uint,access::read> input [[texture(0)]],device uint4 *output [[buffer(0)]],uint2 id [[thread_position_in_grid]]) {output[id.x+id.y*17u]=input.read(id);}" options:nil error:&error];
    auto pipeline=[&](id<MTLLibrary> library,NSString *name) {return [device newComputePipelineStateWithFunction:[library newFunctionWithName:name] error:&error];};
    auto airCopy=pipeline(air,@"air_uint_copy"),mslCopy=pipeline(msl,@"copy_uint"),read=pipeline(msl,@"read_uint"),pack=pipeline(air,@"air_uint_pack");
    auto submit=[&](id<MTLCommandBuffer> commands) { [commands commit];[commands waitUntilCompleted];
        if(commands.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"UINT command failed: %s\n",commands.error.localizedDescription.UTF8String);return false;}return true;};
    if(!queue || !air || !msl || !airCopy || !mslCopy || !read || !pack){fprintf(stderr,"UINT compiler/allocation: %s\n",error.localizedDescription.UTF8String);return 1;}
    constexpr NSUInteger width=17,height=9,pitch=128,offset=64,bytes=4096;
    auto source=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared],target=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if(!source.contents || !target.contents)return 1;
    memset(source.contents,0xa5,bytes);
    auto pattern=[](unsigned x,unsigned y){return 0x80000001u^(x*0x1020305u+y*0x70133u);};
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
        ((uint32_t *)((uint8_t *)source.contents+offset+y*pitch))[x]=pattern(x,y);
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:width height:height mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView;
    auto input=[source newTextureWithDescriptor:descriptor offset:offset bytesPerRow:pitch];
    descriptor.pixelFormat=MTLPixelFormatBGRA8Unorm;
    auto parent=[target newTextureWithDescriptor:descriptor offset:offset bytesPerRow:pitch];
    auto output=[parent newTextureViewWithPixelFormat:MTLPixelFormatR32Uint];
    auto guard=[&]() {const auto *p=(const uint8_t *)target.contents;
        for(NSUInteger i=0;i<bytes;++i) {
            if(!(i>=offset && (i-offset)/pitch<height && (i-offset)%pitch<width*4) && p[i]!=0xa5){fprintf(stderr,"UINT guard byte=%lu\n",(unsigned long)i);return false;}
        }
        return true;
    };
    if(!input || !parent || !output)return 1;
    for(auto kernel:{airCopy,mslCopy}) {
        memset(target.contents,0xa5,bytes);
        auto commands=[queue commandBuffer];auto encoder=[commands computeCommandEncoder];[encoder setComputePipelineState:kernel];
        [encoder setTexture:input atIndex:0];[encoder setTexture:output atIndex:7];
        [encoder dispatchThreads:MTLSizeMake(24,16,1) threadsPerThreadgroup:MTLSizeMake(8,4,1)];[encoder endEncoding];
        if(!submit(commands) || !guard())return 1;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)if(((uint32_t *)((uint8_t *)target.contents+offset+y*pitch))[x]!=(pattern(x,y)^0x80000001u))return 1;
    }
    auto swizzled=[input newTextureViewWithPixelFormat:MTLPixelFormatR32Uint textureType:MTLTextureType2D levels:NSMakeRange(0,1) slices:NSMakeRange(0,1)
        swizzle:MTLTextureSwizzleChannels{MTLTextureSwizzleRed,MTLTextureSwizzleAlpha,MTLTextureSwizzleZero,MTLTextureSwizzleOne}];
    auto values=[device newBufferWithLength:width*height*16 options:MTLResourceStorageModeShared];
    auto commands=[queue commandBuffer];auto encoder=[commands computeCommandEncoder];[encoder setComputePipelineState:read];[encoder setTexture:swizzled atIndex:0];[encoder setBuffer:values offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(width,height,1) threadsPerThreadgroup:MTLSizeMake(8,4,1)];[encoder endEncoding];
    if(!swizzled || !values.contents || !submit(commands))return 1;
    const auto *got=(const uint32_t *)values.contents;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){auto i=(x+y*width)*4;if(got[i]!=pattern(x,y) || got[i+1]!=1 || got[i+2] || got[i+3]!=1)return 1;}
    auto rgba=[device newBufferWithLength:16 options:MTLResourceStorageModeShared];if(!rgba.contents)return 1;
    const float channels[]={0.0f,0.25f,0.5f,1.0f};memcpy(rgba.contents,channels,sizeof(channels));
    auto packInto=[&](id<MTLTexture> texture) {auto cb=[queue commandBuffer];auto ce=[cb computeCommandEncoder];[ce setComputePipelineState:pack];[ce setBuffer:rgba offset:0 atIndex:0];[ce setTexture:texture atIndex:1];
        [ce dispatchThreads:MTLSizeMake((texture.width+7)&~NSUInteger(7),(texture.height+7)&~NSUInteger(7),1) threadsPerThreadgroup:MTLSizeMake(8,4,1)];[ce endEncoding];return submit(cb);};
    memset(target.contents,0xa5,bytes);if(!packInto(output) || !guard())return 1;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)if(((uint32_t *)((uint8_t *)target.contents+offset+y*pitch))[x]!=0xff804000u)return 1;
    puts("UINT_TEXTURE planar=AIR+MSL high_bits=exact swizzle=exact unorm4x8=exact BGRA_word_view=exact padding=unchanged oversized_dispatch=guarded");
    for(unsigned test=0;test<3;++test) {
        const NSUInteger sw=test==2?810:width,sh=test==2?1440:height,sp=test==2?3264:pitch,so=test==2?64:0;
        const NSUInteger allocation=test==0?16384:so+sp*sh;
        NSDictionary *properties=@{(id)kIOSurfaceWidth:@(sw),(id)kIOSurfaceHeight:@(sh),(id)kIOSurfaceBytesPerElement:@4,
            (id)kIOSurfaceBytesPerRow:@(sp),(id)kIOSurfaceAllocSize:@(allocation),(id)kIOSurfacePixelFormat:@(0x42475241u),@"IOSurfaceOffset":@(so)};
        IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);if(!surface)return 1;
        descriptor.pixelFormat=MTLPixelFormatR32Uint;descriptor.width=sw;descriptor.height=sh;
        if(IOSurfaceLock(surface,0,nullptr)!=KERN_SUCCESS){CFRelease(surface);return 1;}
        void *base=IOSurfaceGetBaseAddress(surface);if(base)memset(base,0xa5,sp*sh);
        IOSurfaceUnlock(surface,0,nullptr);
        auto imported=[device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
        bool surfaceOK=base && imported && packInto(imported);
        if(surfaceOK && IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr)==KERN_SUCCESS) {
            const auto *pixels=(const uint8_t *)IOSurfaceGetBaseAddress(surface);
            for(NSUInteger y=0;y<sh && surfaceOK;++y) {
                for(NSUInteger x=0;x<sw;++x)if(((const uint32_t *)(pixels+y*sp))[x]!=0xff804000u){surfaceOK=false;break;}
                for(NSUInteger b=sw*4;b<sp && surfaceOK;++b)if(pixels[y*sp+b]!=0xa5)surfaceOK=false;
            }
            IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr);
        } else surfaceOK=false;
        printf("UINT_TEXTURE IOSurface_case=%u size=%lux%lu pitch=%lu offset=%lu allocation=%lu pixels_and_padding=%s\n",test,(unsigned long)sw,(unsigned long)sh,(unsigned long)sp,(unsigned long)so,(unsigned long)allocation,surfaceOK?"pass":"fail");
        CFRelease(surface);if(!surfaceOK)return 1;
    }
    for(unsigned shape=0;shape<4;++shape) {
        const char *types[]={"texture2d","texture2d_array","texturecube","texturecube_array"};
        std::string indices="id,";if(shape&2)indices+="2u,";if(shape&1)indices+="1u,";indices+="1u";
        auto code=std::string("kernel void uint_shape(")+types[shape]+"<uint,access::write> output [[texture(0)]],uint2 id [[thread_position_in_grid]]) {output.write(uint4(2147483649u+id.x+id.y*31u),"+indices+");}";
        auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:code.c_str()] options:nil error:&error];auto kernel=pipeline(library,@"uint_shape");
        auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:8 height:8 mipmapped:YES];
        td.textureType=shape==0?MTLTextureType2D:shape==1?MTLTextureType2DArray:shape==2?MTLTextureTypeCube:MTLTextureTypeCubeArray;
        td.arrayLength=(shape&1)?2:1;td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderWrite|MTLTextureUsageShaderRead;
        auto texture=[device newTextureWithDescriptor:td];if(!kernel || !texture){fprintf(stderr,"UINT shape compiler: %s\n",error.localizedDescription.UTF8String);return 1;}
        auto cb=[queue commandBuffer];auto ce=[cb computeCommandEncoder];[ce setComputePipelineState:kernel];[ce setTexture:texture atIndex:0];
        [ce dispatchThreads:MTLSizeMake(8,8,1) threadsPerThreadgroup:MTLSizeMake(4,4,1)];[ce endEncoding];if(!submit(cb))return 1;
        uint32_t pixels[16]{};NSUInteger slice=(shape&2)?((shape&1)?8:2):((shape&1)?1:0);
        [texture getBytes:pixels bytesPerRow:16 bytesPerImage:64 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:1 slice:slice];
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)if(pixels[x+y*4]!=0x80000001u+x+y*31u){fprintf(stderr,"UINT mip mismatch shape=%u x=%u y=%u got=%08x\n",shape,x,y,pixels[x+y*4]);return 1;}
    }
    puts("UINT_TEXTURE dimensions=2D+array+cube+cube_array mip=1 oversized_dispatch=guarded");
    auto halfBits=pipeline(air,@"air_half_bits"),local=pipeline(air,@"air_private_array");
    constexpr unsigned count=65536;
    auto raw=[device newBufferWithLength:count*8 options:MTLResourceStorageModeShared],result=[device newBufferWithLength:count*8 options:MTLResourceStorageModeShared];
    if(!halfBits || !local || !raw.contents || !result.contents)return 1;
    auto *rawWords=(uint32_t *)raw.contents;for(unsigned i=0;i<count;++i){rawWords[2*i]=i|((i^0x7fffu)<<16);rawWords[2*i+1]=(i^0x8000u)|((i^0xffffu)<<16);}
    commands=[queue commandBuffer];encoder=[commands computeCommandEncoder];[encoder setComputePipelineState:halfBits];[encoder setBuffer:raw offset:0 atIndex:0];[encoder setBuffer:result offset:0 atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];[encoder endEncoding];if(!submit(commands))return 1;
    got=(const uint32_t *)result.contents;for(unsigned i=0;i<count*2;++i)if(got[i]!=((rawWords[i]<<16)|(rawWords[i]>>16))){fprintf(stderr,"HALF_BITCAST mismatch word=%u input=%08x got=%08x\n",i,rawWords[i],got[i]);return 1;}
    commands=[queue commandBuffer];encoder=[commands computeCommandEncoder];[encoder setComputePipelineState:local];[encoder setBuffer:result offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(256,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[encoder endEncoding];if(!submit(commands))return 1;
    got=(const uint32_t *)result.contents;for(unsigned i=0;i<256;++i)for(unsigned lane=0;lane<4;++lane)if(got[i*4+lane]!=(i^(i&7u)))return 1;
    puts("UINT_TEXTURE half_bitcast_patterns=65536 NaN_payloads=exact private_array_invocations=256 private_array_words=32 result=pass");return 0;
}
static int rg32Test(id<MTLDevice> device) {
    NSError *error=nil;
    NSString *source=@R"SHADER(struct RGVertex { float4 position [[position]]; };
vertex RGVertex rg32_vertex(uint i [[vertex_id]]) {
 RGVertex o; float x=i==1?3.0:-1.0; float y=i==2?3.0:-1.0;
 o.position=float4(x,y,0.0,1.0); return o;
}
struct RGColors { float4 first [[color(0)]]; float4 second [[color(1)]]; };
fragment RGColors rg32_write() {
 RGColors o; o.first=float4(0.0,0.0,0.0,1.0); o.second=float4(-0.375123,1.251234,0.0,1.0); return o;
}
fragment float4 rg32_fetch(float2 previous [[color(1)]]) {
 return float4(1.0+previous.x,previous.y*0.4,0.25,1.0);
}
)SHADER";
    auto library=[device newLibraryWithSource:source options:nil error:&error];
    auto queue=[device newCommandQueue];
    id<MTLBuffer> storage[2]; id<MTLTexture> textures[2];
    const NSUInteger pitch[2]={256,512},cpp[2]={4,8};
    for(unsigned a=0;a<2;++a) {
        storage[a]=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        memset(storage[a].contents,0xa5,4096);
        auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:a?MTLPixelFormatRG32Float:MTLPixelFormatBGRA8Unorm width:7 height:5 mipmapped:NO];
        td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        textures[a]=[storage[a] newTextureWithDescriptor:td offset:64 bytesPerRow:pitch[a]];
    }
    if(!library || !queue || !textures[0] || !textures[1]) {
        fprintf(stderr,"RG32 allocation/compiler failed: %s\n",error.localizedDescription.UTF8String);return 1;
    }
    auto guards=[&]() {
        for(unsigned a=0;a<2;++a) {
            const auto *p=(const uint8_t *)storage[a].contents;
            for(NSUInteger n=0;n<4096;++n) {
                bool active=n>=64 && (n-64)/pitch[a]<5 && (n-64)%pitch[a]<7*cpp[a];
                if(!active && p[n]!=0xa5) {fprintf(stderr,"RG32 guard changed: attachment=%u byte=%lu\n",a,(unsigned long)n);return false;}
            }
        }
        return true;
    };
    for(unsigned phase=0;phase<3;++phase) {
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        for(unsigned a=0;a<2;++a) {
            pass.colorAttachments[a].texture=textures[a];
            pass.colorAttachments[a].loadAction=phase?MTLLoadActionLoad:MTLLoadActionClear;
            pass.colorAttachments[a].storeAction=MTLStoreActionStore;
            pass.colorAttachments[a].clearColor=a?MTLClearColorMake(-0.1234567,1.234567,0,1):MTLClearColorMake(0,0,0,0);
        }
        id<MTLRenderPipelineState> pipeline=nil;
        if(phase) {
            auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"rg32_vertex"];
            pd.fragmentFunction=[library newFunctionWithName:phase==1?@"rg32_write":@"rg32_fetch"];
            pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;pd.colorAttachments[1].pixelFormat=MTLPixelFormatRG32Float;
            pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
            if(!pipeline){fprintf(stderr,"RG32 pipeline failed phase=%u: %s\n",phase,error.localizedDescription.UTF8String);return 1;}
        }
        auto commands=[queue commandBuffer];auto render=[commands renderCommandEncoderWithDescriptor:pass];
        if(!render)return 1;
        if(phase){[render setRenderPipelineState:pipeline];[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];}
        [render endEncoding];if(!completed(commands))return 1;
        float values[70];uint8_t colors[140];
        [textures[1] getBytes:values bytesPerRow:56 fromRegion:MTLRegionMake2D(0,0,7,5) mipmapLevel:0];
        [textures[0] getBytes:colors bytesPerRow:28 fromRegion:MTLRegionMake2D(0,0,7,5) mipmapLevel:0];
        const float red=phase?-0.375123f:-0.1234567f,green=phase?1.251234f:1.234567f;
        for(unsigned n=0;n<35;++n) {
            if(fabsf(values[n*2]-red)>0.000001f || fabsf(values[n*2+1]-green)>0.000001f) {
                fprintf(stderr,"RG32 float mismatch phase=%u pixel=%u got=%.9g,%.9g expected=%.9g,%.9g\n",phase,n,values[n*2],values[n*2+1],red,green);return 1;
            }
            if(phase==2) {
                const unsigned expected[4]={64,(unsigned)lroundf(green*0.4f*255),(unsigned)lroundf((1+red)*255),255};
                for(unsigned c=0;c<4;++c)if(abs(int(colors[n*4+c])-int(expected[c]))>1) {
                    fprintf(stderr,"RG32 fetch mismatch pixel=%u channel=%u got=%u expected=%u\n",n,c,colors[n*4+c],expected[c]);return 1;
                }
            }
        }
        if(!guards())return 1;
        printf("RG32_TEST phase=%u pixels=35 float32=pass padding=pass\n",phase);
    }
    // Match Mercury: an RG32 memoryless input, cleared then blended and
    // fetched within one pass. Encode both before committing either, in the
    // reverse order too. Same-queue reuse must preserve each clear; different
    // queues must not share attachment storage.
    auto transientDescriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Float width:7 height:5 mipmapped:NO];
    transientDescriptor.storageMode=MTLStorageModeMemoryless;transientDescriptor.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    auto transient=[device newTextureWithDescriptor:transientDescriptor];
    auto outputDescriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:7 height:5 mipmapped:NO];
    outputDescriptor.storageMode=MTLStorageModeShared;outputDescriptor.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> outputs[2]={textures[0],[device newTextureWithDescriptor:outputDescriptor]};
    if(!transient || transient.storageMode!=MTLStorageModeMemoryless || transient.allocatedSize || transient.buffer || !outputs[1]) {
        fputs("Memoryless texture creation/metadata failed\n",stderr);return 1;
    }
    auto writeDescriptor=[MTLRenderPipelineDescriptor new];
    writeDescriptor.vertexFunction=[library newFunctionWithName:@"rg32_vertex"];writeDescriptor.fragmentFunction=[library newFunctionWithName:@"rg32_write"];
    writeDescriptor.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;writeDescriptor.colorAttachments[0].writeMask=MTLColorWriteMaskNone;
    auto second=writeDescriptor.colorAttachments[1];second.pixelFormat=MTLPixelFormatRG32Float;second.blendingEnabled=YES;
    second.sourceRGBBlendFactor=second.sourceAlphaBlendFactor=MTLBlendFactorSourceAlpha;
    second.destinationRGBBlendFactor=MTLBlendFactorOne;second.destinationAlphaBlendFactor=MTLBlendFactorDestinationAlpha;
    auto writer=[device newRenderPipelineStateWithDescriptor:writeDescriptor error:&error];
    MTLRenderPipelineDescriptor *fetchDescriptor=[writeDescriptor copy];fetchDescriptor.fragmentFunction=[library newFunctionWithName:@"rg32_fetch"];
    fetchDescriptor.colorAttachments[0].writeMask=MTLColorWriteMaskAll;
    fetchDescriptor.colorAttachments[1].writeMask=MTLColorWriteMaskNone;fetchDescriptor.colorAttachments[1].blendingEnabled=NO;
    auto fetcher=[device newRenderPipelineStateWithDescriptor:fetchDescriptor error:&error];
    if(!writer || !fetcher){fprintf(stderr,"Memoryless blend/fetch pipeline failed: %s\n",error.localizedDescription.UTF8String);return 1;}
    auto secondQueue=[device newCommandQueue];if(!secondQueue)return 1;
    for(unsigned scenario=0;scenario<2;++scenario) {
    id<MTLCommandBuffer> pending[2];
    for(unsigned i=0;i<2;++i) {
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=outputs[i];pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);
        pass.colorAttachments[1].texture=transient;pass.colorAttachments[1].loadAction=MTLLoadActionClear;
        pass.colorAttachments[1].storeAction=MTLStoreActionDontCare;pass.colorAttachments[1].clearColor=MTLClearColorMake(i?0:-0.25,i?-0.25:-0.75,0,1);
        id<MTLCommandQueue> passQueue=scenario && i?secondQueue:queue;
        pending[i]=[passQueue commandBuffer];auto encoder=[pending[i] renderCommandEncoderWithDescriptor:pass];
        if(!encoder){fputs("Memoryless render pass failed\n",stderr);return 1;}
        [encoder setRenderPipelineState:writer];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder setRenderPipelineState:fetcher];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
    }
    [pending[1] commit];[pending[0] commit];
    for(unsigned i=0;i<2;++i) {
        [pending[i] waitUntilCompleted];if(pending[i].status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"Memoryless completion failed: %s\n",pending[i].error.localizedDescription.UTF8String);return 1;}
        uint8_t pixels[140];[outputs[i] getBytes:pixels bytesPerRow:28 fromRegion:MTLRegionMake2D(0,0,7,5) mipmapLevel:0];
        const float red=(i?0:-0.25f)-0.375123f,green=(i?-0.25f:-0.75f)+1.251234f;
        const unsigned expected[4]={64,(unsigned)lroundf(green*0.4f*255),(unsigned)lroundf((1+red)*255),255};
        for(unsigned n=0;n<35;++n)for(unsigned c=0;c<4;++c)if(abs(int(pixels[n*4+c])-int(expected[c]))>1) {
            fprintf(stderr,"Memoryless pixel mismatch pass=%u pixel=%u channel=%u got=%u expected=%u\n",i,n,c,pixels[n*4+c],expected[c]);return 1;
        }
        printf("MEMORYLESS_TEST scenario=%u pass=%u pixels=35 isolated=pass float_blend=pass fetch=pass\n",scenario,i);
    }
    }
    // Nine 1080x1920 RG32 passes used to allocate ~143MiB before any commit,
    // exceeding the 128MiB native client limit. One queue needs one backing.
    @autoreleasepool {
        auto largeDescriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Float width:1080 height:1920 mipmapped:NO];
        largeDescriptor.storageMode=MTLStorageModeMemoryless;largeDescriptor.usage=MTLTextureUsageRenderTarget;
        auto large=[device newTextureWithDescriptor:largeDescriptor];if(!large)return 1;
        NSUInteger before=device.currentAllocatedSize;
        NSMutableArray<id<MTLCommandBuffer>> *commands=[NSMutableArray new];
        for(unsigned i=0;i<9;++i) {
            auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=large;
            pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionDontCare;
            pass.colorAttachments[0].clearColor=MTLClearColorMake(i,0,0,1);
            auto command=[queue commandBuffer];auto encoder=[command renderCommandEncoderWithDescriptor:pass];
            if(!encoder){fprintf(stderr,"Memoryless full-size encoding failed at pass=%u\n",i);return 1;}
            [encoder endEncoding];[commands addObject:command];
            NSUInteger after=device.currentAllocatedSize;
            if(after>before && after-before>16u*1024*1024) {
                fprintf(stderr,"Memoryless queue storage grew by %lu bytes at pass=%u\n",(unsigned long)(after-before),i);return 1;
            }
        }
        NSUInteger after=device.currentAllocatedSize;
        for(id<MTLCommandBuffer> command in commands)[command commit];
        for(id<MTLCommandBuffer> command in commands) {
            [command waitUntilCompleted];if(command.status!=MTLCommandBufferStatusCompleted) {
                fprintf(stderr,"Memoryless full-size completion failed: %s\n",command.error.localizedDescription.UTF8String);return 1;
            }
        }
        printf("MEMORYLESS_BUDGET passes=9 size=1080x1920 allocated_delta=%lu max_backing=16777216 status=pass\n",(unsigned long)(after>before?after-before:0));
    }
    if(!guards())return 1;
    puts("RG32 raw paired clear, MRT slot1 write and framebuffer fetch verified");return 0;
}

int main(int argc, char **argv) {
    bool system = argc >= 3 && !strcmp(argv[1], "--system");
    if (system) { --argc; ++argv; }
    bool patternSized=argc==4 && !strcmp(argv[1],"render-state-test");
    bool sized=patternSized || ((argc==4 || argc==5) && !strcmp(argv[1],"ca-render-probe"));NSUInteger width=64,height=64;
    if(sized) {
        for(unsigned i=0;i<2;++i) {
            const char *text=argv[2+i];size_t length=strlen(text);unsigned long value=length && length<=4 && strspn(text,"0123456789")==length?strtoul(text,nullptr,10):0;
            if(value<(patternSized?32u:4u) || value>4096 || value%4){fputs("Render dimensions must be multiples of four within the supported range\n",stderr);return 64;}
            (i?height:width)=value;
        }
        if(argc==5 && strcmp(argv[4],"iosurface"))return 64;
    }
    if ((argc != 2 && !sized) || (strcmp(argv[1], "gmem-test") && strcmp(argv[1], "clear-target-test") && strcmp(argv[1], "clear-tail-target-test") && strcmp(argv[1], "clear-test") && strcmp(argv[1], "word-window-test") && strcmp(argv[1], "readonly-feedback-test") && strcmp(argv[1], "astc-test") && strcmp(argv[1], "fluidity-assets") && strcmp(argv[1], "fluidity-test") && strcmp(argv[1], "rg32-test") && strcmp(argv[1], "uint-texture-test") && strcmp(argv[1], "air-nir-output-test") && strcmp(argv[1], "render-clear-test") && strcmp(argv[1], "info") && strcmp(argv[1], "cleanup-test") && strcmp(argv[1], "mixed-test") && strcmp(argv[1], "constant-window-test") && strcmp(argv[1], "shader-probe") && strcmp(argv[1], "shader-empty-probe") && strcmp(argv[1], "shader-store-probe") && strcmp(argv[1], "render-probe") && strcmp(argv[1], "ca-render-probe") && strcmp(argv[1], "blit-test") && strcmp(argv[1], "fence-test") && strcmp(argv[1], "pipeline-test") && strcmp(argv[1], "air-test") && strcmp(argv[1], "jit-test") && strcmp(argv[1], "render-state-test") && strcmp(argv[1], "indexed-test") && strcmp(argv[1], "varying-test") && strcmp(argv[1], "depth-stencil-test") && strcmp(argv[1], "vertex-test") && strcmp(argv[1], "vertex-air-test") && strcmp(argv[1], "surface-test") && strcmp(argv[1], "direct-drawable-test") && strcmp(argv[1], "presentation-test") && strcmp(argv[1], "inline-test"))) {
        fprintf(stderr, "usage: metalctl [--system] info|gmem-test|clear-test|clear-target-test|clear-tail-target-test|word-window-test|readonly-feedback-test|astc-test|fluidity-assets|fluidity-test|rg32-test|uint-texture-test|air-nir-output-test|render-clear-test|cleanup-test|mixed-test|constant-window-test|shader-probe|shader-empty-probe|shader-store-probe|render-probe|ca-render-probe|blit-test|fence-test|pipeline-test|air-test|jit-test|render-state-test|indexed-test|varying-test|depth-stencil-test|vertex-test|vertex-air-test|surface-test|direct-drawable-test|presentation-test|inline-test\n       metalctl [--system] ca-render-probe [width height [iosurface]]\n       metalctl [--system] render-state-test [width height]\n"); return 64;
    }
    @autoreleasepool {
        NSError *error = nil;
        id<MTLDevice> device = system ? MTLCreateSystemDefaultDevice() : explicitDevice(&error);
        if (!device) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String ?: "System Metal did not return a device"); return 1; }
        printf("%s registry=%llu max-buffer=%lu discovery=%s\n",
            device.name.UTF8String, (unsigned long long)device.registryID, (unsigned long)device.maxBufferLength,
            system ? "Metal.framework" : "explicit factory");
        if (!strcmp(argv[1], "info")) {
            Dl_info image{};Method method=class_getInstanceMethod([device class],@selector(name));
            bool located=method && dladdr(reinterpret_cast<const void *>(method_getImplementation(method)),&image);
            const auto *header=located?static_cast<const mach_header_64 *>(image.dli_fbase):nullptr;
            char uuid[33]="unavailable";
            if(header && header->magic==MH_MAGIC_64 && header->ncmds<=4096 && header->sizeofcmds<=1024*1024) {
                auto *cursor=reinterpret_cast<const uint8_t *>(header+1),*end=cursor+header->sizeofcmds;
                for(uint32_t i=0;i<header->ncmds && size_t(end-cursor)>=sizeof(load_command);++i) {
                    const auto *command=reinterpret_cast<const load_command *>(cursor);
                    if(command->cmdsize<sizeof(load_command) || command->cmdsize>size_t(end-cursor))break;
                    if(command->cmd==LC_UUID && command->cmdsize>=sizeof(uuid_command)) {
                        const auto *value=reinterpret_cast<const uuid_command *>(command);
                        for(unsigned j=0;j<16;++j)snprintf(uuid+j*2,3,"%02x",value->uuid[j]);break;
                    }
                    cursor+=command->cmdsize;
                }
            }
            NSBundle *bundle=[NSBundle bundleForClass:[device class]];
            NSString *bundleVersion=[bundle objectForInfoDictionaryKey:@"CFBundleVersion"];
            printf("METAL_IMPLEMENTATION class=%s image=%s uuid=%s bundle_version=%s temporary_directory=%s uid=%u\n",
                class_getName([device class]),located?image.dli_fname:"unavailable",uuid,
                bundleVersion.UTF8String?:"unavailable",NSTemporaryDirectory().UTF8String?:"unavailable",getuid());
            if([(id)device respondsToSelector:@selector(qcomMetalDiagnosticsEnabled)])
                printf("METAL_DIAGNOSTICS enabled=%u\n",[(id<QcomMetalDiagnosticConfiguration>)device qcomMetalDiagnosticsEnabled]?1:0);
            return 0;
        }
        if (!strcmp(argv[1], "clear-test")) return clearKernels(device);
        if (!strcmp(argv[1], "clear-target-test")) return clearKernels(device,true);
        if (!strcmp(argv[1], "clear-tail-target-test")) return clearKernels(device,false,true);
        if (!strcmp(argv[1], "cleanup-test")) return retiredCleanup(device);
        if (!strcmp(argv[1], "shader-probe")) return shaderProbe(device);
        if (!strcmp(argv[1], "shader-empty-probe")) return shaderProbe(device,0,1);
        if (!strcmp(argv[1], "shader-store-probe")) return shaderProbe(device,1,2);
        if (!strcmp(argv[1], "render-probe")) return renderProbe(device);
        if (!strcmp(argv[1], "ca-render-probe")) return nativeRendererProbe(device,width,height,argc==5);
        if (!strcmp(argv[1], "fence-test")) return fences(device);
        if (!strcmp(argv[1], "astc-test")) return astcTextures(device);
        if (!strcmp(argv[1], "gmem-test")) return gmemTest(device);
        if (!strcmp(argv[1], "readonly-feedback-test")) return framebufferReadOnly(device);
        if (!strcmp(argv[1], "direct-drawable-test")) return directDrawableAdmission(device);
        if (!strcmp(argv[1], "presentation-test")) return presentationPipeline(device);
        if (!strcmp(argv[1], "inline-test")) return inlineConstants(device);
        if (!strcmp(argv[1], "surface-test")) return pipeline(device,false,false,false,false,false,false,true);
        if (!strcmp(argv[1], "vertex-test")) return pipeline(device,false,false,false,false,false,true);
        if (!strcmp(argv[1], "vertex-air-test")) return pipeline(device,true,false,false,false,false,true);
        if (!strcmp(argv[1], "depth-stencil-test")) return depthStencil(device);
        if (!strcmp(argv[1], "fluidity-assets")) return fluidityAssets(device);
        if (!strcmp(argv[1], "fluidity-test")) return fluidityTest(device);
        if (!strcmp(argv[1], "rg32-test")) return rg32Test(device);
        if (!strcmp(argv[1], "uint-texture-test")) return uintTextures(device);
        if (!strcmp(argv[1], "air-nir-output-test")) return airNIROutputs(device);
        if (!strcmp(argv[1], "render-clear-test")) return renderClearTest(device);
        if (!strcmp(argv[1], "pipeline-test")) return pipeline(device);
        if (!strcmp(argv[1], "air-test")) return pipeline(device, true);
        if (!strcmp(argv[1], "jit-test")) return pipeline(device, false, true);
        if (!strcmp(argv[1], "render-state-test")) {
            if(pipeline(device,false,false,true))return 1;
            return framebufferSnapshots(device,patternSized?width:1040,patternSized?height:67);
        }
        if (!strcmp(argv[1], "mixed-test")) return mixedBatch(device);
        if (!strcmp(argv[1], "constant-window-test")) return constantWindowTest(device);
        if (!strcmp(argv[1], "word-window-test")) return wordWindowTest(device);
        if (!strcmp(argv[1], "indexed-test")) return pipeline(device, false, false, false, true);
        if (!strcmp(argv[1], "varying-test")) return pipeline(device, false, false, false, false, true);
        const NSUInteger bytes = 16384;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> source = [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        id<MTLBuffer> destination = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:2048 mipmapped:NO];
        td.storageMode=MTLStorageModeShared;id<MTLTexture> rows=[device newTextureWithDescriptor:td];
        // Read the GPU-produced source as well as the copied texture. This
        // distinguishes a failed fill from a failed pitched transfer.
        id<MTLBuffer> strided=[device newBufferWithLength:2048*256 options:MTLResourceStorageModeShared];
        if (!queue || !source || !destination || !commands || !rows || !strided) { fprintf(stderr, "Native allocation failed\n"); return 1; }
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (!blit) return 1;
        [blit fillBuffer:source range:NSMakeRange(0, bytes) value:0x5a];
        [blit fillBuffer:source range:NSMakeRange(3, 10) value:0xa5];
        [blit copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0 size:bytes];
        [blit fillBuffer:strided range:NSMakeRange(0,strided.length) value:0x3c];
        [blit copyFromBuffer:strided sourceOffset:0 sourceBytesPerRow:256 sourceBytesPerImage:0 sourceSize:MTLSizeMake(1,2048,1)
            toTexture:rows destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [blit endEncoding]; [commands commit]; [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "%s\n", commands.error.localizedDescription.UTF8String); return 1;
        }
        const uint8_t *data = (const uint8_t *)destination.contents;
        if (!data || source.contents) {fprintf(stderr,"Blit CPU mapping mismatch: destination=%u private_source=%u\n",data!=nullptr,source.contents!=nullptr);return 1;}
        for (NSUInteger i = 0; i < bytes; ++i) if (data[i] != (i >= 3 && i < 13 ? 0xa5 : 0x5a)) {
            fprintf(stderr, "Blit mismatch at %lu\n", (unsigned long)i); return 1;
        }
        bool valid=true,rowsValid=true;
        const auto *sourceBytes=static_cast<const uint8_t *>(strided.contents);
        if(!sourceBytes)return 1;
        NSUInteger sourceCorrect=0,firstSourceBad=strided.length;
        for(NSUInteger i=0;i<strided.length;++i) {
            if(sourceBytes[i]==0x3c)++sourceCorrect;
            else if(firstSourceBad==strided.length)firstSourceBad=i;
        }
        printf("Blit GPU fill source: correct=%lu total=%lu first_bad=%lu first=%02x last=%02x\n",
            (unsigned long)sourceCorrect,(unsigned long)strided.length,(unsigned long)firstSourceBad,sourceBytes[0],sourceBytes[strided.length-1]);
        valid=sourceCorrect==strided.length;
        uint8_t pixels[8192];[rows getBytes:pixels bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,2048) mipmapLevel:0];
        for(NSUInteger i=0;i<sizeof(pixels);++i)if(pixels[i]!=0x3c) {
            fprintf(stderr,"Blit CP row readback mismatch: row=%lu channel=%lu actual=0x%02x expected=0x3c first=%02x,%02x,%02x,%02x last=%02x,%02x,%02x,%02x\n",
                (unsigned long)(i/4),(unsigned long)(i%4),pixels[i],pixels[0],pixels[1],pixels[2],pixels[3],pixels[8188],pixels[8189],pixels[8190],pixels[8191]);rowsValid=false;valid=false;break;
        }
        if(rowsValid && valid)puts("Metal blit: private fill/copy and all 2048 pitched CP rows verified");
        if(!rowsValid) {
            memset(strided.contents,0x69,strided.length);
            auto retry=[queue commandBuffer];auto encoder=[retry blitCommandEncoder];
            [encoder copyFromBuffer:strided sourceOffset:0 sourceBytesPerRow:256 sourceBytesPerImage:0 sourceSize:MTLSizeMake(1,2048,1)
                toTexture:rows destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
            [encoder endEncoding];if(!completed(retry))return 1;
            [rows getBytes:pixels bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,2048) mipmapLevel:0];
            NSUInteger correct=0;for(auto pixel:pixels)if(pixel==0x69)++correct;
            printf("Blit CPU source pitched copy: correct=%lu total=%lu first=%02x last=%02x\n",
                (unsigned long)correct,(unsigned long)sizeof(pixels),pixels[0],pixels[sizeof(pixels)-1]);
        }
        {
            // Exercise both sides of the observed 64 KiB boundary, including
            // the exact multiple; every run must retain the outer guards.
            const NSUInteger sizes[]={65532,65536,65540,131072};
            for(auto size:sizes) {
                memset(strided.contents,0x69,strided.length);
                auto probe=[queue commandBuffer];auto encoder=[probe blitCommandEncoder];
                [encoder fillBuffer:strided range:NSMakeRange(0,size) value:0x3c];[encoder endEncoding];
                if(!completed(probe))return 1;
                const auto *bytes=static_cast<const uint8_t *>(strided.contents);NSUInteger correct=0,guard=0;
                for(NSUInteger i=0;i<size;++i)if(bytes[i]==0x3c)++correct;
                for(NSUInteger i=size;i<strided.length;++i)if(bytes[i]!=0x69)++guard;
                if(correct!=size || guard)valid=false;
                printf("Blit fill size: bytes=%lu correct=%lu guard_errors=%lu first=%02x last=%02x\n",
                    (unsigned long)size,(unsigned long)correct,(unsigned long)guard,bytes[0],bytes[size-1]);
            }
        }
        if(!commandReferences(device)){fputs("Blit subcheck failed: command references\n",stderr);valid=false;}
        if(!bufferTextures(device,true)){fputs("Blit subcheck failed: heap buffer textures\n",stderr);valid=false;}
        if(!packedClearGuards(device)){fputs("Blit subcheck failed: packed/constrained clear\n",stderr);valid=false;}
        // The intentional native write rejection invalidates this device's
        // submission stream. Run it after all successful-path diagnostics.
        if(!bufferTextures(device)){fputs("Blit subcheck failed: readonly buffer textures\n",stderr);valid=false;}
        if(!valid)return 1;
        puts("Metal buffer/blit path: frozen layouts, private byte/bulk transfers,32 CP row batches and actual readback verified");
    }
    return 0;
}
