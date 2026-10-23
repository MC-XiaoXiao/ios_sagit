// SPDX-License-Identifier: BSD-2-Clause
#import "QcomMTL.h"
#import "NativeMetal.h"
#include "MetalSync.h"
#include "MetalTexture.h"
#include "MetalColor.h"
#include "MetalHeap.h"
#include "MetalReference.h"
#include "MetalArchive.h"
#include "CoreFSCache.h"
#include "MetalBatch.h"
#include "MetalUploadPool.h"
#include "MetalRetirement.h"
#include "MetalShaderCapture.h"
#include <GPUArchiveBuild.h>
#include "GPUConnection.h"
#include <GPUShaderProtocol.h>
#include <GPUSurfaceProtocol.h>
#include <GPUBlit.h>
#include <GPUMemoryRanges.h>
#include <GPUNativeMemory.h>
#include <GPUPrograms.h>
#include "Compiler.h"
#include <GPUSystemShaders.h>
#include "AIRFailureCache.h"
#include "LibraryArchive.h"
#include <IOKit/IOReturn.h>
#import <objc/runtime.h>
#import <IOSurface/IOSurfaceRef.h>
#include <CommonCrypto/CommonDigest.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <cmath>
#include <time.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <map>
#include <set>
#include <vector>

namespace {
NSError *failure(NSString *message, NSInteger code = MTLCommandBufferErrorInternal) {
    return [NSError errorWithDomain:MTLCommandBufferErrorDomain code:code
                          userInfo:@{NSLocalizedDescriptionKey: message}];
}
NSError *transportError(kern_return_t status) {
    NSInteger code = status == kIOReturnTimeout ? MTLCommandBufferErrorTimeout :
        status == kIOReturnNoMemory || status == kIOReturnNoResources ? MTLCommandBufferErrorOutOfMemory :
        status == kIOReturnBadArgument ? MTLCommandBufferErrorInvalidResource : MTLCommandBufferErrorInternal;
    return [NSError errorWithDomain:MTLCommandBufferErrorDomain code:code userInfo:@{
        NSLocalizedDescriptionKey: [NSString stringWithFormat:@"Native IOGPU submission failed: 0x%08x", status],
        @"IOKitReturn": @(status)}];
}
bool validRange(NSUInteger capacity, NSUInteger offset, NSUInteger bytes) {
    return offset <= capacity && bytes <= capacity - offset;
}
bool resourceOptions(MTLResourceOptions options,MTLHazardTrackingMode defaultHazards,MTLResourceOptions &normalized) {
    constexpr NSUInteger allowed=MTLResourceCPUCacheModeMask|MTLResourceStorageModeMask|MTLResourceHazardTrackingModeMask;
    auto storage=(options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift;
    auto hazards=(options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift;
    if((options&~allowed) || (storage!=MTLStorageModeShared && storage!=MTLStorageModePrivate) ||
        (options&MTLResourceCPUCacheModeMask)>MTLCPUCacheModeWriteCombined || hazards>MTLHazardTrackingModeTracked)return false;
    normalized=options;if(!hazards)normalized|=NSUInteger(defaultHazards)<<MTLResourceHazardTrackingModeShift;return true;
}
bool nativeMetalABI(NSString **diagnostic) {
    // Reject other framework generations before using the projected limits.
    // Only the returned limits structure is projected, never private ivars.
    auto os = NSProcessInfo.processInfo.operatingSystemVersion;
    if (os.majorVersion != 18 || os.minorVersion < 6 || os.minorVersion > 7) {
        if (diagnostic) *diagnostic = [NSString stringWithFormat:@"Unsupported Metal framework OS version %ld.%ld.%ld", (long)os.majorVersion, (long)os.minorVersion, (long)os.patchVersion];
        return false;
    }
    Protocol *spi = objc_getProtocol("MTLDeviceSPI");
    Class base = [_MTLDevice class];
    Ivar limits = class_getInstanceVariable(base, "_limits");
    Method entry = class_getInstanceMethod(base, @selector(initWithAcceleratorPort:));
    if (!spi || !limits || !entry || method_getNumberOfArguments(entry) != 3) {
        if (diagnostic) *diagnostic = [NSString stringWithFormat:@"Metal framework ABI missing: SPI=%d limits=%d initializer=%d", !!spi, !!limits, !!entry];
        return false;
    }
    NSUInteger bytes = 0, alignment = 0;
    NSGetSizeAndAlignment(ivar_getTypeEncoding(limits), &bytes, &alignment);
    char argument[16] = {};
    method_getArgumentType(entry, 2, argument, sizeof(argument));
    if (bytes != sizeof(QcomMTLDeviceLimits) || alignment != alignof(QcomMTLDeviceLimits) ||
        strcmp(argument, @encode(uint32_t))) {
        if (diagnostic) *diagnostic = [NSString stringWithFormat:@"Metal framework ABI mismatch: limits=%lu alignment=%lu service=%s", (unsigned long)bytes, (unsigned long)alignment, argument];
        return false;
    }
    // The actual 22H374 _MTLDevice has no base protocols. Apple device
    // subclasses adopt MTLDeviceSPI; the abstract superclass cannot grant it.
    // Adopt the framework's registered protocol on our class, after checking
    // the projected ABI, without declaring a duplicate private protocol.
    Class device = objc_getClass("QcomMTLDevice");
    if (device && class_getSuperclass(device) == base && !class_conformsToProtocol(device, spi))
        class_addProtocol(device, spi);
    if (!device || class_getSuperclass(device) != base || !class_conformsToProtocol(device, spi)) {
        if (diagnostic) *diagnostic = @"Unable to adopt the native MTLDeviceSPI protocol on QcomMTLDevice";
        return false;
    }
    return class_conformsToProtocol(device, spi);
}
const char *protocolTypes(Protocol *protocol, SEL selector) {
    auto method = protocol_getMethodDescription(protocol, selector, YES, YES);
    if (!method.types) method = protocol_getMethodDescription(protocol, selector, NO, YES);
    if (method.types) return method.types;
    unsigned count = 0;
    Protocol *__unsafe_unretained *parents = protocol_copyProtocolList(protocol, &count);
    const char *types = nullptr;
    for (unsigned i = 0; !types && i < count; ++i) types = protocolTypes(parents[i], selector);
    free(parents); return types;
}
void clearReturn(NSInvocation *invocation) {
    NSUInteger bytes = invocation.methodSignature.methodReturnLength;
    if (bytes) { void *zero = calloc(1, bytes); if (!zero) abort();
        [invocation setReturnValue:zero]; free(zero); }
}
void publishCPU() { __asm__ volatile("dmb osh" ::: "memory"); }
uint64_t nativeCompletionClock() {
    timespec value{};if(clock_gettime(CLOCK_MONOTONIC,&value))return 0;
    return uint64_t(value.tv_sec)*1000000000+uint64_t(value.tv_nsec);
}
uint32_t nativeColorFormat(MTLPixelFormat format) {
    switch (format) {
        case MTLPixelFormatRGBA8Unorm: return GPU::Shader::RGBA8;
        case MTLPixelFormatBGRA8Unorm: return GPU::Shader::BGRA8;
        case MTLPixelFormatRGBA8Unorm_sRGB: return GPU::Shader::RGBA8SRGB;
        case MTLPixelFormatBGRA8Unorm_sRGB: return GPU::Shader::BGRA8SRGB;
        case MTLPixelFormatR8Unorm: return GPU::Shader::R8;
        case MTLPixelFormatRG8Unorm: return GPU::Shader::RG8;
        case MTLPixelFormatA8Unorm: return GPU::Shader::A8;
        case MTLPixelFormatRGBA16Float: return GPU::Shader::RGBA16Float;
        case MTLPixelFormatRGBA16Unorm: return GPU::Shader::RGBA16Unorm;
        case MTLPixelFormatASTC_4x4_LDR: return GPU::Shader::ASTC4x4;
        case MTLPixelFormatASTC_4x4_sRGB: return GPU::Shader::ASTC4x4SRGB;
        case MTLPixelFormatRG32Float: return GPU::Shader::RG32Float;
        case MTLPixelFormatR32Uint: return GPU::Shader::R32Uint;
        default: return 0;
    }
}
bool colorFormat(MTLPixelFormat format) { return nativeColorFormat(format)!=0; }
bool renderableColorFormat(MTLPixelFormat format) { return GPU::Shader::renderableColorFormat(nativeColorFormat(format)); }
uint32_t nativeSwizzle(MTLTextureSwizzleChannels s) {return GPU::Shader::packSwizzle(s.red,s.green,s.blue,s.alpha);}
bool depthFormat(MTLPixelFormat format) { return format==MTLPixelFormatDepth32Float || format==MTLPixelFormatDepth32Float_Stencil8; }
bool stencilFormat(MTLPixelFormat format) { return format==MTLPixelFormatStencil8 || format==MTLPixelFormatDepth32Float_Stencil8; }
bool cubeTextureType(MTLTextureType type) {return type==MTLTextureTypeCube || type==MTLTextureTypeCubeArray;}
bool arrayTextureType(MTLTextureType type) {return type==MTLTextureType2DArray || type==MTLTextureTypeCubeArray || type==MTLTextureType2DMultisampleArray;}
bool multisampleTextureType(MTLTextureType type) {return type==MTLTextureType2DMultisample || type==MTLTextureType2DMultisampleArray;}
bool colorTextureType(MTLTextureType type) {return type==MTLTextureType2D || multisampleTextureType(type) || arrayTextureType(type) || type==MTLTextureTypeCube;}
NSUInteger textureSliceCount(id<MTLTexture> texture) {return texture.arrayLength*(cubeTextureType(texture.textureType)?6:1);}
bool textureDescriptor(MTLTextureDescriptor *d) {
    return d && [d respondsToSelector:@selector(protectionOptions)] && !d.protectionOptions &&
        colorTextureType(d.textureType) && d.width && d.height &&
        d.width<=GPU::Surface::MaxDimension && d.height<=GPU::Surface::MaxDimension &&
        d.depth==1 && d.mipmapLevelCount && d.mipmapLevelCount<=GPU::Shader::mipLevelCount(uint32_t(d.width),uint32_t(d.height)) &&
        d.arrayLength && d.arrayLength<=2048 && (arrayTextureType(d.textureType) || d.arrayLength==1) && (!cubeTextureType(d.textureType) || d.width==d.height) &&
        ((multisampleTextureType(d.textureType) && (d.sampleCount==2 || d.sampleCount==4) && d.mipmapLevelCount==1 && d.storageMode==MTLStorageModePrivate && !(d.usage&MTLTextureUsageShaderWrite)) ||
         (!multisampleTextureType(d.textureType) && d.sampleCount==1)) &&
        (colorFormat(d.pixelFormat) || depthFormat(d.pixelFormat) || stencilFormat(d.pixelFormat)) &&
        (!GPU::Shader::unsignedIntegerFormat(nativeColorFormat(d.pixelFormat)) ||
            (d.sampleCount==1 && !(d.usage&MTLTextureUsageRenderTarget))) &&
        (!GPU::Shader::sampledOnlyFormat(nativeColorFormat(d.pixelFormat)) ||
            (d.sampleCount==1 && !(d.usage&~(MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView)))) &&
        !(d.usage&~(MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView)) &&
        (![d respondsToSelector:@selector(rotation)] || !d.rotation) &&
        d.compressionType==MTLTextureCompressionTypeLossless && GPU::Shader::validSwizzle(nativeSwizzle(d.swizzle)) &&
        (colorFormat(d.pixelFormat) || (!nativeSwizzle(d.swizzle) && !cubeTextureType(d.textureType)));
}
}

// This is a deliberately partial Metal implementation. Required methods not
// implemented below keep their SDK signatures and fail explicitly; capability
// queries never claim a complete GPU family beyond the registered AOT profile.
static void reportUnsupportedMetalCall(id object,SEL selector);
@interface QcomMTLObject : NSObject
+ (Protocol *)metalProtocol;
@end
@implementation QcomMTLObject
+ (Protocol *)metalProtocol { return @protocol(NSObject); }
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector {
    NSMethodSignature *signature = [super methodSignatureForSelector:selector];
    const char *types = signature ? nullptr : protocolTypes([[self class] metalProtocol], selector);
    return signature ?: (types ? [NSMethodSignature signatureWithObjCTypes:types] : nil);
}
- (void)doesNotRecognizeSelector:(SEL)selector {
    reportUnsupportedMetalCall(self,selector);[super doesNotRecognizeSelector:selector];
}
- (void)forwardInvocation:(NSInvocation *)invocation {
    reportUnsupportedMetalCall(self,invocation.selector);
    [NSException raise:NSInvalidArgumentException format:@"%@ does not implement %@",
        NSStringFromClass([self class]), NSStringFromSelector(invocation.selector)];
}
@end

@interface QcomMTLArchitecture : MTLArchitecture
@end
@implementation QcomMTLArchitecture
- (NSString *)name { return @"Adreno 540"; }
@end

@class QcomMTLBinaryArchive, QcomMTLDevice, QcomMTLBuffer, QcomMTLHeap, QcomMTLHeapAllocation, QcomMTLCommandQueue, QcomMTLCommandBuffer, QcomMTLFence, QcomMTLFencePoint;
@interface QcomMTLQueueEntry : NSObject
@property (nonatomic, weak) QcomMTLCommandBuffer *buffer;
@property (nonatomic, strong) QcomMTLCommandBuffer *submitted;
@end
@implementation QcomMTLQueueEntry
@end

// Only the partial protocol warning is suppressed. SDK type checking and all
// other warnings remain errors; unsupported calls use forwarding above.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
__attribute__((visibility("default")))
@interface QcomMTLDevice : _MTLDevice <MTLDevice> {
@public
    GPU::NativeConnection _connection;
    NSLock *_transportLock;
    NSLock *_dependencyLock;
    NSHashTable<QcomMTLCommandBuffer *> *_nativeWaiters;
    NSHashTable<QcomMTLCommandQueue *> *_nativeQueues;
    uint32_t _nativeJobs;
    kern_return_t _submissionError;
    uint64_t _registryID;
    NSUInteger _allocated;
    GPU::MetalUpload::Pool<> _inlinePool;
    NSLock *_compilerLock;
    NSMapTable<NSString *, id> *_libraryCache;
    BOOL _jit,_batchShaders,_mixedBatchShaders,_pipelinedAdmission,_completionNotifications,_surfaceFences,_gmem,_gmemSupported;
    uint64_t _gmemPasses;
    uint32_t _acceleratorService;
    QcomMTLDeviceLimits _nativeLimits;
    GPU::Native::Memory::Limits _memoryLimits;
    BOOL _featureQueriesInitialized;
    BOOL _metalDiagnosticsEnabled;
    BOOL _renderClearEnabled;
    BOOL _metalTimingEnabled;
    dispatch_queue_t _metalTimingQueue;
    BOOL _shaderCaptureEnabled;
    GPU::MetalShaderCapture::Budget _shaderCaptureBudget;
    std::set<std::pair<uint64_t,uint64_t>> _shaderCaptureHashes;
    std::set<std::pair<uint64_t,uint64_t>> _shaderCaptureArchiveAttempts;
    NSLock *_metalDiagnosticLock;
    NSMutableSet<NSData *> *_metalFailureKeys;
    std::set<std::array<uint8_t,32>> _shaderCacheMissKeys;
}
- (instancetype)initWithService:(uint32_t)service error:(NSError **)error;
- (void)reportMetalFailure:(GPU::Diagnostics::MetalStage)stage error:(NSError *)error;
- (BOOL)qcomMetalDiagnosticsEnabled;
- (BOOL)qcomRenderClearEnabled;
- (BOOL)qcomSetRenderClearEnabled:(BOOL)enabled;
- (void)reportMetalTiming:(const GPU::MetalTiming::Record &)record;
@end

namespace {
// CPU Mach ticks are directly comparable with the kernel job/display clocks.
// Reports are copied to a separate serial queue, never sent under compiler or
// command-buffer locks. Durations are wall time, including lock contention.
class MetalTimingScope {
    QcomMTLDevice *device_;
public:
    GPU::MetalTiming::Record record;
    MetalTimingScope(QcomMTLDevice *d,GPU::MetalTiming::Stage stage,uint64_t identity=0):device_(d) {
        record.stage=stage;record.identity=identity;if(d->_metalTimingEnabled)record.begin=mach_absolute_time();
    }
    ~MetalTimingScope(){if(record.begin){record.end=mach_absolute_time();[device_ reportMetalTiming:record];}}
};
void metalTimingSpan(QcomMTLDevice *d,GPU::MetalTiming::Stage stage,uint64_t begin,uint64_t end,uint64_t identity=0) {
    GPU::MetalTiming::Record r;r.stage=stage;r.begin=begin;r.end=end;r.identity=identity;[d reportMetalTiming:r];
}
uint64_t airLowerBoundary(uint64_t begin,uint64_t end,const GPU::Compiler::AIRCompileInfo &info) {
    static const mach_timebase_info_data_t scale=[] {mach_timebase_info_data_t s{};mach_timebase_info(&s);return s;}();
    if(end<=begin || !scale.numer)return begin;
    const auto ticks=static_cast<unsigned __int128>(info.lowerNS)*scale.denom/scale.numer;
    return begin+uint64_t(std::min<unsigned __int128>(ticks,end-begin));
}
// Preserve the native NSError and return behavior, including callers which
// omit their error pointer. Reporting is optional and never waits on transport
// while a compiler, encoder, or command-buffer condition is held.
class MetalFailureScope {
    QcomMTLDevice *device_;
    GPU::Diagnostics::MetalStage stage_;
    NSError *__autoreleasing *error_;
public:
    MetalFailureScope(QcomMTLDevice *device,GPU::Diagnostics::MetalStage stage,NSError *__autoreleasing *error):
        device_(device),stage_(stage),error_(error){}
    ~MetalFailureScope(){if(error_ && *error_)[device_ reportMetalFailure:stage_ error:*error_];}
    MetalFailureScope(const MetalFailureScope &)=delete;
    MetalFailureScope &operator=(const MetalFailureScope &)=delete;
};
}

@interface QcomMTLBuffer : QcomMTLObject <MTLBuffer> {
@public
    QcomMTLDevice *_owner;
    GPU::Native::ResourceData _resource;
    NSUInteger _length;
    NSUInteger _nativeOffset;
    NSUInteger _purgeableState;
    QcomMTLBuffer *_allocationRoot;
    QcomMTLHeapAllocation *_heapAllocation;
    MTLResourceOptions _options;
    BOOL _inlinePoolPage,_inlinePoolRetired;
    IOSurfaceRef _surface;
    void *_clientBytes;
    void (^_clientDeallocator)(void *, NSUInteger);
}
@property (atomic, copy) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device length:(NSUInteger)length options:(MTLResourceOptions)options;
- (instancetype)initWithDevice:(QcomMTLDevice *)device length:(NSUInteger)length options:(MTLResourceOptions)options inlinePage:(BOOL)inlinePage;
- (instancetype)initWithDevice:(QcomMTLDevice *)device surface:(IOSurfaceRef)surface plane:(NSUInteger)plane options:(MTLResourceOptions)options;
- (instancetype)initWithDevice:(QcomMTLDevice *)device bytesNoCopy:(void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^)(void *, NSUInteger))deallocator;
- (instancetype)initWithRoot:(QcomMTLBuffer *)root allocation:(QcomMTLHeapAllocation *)allocation length:(NSUInteger)length options:(MTLResourceOptions)options;
- (instancetype)initWithInlineRoot:(QcomMTLBuffer *)root offset:(NSUInteger)offset length:(NSUInteger)length;
- (NSUInteger)protectionOptions;
@end

@interface QcomMTLHeapAllocation : NSObject {
@public QcomMTLHeap *_heap;GPU::MetalHeap::Range _range;BOOL _aliasable;
}
@end

@interface QcomMTLHeap : QcomMTLObject <MTLHeap> {
@public QcomMTLDevice *_owner;QcomMTLBuffer *_backing;NSLock *_lock;
    GPU::MetalHeap::Allocator _allocator;MTLResourceOptions _options;MTLHeapType _type;
}
@property (atomic,copy) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLHeapDescriptor *)descriptor;
- (QcomMTLHeapAllocation *)allocationWithBytes:(NSUInteger)bytes offset:(NSUInteger)offset;
- (void)releaseAllocation:(QcomMTLHeapAllocation *)allocation aliasable:(BOOL)aliasable;
- (BOOL)isAllocationAliasable:(QcomMTLHeapAllocation *)allocation;
- (BOOL)options:(MTLResourceOptions)requested effective:(MTLResourceOptions *)effective;
- (id<MTLBuffer>)bufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset placement:(BOOL)placement;
- (id<MTLTexture>)textureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset placement:(BOOL)placement;
@end

@interface QcomMTLOperation : NSObject {
@public
    NSUInteger _nativeRow,_nativeByte;
    BOOL _borrowed;
    MetalLifetime::Reference<QcomMTLBuffer *> _sourceReference,_destinationReference;
    MetalLifetime::Reference<id<MTLEvent>> _eventReference;
    MetalLifetime::Reference<QcomMTLFence *> _fenceReference;
    std::vector<MetalLifetime::Reference<id>> _borrowedResources;
    NSArray *_resources;
}
@property (nonatomic, strong) QcomMTLBuffer *source;
@property (nonatomic, strong) QcomMTLBuffer *destination;
@property (nonatomic) NSUInteger sourceOffset, destinationOffset, bytes;
@property (nonatomic) NSUInteger rows, sourceStride, destinationStride;
@property (nonatomic) uint8_t value;
@property (nonatomic) BOOL wordPattern, pairPattern;
@property (nonatomic) uint32_t wordValue, wordHigh, shaderOpcode;
@property (nonatomic) BOOL shaderBufferWrites, immutableFill, gmemEligible, framebufferReads;
@property (nonatomic, copy) NSData *renderPass;
@property (nonatomic) NSUInteger renderPassDraws;
@property (nonatomic, copy) NSData *shaderPayload;
@property (nonatomic, copy) NSData *responsibleTaskIDs;
@property (nonatomic, copy) NSArray *resources;
@property (nonatomic, strong) id<MTLEvent> event;
@property (nonatomic) uint64_t eventValue;
@property (nonatomic) BOOL signalEvent;
@property (nonatomic, strong) QcomMTLFence *fence;
@property (nonatomic, strong) QcomMTLFencePoint *fencePoint;
@property (nonatomic, copy) NSArray<QcomMTLFencePoint *> *fencePoints;
@property (nonatomic) BOOL signalFence, fenceSubscribed, barrier;
- (void)configureRetainedReferences:(BOOL)retained;
- (NSArray *)copyLiveResourcesWithValidity:(BOOL *)valid;
@end
@implementation QcomMTLOperation
- (QcomMTLBuffer *)source { return _sourceReference.get(); }
- (void)setSource:(QcomMTLBuffer *)source { _sourceReference.set(source,!_borrowed); }
- (QcomMTLBuffer *)destination { return _destinationReference.get(); }
- (void)setDestination:(QcomMTLBuffer *)destination { _destinationReference.set(destination,!_borrowed); }
- (id<MTLEvent>)event { return _eventReference.get(); }
- (void)setEvent:(id<MTLEvent>)event { _eventReference.set(event,!_borrowed); }
- (QcomMTLFence *)fence { return _fenceReference.get(); }
- (void)setFence:(QcomMTLFence *)fence { _fenceReference.set(fence,!_borrowed); }
- (NSArray *)resources { return _resources; }
- (void)setResources:(NSArray *)resources {
    if(!_borrowed){_resources=[resources copy];return;}
    // Texture blits attach their view objects after copyRows has recorded the
    // underlying buffer operation. Those late references must stay borrowed.
    _borrowedResources.clear();
    for(id resource in resources) {
        MetalLifetime::Reference<id> reference;reference.set(resource,false);_borrowedResources.push_back(reference);
    }
}
- (void)configureRetainedReferences:(BOOL)retained {
    if(retained || _borrowed)return;_borrowed=YES;
    for(id resource in _resources) {
        MetalLifetime::Reference<id> reference;reference.set(resource,false);_borrowedResources.push_back(reference);
    }
    _sourceReference.borrow();_destinationReference.borrow();_eventReference.borrow();_fenceReference.borrow();_resources=nil;
}
- (NSArray *)copyLiveResourcesWithValidity:(BOOL *)valid {
    *valid=YES;if(!_borrowed)return @[];
    NSMutableArray *live=[NSMutableArray new];
    // Never treat a vanished copy source as a fill, or a vanished fence/event
    // as an ordinary operation. The strong snapshot lasts only for admission.
    for(const auto &reference:_borrowedResources) {
        id resource=reference.get();if(resource)[live addObject:resource];else *valid=NO;
    }
    QcomMTLBuffer *source=self.source,*destination=self.destination;id<MTLEvent> event=self.event;QcomMTLFence *fence=self.fence;
    if((_sourceReference.assigned() && !source) || (_destinationReference.assigned() && !destination) ||
        (_eventReference.assigned() && !event) || (_fenceReference.assigned() && !fence))*valid=NO;
    if(source)[live addObject:source];if(destination)[live addObject:destination];
    if(event)[live addObject:event];if(fence)[live addObject:fence];
    if(_resources)[live addObjectsFromArray:_resources];return live;
}
@end

@interface QcomMTLEvent : QcomMTLObject <MTLEvent> {
    QcomMTLDevice *_owner;
    NSCondition *_condition;
    uint64_t _value;
}
@property (atomic, copy) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device;
- (BOOL)waitForValue:(uint64_t)value;
- (void)signalValue:(uint64_t)value;
@end

@interface QcomMTLFencePoint : NSObject {
@public
    __weak QcomMTLCommandBuffer *_producer;
    NSLock *_lock;
    MetalSync::Point _point;
    NSMutableArray *_waiters;
}
- (BOOL)notifyWhenFinished:(dispatch_block_t)callback;
- (kern_return_t)status;
- (BOOL)pending;
- (BOOL)admitForQueue:(uint64_t)queue token:(uint64_t)token;
- (MetalSync::Observation)observationForQueue:(uint64_t)queue;
- (BOOL)retireThrough:(uint64_t)token;
- (void)finish:(kern_return_t)status queue:(uint64_t)queue token:(uint64_t)token;
@end

@interface QcomMTLFence : QcomMTLObject <MTLFence> {
@public
    QcomMTLDevice *_owner;
    NSMutableArray<QcomMTLFencePoint *> *_producers; // guarded by the device dependency lock
    BOOL _broken;
}
@property (atomic, copy) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device;
@end

@interface QcomMTLEncoderScope : NSObject {
    QcomMTLCommandBuffer *_buffer;
    NSUInteger _start;
    std::vector<MetalLifetime::Reference<QcomMTLFence *>> _waits,_updates;
}
- (instancetype)initWithBuffer:(QcomMTLCommandBuffer *)buffer;
- (void)fence:(id<MTLFence>)fence signal:(BOOL)signal;
- (void)finish;
@end

@interface QcomMTLPresentation : NSObject
@property (nonatomic, strong) id<MTLDrawable> drawable;
@property (nonatomic, strong) id<MTLTexture> texture;
@property (nonatomic, copy) NSDictionary *options;
@property (nonatomic) NSUInteger mode;
@property (nonatomic) CFTimeInterval time;
- (void)present;
@end

@implementation QcomMTLPresentation
- (void)present {
    switch (_mode) {
    case 0: [_drawable present]; break;
    case 1: [_drawable presentAtTime:_time]; break;
    case 2: [_drawable presentAfterMinimumDuration:_time]; break;
    case 3: [(id<QcomNativeDrawableOptions>)_drawable presentWithOptions:_options]; break;
    }
}
@end

@interface QcomMTLCommandQueue : QcomMTLObject <MTLCommandQueue> {
@public
    QcomMTLDevice *_owner;
    GPU::Native::QueueData _queue;
    GPU::Native::ShmemData _shmem;
    GPU::Native::NotificationData _notifications;
    GPU::Native::ResourceData _blitParameters;
    kern_return_t _completionError;
    uint64_t _token, _retiredToken;
    GPU::Native::CompletionWindow _completions;
    uint64_t _nativeProgress;
    BOOL _pollScheduled,_retryScheduled,_markerOnly;
    mach_port_t _completionPort;
    dispatch_source_t _completionSource;
    uint64_t _eventWakeups,_eventRetired,_watchdogRetired,_nextCompletionReport;
    uint64_t _admissionPressureSince;
    dispatch_queue_t _worker, _callbacks, _cleanup;
    MetalRetirement::Budget _cleanupBudget;
    uint64_t _cleanupRuns,_cleanupNanos,_cleanupMaxNanos,_cleanupQueueNanos,_cleanupWaits,_cleanupExclusiveRuns;
    NSRecursiveLock *_orderLock;
    NSLock *_executionLock;
    NSMutableArray<QcomMTLQueueEntry *> *_pending;
    NSMutableArray<QcomMTLCommandBuffer *> *_ready;
    NSMutableArray<QcomMTLCommandBuffer *> *_retiring;
    NSMutableArray<QcomMTLFencePoint *> *_fenceUpdates; // transport lock; native retirement owns completion
    dispatch_semaphore_t _capacity;
    BOOL _hasSubmitted,_drainScheduled,_finishScheduled;
    NSUInteger _priority,_backgroundPriority;
    __weak QcomMTLCommandBuffer *_executing;
    __weak QcomMTLCommandBuffer *_presentationBarrier;
}
@property (atomic, copy) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device capacity:(NSUInteger)capacity;
- (void)schedule:(QcomMTLCommandBuffer *)buffer commit:(BOOL)commit;
- (void)pump;
- (void)drain;
- (void)drainThrough:(QcomMTLCommandBuffer *)limit;
- (void)finishRetirements;
- (void)releaseCleanupBytes:(uint64_t)bytes admission:(MetalRetirement::Budget::Admission)admission nanos:(uint64_t)nanos queued:(uint64_t)queued;
- (NSDictionary *)qcomRetiredCleanupStatistics;
- (void)qcomWaitForRetiredCleanup;
- (void)wakeForNativeError;
- (void)armRetirement;
- (void)pollNative:(BOOL)watchdog;
- (void)notificationReady;
- (void)failNative:(kern_return_t)status;
- (kern_return_t)admitNative;
- (kern_return_t)deferNativeForBuffer:(QcomMTLCommandBuffer *)buffer;
- (kern_return_t)waitNative:(uint64_t)token buffer:(QcomMTLCommandBuffer *)buffer allQueues:(BOOL)all;
- (void)abandon;
- (void)setSubmissionQueue:(dispatch_queue_t)queue;
- (void)setCompletionQueue:(dispatch_queue_t)queue;
- (BOOL)_setGPUPriority:(NSUInteger)priority backgroundPriority:(NSUInteger)background;
- (BOOL)setGPUPriority:(NSUInteger)priority;
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority;
- (kern_return_t)submitCommands:(const GPU::Command *)commands count:(uint32_t)count;
- (kern_return_t)submitBlit:(const GPU::Blit::ComputePlan &)plan source:(QcomMTLBuffer *)source destination:(QcomMTLBuffer *)destination;
- (kern_return_t)execute:(QcomMTLOperation *)operation;
- (kern_return_t)executeEvent:(QcomMTLOperation *)operation;
- (kern_return_t)submitShader:(uint32_t)opcode payload:(NSData *)payload;
- (kern_return_t)submitShaderBatch:(NSArray<QcomMTLOperation *> *)operations;
@end

@interface QcomMTLCommandBuffer : QcomMTLObject <MTLCommandBuffer> {
@public
    QcomMTLCommandQueue *_queue;
    NSCondition *_condition;
    MTLCommandBufferStatus _status;
    NSError *_error;
    NSMutableArray<QcomMTLOperation *> *_operations;
    NSMutableArray<QcomMTLPresentation *> *_presentations;
    NSMutableArray *_scheduledHandlers, *_completedHandlers;
    NSUInteger _scheduledCallbacks,_completedCallbacks;
    BOOL _encoderActive, _hasCapacity, _retainedReferences;
    NSMutableArray *_ownedResources;
    QcomMTLBuffer *_inlinePage;
    NSUInteger _inlineUsed, _inlinePages;
    BOOL _blocked, _eventPending, _eventReady, _readyForExecution;
    BOOL _awaitingRetirement,_inRetirement,_nativeAdmitted;
    BOOL _presentationsSubmitted,_writesDrawable;
    BOOL _cleanupStarted,_cleanupFinished;
    uint64_t _completionToken,_timingIdentity;
    NSUInteger _nextOperation;
    NSData *_executionResponsibleTasks;
    kern_return_t _eventResult;
}
@property (atomic, copy) NSString *label;
- (instancetype)initWithQueue:(QcomMTLCommandQueue *)queue retainedReferences:(BOOL)retained;
- (void)setResponsibleTaskIDs:(const uint32_t *)ids count:(uint32_t)count;
- (void)retainInternalResource:(id)resource;
- (QcomMTLBuffer *)newInlineSnapshot:(const void *)bytes length:(NSUInteger)length;
- (void)fail:(NSError *)error;
- (void)record:(QcomMTLOperation *)operation;
- (BOOL)run;
- (BOOL)completeRetirement;
- (BOOL)blocked;
- (void)wake;
- (void)resumeEvent:(kern_return_t)status;
- (NSArray *)copyResourcesAndResolveFences;
- (void)recordBarrierWithResources:(NSArray *)resources;
- (void)markScheduled;
- (void)markNativeAdmitted;
- (BOOL)nativeAdmitted;
- (void)dispatchHandlers:(NSArray *)handlers scheduled:(BOOL)scheduled;
- (void)recordEvent:(id<MTLEvent>)event value:(uint64_t)value signal:(BOOL)signal;
- (void)recordPresentation:(id<MTLDrawable>)drawable mode:(NSUInteger)mode time:(CFTimeInterval)time options:(NSDictionary *)options;
- (void)submitPresentations;
- (void)commitWithDirectDrawableAdmission:(BOOL)direct;
- (BOOL)commitAndWaitUntilSubmitted;
- (BOOL)canFlushDirectDrawable;
- (void)flushDirectDrawable;
@end

@interface QcomMTLBlitCommandEncoder : QcomMTLObject <MTLBlitCommandEncoder> {
    QcomMTLCommandBuffer *_buffer;
    QcomMTLEncoderScope *_scope;
    BOOL _ended;
}
@property (atomic, copy) NSString *label;
- (instancetype)initWithBuffer:(QcomMTLCommandBuffer *)buffer;
- (QcomMTLOperation *)copyRows:(id<MTLBuffer>)source offset:(NSUInteger)sourceOffset stride:(NSUInteger)sourceStride
    to:(id<MTLBuffer>)destination offset:(NSUInteger)destinationOffset stride:(NSUInteger)destinationStride
    bytes:(NSUInteger)bytes rows:(NSUInteger)rows;
@end

@interface QcomMTLTexture : QcomMTLObject <MTLTexture> {
@public
    QcomMTLBuffer *_backing;
    QcomMTLTexture *_parent;
    NSUInteger _width, _height, _pitch, _stencilPitch, _stencilOffset, _bytesPerPixel, _levels, _offset, _parentLevel, _samples;
    NSUInteger _layers, _layerStride, _stencilLayerStride, _parentSlice,_storageBytes;
    MTLTextureType _type;
    GPU::Shader::TextureLayout _layout;
    MTLPixelFormat _format;
    MTLTextureUsage _usage;
    BOOL _framebufferOnly,_isDrawable,_bufferBacked,_memoryless;
    QcomMTLDevice *_memorylessDevice;
    NSLock *_passBackingLock;
    NSMapTable<QcomMTLCommandQueue *,QcomMTLTexture *> *_passBackings;
    MTLResourceOptions _memorylessOptions;
    uint32_t _swizzle;
}
@property (atomic, copy) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLTextureDescriptor *)descriptor;
- (instancetype)initLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor;
- (QcomMTLTexture *)newPassBackingForQueue:(QcomMTLCommandQueue *)queue;
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLTextureDescriptor *)descriptor surface:(IOSurfaceRef)surface plane:(NSUInteger)plane;
- (instancetype)initWithBuffer:(QcomMTLBuffer *)buffer descriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytes;
- (instancetype)initWithParent:(QcomMTLTexture *)parent format:(MTLPixelFormat)format levels:(NSRange)levels swizzle:(uint32_t)swizzle;
- (instancetype)initWithParent:(QcomMTLTexture *)parent format:(MTLPixelFormat)format type:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices swizzle:(uint32_t)swizzle;
- (GPU::Shader::Texture)nativeTexture;
- (GPU::Shader::Texture)nativeAspect:(BOOL)stencil;
- (GPU::Shader::Texture)nativeTextureAtLevel:(NSUInteger)level;
- (GPU::Shader::Texture)nativeTextureAtLevel:(NSUInteger)level slice:(NSUInteger)slice;
- (GPU::Shader::Texture)nativePlane:(BOOL)stencil;
- (GPU::Shader::Texture)nativePlane:(BOOL)stencil level:(NSUInteger)level;
- (GPU::Shader::Texture)nativePlane:(BOOL)stencil level:(NSUInteger)level slice:(NSUInteger)slice;
- (BOOL)blitPlane:(MTLBlitOption)options level:(NSUInteger)level descriptor:(GPU::Shader::Texture *)plane;
- (BOOL)blitPlane:(MTLBlitOption)options level:(NSUInteger)level slice:(NSUInteger)slice descriptor:(GPU::Shader::Texture *)plane;
@end

@interface QcomMTLTextureLayout : QcomMTLObject <QcomNativeTextureLayout> {
    GPU::MetalTexture::MemoryLayout _memoryLayout;
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLTextureDescriptor *)descriptor
    isHeapOrBufferBacked:(BOOL)heapOrBufferBacked;
@end

@interface QcomMTLSampler : QcomMTLObject <MTLSamplerState> {
@public QcomMTLDevice *_owner; GPU::Shader::Sampler _state;
}
@property (atomic, strong) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLSamplerDescriptor *)descriptor;
@end

@interface QcomMTLFunction : QcomMTLObject <MTLFunction> {
@public QcomMTLDevice *_owner; const GPU::Shader::Program *_program; id _dynamicProgram; NSUInteger _maxThreads;
    NSString *_declaredName;NSDictionary *_constantInfo;unsigned _reflectedStage;
    MTLFunctionOptions _options;
    NSData *_binaryKey;
}
@property (atomic, copy) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device program:(const GPU::Shader::Program *)program binaryKey:(NSData *)key;
- (instancetype)initWithDevice:(QcomMTLDevice *)device name:(NSString *)name stage:(unsigned)stage constants:(NSDictionary *)constants;
@end

@interface QcomMTLLibrary : QcomMTLObject <MTLLibrary> {
    QcomMTLDevice *_owner; NSArray<NSString *> *_names; NSMutableDictionary *_programs; NSUInteger _maxThreads;
    QcomMTLLibrary *_compiledLibrary;
    NSData *_archive,*_binaryIdentity;NSLock *_entryLock;
    NSMutableDictionary *_reflectionCache;
    std::vector<GPU::Compiler::LibraryArchive::Function> _archiveFunctions;
}
@property (atomic, copy) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device names:(NSArray *)names;
- (instancetype)initWithDevice:(QcomMTLDevice *)device images:(const std::vector<QGPUProgramImage> &)images error:(NSError **)error;
- (instancetype)initWithDevice:(QcomMTLDevice *)device archive:(NSData *)data identity:(NSData *)identity error:(NSError **)error;
- (id<MTLFunction>)functionWithName:(NSString *)name values:(MTLFunctionConstantValues *)values specialized:(BOOL)specialized error:(NSError **)error;
- (id<MTLFunction>)functionWithName:(NSString *)name values:(MTLFunctionConstantValues *)values specialized:(BOOL)specialized
    archives:(NSArray *)archives failOnMiss:(BOOL)fail error:(NSError **)error;
- (QcomMTLLibrary *)withMaxThreads:(NSUInteger)limit;
@end

@interface QcomMTLProgram : NSObject {
@public GPU::Shader::Program program;
    GPU::Shader::Program _msaaProgram;
    QcomMTLDevice *_owner;
    NSData *_image;
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device image:(const QGPUProgramImage &)image error:(NSError **)error;
@end

@interface QcomMTLComputePipeline : QcomMTLObject <MTLComputePipelineState> {
@public QcomMTLFunction *_function; NSUInteger _maxThreads; BOOL _threadgroupMultiple;
}
@property (atomic, strong) NSString *label;
- (instancetype)initWithFunction:(QcomMTLFunction *)function;
@end

@interface QcomMTLRenderPipeline : QcomMTLObject <MTLRenderPipelineState> {
@public QcomMTLFunction *_vertex, *_fragment; MTLPixelFormat _formats[QGPU_COLOR_TARGET_COUNT],_depthFormat,_stencilFormat;
    GPU::Shader::ColorState _colorStates[QGPU_COLOR_TARGET_COUNT];
    uint32_t _samples,_alphaToCoverage;
    GPU::Shader::RenderState _state;
    GPU::Shader::Draw _vertexInput;
}
@property (atomic, strong) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLRenderPipelineDescriptor *)descriptor;
@end

@interface QcomMTLBinaryArchive : QcomMTLObject <MTLBinaryArchive> {
    QcomMTLDevice *_owner;NSLock *_lock;
    std::map<GPU::MetalArchive::Key,GPU::MetalArchive::Function> _functions;
    std::map<GPU::MetalArchive::Key,GPU::MetalArchive::Pipeline> _pipelines;
    NSMapTable<NSData *,QcomMTLProgram *> *_registered;
}
@property (atomic,copy) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error;
- (instancetype)initWithDevice:(QcomMTLDevice *)device archiveData:(NSData *)data error:(NSError **)error;
- (QcomMTLFunction *)functionForKey:(NSData *)key name:(NSString *)name stage:(unsigned)stage maxThreads:(NSUInteger)limit
    found:(BOOL *)found error:(NSError **)error;
- (BOOL)containsPipeline:(const GPU::MetalArchive::Pipeline &)pipeline;
- (BOOL)addFunctions:(NSArray<QcomMTLFunction *> *)functions pipeline:(const GPU::MetalArchive::Pipeline &)pipeline error:(NSError **)error;
@end

@interface QcomMTLDepthStencilState : QcomMTLObject <MTLDepthStencilState> {
@public QcomMTLDevice *_owner; GPU::Shader::DepthStencilState _state;
}
@property (atomic, strong) NSString *label;
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLDepthStencilDescriptor *)descriptor;
@end

@interface QcomMTLComputeEncoder : QcomMTLObject <MTLComputeCommandEncoder> {
    QcomMTLCommandBuffer *_buffer; MetalLifetime::Reference<QcomMTLComputePipeline *> _pipeline;
    QcomMTLEncoderScope *_scope;
    MetalLifetime::Reference<QcomMTLBuffer *> _buffers[GPU::Shader::MaxBindings]; NSUInteger _offsets[GPU::Shader::MaxBindings];
    uint32_t _threadgroupLengths[GPU::Shader::MaxBindings];
    MetalLifetime::Reference<QcomMTLTexture *> _textures[QGPU_COMPUTE_IMAGE_COUNT];
    MetalLifetime::Reference<QcomMTLSampler *> _samplers[QGPU_COMPUTE_IMAGE_COUNT];
    GPU::Shader::Sampler _samplerStates[QGPU_COMPUTE_IMAGE_COUNT];
    BOOL _ended;
}
@property (atomic, copy) NSString *label;
- (NSArray *)copyLiveBindings;
- (instancetype)initWithBuffer:(QcomMTLCommandBuffer *)buffer;
- (void)encodeDispatch:(MTLSize)groups local:(MTLSize)local grid:(MTLSize)grid;
@end

@interface QcomMTLRenderEncoder : QcomMTLObject <MTLRenderCommandEncoder> {
    QcomMTLCommandBuffer *_buffer; MetalLifetime::Reference<QcomMTLRenderPipeline *> _pipeline;
    MetalLifetime::Reference<QcomMTLTexture *> _targets[QGPU_COLOR_TARGET_COUNT],_depth,_stencil;
    QcomMTLEncoderScope *_scope;
    MetalLifetime::Reference<QcomMTLTexture *> _sampledTextures[2][QGPU_METAL_TEXTURE_BINDING_COUNT];
    MetalLifetime::Reference<QcomMTLSampler *> _samplers[2][QGPU_STAGE_SAMPLER_COUNT];
    GPU::Shader::Sampler _samplerStates[2][QGPU_STAGE_SAMPLER_COUNT];
    GPU::Shader::Texture _attachments[QGPU_COLOR_TARGET_COUNT+2],_resolvePlanes[QGPU_COLOR_TARGET_COUNT+2];
    MetalLifetime::Reference<QcomMTLTexture *> _resolveTargets[QGPU_COLOR_TARGET_COUNT+2];
    GPU::Shader::DepthStencilResolvePlan _planeResolve[2];
    QcomMTLBuffer *_resolveParameters[2];
    const GPU::Shader::Program *_planeResolvePrograms[2];
    uint32_t _samples;
    MetalLifetime::Reference<QcomMTLBuffer *> _buffers[GPU::Shader::MaxBindings]; NSUInteger _offsets[GPU::Shader::MaxBindings];
    MetalLifetime::Reference<QcomMTLBuffer *> _fragmentBuffers[GPU::Shader::MaxBindings]; NSUInteger _fragmentOffsets[GPU::Shader::MaxBindings];
    MTLScissorRect _scissor; BOOL _ended;
    GPU::Shader::RenderState _state; BOOL _emptyViewport;
    MTLViewport _viewport;
    GPU::Shader::DepthStencilState _depthStencil;
    NSUInteger _width,_height;
    GPU::RenderPass::Description _renderPass;
    NSUInteger _passStart,_passDrawStart;
    BOOL _gmemCandidate;
}
@property (atomic, copy) NSString *label;
- (NSArray *)copyLiveBindings;
- (instancetype)initWithBuffer:(QcomMTLCommandBuffer *)buffer descriptor:(MTLRenderPassDescriptor *)descriptor rejection:(NSString *__autoreleasing *)rejection;
- (void)encodeDraw:(MTLPrimitiveType)primitive start:(NSUInteger)start count:(NSUInteger)count
    instances:(NSUInteger)instances baseInstance:(NSUInteger)baseInstance
    indices:(QcomMTLBuffer *)indices indexSize:(NSUInteger)indexSize offset:(NSUInteger)indexOffset baseVertex:(NSInteger)baseVertex;
@end

namespace {
uint64_t retiredStorageBytes(NSArray<QcomMTLOperation *> *operations,NSArray *owned,NSArray<QcomMTLPresentation *> *presentations) {
    // Charge each native backing once, including a heap's whole root, and
    // command payloads/reference storage. Borrowed bindings must stay weak;
    // loading them here could become their last owner after real retirement.
    std::set<uint32_t> seen;uint64_t bytes=0;
    auto add=[&](uint64_t n){bytes=n>UINT64_MAX-bytes?UINT64_MAX:bytes+n;};
    auto resource=[&](id object) {
        QcomMTLBuffer *buffer=nil;
        if([object isKindOfClass:[QcomMTLBuffer class]])buffer=object;
        else if([object isKindOfClass:[QcomMTLTexture class]])buffer=((QcomMTLTexture *)object)->_backing;
        else if([object isKindOfClass:[QcomMTLHeap class]])buffer=((QcomMTLHeap *)object)->_backing;
        if(buffer && buffer->_resource.id && seen.insert(buffer->_resource.id).second)add(buffer->_resource.bytes);
    };
    add((operations.count+owned.count+presentations.count)*sizeof(id));
    for(id object in owned)resource(object);
    for(QcomMTLPresentation *presentation in presentations)resource(presentation.texture);
    for(QcomMTLOperation *operation in operations) {
        add(class_getInstanceSize([QcomMTLOperation class]));
        add(operation.shaderPayload.length);add(operation.responsibleTaskIDs.length);
        add(operation->_resources.count*sizeof(id));
        if(!operation->_borrowed) {
            resource(operation.source);resource(operation.destination);
            for(id object in operation->_resources)resource(object);
        }
    }
    return bytes;
}
template<class Object> bool captureReference(const MetalLifetime::Reference<Object> &reference,NSMutableArray *live) {
    Object object=reference.get();if(reference.assigned() && !object)return false;
    if(object)[live addObject:object];return true;
}
// These public Metal enum values are also the semantic wire values. Assert
// every admitted value so an SDK change cannot silently alter the mapping.
#define QCOM_COMPARE(name) static_assert(uint32_t(MTLCompareFunction##name)==GPU::Shader::name,"Metal depth compare ABI")
static_assert(MTLVertexFormatFloat==28 && MTLVertexFormatFloat4==31 && MTLVertexFormatUInt4==39 &&
    MTLVertexFormatHalf==53 && MTLVertexFormatFloatRG11B10==54 && MTLVertexStepFunctionConstant==0 &&
    MTLVertexStepFunctionPerVertex==1 && MTLVertexStepFunctionPerInstance==2,"Metal vertex layout ABI");
QCOM_COMPARE(Never); QCOM_COMPARE(Less); QCOM_COMPARE(Equal); QCOM_COMPARE(LessEqual);
QCOM_COMPARE(Greater); QCOM_COMPARE(NotEqual); QCOM_COMPARE(GreaterEqual); QCOM_COMPARE(Always);
#undef QCOM_COMPARE
static_assert(MTLStencilOperationKeep==0 && MTLStencilOperationZero==1 && MTLStencilOperationReplace==2 &&
    MTLStencilOperationIncrementClamp==3 && MTLStencilOperationDecrementClamp==4 && MTLStencilOperationInvert==5 &&
    MTLStencilOperationIncrementWrap==6 && MTLStencilOperationDecrementWrap==7,"Metal stencil operation ABI");
#define QCOM_BLEND_FACTOR(name) static_assert(uint32_t(MTLBlendFactor##name) == GPU::Shader::name, "Metal blend-factor ABI")
QCOM_BLEND_FACTOR(Zero); QCOM_BLEND_FACTOR(One); QCOM_BLEND_FACTOR(SourceColor); QCOM_BLEND_FACTOR(OneMinusSourceColor);
QCOM_BLEND_FACTOR(SourceAlpha); QCOM_BLEND_FACTOR(OneMinusSourceAlpha); QCOM_BLEND_FACTOR(DestinationColor);
QCOM_BLEND_FACTOR(OneMinusDestinationColor); QCOM_BLEND_FACTOR(DestinationAlpha); QCOM_BLEND_FACTOR(OneMinusDestinationAlpha);
QCOM_BLEND_FACTOR(SourceAlphaSaturated); QCOM_BLEND_FACTOR(BlendColor); QCOM_BLEND_FACTOR(OneMinusBlendColor);
QCOM_BLEND_FACTOR(BlendAlpha); QCOM_BLEND_FACTOR(OneMinusBlendAlpha);
#undef QCOM_BLEND_FACTOR
#define QCOM_BLEND_OPERATION(name) static_assert(uint32_t(MTLBlendOperation##name) == GPU::Shader::name, "Metal blend-operation ABI")
QCOM_BLEND_OPERATION(Add); QCOM_BLEND_OPERATION(Subtract); QCOM_BLEND_OPERATION(ReverseSubtract);
QCOM_BLEND_OPERATION(Min); QCOM_BLEND_OPERATION(Max);
#undef QCOM_BLEND_OPERATION
static_assert(uint32_t(MTLCullModeNone) == GPU::Shader::CullNone && uint32_t(MTLCullModeFront) == GPU::Shader::CullFront &&
    uint32_t(MTLCullModeBack) == GPU::Shader::CullBack && uint32_t(MTLWindingClockwise) == GPU::Shader::Clockwise &&
    uint32_t(MTLWindingCounterClockwise) == GPU::Shader::CounterClockwise, "Metal raster ABI");

NSDictionary *registeredLibrary() {
    static NSDictionary *library; static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSData *data = [NSData dataWithBytes:GPUPrograms::Library length:sizeof(GPUPrograms::Library)-1];
        library = [NSJSONSerialization JSONObjectWithData:data options:0 error:nullptr];
        if (!library) abort(); // A corrupt generated driver binary cannot be used.
    });
    return library;
}
NSError *shaderFailure(NSString *message, NSInteger code=MTLLibraryErrorCompileFailure) {
    return [NSError errorWithDomain:MTLLibraryErrorDomain code:code
        userInfo:@{NSLocalizedDescriptionKey:message}];
}
bool pipelineOptions(MTLPipelineOption options,NSError **error) {
    if(options&(MTLPipelineOptionBindingInfo|MTLPipelineOptionBufferTypeInfo)) {
        if(error)*error=shaderFailure(@"A540 pipeline binding/type reflection is not available",MTLLibraryErrorUnsupported);return false;
    }
    if(options&~MTLPipelineOptionFailOnBinaryArchiveMiss) {
        if(error)*error=shaderFailure(@"Unknown A540 pipeline options",MTLLibraryErrorUnsupported);return false;
    }
    return true;
}
bool emptyLinkedFunctions(MTLLinkedFunctions *functions) {
    return !functions.functions.count && !functions.binaryFunctions.count &&
        !functions.privateFunctions.count && !functions.groups.count;
}
bool nativeFunctionConstantABI() {
    const char *bindings[][3]={
        {"MTLFunctionConstantValuesInternal","newIndexedConstantArray","@16@0:8"},
        {"MTLFunctionConstantValuesInternal","newNamedConstantArray","@16@0:8"},
        {"MTLIndexedConstantValue","dataType","Q16@0:8"},{"MTLIndexedConstantValue","index","Q16@0:8"},
        {"MTLIndexedConstantValue","data","^v16@0:8"},{"MTLNamedConstantValue","dataType","Q16@0:8"},
        {"MTLNamedConstantValue","name","@16@0:8"},{"MTLNamedConstantValue","data","^v16@0:8"},
        {"MTLFunctionConstantInternal","initWithName:type:index:required:","@44@0:8@16Q24Q32B40"}};
    for(const auto &binding:bindings) {
        Class cls=objc_getClass(binding[0]);Method method=cls?class_getInstanceMethod(cls,sel_registerName(binding[1])):nullptr;
        if(!method || strcmp(method_getTypeEncoding(method),binding[2]))return false;
    }
    return true;
}
bool snapshotConstants(MTLFunctionConstantValues *values,std::vector<GPU::Compiler::FunctionConstantValue> &out,NSError **error) {
    out.clear();if(!values)return true;
    auto reject=[&](NSString *message){if(error)*error=shaderFailure(message);out.clear();return false;};
    if(!nativeFunctionConstantABI() || ![values isKindOfClass:[MTLFunctionConstantValues class]])return reject(@"Unsupported native function constant ABI");
    // Original Metal creates independent value objects, preserving public
    // set-by-name/index/range/reset/copy semantics without driver interposing.
    MTLFunctionConstantValues *copy=[values copy];
    NSArray *indexed=[copy newIndexedConstantArray],*named=[copy newNamedConstantArray];
    if(indexed.count>GPU::Compiler::MaxConstantValues || named.count>GPU::Compiler::MaxConstantValues-indexed.count)
        return reject(@"Too many supplied function constants");
    for(unsigned group=0;group<2;++group)for(id object in group?named:indexed) {
        Class cls=objc_getClass(group?"MTLNamedConstantValue":"MTLIndexedConstantValue");
        if(![object isKindOfClass:cls])return reject(@"Invalid native constant snapshot");
        auto constant=(id<QcomNativeConstantData>)object;NSUInteger type=constant.dataType;
        if(type>UINT32_MAX || !GPU::Compiler::constantBytes(uint32_t(type)) || !constant.data)return reject(@"Unsupported function constant type");
        GPU::Compiler::FunctionConstantValue value;value.type=uint32_t(type);
        if(group) {
            NSString *name=[(id<QcomNativeNamedConstantData>)object name];
            if(!name.length || !name.UTF8String)return reject(@"Invalid function constant name");value.name=name.UTF8String;
        }else {
            auto index=[(id<QcomNativeIndexedConstantData>)object index];if(index>65535)return reject(@"Function constant index outside Metal range");value.index=uint32_t(index);
        }
        memcpy(value.bytes.data(),constant.data,GPU::Compiler::constantBytes(value.type));out.push_back(std::move(value));
    }
    return true;
}
NSDictionary *constantReflection(const std::vector<GPU::Compiler::FunctionConstant> &constants,NSError **error) {
    NSMutableDictionary *result=[NSMutableDictionary new];if(constants.empty())return result;
    if(!nativeFunctionConstantABI()){if(error)*error=shaderFailure(@"Unsupported native constant reflection ABI");return nil;}
    Class cls=objc_getClass("MTLFunctionConstantInternal");
    for(const auto &constant:constants) {
        NSString *name=[[NSString alloc] initWithBytes:constant.name.data() length:constant.name.size() encoding:NSUTF8StringEncoding];
        MTLFunctionConstant *object=name?[(MTLFunctionConstant *)[cls alloc] initWithName:name type:MTLDataType(constant.type) index:constant.index required:constant.required]:nil;
        if(!object){if(error)*error=shaderFailure(@"Invalid function constant reflection");return nil;}
        result[name]=object;
    }
    return [result copy];
}
NSString *constantCacheKey(NSString *name,const std::vector<GPU::Compiler::FunctionConstantValue> &values) {
    NSMutableData *data=[NSMutableData new];
    for(const auto &v:values) {
        uint32_t fields[]={v.index,v.type};[data appendBytes:fields length:sizeof(fields)];[data appendBytes:v.bytes.data() length:GPU::Compiler::constantBytes(v.type)];
    }
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,hash);
    return [NSString stringWithFormat:@"specialized:%@:%@",name,[[NSData dataWithBytes:hash length:sizeof(hash)] base64EncodedStringWithOptions:0]];
}
namespace Archive=GPU::MetalArchive;
Archive::Key archiveHash(const void *bytes,size_t size) {
    Archive::Key key{};CC_SHA256(bytes,CC_LONG(size),key.data());return key;
}
std::string airFailureContext(const GPU::Compiler::LibraryArchive::Function &function,const void *air,
    const std::vector<GPU::Compiler::FunctionConstantValue> &constants) {
    constexpr char hex[]="0123456789abcdef";
    auto appendHex=[&](std::string &s,const uint8_t *bytes,size_t count) {
        for(size_t i=0;i<count;++i){s+=hex[bytes[i]>>4];s+=hex[bytes[i]&15];}
    };
    auto hash=archiveHash(air,function.size);
    std::string result="\nGPU_AIR_FAILURE entry="+function.name.substr(0,128)+" bytes="+std::to_string(function.size)+" sha256=";
    appendHex(result,hash.data(),hash.size());result+=" constants="+std::to_string(constants.size())+" values=[";
    // These are the resolved, bounded values used for this exact variant.
    // Emit only on failure, so a native client can report a reproducible input.
    for(const auto &v:constants) {
        result+=std::to_string(v.index)+":"+std::to_string(v.type)+":";
        appendHex(result,v.bytes.data(),GPU::Compiler::constantBytes(v.type));result+=";";
    }
    return result+"]";
}
Archive::Key archiveFileDigest(const void *bytes,size_t size) {
    Archive::Key key{},zero{};if(size<Archive::HeaderBytes)return key;
    const auto *data=static_cast<const uint8_t *>(bytes);CC_SHA256_CTX context;
    CC_SHA256_Init(&context);CC_SHA256_Update(&context,data,Archive::DigestOffset);
    CC_SHA256_Update(&context,zero.data(),zero.size());
    CC_SHA256_Update(&context,data+Archive::HeaderBytes,CC_LONG(size-Archive::HeaderBytes));
    CC_SHA256_Final(key.data(),&context);return key;
}
NSData *keyData(const Archive::Key &key) {return [NSData dataWithBytes:key.data() length:key.size()];}
// Generated bytes are immutable for this process. Library wrappers and AOT
// functions share their identity without rescanning the complete manifest.
NSData *builtinLibraryIdentity() {
    static NSData *identity;static dispatch_once_t once;
    dispatch_once(&once,^{identity=keyData(archiveHash(GPUPrograms::Library,sizeof(GPUPrograms::Library)-1));});
    return identity;
}
Archive::Key archiveKey(NSData *data) {
    Archive::Key key{};if(data.length==key.size())memcpy(key.data(),data.bytes,key.size());return key;
}
class ProcessAIRCache {
    NSLock *lock=[NSLock new];
    GPU::Compiler::AIRFailureCache failures;
    struct Reflection {
        Archive::Key key{};
        std::vector<GPU::Compiler::FunctionConstant> constants;
        unsigned stage=UINT32_MAX;bool valid=false;
    };
    std::array<Reflection,32> reflections{};size_t nextReflection=0;
    struct Guard {NSLock *lock;Guard(NSLock *l):lock(l){[lock lock];}~Guard(){[lock unlock];}};
public:
    static ProcessAIRCache &shared(){static ProcessAIRCache cache;return cache;}
    bool failure(NSData *key,std::string &reason) {
        if(key.length!=32 || !lock)return false;
        Guard held(lock);return failures.lookup(archiveKey(key),reason);
    }
    void rememberFailure(NSData *key,const std::string &entry,const std::string &reason) {
        if(key.length!=32 || !lock)return;
        Guard held(lock);failures.remember(archiveKey(key),entry,reason);
    }
    bool reflection(NSData *key,std::vector<GPU::Compiler::FunctionConstant> &constants,unsigned &stage) {
        if(key.length!=32 || !lock)return false;
        Guard held(lock);const auto identity=archiveKey(key);
        for(const auto &at:reflections)if(at.valid && at.key==identity){constants=at.constants;stage=at.stage;return true;}
        return false;
    }
    void rememberReflection(NSData *key,const std::vector<GPU::Compiler::FunctionConstant> &constants,unsigned stage) {
        if(key.length!=32 || !lock || constants.size()>512)return;
        size_t names=0;for(const auto &c:constants){if(c.name.size()>65536-names)return;names+=c.name.size();}
        Guard held(lock);const auto identity=archiveKey(key);
        for(const auto &at:reflections)if(at.valid && at.key==identity)return;
        auto &at=reflections[nextReflection];at.key=identity;at.constants=constants;at.stage=stage;at.valid=true;
        nextReflection=(nextReflection+1)%reflections.size();
    }
};

Archive::Key archiveContext() {
    static Archive::Key context{};static dispatch_once_t once;
    dispatch_once(&once,^{
        NSMutableData *bytes=[NSMutableData dataWithBytes:GPUArchiveBuild::Context length:sizeof(GPUArchiveBuild::Context)];
        [bytes appendData:[NSProcessInfo.processInfo.operatingSystemVersionString dataUsingEncoding:NSUTF8StringEncoding]];
        context=archiveHash(bytes.bytes,bytes.length);
    });return context;
}
void archiveWord(NSMutableData *bytes,uint64_t value) {
    uint8_t packed[8];for(unsigned i=0;i<8;++i)packed[i]=uint8_t(value>>(i*8));[bytes appendBytes:packed length:8];
}
NSData *functionBinaryKey(NSData *library,NSString *specialization,NSUInteger maxThreads) {
    NSMutableData *bytes=[NSMutableData dataWithData:keyData(archiveContext())];[bytes appendData:library];
    NSData *name=[specialization dataUsingEncoding:NSUTF8StringEncoding];archiveWord(bytes,name.length);[bytes appendData:name];
    archiveWord(bytes,maxThreads);return keyData(archiveHash(bytes.bytes,bytes.length));
}
class CompiledFunctionCache {
    GPU::CoreFSCache::Store store_;
    bool temporary_=false;
public:
    CompiledFunctionCache() {
        if(!store_.available())return;
        const auto context=archiveContext();
        for(bool temporary:{false,true}) {
            NSString *directory=temporary?NSTemporaryDirectory():nil;
            NSURL *base=temporary?(directory.length?[NSURL fileURLWithPath:directory isDirectory:YES]:nil):
                [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
            if(!base.isFileURL)continue;
            // Match the native Metal caller's bundle scope, but keep our own
            // namespace. Daemons may only write their scoped temporary path.
            NSString *bundle=NSBundle.mainBundle.bundleIdentifier;
            if(!temporary && bundle.length)base=[base URLByAppendingPathComponent:bundle isDirectory:YES];
            NSURL *folder=[base URLByAppendingPathComponent:@"org.darwin.QcomMTL/corefs" isDirectory:YES];
            if(![NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nullptr])continue;
            // CoreFSCache locks each backing store across processes. As in
            // Metal's openSync, another owner selects a separate bounded slot.
            for(unsigned slot=0;slot<8;++slot) {
                NSURL *url=[folder URLByAppendingPathComponent:[NSString stringWithFormat:@"functions-%u",slot]];
                const int status=store_.open(url.fileSystemRepresentation,context.data(),32u*1024*1024);
                if(!status){temporary_=temporary;return;}
                if(status!=GPU::CoreFSCache::Busy)break;
            }
        }
    }
    static CompiledFunctionCache &shared(){static CompiledFunctionCache cache;return cache;}
    NSData *copy(NSData *key){return key.length==32?store_.copy(static_cast<const uint8_t *>(key.bytes),Archive::MaxBytes):nil;}
    uint64_t hitDetail() const {return 512|(temporary_?8:1);}
    uint64_t store(NSData *key,NSData *data) {
        return key.length==32 && store_.store(static_cast<const uint8_t *>(key.bytes),data)?512|(temporary_?16:2):4;
    }
};
QcomMTLFunction *cachedCompiledFunction(QcomMTLDevice *device,NSData *key,NSString *name,unsigned stage,NSUInteger maxThreads,uint64_t &detail) {
    auto &cache=CompiledFunctionCache::shared();NSData *data=cache.copy(key);
    if(!data)return nil;
    NSError *ignored=nil;
    QcomMTLBinaryArchive *archive=[[QcomMTLBinaryArchive alloc] initWithDevice:device archiveData:data error:&ignored];
    BOOL found=NO;
    QcomMTLFunction *function=[archive functionForKey:key name:name stage:stage maxThreads:maxThreads found:&found error:&ignored];
    if(function && [function->_dynamicProgram isKindOfClass:[QcomMTLProgram class]]) {
        detail|=cache.hitDetail();return function;
    }
    return nil;
}
uint64_t storeCompiledFunction(QcomMTLProgram *program,NSData *key,NSUInteger maxThreads) {
    Archive::Function record;record.key=archiveKey(key);record.kind=Archive::Compiled;
    record.stage=program->program.stage;record.name=program->program.name;
    record.maxThreads=uint32_t(MIN(maxThreads,(program->program.flags&QGPU_VARIABLE_LOCAL_SIZE)?GPU::Shader::MaxComputeThreads:64));
    const auto *image=static_cast<const uint8_t *>(program->_image.bytes);
    record.image.assign(image,image+program->_image.length);
    Archive::Contents contents;contents.functions.push_back(std::move(record));std::vector<uint8_t> bytes;
    if(!Archive::encode(contents,archiveContext(),GPUPrograms::Registry,bytes))return 4;
    auto digest=archiveFileDigest(bytes.data(),bytes.size());memcpy(bytes.data()+Archive::DigestOffset,digest.data(),digest.size());
    NSData *data=[NSData dataWithBytes:bytes.data() length:bytes.size()];
    return CompiledFunctionCache::shared().store(key,data);
}

NSError *archiveFailure(NSString *message,MTLBinaryArchiveError code=MTLBinaryArchiveErrorCompilationFailure) {
    return [NSError errorWithDomain:MTLBinaryArchiveDomain code:code userInfo:@{NSLocalizedDescriptionKey:message}];
}
bool validBinaryArchives(NSArray *archives,QcomMTLDevice *device,NSError **error) {
    if(archives && (![archives isKindOfClass:[NSArray class]] || archives.count>16)) {
        if(error)*error=shaderFailure(@"At most16 native A540 binary archives are supported",MTLLibraryErrorUnsupported);return false;
    }
    for(id archive in archives)if(![archive isKindOfClass:[QcomMTLBinaryArchive class]] || [archive device]!=device) {
        if(error)*error=shaderFailure(@"A binary archive must belong to this A540 Metal device",MTLLibraryErrorUnsupported);return false;
    }
    return true;
}
Archive::Pipeline archivePipeline(QcomMTLComputePipeline *pipeline) {
    Archive::Pipeline result;result.kind=Archive::Compute;result.functions[0]=archiveKey(pipeline->_function->_binaryKey);
    NSMutableData *bytes=[NSMutableData dataWithData:keyData(archiveContext())];archiveWord(bytes,result.kind);
    [bytes appendData:pipeline->_function->_binaryKey];archiveWord(bytes,pipeline->_maxThreads);archiveWord(bytes,pipeline->_threadgroupMultiple);
    result.key=archiveHash(bytes.bytes,bytes.length);return result;
}
Archive::Pipeline archivePipeline(QcomMTLRenderPipeline *pipeline) {
    Archive::Pipeline result;result.kind=Archive::Render;result.functions[0]=archiveKey(pipeline->_vertex->_binaryKey);result.functions[1]=archiveKey(pipeline->_fragment->_binaryKey);
    NSMutableData *bytes=[NSMutableData dataWithData:keyData(archiveContext())];archiveWord(bytes,result.kind);
    [bytes appendData:pipeline->_vertex->_binaryKey];[bytes appendData:pipeline->_fragment->_binaryKey];
    archiveWord(bytes,pipeline->_vertex->_maxThreads);archiveWord(bytes,pipeline->_fragment->_maxThreads);
    for(auto format:pipeline->_formats)archiveWord(bytes,format);
    archiveWord(bytes,pipeline->_depthFormat);archiveWord(bytes,pipeline->_stencilFormat);archiveWord(bytes,pipeline->_samples);archiveWord(bytes,pipeline->_alphaToCoverage);
    [bytes appendBytes:pipeline->_colorStates length:sizeof(pipeline->_colorStates)];
    [bytes appendBytes:pipeline->_vertexInput.attributes length:sizeof(pipeline->_vertexInput.attributes)];
    [bytes appendBytes:pipeline->_vertexInput.layouts length:sizeof(pipeline->_vertexInput.layouts)];
    result.key=archiveHash(bytes.bytes,bytes.length);return result;
}
template<class P> bool pipelineArchiveLookup(NSArray *archives,QcomMTLDevice *device,P *pipeline,MTLPipelineOption options,NSError **error) {
    if(!validBinaryArchives(archives,device,error))return false;
    // Ordinary pipeline creation has no archive lookup and needs no key.
    if(archives.count) {
        const auto record=archivePipeline(pipeline);
        for(QcomMTLBinaryArchive *archive in archives)if([archive containsPipeline:record])return true;
    }
    if(options&MTLPipelineOptionFailOnBinaryArchiveMiss) {
        if(error)*error=archiveFailure(@"No matching A540 compiled pipeline in the supplied binary archives");return false;
    }
    return true;
}
// Diagnostic-only cold compilation capture. No client buffer contents,
// GPU ownership or completion state are read or changed. The caller holds
// this device's compiler lock. Bounded writes never turn a valid shader into
// a compile failure, and failed writes are charged to the same disk budget.
void captureNativeShader(QcomMTLDevice *device,const GPU::Compiler::Entry &entry,const QGPUProgramImage &image,
    const GPU::Compiler::LibraryArchive::Function &function,const std::vector<GPU::Compiler::FunctionConstantValue> &constants,const char *format) {
    namespace Capture=GPU::MetalShaderCapture;
    if(!device->_shaderCaptureEnabled || image.info.stage!=GPU::Shader::Fragment ||
        device->_shaderCaptureHashes.size()>=Capture::MaxFiles)return;
    const auto identity=Capture::programHash(image);
    const auto msaaIdentity=Capture::programHash(image,true);
    if(!identity || !device->_shaderCaptureHashes.insert({identity,msaaIdentity}).second)return;
    fprintf(stderr,"GPU_SHADER_CAPTURE_ATTEMPT pid=%d operation_hash=%llx msaa_operation_hash=%llx glsl_bytes=%zu words=%u framebuffer_mask=%x format=%s\n",
        getpid(),(unsigned long long)identity,(unsigned long long)msaaIdentity,entry.glsl.size(),image.info.codeWords,image.info.framebufferMask,format);
    if(entry.glsl.size()>256*1024)return;
    NSString *source=[[NSString alloc] initWithBytes:entry.glsl.data() length:entry.glsl.size() encoding:NSUTF8StringEncoding];
    if(!source)return;
    NSMutableArray *values=[NSMutableArray new];
    for(const auto &v:constants) {
        const unsigned bytes=GPU::Compiler::constantBytes(v.type);if(!bytes || bytes>v.bytes.size())return;
        [values addObject:@{@"index":@(v.index),@"type":@(v.type),@"name":[NSString stringWithUTF8String:v.name.c_str()]?:@"",
            @"data":[NSString stringWithUTF8String:Capture::hex(v.bytes.data(),bytes).c_str()]}];
    }
    NSDictionary *record=@{@"version":@1,@"pid":@(getpid()),@"name":[NSString stringWithUTF8String:entry.name.c_str()]?:@"",
        @"glsl":source,@"operation_hash":[NSString stringWithFormat:@"%llx",(unsigned long long)identity],
        @"msaa_operation_hash":[NSString stringWithFormat:@"%llx",(unsigned long long)msaaIdentity],
        @"registry":[NSString stringWithFormat:@"%llx",(unsigned long long)image.registry],@"jit_version":@(image.version),
        @"compiler_context":[NSString stringWithUTF8String:Capture::hex(GPUArchiveBuild::Context,sizeof(GPUArchiveBuild::Context)).c_str()],
        @"source_format":[NSString stringWithUTF8String:format],
        @"source_sha256":[NSString stringWithUTF8String:Capture::hex(function.hash.data(),function.hash.size()).c_str()],
        @"function_constants":values,
        @"reflection_base64":[[NSData dataWithBytes:&entry.reflection length:sizeof(entry.reflection)] base64EncodedStringWithOptions:0],
        @"program_image_base64":[[NSData dataWithBytes:&image length:sizeof(image)] base64EncodedStringWithOptions:0]};
    NSData *json=[NSJSONSerialization dataWithJSONObject:record options:0 error:nullptr];
    if(!json || json.length>Capture::MaxJSONBytes)return;
    const auto digest=archiveHash(json.bytes,json.length);const auto sha=Capture::hex(digest.data(),digest.size());
    const auto framed=Capture::frame(json.bytes,json.length,sha,identity);
    if(!device->_shaderCaptureBudget.admit(framed.size()))return;
    const auto context=Capture::hex(GPUArchiveBuild::Context,8);
    // Platform UI processes run as mobile inside their own sandbox. Use the
    // process temporary directory supplied by Foundation rather than assuming
    // the root diagnostic tool's global /private/var/tmp is writable there.
    NSString *directory=NSTemporaryDirectory();
    NSString *filename=[NSString stringWithFormat:@"qcom-native-shader-%s-%d-%llx-%llx.txt",context.c_str(),getpid(),(unsigned long long)identity,(unsigned long long)msaaIdentity];
    NSString *path=directory.length?[directory stringByAppendingPathComponent:filename]:nil;
    NSError *error=nil;
    const BOOL written=path && [[NSData dataWithBytes:framed.data() length:framed.size()] writeToFile:path options:NSDataWritingAtomic error:&error];
    fprintf(stderr,"GPU_SHADER_CAPTURE_FILE path=%s bytes=%zu json_bytes=%lu operation_hash=%llx sha256=%s status=%u error=%ld\n",
        path.UTF8String?:"unavailable",framed.size(),(unsigned long)json.length,(unsigned long long)identity,sha.c_str(),written?0:1,(long)error.code);
    if(!written) {
        // Daemon stderr need not be connected to the development UART. Retain
        // the real write error through the existing opt-in failure channel.
        NSString *message=[NSString stringWithFormat:@"Shader capture hash=%llx path=%@ failed: %@ (%@/%ld)",
            (unsigned long long)identity,path?:@"unavailable",error.localizedDescription?:@"No process temporary directory",error.domain?:@"Foundation",(long)error.code];
        [device reportMetalFailure:GPU::Diagnostics::Library error:[NSError errorWithDomain:@"QcomMTLShaderCapture" code:error?error.code:-1
            userInfo:@{NSLocalizedDescriptionKey:message}]];
    }
}
void captureArchivedNativeShader(QcomMTLDevice *device,QcomMTLFunction *function,NSData *air,
    const GPU::Compiler::LibraryArchive::Function &selected,const std::vector<GPU::Compiler::FunctionConstantValue> &constants) {
    if(!device->_shaderCaptureEnabled || !function ||
        ![function->_dynamicProgram isKindOfClass:[QcomMTLProgram class]])return;
    auto *program=(QcomMTLProgram *)function->_dynamicProgram;
    if(program->_image.length!=sizeof(QGPUProgramImage))return;
    const auto &image=*static_cast<const QGPUProgramImage *>(program->_image.bytes);
    if(image.info.stage!=GPU::Shader::Fragment)return;
    const auto identity=GPU::MetalShaderCapture::programHash(image);
    const auto identityPair=std::make_pair(identity,GPU::MetalShaderCapture::programHash(image,true));
    [device->_compilerLock lock];
    if(identity && device->_shaderCaptureHashes.size()<GPU::MetalShaderCapture::MaxFiles &&
        !device->_shaderCaptureHashes.count(identityPair) &&
        device->_shaderCaptureArchiveAttempts.size()<GPU::MetalShaderCapture::MaxFiles &&
        device->_shaderCaptureArchiveAttempts.insert(identityPair).second) {
        // The archive supplies the actual validated ISA. Reconstruct only its
        // source/reflection from the selected original AIR and resolved values;
        // do not compile, register or substitute a different GPU program.
        std::vector<GPU::Compiler::Entry> entries;std::string diagnostic;
        const bool ok=selected.offset<=air.length && selected.size<=air.length-selected.offset &&
            GPU::Compiler::lowerAIR(static_cast<const uint8_t *>(air.bytes)+selected.offset,selected.size,
                entries,diagnostic,selected.name,constants);
        if(ok && entries.size()==1 && entries[0].reflection.stage==image.info.stage)
            captureNativeShader(device,entries[0],image,selected,constants,"metallib-air-binary-archive");
        else [device reportMetalFailure:GPU::Diagnostics::Library error:shaderFailure(
            [NSString stringWithFormat:@"Archive shader capture hash=%llx source reconstruction failed: %s",
                (unsigned long long)identity,diagnostic.c_str()])];
    }
    [device->_compilerLock unlock];
}
QcomMTLLibrary *compileLibrary(QcomMTLDevice *, NSData *, BOOL, NSError **, NSData *ownedIdentity=nil);
// Only actual cache misses reaching JIT are recorded. Snapshot existing metadata
// under the normal library lock, then serialize/write on the utility queue.
// This never reconstructs GLSL, reruns LLVM, or reports a successful compile as
// a Metal/GPU failure. At most 64 unique requests are recorded per device.
void recordShaderCacheMiss(QcomMTLDevice *device,NSData *binaryKey,NSData *libraryIdentity,
    const GPU::Compiler::LibraryArchive::Function &selected,
    const std::vector<GPU::Compiler::FunctionConstantValue> &effective,NSUInteger maxThreads,
    const char *catalogState,const GPU::Compiler::AIRCompileInfo &info,bool compiledOK,uint64_t identity) {
    if((!device->_metalTimingEnabled && !device->_metalDiagnosticsEnabled) || binaryKey.length!=32)return;
    const auto key=archiveKey(binaryKey);
    [device->_metalDiagnosticLock lock];
    const bool record=device->_shaderCacheMissKeys.size()<64 && device->_shaderCacheMissKeys.insert(key).second;
    [device->_metalDiagnosticLock unlock];if(!record)return;
    const auto airHash=selected.hash,context=archiveContext();const auto name=selected.name;
    const auto constants=effective;const auto compileInfo=info;
    NSData *library=[libraryIdentity copy];const uint64_t ticks=mach_absolute_time();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
      @autoreleasepool {
        auto hex=[](const uint8_t *bytes,size_t size)->NSString * {
            const auto value=GPU::MetalShaderCapture::hex(bytes,size);
            return [NSString stringWithUTF8String:value.c_str()];
        };
        NSMutableArray *values=[NSMutableArray new];
        for(const auto &v:constants)
            [values addObject:@[@(v.index),@(v.type),hex(v.bytes.data(),GPU::Compiler::constantBytes(v.type))]];
        uint64_t argument=0;memcpy(&argument,key.data(),sizeof(argument));
        NSString *diagnostic=[[NSString alloc] initWithBytes:compileInfo.nirDiagnostic.data()
            length:compileInfo.nirDiagnostic.size() encoding:NSUTF8StringEncoding]?:@"<non-UTF8 diagnostic>";
        NSString *route=compileInfo.route==GPU::Compiler::AIRRoute::NIR?@"nir":
            compileInfo.route==GPU::Compiler::AIRRoute::GLSLFallback?@"glsl-fallback":@"glsl";
        NSDictionary *report=@{@"schema":@1,@"event":@"shader-cache-miss",@"pid":@(getpid()),@"ticks":@(ticks),
            @"identity":[NSString stringWithFormat:@"%016llx",(unsigned long long)identity],
            @"argument":[NSString stringWithFormat:@"%016llx",(unsigned long long)argument],
            @"key":hex(key.data(),key.size()),@"archive_context":hex(context.data(),context.size()),
            @"registry":[NSString stringWithFormat:@"%016llx",(unsigned long long)GPUPrograms::Registry],
            @"air_sha256":hex(airHash.data(),airHash.size()),@"air_entry":[NSString stringWithUTF8String:name.c_str()],
            @"library_sha256":hex(static_cast<const uint8_t *>(library.bytes),library.length),
            @"max_threads":@(maxThreads),@"effective_constants":values,
            @"catalog":[NSString stringWithUTF8String:catalogState],@"compiled_ok":@(compiledOK),
            @"route":route,@"nir_failure_cache_hit":@(compileInfo.nirFailureCacheHit),@"nir_diagnostic":diagnostic};
        NSError *error=nil;NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:&error];
        NSString *directory=NSTemporaryDirectory(),*path=directory.length?[directory stringByAppendingPathComponent:
            [NSString stringWithFormat:@"qcom-shader-miss-%d-%@.json",getpid(),hex(key.data(),key.size())]]:nil;
        BOOL written=json && path && [json writeToFile:path options:NSDataWritingAtomic error:&error];
        fprintf(stderr,"QCOM_SHADER_CACHE_MISS pid=%d argument=%016llx catalog=%s route=%s path=%s bytes=%lu written=%u error=%ld\n",
            getpid(),(unsigned long long)argument,catalogState,route.UTF8String,path.UTF8String?:"unavailable",
            (unsigned long)json.length,unsigned(written),(long)error.code);
      }
    });
}

bool libraryIndex(NSData *data,std::vector<GPU::Compiler::LibraryArchive::Function> &functions,std::string &error) {
    if(!GPU::Compiler::LibraryArchive::parse(data.bytes,data.length,functions,error))return false;
    if(functions.size()>GPU::JIT::MaxPrograms) {error="Metal library function count exceeds the driver quota";return false;}
    for(const auto &function:functions) {
        if(function.type>2 || ![[NSString alloc] initWithBytes:function.name.data() length:function.name.size() encoding:NSUTF8StringEncoding]) {
            error="External functions or invalid UTF8 Metal names are unsupported";return false;
        }
        unsigned char hash[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(static_cast<const uint8_t *>(data.bytes)+function.offset,(CC_LONG)function.size,hash);
        if(memcmp(hash,function.hash.data(),sizeof(hash))) {error="MTLB AIR SHA-256 mismatch";return false;}
    }
    return true;
}
QcomMTLLibrary *loadLibrary(QcomMTLDevice *device, NSData *data, NSError **error) {
    MetalTimingScope timing(device,GPU::MetalTiming::Library,1);
    NSError *__autoreleasing diagnosticError=nil;if(!error)error=&diagnosticError;*error=nil;
    MetalFailureScope failureScope(device,GPU::Diagnostics::Library,error);
    if(!data.length || data.length>GPU::Compiler::LibraryArchive::MaxBytes) {
        if(error)*error=shaderFailure(@"A nonempty Metal library of at most16 MiB is required");return nil;
    }
    // Own the bytes before hashing or retaining an archive from a mapped file.
    data=[NSData dataWithBytes:data.bytes length:data.length];
    NSData *expected = [NSData dataWithBytes:GPUPrograms::Library length:sizeof(GPUPrograms::Library)-1];
    if (![data isEqualToData:expected]) {
        const auto hash=archiveHash(data.bytes,data.length);NSData *identity=keyData(hash);
        const auto hex=GPU::MetalShaderCapture::hex(hash.data(),hash.size());
        NSString *fingerprint=[NSString stringWithUTF8String:hex.c_str()];
        for (NSDictionary *entry in registeredLibrary()[@"binaries"]) {
            if ([entry[@"bytes"] unsignedIntegerValue] == data.length && [entry[@"sha256"] isEqualToString:fingerprint])
                return [[QcomMTLLibrary alloc] initWithDevice:device names:entry[@"names"]];
        }
        if(GPU::Compiler::LibraryArchive::container(data.bytes,data.length)) {
            NSString *key=[@"archive:" stringByAppendingString:fingerprint];
            [device->_compilerLock lock];
            QcomMTLLibrary *library=[device->_libraryCache objectForKey:key];
            if(!library) {
                library=[[QcomMTLLibrary alloc] initWithDevice:device archive:data identity:identity error:error];
                if(library) {
                    if(device->_libraryCache.count>=GPU::JIT::MaxPrograms)[device->_libraryCache removeAllObjects];
                    [device->_libraryCache setObject:library forKey:key];
                }
            }
            [device->_compilerLock unlock];return library;
        }
        return compileLibrary(device,data,NO,error,identity);
    }
    NSMutableArray *names = [NSMutableArray new];
    for (NSDictionary *entry in registeredLibrary()[@"functions"]) [names addObject:entry[@"name"]];
    return [[QcomMTLLibrary alloc] initWithDevice:device names:names];
}

// Compile only unregistered input. The weak per-device cache shares a library
// while callers retain it without creating device/library ownership cycles.
QcomMTLLibrary *compileLibrary(QcomMTLDevice *device, NSData *data, BOOL msl, NSError **error, NSData *ownedIdentity) {
    MetalTimingScope timing(device,GPU::MetalTiming::Library,2);
    if (!device->_jit || !data.length || data.length > (msl ? 1024u*1024 : 16u*1024*1024)) {
        if (error) *error = shaderFailure(@"A compatible JIT backend and a bounded nonempty shader library are required"); return nil;
    }
    // loadLibrary already owns and hashed its immutable snapshot. Direct MSL
    // input takes its own snapshot here before deriving the same source key.
    if(!ownedIdentity) {
        data=[NSData dataWithBytes:data.bytes length:data.length];
        ownedIdentity=keyData(archiveHash(data.bytes,data.length));
    }
    NSString *key=[NSString stringWithFormat:@"%d:%@",msl,[ownedIdentity base64EncodedStringWithOptions:0]];
    [device->_compilerLock lock];
    QcomMTLLibrary *library = [device->_libraryCache objectForKey:key];
    if (!library) {
        std::vector<GPU::Compiler::Entry> entries; std::string diagnostic;
        std::vector<QGPUProgramImage> images;
        GPU::Compiler::AIRCompileInfo compileInfo;bool ok;
        if(msl)ok=GPU::Compiler::lowerMSL(std::string((const char *)data.bytes,data.length),entries,diagnostic) &&
            GPU::Compiler::compile(entries,GPUPrograms::Registry,images,diagnostic);
        else {
            ok=GPU::Compiler::compileAIR(data.bytes,data.length,GPUPrograms::Registry,images,diagnostic,{}, {},&compileInfo);
            if(compileInfo.route==GPU::Compiler::AIRRoute::NIR)timing.record.detail|=128;
            else if(compileInfo.route==GPU::Compiler::AIRRoute::GLSLFallback)timing.record.detail|=256;
            // GLSL reconstruction is only for the existing opt-in capture.
            if(ok && device->_shaderCaptureEnabled){std::string captureError;
                (void)GPU::Compiler::lowerAIR(data.bytes,data.length,entries,captureError);}
        }
        if(ok && entries.size()==images.size()) {
            GPU::Compiler::LibraryArchive::Function origin{};origin.hash=archiveKey(ownedIdentity);origin.size=data.length;
            const std::vector<GPU::Compiler::FunctionConstantValue> constants;
            for(size_t i=0;i<images.size();++i)captureNativeShader(device,entries[i],images[i],origin,constants,msl?"msl":compileInfo.route==GPU::Compiler::AIRRoute::NIR?"air-nir":"air");
        }
        if (ok) library = [[QcomMTLLibrary alloc] initWithDevice:device images:images error:error];
        else if (error) *error = shaderFailure([NSString stringWithUTF8String:diagnostic.c_str()] ?: @"Shader compilation failed");
        if (library) {
            if (device->_libraryCache.count >= GPU::JIT::MaxPrograms) [device->_libraryCache removeAllObjects];
            [device->_libraryCache setObject:library forKey:key];
        }
    }
    [device->_compilerLock unlock]; return library;
}
}

@implementation QcomMTLDevice
- (void)reportMetalTiming:(const GPU::MetalTiming::Record &)record {
    if(!_metalTimingEnabled)return;const auto saved=record;
    dispatch_async(_metalTimingQueue,^{
        [self->_transportLock lock];(void)self->_connection.reportMetalTiming(saved);[self->_transportLock unlock];
    });
}
- (void)reportMetalFailure:(GPU::Diagnostics::MetalStage)stage error:(NSError *)error {
    if(!_metalDiagnosticsEnabled || !error)return;
    GPU::Diagnostics::MetalFailure report;report.stage=stage;report.code=uint32_t(error.code);
    NSData *text=[error.localizedDescription dataUsingEncoding:NSUTF8StringEncoding];
    auto *bytes=static_cast<const unsigned char *>(text.bytes);
    size_t count=MIN(text.length,sizeof(report.message)-1);
    if(count<text.length)while(count && (bytes[count]&0xc0)==0x80)--count;
    if(!count)return;memcpy(report.message,bytes,count);
    for(auto &c:report.message)if(c && (uint8_t(c)<32 || uint8_t(c)==127))c=' ';
    NSData *data=[NSData dataWithBytes:&report length:sizeof(report)];
    [_metalDiagnosticLock lock];
    BOOL send=_metalFailureKeys.count<GPU::Diagnostics::MaxMetalFailures && ![_metalFailureKeys containsObject:data];
    if(send)[_metalFailureKeys addObject:data];[_metalDiagnosticLock unlock];
    if(!send)return;
    // The kernel history has a small, fixed-size message. Preserve longer
    // compiler failures (including the exact resolved constants) in the
    // process's sandbox-approved temporary directory, only in diagnostics
    // mode and under the same bounded per-device failure quota.
    if(text.length>sizeof(report.message)-1 && text.length<=64*1024) {
        const auto digest=archiveHash(text.bytes,text.length);
        const auto hex=GPU::MetalShaderCapture::hex(digest.data(),digest.size());
        NSString *directory=NSTemporaryDirectory();
        NSString *path=directory.length?[directory stringByAppendingPathComponent:
            [NSString stringWithFormat:@"qcom-metal-failure-%d-%s.txt",getpid(),hex.c_str()]]:nil;
        NSError *writeError=nil;
        if(path && [text writeToFile:path options:NSDataWritingAtomic error:&writeError]) {
            // Keep the failure itself first, then publish the checked file's
            // identity. The original complete text is never replaced by a
            // truncated "reproduction" or guessed constants.
            NSString *location=[NSString stringWithFormat:@" FULL_ERROR path=%@ bytes=%lu sha256=%s",
                path,(unsigned long)text.length,hex.c_str()];
            NSData *suffix=[location dataUsingEncoding:NSUTF8StringEncoding];
            if(suffix.length<sizeof(report.message)/2) {
                size_t prefix=MIN(count,sizeof(report.message)-1-suffix.length);
                while(prefix && (bytes[prefix]&0xc0)==0x80)--prefix;
                memset(report.message,0,sizeof(report.message));
                memcpy(report.message,bytes,prefix);
                memcpy(report.message+prefix,suffix.bytes,suffix.length);
                for(auto &c:report.message)if(c && (uint8_t(c)<32 || uint8_t(c)==127))c=' ';
            }
        }
    }
    // Deliver the failure before a Swift caller can trap on a nil result.
    // IOKit supports concurrent calls; this metadata-only gated selector does
    // not order/submit GPU work and must not acquire the job transport lock
    // (some failures are reported while that lock is already held).
    (void)_connection.reportMetalFailure(report);
}
+ (Protocol *)metalProtocol { return @protocol(MTLDevice); }
- (BOOL)qcomMetalDiagnosticsEnabled {return _metalDiagnosticsEnabled;}
- (BOOL)qcomRenderClearEnabled {return __atomic_load_n(&_renderClearEnabled,__ATOMIC_RELAXED);}
- (BOOL)qcomSetRenderClearEnabled:(BOOL)enabled {
    if(!_metalDiagnosticsEnabled)return NO;
    __atomic_store_n(&_renderClearEnabled,enabled,__ATOMIC_RELAXED);return YES;
}
- (BOOL)qcomSetGMEMEnabled:(BOOL)enabled {
    if(!_metalDiagnosticsEnabled || !_gmemSupported)return NO;
    __atomic_store_n(&_gmem,enabled,__ATOMIC_RELAXED);return YES;
}
- (uint64_t)qcomGMEMPasses {return __atomic_load_n(&_gmemPasses,__ATOMIC_RELAXED);}
- (instancetype)init { return [self initWithService:0 error:nullptr]; }
- (instancetype)initWithAcceleratorPort:(uint32_t)service {
    // Metal owns its descriptor's service reference. Keep an independent one.
    NSError *error=nil;
    auto device=[self initWithService:service error:&error];
    if(!device)NSLog(@"QcomMTL: device initialization failed pid=%d service=%u error=%@",getpid(),service,error);
    return device;
}
- (instancetype)initWithService:(uint32_t)service error:(NSError **)error {
    // Apple's dealloc releases the queues created by its init. Initialize
    // that state even on our failure paths, including a plain -init call.
    if (!(self = [super init])) return nil;
    NSString *abiDiagnostic = nil;
    if (!nativeMetalABI(&abiDiagnostic)) {
        if (error) *error = failure(abiDiagnostic ?: @"Unsupported native Metal.framework ABI");
        return nil;
    }
    _transportLock = [NSLock new];_dependencyLock=[NSLock new];
    _nativeWaiters=[NSHashTable weakObjectsHashTable];_nativeQueues=[NSHashTable weakObjectsHashTable];
    _compilerLock = [NSLock new]; _libraryCache = [NSMapTable strongToWeakObjectsMapTable];
    if (!IOObjectConformsTo(service, "QcomGPU")) {
        if (error) *error = failure(@"The service is not a QcomGPU accelerator"); return nil;
    }
    auto retained = IOObjectRetain(service);
    if (retained) { if (error) *error = transportError(retained); return nil; }
    _acceleratorService = service;
    auto status = IORegistryEntryGetRegistryEntryID(service, &_registryID);
    const char *stage="registry-entry-id";
    if (!status) {stage="IOServiceOpen type=1";status = _connection.open(service);}
    GPU::MetalCapabilities::Snapshot caps;
    if (!status) {stage="native Metal capabilities";status=_connection.metalCapabilities(caps);}
    if(status) {
        if(error)*error=failure([NSString stringWithFormat:@"%s failed: IOReturn=0x%08x (%@)",stage,status,transportError(status).localizedDescription]);
        return nil;
    }
    const uint32_t required=GPU::MetalCapabilities::Ready|GPU::MetalCapabilities::AddressSpaces|GPU::MetalCapabilities::ImmutableBlit;
    if(caps.version!=GPU::MetalCapabilities::Version || caps.bytes!=sizeof(caps) ||
        caps.command!=GPU::Native::CommandVersion || caps.completion!=GPU::Native::CompletionVersion ||
        caps.shader!=GPU::Shader::Version || caps.blit!=GPU::Blit::NativeLayoutVersion ||
        caps.maxJobs!=GPU::MaxJobs || caps.backpressure!=GPU::Native::AdmissionBackpressureVersion ||
        caps.registry!=GPUPrograms::Registry || (caps.flags&required)!=required || !GPU::Native::Memory::validLimits(caps.memory)) {
        if(error)*error=failure([NSString stringWithFormat:@"A540 capability mismatch: version=%u bytes=%u command=%u completion=%u shader=%u blit=%u jobs=%u admission=%u registry=%016llx flags=%x aperture=%llu",
            caps.version,caps.bytes,caps.command,caps.completion,caps.shader,caps.blit,caps.maxJobs,caps.backpressure,
            (unsigned long long)caps.registry,caps.flags,(unsigned long long)caps.memory.apertureBytes]);
        return nil;
    }
    _memoryLimits=caps.memory;
    _jit=caps.jit==GPU::JIT::Version && caps.maxShmem>=GPU::JIT::UploadChunkBytes;
    _batchShaders=caps.batch==GPU::Native::BatchCommandVersion;
    _mixedBatchShaders=_batchShaders && (caps.flags&GPU::MetalCapabilities::MixedBatch);
    _gmemSupported=caps.flags&GPU::MetalCapabilities::GMEM;
    // Tiled replay is correct, but without VSC binning the measured Hello
    // pass costs more than BYPASS. Keep GMEM available for diagnostics until
    // the geometry replay cost is addressed.
    _gmem=NO;
    _pipelinedAdmission=caps.flags&GPU::MetalCapabilities::Pipeline;
    _completionNotifications=caps.flags&GPU::MetalCapabilities::Notifications;
    _surfaceFences=caps.flags&GPU::MetalCapabilities::SurfaceFences;
    _metalDiagnosticsEnabled=caps.flags&GPU::MetalCapabilities::Diagnostics;
    // A540 sysmem draws are slower than the vector compute clear. Keep the
    // draw path available to the diagnostic comparison only.
    _renderClearEnabled=NO;
    _shaderCaptureEnabled=_metalDiagnosticsEnabled && (caps.flags&GPU::MetalCapabilities::ShaderCapture);
    _metalTimingEnabled=_metalDiagnosticsEnabled && (caps.flags&GPU::MetalCapabilities::Timing);
    _metalDiagnosticLock=[NSLock new];_metalFailureKeys=[NSMutableSet new];
    if(_metalTimingEnabled)_metalTimingQueue=dispatch_queue_create("org.darwin.QcomMTL.timing",DISPATCH_QUEUE_SERIAL);
    NSLog(@"QcomMTL: device ready pid=%d registry=%016llx capabilities=%x",getpid(),(unsigned long long)_registryID,caps.flags);

    [self initLimits];
    [self initFeatureQueries];
    return self;
}
- (void)dealloc {
    // Cached IDs still contribute to currentAllocatedSize. They have no ObjC
    // owner cycle, and must be released before closing their native device.
    while(_inlinePool.count()) {
        const auto resource=_inlinePool.take();
        _connection.deleteResource(resource.id);_allocated-=resource.bytes;
    }
    _connection.close();
    if (_acceleratorService) IOObjectRelease(_acceleratorService);
}
- (uint32_t)acceleratorPort { return _acceleratorService; }
- (NSUInteger)featureProfile {
    // Audited default branch: empty GPU-family list, non-AGX feature queries.
    // A540 does not implement a complete Apple feature profile.
    return NSUIntegerMax;
}
- (void)initLimits {
    // Apple limit getters dispatch through -limits. Keep our structure in
    // subclass-owned storage instead of writing the superclass's private ABI.
    QcomMTLDeviceLimits value{};
    value.maxFramebufferStorageBits = 32*QGPU_COLOR_TARGET_COUNT;
    value.maxColorAttachments = QGPU_COLOR_TARGET_COUNT;
    value.maxVertexAttributes = QGPU_MAX_ATTRIBUTES;
    value.maxVertexBuffers = value.maxFragmentBuffers = value.maxComputeBuffers = GPU::Shader::MaxBindings;
    value.maxVertexTextures=QGPU_METAL_TEXTURE_BINDING_COUNT;value.maxVertexSamplers=QGPU_STAGE_SAMPLER_COUNT;
    value.maxVertexInlineDataSize=value.maxFragmentInlineDataSize=value.maxComputeInlineDataSize=4096;
    value.maxConstantBufferArguments=8;
    value.maxFunctionConstantIndices=nativeFunctionConstantABI()?65536:0;
    value.maxFragmentTextures=QGPU_METAL_TEXTURE_BINDING_COUNT;value.maxFragmentSamplers=QGPU_STAGE_SAMPLER_COUNT;
    value.maxComputeTextures=QGPU_COMPUTE_IMAGE_COUNT;
    value.maxComputeSamplers=QGPU_COMPUTE_IMAGE_COUNT;
    value.maxInterpolants = value.maxInterpolatedComponents = QGPU_MAX_VARYINGS;
    value.maxTotalComputeThreadsPerThreadgroup = GPU::Shader::MaxComputeThreads;
    value.maxComputeThreadgroupMemory = QGPU_MAX_THREADGROUP_BYTES;
    value.maxComputeLocalMemorySizes = GPU::Shader::MaxBindings;
    value.maxComputeThreadgroupMemoryAlignmentBytes = 16;
    value.maxLineWidth = value.maxPointSize = 1;
    value.minConstantBufferAlignmentBytes = 4;
    value.maxTextureWidth2D = value.maxTextureHeight2D = GPU::Surface::MaxDimension;
    value.maxTextureLayers=2048;value.maxTextureDimensionCube=GPU::Surface::MaxDimension;
    value.linearTextureAlignmentBytes = value.deviceLinearTextureAlignmentBytes = 64;
    value.deviceLinearReadOnlyTextureAlignmentBytes = 64;
    value.iosurfaceTextureAlignmentBytes = value.iosurfaceReadOnlyTextureAlignmentBytes = 64;
    value.maxViewportCount = 1;
    value.maxBufferLength = _memoryLimits.resourceBytes;
    _nativeLimits = value;
}
- (const QcomMTLDeviceLimits *)limits { return &_nativeLimits; }
- (void)initFeatureQueries {
    // Explicit factories and MTLAddDevice share the same initialization.
    if (!_featureQueriesInitialized) {
        [super initFeatureQueries];
        _featureQueriesInitialized = YES;
    }
}
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector {
    NSMethodSignature *signature = [super methodSignatureForSelector:selector];
    const char *types = signature ? nullptr : protocolTypes(@protocol(MTLDevice), selector);
    return signature ?: (types ? [NSMethodSignature signatureWithObjCTypes:types] : nil);
}
- (NSString *)name { return @"Adreno 540"; }
- (MTLArchitecture *)architecture { return [QcomMTLArchitecture new]; }
- (uint64_t)registryID { return _registryID; }
- (BOOL)hasUnifiedMemory { return YES; }
- (uint64_t)recommendedMaxWorkingSetSize { return _memoryLimits.clientBytes; }
- (NSUInteger)sparseTileSizeInBytes { return 0; }
#define QCOM_FALSE_QUERY(name) - (BOOL)name { return NO; }
QCOM_FALSE_QUERY(isLowPower)
QCOM_FALSE_QUERY(isHeadless)
QCOM_FALSE_QUERY(isRemovable)
QCOM_FALSE_QUERY(isDepth24Stencil8PixelFormatSupported)
QCOM_FALSE_QUERY(areRasterOrderGroupsSupported)
QCOM_FALSE_QUERY(supports32BitFloatFiltering)
QCOM_FALSE_QUERY(supports32BitMSAA)
QCOM_FALSE_QUERY(supportsQueryTextureLOD)
QCOM_FALSE_QUERY(supportsBCTextureCompression)
QCOM_FALSE_QUERY(supportsPullModelInterpolation)
QCOM_FALSE_QUERY(areBarycentricCoordsSupported)
QCOM_FALSE_QUERY(supportsShaderBarycentricCoordinates)
QCOM_FALSE_QUERY(areProgrammableSamplePositionsSupported)
QCOM_FALSE_QUERY(supportsDynamicLibraries)
QCOM_FALSE_QUERY(supportsRenderDynamicLibraries)
QCOM_FALSE_QUERY(supportsRaytracing)
QCOM_FALSE_QUERY(supportsFunctionPointers)
QCOM_FALSE_QUERY(supportsFunctionPointersFromRender)
QCOM_FALSE_QUERY(supportsRaytracingFromRender)
QCOM_FALSE_QUERY(supportsPrimitiveMotionBlur)
#undef QCOM_FALSE_QUERY
@dynamic argumentBuffersSupport; // No tier is valid until argument encoding exists.
- (NSUInteger)maxBufferLength { return _memoryLimits.resourceBytes; }
- (NSUInteger)currentAllocatedSize {
    [_transportLock lock]; NSUInteger bytes = _allocated; [_transportLock unlock]; return bytes;
}
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(GPU::Shader::MaxComputeThreads, GPU::Shader::MaxComputeThreads, 64); }
- (NSUInteger)maxThreadgroupMemoryLength { return QGPU_MAX_THREADGROUP_BYTES; }
- (NSUInteger)maxArgumentBufferSamplerCount { return 0; }
- (MTLReadWriteTextureTier)readWriteTextureSupport { return MTLReadWriteTextureTierNone; }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set { (void)set; return NO; }
#pragma clang diagnostic pop

- (BOOL)supportsFamily:(MTLGPUFamily)family { (void)family; return NO; }
- (BOOL)supportsTextureSampleCount:(NSUInteger)count { return count==1 || count==2 || count==4; }
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format {
    auto alignment=GPU::MetalTexture::linearTextureAlignment(nativeColorFormat(format));
    if(!alignment)[NSException raise:NSInvalidArgumentException format:@"Unsupported linear texture pixel format"];
    return alignment;
}
- (void)getDefaultSamplePositions:(MTLSamplePosition *)positions count:(NSUInteger)count {
    if(!positions || (count!=1 && count!=2 && count!=4))[NSException raise:NSInvalidArgumentException format:@"Unsupported sample position count"];
    static const MTLSamplePosition one[]={{0.5f,0.5f}},two[]={{0.75f,0.75f},{0.25f,0.25f}},four[]={{0.375f,0.125f},{0.875f,0.375f},{0.125f,0.625f},{0.625f,0.875f}};
    memcpy(positions,count==4?four:count==2?two:one,count*sizeof(*positions));
}
- (NSArray *)counterSets { return @[]; }
- (void)forwardInvocation:(NSInvocation *)invocation {
    // New SDK capability booleans are false until their implementation exists.
    NSString *selector = NSStringFromSelector(invocation.selector);
    const char *type = invocation.methodSignature.methodReturnType;
    if (([selector hasPrefix:@"supports"] || [selector hasSuffix:@"Supported"]) &&
        (!strcmp(type, @encode(BOOL)))) { clearReturn(invocation); return; }
    reportUnsupportedMetalCall(self,invocation.selector);
    [super forwardInvocation:invocation];
}
- (void)doesNotRecognizeSelector:(SEL)selector {
    reportUnsupportedMetalCall(self,selector);[super doesNotRecognizeSelector:selector];
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    auto buffer=[[QcomMTLBuffer alloc] initWithDevice:self length:length options:options];
    if(!buffer)[self reportMetalFailure:GPU::Diagnostics::Resource error:failure(
        [NSString stringWithFormat:@"Buffer allocation rejected: bytes=%lu options=%lu",(unsigned long)length,(unsigned long)options],MTLCommandBufferErrorInvalidResource)];
    return buffer;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
    if (!bytes) return nil;
    QcomMTLBuffer *buffer = (QcomMTLBuffer *)[self newBufferWithLength:length options:options];
    if (buffer) { memcpy((void *)(uintptr_t)buffer->_resource.cpuAddress, bytes, length); publishCPU(); }
    return buffer;
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options
    deallocator:(void (^)(void *, NSUInteger))deallocator {
    auto buffer=[[QcomMTLBuffer alloc] initWithDevice:self bytesNoCopy:bytes length:length options:options deallocator:deallocator];
    if(!buffer)[self reportMetalFailure:GPU::Diagnostics::Resource error:failure(
        [NSString stringWithFormat:@"Client buffer import rejected: bytes=%lu options=%lu",(unsigned long)length,(unsigned long)options],MTLCommandBufferErrorInvalidResource)];
    return buffer;
}
- (id<MTLCommandQueue>)newCommandQueue { return [self newCommandQueueWithMaxCommandBufferCount:16]; }
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count {
    if (!count || count > 256) return nil;
    return [[QcomMTLCommandQueue alloc] initWithDevice:self capacity:count];
}
- (id<MTLCommandQueue>)newCommandQueueWithDescriptor:(MTLCommandQueueDescriptor *)descriptor {
    return descriptor.logState ? nil : [self newCommandQueueWithMaxCommandBufferCount:descriptor.maxCommandBufferCount];
}
- (id<MTLEvent>)newEvent { return [[QcomMTLEvent alloc] initWithDevice:self]; }
- (id<MTLEvent>)newEventWithOptions:(NSUInteger)options { return options ? nil : [self newEvent]; }
- (BOOL)supportsEvents { return YES; }
// The real superclass supplies _MTLSharedEvent / IOSurfaceSharedEvent,
// including its mach-port handles and cross-process notifications. Commands
// below signal that native object only after preceding A540 work retires.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {
    auto texture=[[QcomMTLTexture alloc] initWithDevice:self descriptor:descriptor];
    if(!texture)[self reportMetalFailure:GPU::Diagnostics::Resource error:failure(
        [NSString stringWithFormat:@"Texture rejected: type=%lu format=%lu size=%lux%lux%lu levels=%lu samples=%lu usage=%lu storage=%lu",
            (unsigned long)descriptor.textureType,(unsigned long)descriptor.pixelFormat,(unsigned long)descriptor.width,
            (unsigned long)descriptor.height,(unsigned long)descriptor.depth,(unsigned long)descriptor.mipmapLevelCount,
            (unsigned long)descriptor.sampleCount,(unsigned long)descriptor.usage,(unsigned long)descriptor.storageMode],MTLCommandBufferErrorInvalidResource)];
    return texture;
}
- (id<QcomNativeTextureLayout>)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor
    isHeapOrBufferBacked:(BOOL)heapOrBufferBacked {
    auto layout=[[QcomMTLTextureLayout alloc] initWithDevice:self descriptor:descriptor
        isHeapOrBufferBacked:heapOrBufferBacked];
    if(!layout)[self reportMetalFailure:GPU::Diagnostics::Resource error:failure(
        [NSString stringWithFormat:@"Texture layout rejected: type=%lu format=%lu size=%lux%lux%lu levels=%lu samples=%lu storage=%lu heapOrBuffer=%d",
            (unsigned long)descriptor.textureType,(unsigned long)descriptor.pixelFormat,(unsigned long)descriptor.width,
            (unsigned long)descriptor.height,(unsigned long)descriptor.depth,(unsigned long)descriptor.mipmapLevelCount,
            (unsigned long)descriptor.sampleCount,(unsigned long)descriptor.storageMode,heapOrBufferBacked],MTLCommandBufferErrorInvalidResource)];
    return layout;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane {
    auto texture=[[QcomMTLTexture alloc] initWithDevice:self descriptor:descriptor surface:surface plane:plane];
    if(!texture)[self reportMetalFailure:GPU::Diagnostics::Resource error:failure(
        [NSString stringWithFormat:@"IOSurface texture rejected: format=%lu size=%lux%lu usage=%lu plane=%lu",
            (unsigned long)descriptor.pixelFormat,(unsigned long)descriptor.width,(unsigned long)descriptor.height,
            (unsigned long)descriptor.usage,(unsigned long)plane],MTLCommandBufferErrorInvalidResource)];
    return texture;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor iosurface:(IOSurfaceRef)surface
    plane:(NSUInteger)plane slice:(NSUInteger)slice {
    return slice ? nil : [self newTextureWithDescriptor:descriptor iosurface:surface plane:plane];
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor {
    return [[QcomMTLSampler alloc] initWithDevice:self descriptor:descriptor];
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor {
    return [[QcomMTLDepthStencilState alloc] initWithDevice:self descriptor:descriptor];
}
- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)descriptor {
    return [[QcomMTLHeap alloc] initWithDevice:self descriptor:descriptor];
}
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    uint64_t bytes=0;MTLResourceOptions normalized;
    if(!resourceOptions(options,MTLHazardTrackingModeUntracked,normalized) ||
        !GPU::MetalHeap::bufferSize(length,bytes) || bytes>self.maxBufferLength)return {0,0};
    return {NSUInteger(bytes),NSUInteger(GPU::MetalHeap::Alignment)};
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)descriptor {
    auto texture=[[QcomMTLTexture alloc] initLayoutWithDescriptor:descriptor];uint64_t bytes=0;
    if(!texture || !GPU::MetalHeap::bufferSize(texture->_storageBytes,bytes) || bytes>self.maxBufferLength)return {0,0};
    return {NSUInteger(bytes),NSUInteger(GPU::MetalHeap::Alignment)};
}
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error {
    if(error)*error=nil;return [[QcomMTLBinaryArchive alloc] initWithDevice:self descriptor:descriptor error:error];
}
- (id<MTLFence>)newFence { return [[QcomMTLFence alloc] initWithDevice:self]; }
- (id<MTLLibrary>)newDefaultLibrary {
    return [self newDefaultLibraryWithBundle:NSBundle.mainBundle error:nullptr];
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {
    if(error)*error=nil;
    NSString *path = [bundle pathForResource:@"default" ofType:@"metallib"];
    if (!path) {
        if(error)*error=[NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorFileNotFound
            userInfo:@{NSLocalizedDescriptionKey:@"no default library was found"}];
        return nil;
    }
    return [self newLibraryWithURL:[NSURL fileURLWithPath:path] error:error];
}
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error {
    if (options.preprocessorMacros.count || (options && options.mathMode != MTLMathModeFast) ||
        options.mathFloatingPointFunctions != MTLMathFloatingPointFunctionsFast || options.preserveInvariance ||
        options.libraryType != MTLLibraryTypeExecutable || options.libraries.count || options.installName.length ||
        options.optimizationLevel != MTLLibraryOptimizationLevelDefault || options.compileSymbolVisibility != MTLCompileSymbolVisibilityDefault ||
        options.allowReferencingUndefinedSymbols || options.enableLogging ||
        options.maxTotalThreadsPerThreadgroup > GPU::Shader::MaxComputeThreads || options.languageVersion > MTLLanguageVersion3_2) {
        if (error) *error = shaderFailure(@"Unsupported A540 compile options; use default fast math and executable-library settings without macros or linked libraries"); return nil;
    }
    for (NSDictionary *entry in registeredLibrary()[@"sources"]) if ([entry[@"source"] isEqualToString:source])
        return [[[QcomMTLLibrary alloc] initWithDevice:self names:entry[@"names"]] withMaxThreads:options.maxTotalThreadsPerThreadgroup];
    return [compileLibrary(self,[source dataUsingEncoding:NSUTF8StringEncoding],YES,error) withMaxThreads:options.maxTotalThreadsPerThreadgroup];
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
    if (!data || dispatch_data_get_size(data) > 16*1024*1024) { if (error) *error = shaderFailure(@"Invalid A540 library size"); return nil; }
    const void *bytes = nullptr; size_t length = 0;
    dispatch_data_t mapping = dispatch_data_create_map(data, &bytes, &length);
    if (!mapping) return nil;
    return loadLibrary(self, [NSData dataWithBytes:bytes length:length], error);
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error {
    NSNumber *size = nil;
    NSError *readError = nil;
    if (!url.isFileURL || ![url getResourceValue:&size forKey:NSURLFileSizeKey error:&readError] ||
        !size || size.unsignedLongLongValue > 16*1024*1024) {
        if (error) *error = readError ?: shaderFailure(@"A local AIR/metallib file of at most 16 MiB is required");
        return nil;
    }
    return loadLibrary(self, [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error], error);
}
- (void)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options
    completionHandler:(MTLNewLibraryCompletionHandler)handler {
    if (!handler) return;
    source = [source copy]; options = [options copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        NSError *error = nil; id<MTLLibrary> library = [self newLibraryWithSource:source options:options error:&error];
        handler(library, error);
    });
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    return [self newRenderPipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone reflection:nullptr error:error];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
    options:(MTLPipelineOption)options reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error {
    NSError *__autoreleasing diagnosticError=nil;if(!error)error=&diagnosticError;*error=nil;
    MetalTimingScope timing(self,GPU::MetalTiming::RenderPipeline);
    MetalFailureScope failureScope(self,GPU::Diagnostics::RenderPipeline,error);
    if(reflection)*reflection=nil;
    if(!pipelineOptions(options,error))return nil;
    if(![descriptor isKindOfClass:[MTLRenderPipelineDescriptor class]]) {
        if(error)*error=shaderFailure(@"A native render pipeline descriptor is required");return nil;
    }
    descriptor=[descriptor copy];
    QcomMTLRenderPipeline *pipeline=[[QcomMTLRenderPipeline alloc] initWithDevice:self descriptor:descriptor];
    if(!pipeline){*error=shaderFailure([NSString stringWithFormat:@"Unsupported render pipeline: vertex=%@ fragment=%@ color=%lu samples=%lu depth=%lu stencil=%lu",
        descriptor.vertexFunction.name,descriptor.fragmentFunction.name,(unsigned long)descriptor.colorAttachments[0].pixelFormat,
        (unsigned long)descriptor.rasterSampleCount,(unsigned long)descriptor.depthAttachmentPixelFormat,
        (unsigned long)descriptor.stencilAttachmentPixelFormat]);return nil;}
    if(!pipelineArchiveLookup(descriptor.binaryArchives,self,pipeline,options,error))return nil;return pipeline;
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
    completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler {
    if(!handler)return;descriptor=[descriptor copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{@autoreleasepool {
        NSError *error=nil;id<MTLRenderPipelineState> pipeline=[self newRenderPipelineStateWithDescriptor:descriptor error:&error];
        handler(pipeline,error);
    }});
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options
    completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler {
    if(!handler)return;descriptor=[descriptor copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{@autoreleasepool {
        NSError *error=nil;MTLRenderPipelineReflection *reflection=nil;
        id<MTLRenderPipelineState> pipeline=[self newRenderPipelineStateWithDescriptor:descriptor options:options reflection:&reflection error:&error];
        handler(pipeline,reflection,error);
    }});
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error {
    return [self newComputePipelineStateWithFunction:function options:MTLPipelineOptionNone reflection:nullptr error:error];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
    reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
    if (!function) {
        if (reflection) *reflection=nil;
        if (error) *error=shaderFailure(@"A nonnil compute function is required");
        return nil;
    }
    MTLComputePipelineDescriptor *descriptor=[MTLComputePipelineDescriptor new];descriptor.computeFunction=function;
    return [self newComputePipelineStateWithDescriptor:descriptor options:options reflection:reflection error:error];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor options:(MTLPipelineOption)options
    reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
    NSError *__autoreleasing diagnosticError=nil;if(!error)error=&diagnosticError;*error=nil;
    MetalTimingScope timing(self,GPU::MetalTiming::ComputePipeline);
    MetalFailureScope failureScope(self,GPU::Diagnostics::ComputePipeline,error);
    if(reflection)*reflection=nil;
    if(!pipelineOptions(options,error))return nil;
    if(![descriptor isKindOfClass:[MTLComputePipelineDescriptor class]]) {
        if(error)*error=shaderFailure(@"A native compute pipeline descriptor is required");return nil;
    }
    descriptor=[descriptor copy];id<MTLFunction> function=descriptor.computeFunction;
    QcomMTLFunction *native=[function isKindOfClass:[QcomMTLFunction class]]?(QcomMTLFunction *)function:nil;
    if(!native || !native->_program || native->_owner!=self || native->_program->stage!=GPU::Shader::Compute) {
        if(error)*error=shaderFailure(@"Expected a registered A540 kernel function from this device");return nil;
    }
    // The native getter lazily constructs an empty descriptor. Compare its
    // actual state using Apple's implementation, not its object presence.
    auto stageInput=descriptor.stageInputDescriptor;
    if((stageInput && ![stageInput isEqual:[MTLStageInputOutputDescriptor stageInputOutputDescriptor]]) ||
        descriptor.supportIndirectCommandBuffers || descriptor.supportAddingBinaryFunctions ||
        descriptor.preloadedLibraries.count || !emptyLinkedFunctions(descriptor.linkedFunctions) || descriptor.maxCallStackDepth>1 ||
        descriptor.shaderValidation==MTLShaderValidationEnabled || descriptor.maxTotalThreadsPerThreadgroup>GPU::Shader::MaxComputeThreads) {
        if(error)*error=shaderFailure(@"Unsupported A540 compute pipeline descriptor features",MTLLibraryErrorUnsupported);return nil;
    }
    NSUInteger limit=descriptor.maxTotalThreadsPerThreadgroup?:native->_maxThreads;
    if((!(native->_program->flags&QGPU_VARIABLE_LOCAL_SIZE) && limit<64) ||
        (descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth && MIN(limit,native->_maxThreads)<native->_program->subgroupSize)) {
        if(error)*error=shaderFailure(@"Compute pipeline thread limit cannot execute this kernel");return nil;
    }
    QcomMTLComputePipeline *pipeline=[[QcomMTLComputePipeline alloc] initWithFunction:native];
    if(!pipeline) {if(error)*error=shaderFailure(@"Could not allocate a compute pipeline",MTLLibraryErrorInternal);return nil;}
    pipeline->_maxThreads=MIN(limit,native->_maxThreads);pipeline->_threadgroupMultiple=descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth;
    pipeline.label=descriptor.label;
    if(!pipelineArchiveLookup(descriptor.binaryArchives,self,pipeline,options,error))return nil;return pipeline;
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler {
    if(!handler)return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{@autoreleasepool {
        NSError *error=nil;id<MTLComputePipelineState> pipeline=[self newComputePipelineStateWithFunction:function error:&error];handler(pipeline,error);
    }});
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
    completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
    if(!handler)return;MTLComputePipelineDescriptor *descriptor=[MTLComputePipelineDescriptor new];descriptor.computeFunction=function;
    [self newComputePipelineStateWithDescriptor:descriptor options:options completionHandler:handler];
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor options:(MTLPipelineOption)options
    completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
    if(!handler)return;descriptor=[descriptor copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{@autoreleasepool {
        NSError *error=nil;MTLComputePipelineReflection *reflection=nil;
        id<MTLComputePipelineState> pipeline=[self newComputePipelineStateWithDescriptor:descriptor options:options reflection:&reflection error:&error];
        handler(pipeline,reflection,error);
    }});
}
@end

@implementation QcomMTLFencePoint
- (instancetype)init {
    if ((self=[super init])) {_lock=[NSLock new];_waiters=[NSMutableArray new];}return self;
}
- (BOOL)notifyWhenFinished:(dispatch_block_t)callback {
    [_lock lock];BOOL pending=_point.pending();
    if (pending) [_waiters addObject:[callback copy]];
    [_lock unlock];return pending;
}
- (kern_return_t)status {
    [_lock lock];kern_return_t result=_point.pending()?kIOReturnNotReady:kern_return_t(_point.error());
    [_lock unlock];return result;
}
- (BOOL)pending {[_lock lock];BOOL result=_point.pending();[_lock unlock];return result;}
- (BOOL)admitForQueue:(uint64_t)queue token:(uint64_t)token {
    [_lock lock];BOOL result=_point.admit(queue,token);[_lock unlock];return result;
}
- (MetalSync::Observation)observationForQueue:(uint64_t)queue {
    [_lock lock];MetalSync::Observation result{_point.pending() && !_point.ordered(queue),_point.error()};
    [_lock unlock];return result;
}
- (BOOL)retireThrough:(uint64_t)token {
    [_lock lock];const auto queue=_point.queue(),end=_point.token();
    BOOL ready=!_point.pending() || (end && end<=token);[_lock unlock];
    if(ready)[self finish:KERN_SUCCESS queue:queue token:end];return ready;
}
- (void)finish:(kern_return_t)status queue:(uint64_t)queue token:(uint64_t)token {
    if (!status && (!queue || !token)) status=kIOReturnError;
    [_lock lock];BOOL changed=_point.finish(uint32_t(status),queue,token);
    NSArray *ready=changed?[_waiters copy]:nil;if(changed)[_waiters removeAllObjects];[_lock unlock];
    for (dispatch_block_t callback in ready) dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),callback);
}
@end

@implementation QcomMTLFence
+ (Protocol *)metalProtocol { return @protocol(MTLFence); }
- (instancetype)initWithDevice:(QcomMTLDevice *)device {
    if (!(self=[super init]) || !device) return nil;_owner=device;_producers=[NSMutableArray new];return self;
}
- (id<MTLDevice>)device { return _owner; }
@end

@implementation QcomMTLEncoderScope
- (instancetype)initWithBuffer:(QcomMTLCommandBuffer *)buffer {
    if (!(self=[super init])) return nil;_buffer=buffer;
    [buffer->_condition lock];_start=buffer->_operations.count;[buffer->_condition unlock];
    return self;
}
- (void)fence:(id<MTLFence>)fence signal:(BOOL)signal {
    [_buffer->_condition lock];
    if (_buffer->_status>=MTLCommandBufferStatusCommitted || !_buffer->_encoderActive) {
        [_buffer->_condition unlock];[NSException raise:NSInvalidArgumentException format:@"Encoder is no longer recording"];return;
    }
    BOOL valid=[fence isKindOfClass:[QcomMTLFence class]] && fence.device==_buffer.device;
    auto contains=[&](const auto &list){for(const auto &entry:list)if(entry.get()==fence)return true;return false;};
    if (!valid || (!signal && contains(_updates)) || _waits.size()+_updates.size()>=256) {
        if (!_buffer->_error) _buffer->_error=failure(@"Invalid fence, same-pass wait after update, or fence limit",MTLCommandBufferErrorInvalidResource);
    } else {
        auto &list=signal?_updates:_waits;
        if(!contains(list)) {MetalLifetime::Reference<QcomMTLFence *> reference;reference.set((QcomMTLFence *)fence,_buffer.retainedReferences);list.push_back(reference);}
    }
    [_buffer->_condition unlock];
}
- (void)finish {
    [_buffer->_condition lock];
    if (_buffer->_status>=MTLCommandBufferStatusCommitted || !_buffer->_encoderActive) {
        [_buffer->_condition unlock];[NSException raise:NSInvalidArgumentException format:@"Encoder is no longer recording"];return;
    }
    if (_start>_buffer->_operations.count || _buffer->_operations.count+_waits.size()+_updates.size()>256) {
        if (!_buffer->_error) _buffer->_error=failure(@"Encoder fence command limit exceeded",MTLCommandBufferErrorOutOfMemory);
    } else {
        // iOS waits precede load actions; updates follow all draws/resolves.
        NSUInteger at=_start;
        for(const auto &reference:_waits) {
            QcomMTLFence *fence=reference.get();
            if(!fence){if(!_buffer->_error)_buffer->_error=failure(@"An unretained fence expired during encoding",MTLCommandBufferErrorInvalidResource);continue;}
            auto operation=[QcomMTLOperation new];operation.fence=fence;
            [operation configureRetainedReferences:_buffer.retainedReferences];[_buffer->_operations insertObject:operation atIndex:at++];
        }
        for(const auto &reference:_updates) {
            QcomMTLFence *fence=reference.get();
            if(!fence){if(!_buffer->_error)_buffer->_error=failure(@"An unretained fence expired during encoding",MTLCommandBufferErrorInvalidResource);continue;}
            auto operation=[QcomMTLOperation new];operation.fence=fence;operation.signalFence=YES;
            [operation configureRetainedReferences:_buffer.retainedReferences];[_buffer->_operations addObject:operation];
        }
    }
    _waits.clear();_updates.clear();[_buffer->_condition unlock];
}
@end

@implementation QcomMTLEvent
+ (Protocol *)metalProtocol { return @protocol(MTLEvent); }
- (instancetype)initWithDevice:(QcomMTLDevice *)device {
    if (!(self=[super init]) || !device) return nil;
    _owner=device;_condition=[NSCondition new];return self;
}
- (id<MTLDevice>)device { return _owner; }
- (BOOL)waitForValue:(uint64_t)value {
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:5.0];
    [_condition lock];
    while (_value<value) if (![_condition waitUntilDate:deadline]) break;
    BOOL reached=_value>=value;[_condition unlock];return reached;
}
- (void)signalValue:(uint64_t)value {
    [_condition lock];if (value>_value) {_value=value;[_condition broadcast];}[_condition unlock];
}
@end

static void reportUnsupportedMetalCall(id object,SEL selector) {
    QcomMTLDevice *device=nil;
    if([object isKindOfClass:[QcomMTLDevice class]])device=object;
    else if([object respondsToSelector:@selector(device)])device=(QcomMTLDevice *)[(id<MTLResource>)object device];
    if([device isKindOfClass:[QcomMTLDevice class]])[device reportMetalFailure:GPU::Diagnostics::Resource error:failure(
        [NSString stringWithFormat:@"Unsupported Metal selector: %@ %@",NSStringFromClass([object class]),NSStringFromSelector(selector)])];
}

@implementation QcomMTLHeapAllocation
- (void)dealloc {[_heap releaseAllocation:self aliasable:NO];}
@end

@implementation QcomMTLHeap
+ (Protocol *)metalProtocol {return @protocol(MTLHeap);}
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLHeapDescriptor *)d {
    if(!(self=[super init]) || !device || ![d isKindOfClass:[MTLHeapDescriptor class]] ||
        ![d respondsToSelector:@selector(protectionOptions)] || d.protectionOptions ||
        (d.type!=MTLHeapTypeAutomatic && d.type!=MTLHeapTypePlacement) ||
        !resourceOptions(d.resourceOptions,MTLHazardTrackingModeUntracked,_options))return nil;
    uint64_t bytes=0;if(!GPU::allocationSize(d.size,bytes) || bytes>device.maxBufferLength)return nil;
    _owner=device;_type=d.type;_lock=[NSLock new];
    _backing=[[QcomMTLBuffer alloc] initWithDevice:device length:NSUInteger(bytes) options:_options];
    if(!_lock || !_backing || !_allocator.initialize(_backing.length,_type==MTLHeapTypePlacement))return nil;
    return self;
}
- (id<MTLDevice>)device {return _owner;}
- (MTLHeapType)type {return _type;}
- (MTLResourceOptions)resourceOptions {return _options;}
- (NSUInteger)protectionOptions {return _backing.protectionOptions;}
- (MTLStorageMode)storageMode {return _backing.storageMode;}
- (MTLCPUCacheMode)cpuCacheMode {return _backing.cpuCacheMode;}
- (MTLHazardTrackingMode)hazardTrackingMode {return _backing.hazardTrackingMode;}
- (NSUInteger)size {return NSUInteger(_allocator.size());}
- (NSUInteger)allocatedSize {return _backing.allocatedSize;}
- (NSUInteger)currentAllocatedSize {return _backing.allocatedSize;}
- (NSUInteger)usedSize {[_lock lock];auto bytes=_allocator.used();[_lock unlock];return NSUInteger(bytes);}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment {
    [_lock lock];auto bytes=_allocator.available(alignment);[_lock unlock];return NSUInteger(bytes);
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {return [_backing setPurgeableState:state];}
- (BOOL)options:(MTLResourceOptions)requested effective:(MTLResourceOptions *)effective {
    MTLResourceOptions value=0;
    if(!resourceOptions(requested,self.hazardTrackingMode,value) ||
        (value&(MTLResourceStorageModeMask|MTLResourceCPUCacheModeMask))!=(_options&(MTLResourceStorageModeMask|MTLResourceCPUCacheModeMask)))return NO;
    auto hazards=(value&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift;
    if(hazards==MTLHazardTrackingModeTracked && self.hazardTrackingMode!=MTLHazardTrackingModeTracked)return NO;
    *effective=value;return YES;
}
- (QcomMTLHeapAllocation *)allocationWithBytes:(NSUInteger)bytes offset:(NSUInteger)offset {
    GPU::MetalHeap::Range range;[_lock lock];BOOL valid=_allocator.allocate(bytes,GPU::MetalHeap::Alignment,range,offset);[_lock unlock];
    if(!valid)return nil;auto allocation=[QcomMTLHeapAllocation new];
    if(!allocation){[_lock lock];_allocator.release(range.token);[_lock unlock];return nil;}
    allocation->_heap=self;allocation->_range=range;return allocation;
}
- (void)releaseAllocation:(QcomMTLHeapAllocation *)allocation aliasable:(BOOL)aliasable {
    [_lock lock];
    if(!allocation->_aliasable) {_allocator.release(allocation->_range.token);allocation->_aliasable=aliasable;}
    [_lock unlock];
}
- (BOOL)isAllocationAliasable:(QcomMTLHeapAllocation *)allocation {
    [_lock lock];BOOL value=allocation->_aliasable;[_lock unlock];return value;
}
- (id<MTLBuffer>)bufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset placement:(BOOL)placement {
    if(placement!=(_type==MTLHeapTypePlacement))return nil;MTLResourceOptions effective=0;uint64_t bytes=0;
    if(![self options:options effective:&effective] || !GPU::MetalHeap::bufferSize(length,bytes))return nil;
    auto allocation=[self allocationWithBytes:NSUInteger(bytes) offset:offset];
    return allocation?[[QcomMTLBuffer alloc] initWithRoot:_backing allocation:allocation length:length options:effective]:nil;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    return [self bufferWithLength:length options:options offset:0 placement:NO];
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset {
    return [self bufferWithLength:length options:options offset:offset placement:YES];
}
- (id<MTLTexture>)textureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset placement:(BOOL)placement {
    if(placement!=(_type==MTLHeapTypePlacement))return nil;MTLResourceOptions effective=0;
    if(!d || ![self options:d.resourceOptions effective:&effective])return nil;
    auto texture=[[QcomMTLTexture alloc] initLayoutWithDescriptor:d];uint64_t bytes=0;
    if(!texture || !GPU::MetalHeap::bufferSize(texture->_storageBytes,bytes))return nil;
    auto allocation=[self allocationWithBytes:NSUInteger(bytes) offset:offset];if(!allocation)return nil;
    texture->_backing=[[QcomMTLBuffer alloc] initWithRoot:_backing allocation:allocation length:texture->_storageBytes options:effective];
    if(!texture->_backing)return nil;
    texture->_offset=texture->_backing->_nativeOffset;return texture;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {
    return [self textureWithDescriptor:descriptor offset:0 placement:NO];
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset {
    return [self textureWithDescriptor:descriptor offset:offset placement:YES];
}
@end

@implementation QcomMTLBuffer
+ (Protocol *)metalProtocol { return @protocol(MTLBuffer); }
- (instancetype)initWithDevice:(QcomMTLDevice *)device bytesNoCopy:(void *)bytes length:(NSUInteger)length
    options:(MTLResourceOptions)options deallocator:(void (^)(void *, NSUInteger))deallocator {
    if(!(self=[super init]))return nil;
    MTLResourceOptions normalized;
    // Reuse the client's cacheable shared allocation. Private storage and
    // requests to reinterpret its CPU cache attributes are not supported.
    if(!resourceOptions(options,MTLHazardTrackingModeTracked,normalized) ||
        (normalized&MTLResourceStorageModeMask)!=MTLResourceStorageModeShared ||
        (normalized&MTLResourceCPUCacheModeMask)!=MTLCPUCacheModeDefaultCache || length>device.maxBufferLength)return nil;
    void (^releaseBytes)(void *,NSUInteger)=[deallocator copy];
    if(deallocator && !releaseBytes)return nil;
    _owner=device;_length=length;_options=normalized;_purgeableState=MTLPurgeableStateNonVolatile;
    [device->_transportLock lock];
    GPU::Native::ResourceData resource{};
    auto status=device->_connection.importClientBuffer(bytes,length,resource);
    if(!status){_resource=resource;device->_allocated+=resource.bytes;_clientBytes=bytes;_clientDeallocator=releaseBytes;}
    [device->_transportLock unlock];
    if(status)[device reportMetalFailure:GPU::Diagnostics::Resource error:failure(
        [NSString stringWithFormat:@"Client buffer native import: result=0x%x bytes=%lu address_page_offset=%lu page=%d",
            status,(unsigned long)length,(unsigned long)(uintptr_t(bytes)%getpagesize()),getpagesize()],MTLCommandBufferErrorInvalidResource)];
    return status?nil:self;
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device length:(NSUInteger)length options:(MTLResourceOptions)options {
    return [self initWithDevice:device length:length options:options inlinePage:NO];
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device length:(NSUInteger)length options:(MTLResourceOptions)options inlinePage:(BOOL)inlinePage {
    if (!(self = [super init])) return nil;
    constexpr NSUInteger allowed = MTLResourceCPUCacheModeMask | MTLResourceStorageModeMask | MTLResourceHazardTrackingModeMask;
    auto storage = (options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift;
    auto cache = options & MTLResourceCPUCacheModeMask;
    auto hazards = (options & MTLResourceHazardTrackingModeMask) >> MTLResourceHazardTrackingModeShift;
    uint64_t rounded = 0;
    if ((options & ~allowed) || (storage != MTLStorageModeShared && storage != MTLStorageModePrivate) ||
        cache > MTLCPUCacheModeWriteCombined || hazards > MTLHazardTrackingModeTracked ||
        !GPU::allocationSize(length, rounded) || rounded>device.maxBufferLength ||
        (inlinePage && (length!=GPU::MetalUpload::PageBytes || options!=MTLResourceStorageModeShared))) return nil;
    _owner = device; _length = length; _purgeableState = MTLPurgeableStateNonVolatile;
    _options = hazards == MTLHazardTrackingModeDefault ? options | MTLResourceHazardTrackingModeTracked : options;
    [device->_transportLock lock];
    GPU::Native::ResourceData resource=inlinePage && !device->_submissionError?device->_inlinePool.take():GPU::Native::ResourceData{};
    const bool reused=resource.id!=0;
    const uint64_t allocatedBefore=device->_allocated;
    auto status = reused ? KERN_SUCCESS : device->_connection.newResource(rounded, resource);
    if (!status && (!resource.id || !resource.cpuAddress || !resource.gpuAddress || resource.bytes < rounded)) {
        if (resource.id) device->_connection.deleteResource(resource.id);
        if(reused)device->_allocated-=resource.bytes;
        status = kIOReturnError;
    }
    if (!status) {
        _resource = resource;if(!reused)device->_allocated += _resource.bytes;
        _inlinePoolPage=inlinePage;
        // Clear the actual charged allocation, including native page rounding.
        // Old views no longer exist; no in-flight command can observe this.
        if(inlinePage){memset((void *)(uintptr_t)resource.cpuAddress,0,resource.bytes);publishCPU();}
        const auto hits=device->_inlinePool.hits();
        if(reused && hits>=64 && !(hits&(hits-1)))
            fprintf(stderr,"QCOM_MTL_INLINE_POOL pid=%d hits=%llu misses=%llu returns=%llu cached_pages=%u cached_bytes=%llu\n",getpid(),
                (unsigned long long)hits,(unsigned long long)device->_inlinePool.misses(),(unsigned long long)device->_inlinePool.returns(),
                device->_inlinePool.count(),(unsigned long long)device->_inlinePool.bytes());
    }
    [device->_transportLock unlock];
    if(status)[device reportMetalFailure:GPU::Diagnostics::Resource error:failure(
        [NSString stringWithFormat:@"Native buffer allocation failed result=0x%x requested=%lu rounded=%llu allocated_before=%llu resource_limit=%llu client_limit=%llu global_limit=%llu inline=%u reused=%u",
            status,(unsigned long)length,(unsigned long long)rounded,(unsigned long long)allocatedBefore,
            (unsigned long long)device->_memoryLimits.resourceBytes,(unsigned long long)device->_memoryLimits.clientBytes,
            (unsigned long long)device->_memoryLimits.globalBytes,unsigned(inlinePage),unsigned(reused)],MTLCommandBufferErrorOutOfMemory)];
    return status ? nil : self;
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device surface:(IOSurfaceRef)surface plane:(NSUInteger)plane options:(MTLResourceOptions)options {
    if (!(self=[super init])) return nil;
    constexpr NSUInteger allowed=MTLResourceCPUCacheModeMask|MTLResourceStorageModeMask|MTLResourceHazardTrackingModeMask;
    auto storage=(options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift;
    auto hazards=(options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift;
    if (!surface || CFGetTypeID(surface)!=IOSurfaceGetTypeID() || plane || (options&~allowed) ||
        (storage!=MTLStorageModeShared && storage!=MTLStorageModePrivate) ||
        (options&MTLResourceCPUCacheModeMask)!=MTLCPUCacheModeDefaultCache || hazards>MTLHazardTrackingModeTracked) {
        [device reportMetalFailure:GPU::Diagnostics::Resource error:failure(
            [NSString stringWithFormat:@"IOSurface buffer options rejected surface=%u plane=%lu options=0x%lx",
                unsigned(surface && CFGetTypeID(surface)==IOSurfaceGetTypeID()),(unsigned long)plane,(unsigned long)options],
            MTLCommandBufferErrorInvalidResource)];
        return nil;
    }
    _owner=device;_surface=(IOSurfaceRef)CFRetain(surface);_purgeableState=MTLPurgeableStateNonVolatile;
    _options=hazards==MTLHazardTrackingModeDefault?options|MTLResourceHazardTrackingModeTracked:options;
    [device->_transportLock lock];
    GPU::Native::ResourceData resource{};
    auto status=device->_connection.importSurface(IOSurfaceGetID(surface),uint32_t(plane),resource);
    if (!status) {
        GPU::Surface::Info s{};memcpy(&s,resource.privateInfo,sizeof(s));
        bool valid=IOSurfaceGetPlaneCount(surface)==0 && IOSurfaceGetWidth(surface)==s.width &&
            IOSurfaceGetHeight(surface)==s.height && IOSurfaceGetBytesPerRow(surface)==s.rowBytes &&
            IOSurfaceGetPixelFormat(surface)==s.pixelFormat && IOSurfaceGetBytesPerElement(surface)==s.bytesPerElement &&
            IOSurfaceGetElementWidth(surface)==1 && IOSurfaceGetElementHeight(surface)==1 &&
            IOSurfaceGetAllocSize(surface)==s.allocationBytes;
        void *base=IOSurfaceGetBaseAddress(surface);
        if (!valid || (storage==MTLStorageModeShared && !base)) {
            device->_connection.deleteResource(resource.id);status=kIOReturnBadArgument;
        } else {
            // IOSurface supplies its pixel address; the kernel's backing
            // offset is already included. Never add that offset a second time.
            resource.cpuAddress=uint64_t(uintptr_t(base));_resource=resource;_length=s.imageBytes;
            device->_allocated+=resource.bytes;
        }
    }
    [device->_transportLock unlock];
    if(status)[device reportMetalFailure:GPU::Diagnostics::Resource error:failure(
        [NSString stringWithFormat:@"IOSurface import failed result=0x%x id=%u size=%lux%lu format=0x%x pitch=%lu alloc=%lu options=0x%lx",
            status,IOSurfaceGetID(surface),(unsigned long)IOSurfaceGetWidth(surface),(unsigned long)IOSurfaceGetHeight(surface),
            IOSurfaceGetPixelFormat(surface),(unsigned long)IOSurfaceGetBytesPerRow(surface),(unsigned long)IOSurfaceGetAllocSize(surface),
            (unsigned long)options],MTLCommandBufferErrorInvalidResource)];
    return status?nil:self;
}
- (instancetype)initWithRoot:(QcomMTLBuffer *)root allocation:(QcomMTLHeapAllocation *)allocation length:(NSUInteger)length options:(MTLResourceOptions)options {
    if(!(self=[super init]) || !root || !allocation || !length || length>allocation->_range.bytes ||
        !validRange(root.length,NSUInteger(allocation->_range.offset),NSUInteger(allocation->_range.bytes)))return nil;
    _allocationRoot=root;_heapAllocation=allocation;_owner=root->_owner;_resource=root->_resource;
    _nativeOffset=root->_nativeOffset+NSUInteger(allocation->_range.offset);_length=length;_options=options;return self;
}
- (instancetype)initWithInlineRoot:(QcomMTLBuffer *)root offset:(NSUInteger)offset length:(NSUInteger)length {
    if (!(self=[super init]) || !root || !length || root->_allocationRoot || root->_surface ||
        root.storageMode!=MTLStorageModeShared || !validRange(root.length,offset,length)) return nil;
    // A private view retains its page and exposes only this snapshot's bytes.
    // It neither creates nor deletes a second native resource ID.
    _allocationRoot=root;_owner=root->_owner;_resource=root->_resource;
    _nativeOffset=root->_nativeOffset+offset;_length=length;_options=root->_options;return self;
}
- (void)dealloc {
    if (_resource.id && !_allocationRoot) {
        [_owner->_transportLock lock];
        // Merely reaching Completed/Scheduled is insufficient: only the
        // successful physical retirement path marks a private page reusable.
        // Dealloc proves its final view/reference is gone before ID transfer.
        if(!_inlinePoolPage || !_inlinePoolRetired || _owner->_submissionError || !_owner->_inlinePool.put(_resource)) {
            _owner->_connection.deleteResource(_resource.id);_owner->_allocated -= _resource.bytes;
        }
        [_owner->_transportLock unlock];
    }
    if (_surface) CFRelease(_surface);
    // Native mappings are released first. Retained command resources/views
    // keep this buffer alive until their actual GPU completion. Never call
    // client code while holding the transport lock, or on a failed import.
    if (_clientDeallocator) _clientDeallocator(_clientBytes,_length);
}
- (id<MTLDevice>)device { return _owner; }
- (NSUInteger)length { return _length; }
- (NSUInteger)allocatedSize { return _heapAllocation?NSUInteger(_heapAllocation->_range.bytes):_resource.bytes; }
- (void *)contents {
    return self.storageMode == MTLStorageModeShared ? (void *)(uintptr_t)(_resource.cpuAddress+_nativeOffset) : nullptr;
}
- (uint64_t)gpuAddress { return _resource.gpuAddress+_nativeOffset; }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_options & MTLResourceCPUCacheModeMask); }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift); }
- (MTLHazardTrackingMode)hazardTrackingMode {
    return (MTLHazardTrackingMode)((_options & MTLResourceHazardTrackingModeMask) >> MTLResourceHazardTrackingModeShift);
}
- (MTLResourceOptions)resourceOptions { return _options; }
// Protected IOSurfaces and unknown resource-option bits are rejected during
// allocation/import. Report the actual unprotected backing to QuartzCore.
- (NSUInteger)protectionOptions { return 0; }
- (id<MTLHeap>)heap {return _heapAllocation?_heapAllocation->_heap:nil;}
- (NSUInteger)heapOffset {return self.heap.type==MTLHeapTypePlacement?_nativeOffset:0;}
- (BOOL)isAliasable {return _heapAllocation?[_heapAllocation->_heap isAllocationAliasable:_heapAllocation]:NO;}
- (void)makeAliasable {
    if(!_heapAllocation){[NSException raise:NSInvalidArgumentException format:@"Only heap resources may become aliasable"];return;}
    [_heapAllocation->_heap releaseAllocation:_heapAllocation aliasable:YES];
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
    if (state<MTLPurgeableStateKeepCurrent || state>MTLPurgeableStateEmpty) {
        [NSException raise:NSInvalidArgumentException format:@"Invalid Metal purgeable state"];return MTLPurgeableStateNonVolatile;
    }
    // Heap resources reflect the whole heap's state. Only MTLHeap controls
    // that allocation; a subresource must not invalidate its live siblings.
    if (_allocationRoot) return [_allocationRoot setPurgeableState:MTLPurgeableStateKeepCurrent];
    // Imported storage belongs to IOSurface or the client allocation. Metal
    // resource requests cannot discard memory belonging to that owner.
    if (_surface || _clientBytes) return MTLPurgeableStateNonVolatile;
    NSUInteger previous=__atomic_load_n(&_purgeableState,__ATOMIC_ACQUIRE);
    if (state==MTLPurgeableStateKeepCurrent) return MTLPurgeableState(previous);
    // Native DMA backing remains wired and its address stays stable. Volatile
    // permits discarding but does not require reclamation. Empty invalidates
    // content without freeing memory still referenced by native jobs/views.
    // Preserve Empty across a Volatile request until NonVolatile observes it.
    for (;;) {
        NSUInteger next=previous==MTLPurgeableStateEmpty && state==MTLPurgeableStateVolatile ?
            MTLPurgeableStateEmpty : state;
        if (__atomic_compare_exchange_n(&_purgeableState,&previous,next,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            return MTLPurgeableState(previous);
    }
}
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)identity { (void)identity; return kIOReturnUnsupported; }
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytes {
    auto texture=[[QcomMTLTexture alloc] initWithBuffer:self descriptor:descriptor offset:offset bytesPerRow:bytes];
    if(!texture)[_owner reportMetalFailure:GPU::Diagnostics::Resource error:failure(
        [NSString stringWithFormat:@"Buffer texture rejected: format=%lu size=%lux%lu offset=%lu row=%lu",
            (unsigned long)descriptor.pixelFormat,(unsigned long)descriptor.width,(unsigned long)descriptor.height,
            (unsigned long)offset,(unsigned long)bytes],MTLCommandBufferErrorInvalidResource)];
    return texture;
}
// QuartzCore's native image uploader uses this buffer SPI after importing a
// client allocation. A linear texture is a view of that same native resource;
// the parent buffer and its bytesNoCopy deallocator stay retained by the view.
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset
    bytesPerRow:(NSUInteger)rowBytes bytesPerImage:(NSUInteger)imageBytes {
    if(!descriptor || descriptor.textureType!=MTLTextureType2D || descriptor.mipmapLevelCount!=1 ||
        descriptor.sampleCount!=1 || descriptor.arrayLength!=1 || descriptor.depth!=1)return nil;
    GPU::Shader::Texture plane{};GPU::Shader::TextureLayout layout{};
    if(!GPU::MetalTexture::bufferTexture(_resource.id,_length,offset,descriptor.width,descriptor.height,rowBytes,
        nativeColorFormat(descriptor.pixelFormat),nativeSwizzle(descriptor.swizzle),plane,layout,imageBytes))return nil;
    return [self newTextureWithDescriptor:descriptor offset:offset bytesPerRow:rowBytes];
}
- (void)addDebugMarker:(NSString *)marker range:(NSRange)range { (void)marker; (void)range; }
- (void)removeAllDebugMarkers {}
@end

@implementation QcomMTLTextureLayout
+ (Protocol *)metalProtocol {return @protocol(QcomNativeTextureLayout);}
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLTextureDescriptor *)descriptor
    isHeapOrBufferBacked:(BOOL)heapOrBufferBacked {
    if(!(self=[super init]))return nil;
    MTLTextureDescriptor *d=[descriptor copy];
    if(!d || !colorFormat(d.pixelFormat) || d.sampleCount!=1 || d.storageMode==MTLStorageModeMemoryless)return nil;
    // The adapter has one linear storage interpretation for both flags. Reuse
    // the actual texture allocator's checked layout without allocating a GPU
    // resource. Descriptor mutation after this call cannot change the object.
    (void)heapOrBufferBacked;
    auto shape=[[QcomMTLTexture alloc] initLayoutWithDescriptor:d];
    if(!shape || shape->_storageBytes>device.maxBufferLength)return nil;
    GPU::Shader::Texture plane{1,0,uint32_t(shape->_width),uint32_t(shape->_height),uint32_t(shape->_pitch),
        nativeColorFormat(shape->_format),uint32_t(shape->_levels),shape->_swizzle,
        uint32_t(arrayTextureType(shape->_type)?shape->_layers:0),0,uint32_t(cubeTextureType(shape->_type))};
    return _memoryLayout.initialize(plane) && _memoryLayout.dataSize()==shape->_storageBytes?self:nil;
}
- (NSUInteger)size {return NSUInteger(_memoryLayout.size());}
- (NSUInteger)alignment {return NSUInteger(_memoryLayout.alignment());}
- (MTLSize)copyGranularity {auto block=_memoryLayout.copyGranularity();return MTLSizeMake(block,block,1);}
- (QcomNativeTextureLayoutWatermark)watermark {
    auto value=GPU::MetalTexture::LinearLayoutWatermark;return {{value.words[0],value.words[1]}};
}
- (void)initializeTextureMemory:(void *)memory {
    if(!_memoryLayout.initializeMemory(memory))
        [NSException raise:NSInvalidArgumentException format:@"Invalid texture layout initialization memory"];
}
- (void)finalizeTextureMemory:(void *)memory {
    if(!_memoryLayout.finalizeMemory(memory))
        [NSException raise:NSInvalidArgumentException format:@"Invalid texture layout finalization memory"];
    publishCPU();
}
- (void)copyFromLinearBytes:(const void *)bytes linearOffset:(NSUInteger)offset
    linearBytesPerRow:(NSUInteger)rowBytes linearBytesPerImage:(NSUInteger)imageBytes
    toTextureMemory:(void *)memory textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level
    textureRegion:(MTLRegion)region {
    GPU::MetalTexture::CopyRegion r{region.origin.x,region.origin.y,region.origin.z,
        region.size.width,region.size.height,region.size.depth};
    if(!_memoryLayout.copyToTexture(bytes,offset,rowBytes,imageBytes,memory,slice,level,r))
        [NSException raise:NSInvalidArgumentException format:@"Invalid linear-to-texture layout copy"];
}
- (void)copyFromTextureMemory:(const void *)memory textureSlice:(NSUInteger)slice
    textureLevel:(NSUInteger)level textureRegion:(MTLRegion)region toLinearBytes:(void *)bytes
    linearOffset:(NSUInteger)offset linearBytesPerRow:(NSUInteger)rowBytes linearBytesPerImage:(NSUInteger)imageBytes {
    GPU::MetalTexture::CopyRegion r{region.origin.x,region.origin.y,region.origin.z,
        region.size.width,region.size.height,region.size.depth};
    if(!_memoryLayout.copyFromTexture(memory,slice,level,r,bytes,offset,rowBytes,imageBytes))
        [NSException raise:NSInvalidArgumentException format:@"Invalid texture-to-linear layout copy"];
}
@end

@implementation QcomMTLTexture
+ (Protocol *)metalProtocol { return @protocol(MTLTexture); }
- (instancetype)initWithBuffer:(QcomMTLBuffer *)buffer descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytes {
    if(!(self=[super init]))return nil;
    if(!buffer || buffer->_surface || !textureDescriptor(d) || d.textureType!=MTLTextureType2D ||
        d.mipmapLevelCount!=1 || d.sampleCount!=1 || !colorFormat(d.pixelFormat) ||
        d.storageMode!=buffer.storageMode || ([d respondsToSelector:@selector(isDrawable)] && d.isDrawable) ||
        ([d respondsToSelector:@selector(framebufferOnly)] && d.framebufferOnly))return nil;
    constexpr NSUInteger allowed=MTLResourceCPUCacheModeMask|MTLResourceStorageModeMask|MTLResourceHazardTrackingModeMask;
    if((d.resourceOptions&~allowed) || d.cpuCacheMode>MTLCPUCacheModeWriteCombined ||
        d.hazardTrackingMode>MTLHazardTrackingModeTracked)return nil;
    GPU::Shader::Texture plane{};
    if(!GPU::MetalTexture::bufferTexture(buffer->_resource.id,buffer.length,offset,d.width,d.height,bytes,
        nativeColorFormat(d.pixelFormat),nativeSwizzle(d.swizzle),plane,_layout))return nil;
    _backing=buffer;_bufferBacked=YES;_offset=buffer->_nativeOffset+plane.offset;_width=plane.width;_height=plane.height;_pitch=plane.pitch;
    _bytesPerPixel=GPU::Shader::formatBytes(plane.format);_format=d.pixelFormat;_usage=d.usage;_swizzle=plane.swizzle;
    _levels=_layers=_samples=1;_type=MTLTextureType2D;_layerStride=_layout.layerStride;
    return self;
}
- (instancetype)initLayoutWithDescriptor:(MTLTextureDescriptor *)d {
    if (!(self = [super init])) return nil;
    if (!textureDescriptor(d) || ([d respondsToSelector:@selector(isDrawable)] && d.isDrawable)) return nil;
    _memoryless=d.storageMode==MTLStorageModeMemoryless;
    if(_memoryless) {
        // A540 emulates transient attachment storage with a private backing
        // per queue. No contents persist across render passes; other queues
        // must use independent bytes because they may execute concurrently.
        if(d.textureType!=MTLTextureType2D || d.mipmapLevelCount!=1 || d.sampleCount!=1 ||
            (d.usage&MTLTextureUsageShaderWrite) || !(d.usage&MTLTextureUsageRenderTarget))return nil;
        d=[d copy];d.storageMode=MTLStorageModePrivate;
    }
    MTLResourceOptions normalized;
    if(!resourceOptions(d.resourceOptions,MTLHazardTrackingModeTracked,normalized))return nil;
    if(_memoryless)_memorylessOptions=(normalized&~MTLResourceStorageModeMask)|MTLResourceStorageModeMemoryless;
    _framebufferOnly=[d respondsToSelector:@selector(framebufferOnly)] && d.framebufferOnly;
    bool color=colorFormat(d.pixelFormat), combined=d.pixelFormat==MTLPixelFormatDepth32Float_Stencil8;
    if (!color && (d.storageMode!=MTLStorageModePrivate ||
        (d.usage&~(MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView|(depthFormat(d.pixelFormat)?MTLTextureUsageShaderRead:0))))) return nil;
    _width = d.width; _height = d.height; _levels=d.mipmapLevelCount;
    _bytesPerPixel=color?GPU::Shader::formatBytes(nativeColorFormat(d.pixelFormat)):d.pixelFormat==MTLPixelFormatStencil8?1:4;
    _type=d.textureType;_layers=d.arrayLength;_samples=d.sampleCount;_bytesPerPixel*=_samples;
    const auto nativeFormat=nativeColorFormat(d.pixelFormat);
    NSUInteger alignment=(_samples>1 || _levels>1 || GPU::Shader::compressedFormat(nativeFormat))?64*_bytesPerPixel:64;
    _pitch = (GPU::Shader::formatBlocks(uint32_t(_width),nativeFormat) * _bytesPerPixel + alignment-1) & ~(alignment-1);
    _stencilPitch = combined ? (_width*_samples+64*_samples-1)&~NSUInteger(64*_samples-1) : d.pixelFormat==MTLPixelFormatStencil8 ? _pitch : 0;
    _stencilOffset = combined ? _pitch*_height : 0;
    _format = d.pixelFormat; _usage = d.usage;_swizzle=nativeSwizzle(d.swizzle);
    NSUInteger bytes=_pitch*_height+(combined?_stencilPitch*_height:0);
    if (color) {
        GPU::Shader::Texture plane{1,0,uint32_t(_width),uint32_t(_height),uint32_t(_pitch),nativeColorFormat(_format),uint32_t(_levels),_swizzle,
            uint32_t(arrayTextureType(_type)?_layers:0),0,uint32_t(cubeTextureType(_type)),uint32_t(_samples>1?_samples:0)};
        if (!GPU::Shader::textureLayout(plane,_layout)) return nil;bytes=_layout.bytes;_layerStride=_layout.layerStride;
    } else {
        GPU::Shader::Texture plane{1,0,uint32_t(_width),uint32_t(_height),uint32_t(_pitch),
            _format==MTLPixelFormatStencil8?GPU::Shader::Stencil8:GPU::Shader::Depth32,uint32_t(_levels>1?_levels:0),0,
            uint32_t(arrayTextureType(_type)?_layers:0),0,0,uint32_t(_samples>1?_samples:0)};
        if(!GPU::Shader::planeLayout(plane,plane.format,_layout))return nil;
        bytes=_layout.bytes;_layerStride=_layout.layerStride;
        if(combined) {
            plane.pitch=uint32_t(_stencilPitch);plane.format=GPU::Shader::Stencil8;GPU::Shader::TextureLayout stencil;
            if(!GPU::Shader::planeLayout(plane,plane.format,stencil))return nil;
            _stencilOffset=bytes;_stencilLayerStride=stencil.layerStride;bytes+=stencil.bytes;
        } else if(stencilFormat(_format))_stencilLayerStride=_layerStride;
    }
    if(bytes>GPU::MaxBufferBytes)return nil;_storageBytes=bytes;return self;
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLTextureDescriptor *)d {
    if(!(self=[self initLayoutWithDescriptor:d]) || _storageBytes>device.maxBufferLength)return nil;
    if(_memoryless) {
        _memorylessDevice=device;_passBackingLock=[NSLock new];
        _passBackings=[NSMapTable weakToStrongObjectsMapTable];return self;
    }
    _backing = [[QcomMTLBuffer alloc] initWithDevice:device length:_storageBytes options:d.resourceOptions];
    return _backing ? self : nil;
}
- (QcomMTLTexture *)newPassBackingForQueue:(QcomMTLCommandQueue *)queue {
    if(!_memoryless || !queue || queue.device!=_memorylessDevice)return nil;
    [_passBackingLock lock];
    @try {
        QcomMTLTexture *backing=[_passBackings objectForKey:queue];
        if(!backing) {
            auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:_format width:_width height:_height mipmapped:NO];
            d.resourceOptions=(_memorylessOptions&~MTLResourceStorageModeMask)|MTLResourceStorageModePrivate;
            d.usage=_usage;d.swizzle={MTLTextureSwizzle(GPU::Shader::swizzleChannel(_swizzle,0)),MTLTextureSwizzle(GPU::Shader::swizzleChannel(_swizzle,1)),
                MTLTextureSwizzle(GPU::Shader::swizzleChannel(_swizzle,2)),MTLTextureSwizzle(GPU::Shader::swizzleChannel(_swizzle,3))};
            backing=[[QcomMTLTexture alloc] initWithDevice:_memorylessDevice descriptor:d];
            if(backing)[_passBackings setObject:backing forKey:queue];
        }
        // Clear/discard and every draw remain commands in queue order. The
        // public memoryless object exposes no backing outside a render pass.
        // Each command still retains its internal texture through retirement.
        return backing;
    } @finally {[_passBackingLock unlock];}
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLTextureDescriptor *)d surface:(IOSurfaceRef)surface plane:(NSUInteger)plane {
    if (!(self=[super init])) return nil;
    if (!textureDescriptor(d) || d.textureType!=MTLTextureType2D || d.mipmapLevelCount!=1 || !colorFormat(d.pixelFormat)) {
        [device reportMetalFailure:GPU::Diagnostics::Resource error:failure(
            [NSString stringWithFormat:@"IOSurface descriptor rejected type=%lu fmt=%lu size=%lux%lux%lu levels=%lu samples=%lu array=%lu usage=%lu opts=%lu protect=%lu rotation=%lu compression=%lu swizzle=%u",
                (unsigned long)d.textureType,(unsigned long)d.pixelFormat,(unsigned long)d.width,(unsigned long)d.height,
                (unsigned long)d.depth,(unsigned long)d.mipmapLevelCount,(unsigned long)d.sampleCount,(unsigned long)d.arrayLength,
                (unsigned long)d.usage,(unsigned long)d.resourceOptions,
                (unsigned long)([d respondsToSelector:@selector(protectionOptions)]?d.protectionOptions:NSUIntegerMax),
                (unsigned long)([d respondsToSelector:@selector(rotation)]?d.rotation:0),(unsigned long)d.compressionType,nativeSwizzle(d.swizzle)],
            MTLCommandBufferErrorInvalidResource)];
        return nil;
    }
    _framebufferOnly=[d respondsToSelector:@selector(framebufferOnly)] && d.framebufferOnly;
    _isDrawable=[d respondsToSelector:@selector(isDrawable)] && d.isDrawable;
    if(_isDrawable && (!renderableColorFormat(d.pixelFormat) || GPU::Shader::formatBytes(nativeColorFormat(d.pixelFormat))!=4 || nativeSwizzle(d.swizzle) || (d.usage && !(d.usage&MTLTextureUsageRenderTarget)))) {
        [device reportMetalFailure:GPU::Diagnostics::Resource error:failure(
            [NSString stringWithFormat:@"IOSurface drawable rejected fmt=%lu usage=%lu swizzle=%u",
                (unsigned long)d.pixelFormat,(unsigned long)d.usage,nativeSwizzle(d.swizzle)],MTLCommandBufferErrorInvalidResource)];
        return nil;
    }
    _backing=[[QcomMTLBuffer alloc] initWithDevice:device surface:surface plane:plane options:d.resourceOptions];
    if (!_backing) return nil;
    GPU::Surface::Info s{};memcpy(&s,_backing->_resource.privateInfo,sizeof(s));
    uint32_t cpp=GPU::Shader::formatBytes(nativeColorFormat(d.pixelFormat));
    uint32_t format=GPU::Surface::colorPixelFormat(cpp,GPU::Shader::bgraFormat(nativeColorFormat(d.pixelFormat)));
    // VideoToolbox exposes a BGRA/RGBA IOSurface as R32Uint to store its packed
    // word. This is a raw view of the same four bytes, without color conversion.
    bool compatible=GPU::Shader::unsignedIntegerFormat(nativeColorFormat(d.pixelFormat))?
        (s.bytesPerElement==4 && (s.pixelFormat==GPU::Surface::BGRA || s.pixelFormat==GPU::Surface::RGBA)):
        GPU::Surface::compatibleColorPixelFormat(s.pixelFormat,cpp,GPU::Shader::bgraFormat(nativeColorFormat(d.pixelFormat)));
    if (d.width!=s.width || d.height!=s.height || !compatible) {
        [device reportMetalFailure:GPU::Diagnostics::Resource error:failure(
            [NSString stringWithFormat:@"IOSurface layout mismatch texture=%lux%lu/%u surface=%ux%u/%u pitch=%u",
                (unsigned long)d.width,(unsigned long)d.height,format,s.width,s.height,s.pixelFormat,s.rowBytes],
            MTLCommandBufferErrorInvalidResource)];
        return nil;
    }
    _width=s.width;_height=s.height;_pitch=s.rowBytes;_bytesPerPixel=cpp;_format=d.pixelFormat;_levels=1;_type=MTLTextureType2D;_layers=1;_samples=1;
    // IOGPUMetalTexture keeps resolvedUsage and framebufferOnly separately.
    // The private presentation flag must not erase declared shader access.
    // LA08 remains RG8 storage with the caller's read swizzle. QuartzCore's
    // image function performs the gray/alpha expansion after sampling.
    _usage=d.usage;_swizzle=nativeSwizzle(d.swizzle);
    return self;
}
- (instancetype)initWithParent:(QcomMTLTexture *)parent format:(MTLPixelFormat)format levels:(NSRange)levels swizzle:(uint32_t)swizzle {
    return [self initWithParent:parent format:format type:parent.textureType levels:levels slices:NSMakeRange(0,textureSliceCount(parent)) swizzle:swizzle];
}
- (instancetype)initWithParent:(QcomMTLTexture *)parent format:(MTLPixelFormat)format type:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices swizzle:(uint32_t)swizzle {
    if (!(self=[super init])) return nil;
    if (!parent || parent->_memoryless || !levels.length || !slices.length || !validRange(parent->_levels,levels.location,levels.length) ||
        !validRange(textureSliceCount(parent),slices.location,slices.length) || !colorTextureType(type) || multisampleTextureType(type)!=(parent->_samples>1) ||
        (cubeTextureType(type) ? ((slices.length%6) || (type==MTLTextureTypeCube && slices.length!=6)) :
            ((type==MTLTextureType2D || type==MTLTextureType2DMultisample) && slices.length!=1))) return nil;
    if (colorFormat(parent->_format)) {
        GPU::Shader::Texture view;
        if (!GPU::Shader::textureView([parent nativeTexture],uint32_t(levels.location),uint32_t(levels.length),uint32_t(slices.location),uint32_t(slices.length),
            arrayTextureType(type),nativeColorFormat(format),view,swizzle,cubeTextureType(type)) ||
            !GPU::Shader::textureLayout(view,_layout,false)) return nil;
        _offset=view.offset;_width=view.width;_height=view.height;_pitch=view.pitch;_bytesPerPixel=GPU::Shader::formatBytes(view.format)*parent->_samples;_swizzle=view.swizzle;_layerStride=_layout.layerStride;
    } else {
        if (format!=parent->_format || cubeTextureType(type) || swizzle) return nil;
        const bool stencilOnly=stencilFormat(format) && !depthFormat(format);
        GPU::Shader::Texture view;
        if(!GPU::Shader::textureView([parent nativeAspect:stencilOnly],uint32_t(levels.location),uint32_t(levels.length),
            uint32_t(slices.location),uint32_t(slices.length),arrayTextureType(type),stencilOnly?GPU::Shader::Stencil8:GPU::Shader::Depth32,view) ||
            !GPU::Shader::textureStorageLayout(view,_layout,false))return nil;
        _offset=view.offset;_width=view.width;_height=view.height;_pitch=view.pitch;_layerStride=view.layerStride;_bytesPerPixel=parent->_bytesPerPixel;
        if(format==MTLPixelFormatDepth32Float_Stencil8) {
            GPU::Shader::Texture stencil;
            if(!GPU::Shader::textureView([parent nativeAspect:YES],uint32_t(levels.location),uint32_t(levels.length),
                uint32_t(slices.location),uint32_t(slices.length),arrayTextureType(type),GPU::Shader::Stencil8,stencil) || stencil.offset<_offset)return nil;
            _stencilOffset=stencil.offset-_offset;_stencilPitch=stencil.pitch;_stencilLayerStride=stencil.layerStride;
        } else if(stencilOnly){_stencilPitch=_pitch;_stencilLayerStride=_layerStride;}
    }
    _parent=parent;_parentLevel=levels.location;_parentSlice=slices.location;_backing=parent->_backing;_type=type;_layers=slices.length/(cubeTextureType(type)?6:1);
    _format=format;_usage=parent->_usage;_framebufferOnly=parent->_framebufferOnly;_isDrawable=parent->_isDrawable;_levels=levels.length;_samples=parent->_samples;
    return self;
}
- (GPU::Shader::Texture)nativeTexture {
    if(_memoryless)return {};
    if (!colorFormat(_format))return [self nativeAspect:stencilFormat(_format) && !depthFormat(_format)];
    return {_backing->_resource.id, (uint32_t)_offset, (uint32_t)_width, (uint32_t)_height, (uint32_t)_pitch,
        nativeColorFormat(_format),uint32_t(_levels>1?_levels:0),_swizzle,uint32_t(arrayTextureType(_type)?_layers:0),uint32_t((arrayTextureType(_type) || cubeTextureType(_type))?_layerStride:0),uint32_t(cubeTextureType(_type)),uint32_t(_samples>1?_samples:0)};
}
- (GPU::Shader::Texture)nativeTextureAtLevel:(NSUInteger)level {
    return [self nativeTextureAtLevel:level slice:0];
}
- (GPU::Shader::Texture)nativeTextureAtLevel:(NSUInteger)level slice:(NSUInteger)slice {
    if (level>=_levels || slice>=textureSliceCount(self)) return {};
    if (!colorFormat(_format)) return [self nativePlane:stencilFormat(_format) && !depthFormat(_format) level:level slice:slice];
    GPU::Shader::Texture plane{},out{};
    if (!GPU::Shader::textureSlice([self nativeTexture],uint32_t(slice),plane) || !GPU::Shader::textureLevel(plane,uint32_t(level),out)) return {};
    return out;
}
- (GPU::Shader::Texture)nativePlane:(BOOL)stencil {
    return [self nativePlane:stencil level:0];
}
- (GPU::Shader::Texture)nativePlane:(BOOL)stencil level:(NSUInteger)level {
    return [self nativePlane:stencil level:level slice:0];
}
- (GPU::Shader::Texture)nativePlane:(BOOL)stencil level:(NSUInteger)level slice:(NSUInteger)slice {
    if(level>=_levels || slice>=textureSliceCount(self))return {};
    GPU::Shader::Texture selected{},out{};
    if(!GPU::Shader::textureSlice([self nativeAspect:stencil],uint32_t(slice),selected) ||
        !GPU::Shader::textureLevel(selected,uint32_t(level),out))return {};
    return out;
}
- (GPU::Shader::Texture)nativeAspect:(BOOL)stencil {
    if(_memoryless)return {};
    if(stencil?!stencilFormat(_format):!depthFormat(_format))return {};
    return {_backing->_resource.id,uint32_t(_offset+(stencil?_stencilOffset:0)),uint32_t(_width),uint32_t(_height),
        uint32_t(stencil?_stencilPitch:_pitch),stencil?GPU::Shader::Stencil8:GPU::Shader::Depth32,uint32_t(_levels>1?_levels:0),0,
        uint32_t(arrayTextureType(_type)?_layers:0),uint32_t(arrayTextureType(_type)?(stencil?_stencilLayerStride:_layerStride):0),0,uint32_t(_samples>1?_samples:0)};
}
- (id<MTLDevice>)device { return _memoryless?_memorylessDevice:_backing.device; }
- (NSUInteger)allocatedSize { return _backing.allocatedSize; }
- (MTLCPUCacheMode)cpuCacheMode { return _memoryless?MTLCPUCacheMode(_memorylessOptions&MTLResourceCPUCacheModeMask):_backing.cpuCacheMode; }
- (MTLStorageMode)storageMode { return _memoryless?MTLStorageModeMemoryless:_backing.storageMode; }
- (MTLHazardTrackingMode)hazardTrackingMode { return _memoryless?MTLHazardTrackingMode((_memorylessOptions&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift):_backing.hazardTrackingMode; }
- (MTLResourceOptions)resourceOptions { return _memoryless?_memorylessOptions:_backing.resourceOptions; }
- (NSUInteger)protectionOptions { return _backing.protectionOptions; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return [_backing setPurgeableState:state]; }
- (id<MTLHeap>)heap {return _backing.heap;}
- (NSUInteger)heapOffset {return self.heap.type==MTLHeapTypePlacement?_offset:0;}
- (BOOL)isAliasable {return _backing.isAliasable;}
- (void)makeAliasable {
    if(_parent || !self.heap){[NSException raise:NSInvalidArgumentException format:@"Only direct heap resources may become aliasable"];return;}
    if(!_bufferBacked)[_backing makeAliasable]; // buffer texture views do not release the buffer's allocation
}
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)identity { (void)identity; return kIOReturnUnsupported; }
- (id<MTLResource>)rootResource {
    QcomMTLTexture *root=self;while(root->_parent)root=root->_parent;
    return root->_bufferBacked?(id<MTLResource>)root->_backing:root==self?nil:root;
}
- (id<MTLTexture>)parentTexture { return _parent; }
- (NSUInteger)parentRelativeLevel { return _parentLevel; }
- (NSUInteger)parentRelativeSlice { return _parentSlice; }
- (id<MTLBuffer>)buffer { return _bufferBacked?_backing:nil; }
- (NSUInteger)bufferOffset { return _bufferBacked?_offset-_backing->_nativeOffset:0; }
- (NSUInteger)bufferBytesPerRow { return _bufferBacked?_pitch:0; }
- (IOSurfaceRef)iosurface { return _backing->_surface; }
- (NSUInteger)iosurfacePlane { return 0; }
- (MTLTextureType)textureType { return _type; }
- (MTLPixelFormat)pixelFormat { return _format; }
- (NSUInteger)width { return _width; }
- (NSUInteger)height { return _height; }
- (NSUInteger)depth { return 1; }
- (NSUInteger)mipmapLevelCount { return _levels; }
- (NSUInteger)sampleCount { return _samples; }
- (NSUInteger)arrayLength { return _layers; }
- (MTLTextureUsage)usage { return _usage; }
- (BOOL)isShareable { return _backing->_surface!=nullptr; }
- (BOOL)isFramebufferOnly { return _framebufferOnly; }
- (BOOL)isDrawable { return _isDrawable; }
- (NSUInteger)firstMipmapInTail { return _levels; }
- (NSUInteger)tailSizeInBytes { return 0; }
- (BOOL)isSparse { return NO; }
- (BOOL)allowGPUOptimizedContents { return NO; }
- (MTLTextureCompressionType)compressionType { return MTLTextureCompressionTypeLossless; }
- (MTLResourceID)gpuResourceID { return {_backing->_resource.uniqueID}; }
- (MTLTextureSwizzleChannels)swizzle {
    return {MTLTextureSwizzle(GPU::Shader::swizzleChannel(_swizzle,0)),MTLTextureSwizzle(GPU::Shader::swizzleChannel(_swizzle,1)),
        MTLTextureSwizzle(GPU::Shader::swizzleChannel(_swizzle,2)),MTLTextureSwizzle(GPU::Shader::swizzleChannel(_swizzle,3))};
}
- (BOOL)validRegion:(MTLRegion)r level:(NSUInteger)level slice:(NSUInteger)slice pitch:(NSUInteger)pitch {
    auto plane=[self nativeTextureAtLevel:level slice:slice];
    GPU::Shader::TextureCopyRegion copy;
    return plane.resource && !r.origin.z && r.size.depth == 1 &&
        GPU::Shader::textureCopyRegion(plane,r.origin.x,r.origin.y,r.size.width,r.size.height,copy) &&
        pitch >= copy.rowBytes && (!copy.rows || pitch <= NSUIntegerMax / copy.rows);
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)pitch bytesPerImage:(NSUInteger)imagePitch
    fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
    (void)imagePitch;
    if (_framebufferOnly || !bytes || self.storageMode != MTLStorageModeShared || ![self validRegion:region level:level slice:slice pitch:pitch]) {
        [NSException raise:NSInvalidArgumentException format:@"Invalid CPU texture read or private texture"]; return;
    }
    auto *owner = _backing->_owner; [owner->_transportLock lock]; publishCPU();
    auto surface=_backing->_surface;
    if (surface && IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr)!=KERN_SUCCESS) {
        [owner->_transportLock unlock];[NSException raise:NSInternalInconsistencyException format:@"IOSurface CPU read lock failed"];return;
    }
    auto plane=[self nativeTextureAtLevel:level slice:slice];
    GPU::Shader::TextureCopyRegion copy;GPU::Shader::textureCopyRegion(plane,region.origin.x,region.origin.y,region.size.width,region.size.height,copy);
    const auto *source = (const uint8_t *)(uintptr_t)_backing->_resource.cpuAddress + plane.offset + copy.offset;
    for (NSUInteger y = 0; y < copy.rows; ++y) memcpy((uint8_t *)bytes+y*pitch,source+y*plane.pitch,copy.rowBytes);
    auto status=surface?IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr):KERN_SUCCESS;
    [owner->_transportLock unlock];
    if (status) [NSException raise:NSInternalInconsistencyException format:@"IOSurface CPU read unlock failed"];
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes
    bytesPerRow:(NSUInteger)pitch bytesPerImage:(NSUInteger)imagePitch {
    (void)imagePitch;
    if (_framebufferOnly || !bytes || self.storageMode != MTLStorageModeShared || ![self validRegion:region level:level slice:slice pitch:pitch]) {
        [NSException raise:NSInvalidArgumentException format:@"Invalid CPU texture write or private texture"]; return;
    }
    auto *owner = _backing->_owner; [owner->_transportLock lock];
    auto surface=_backing->_surface;
    if (surface && IOSurfaceLock(surface,0,nullptr)!=KERN_SUCCESS) {
        [owner->_transportLock unlock];[NSException raise:NSInternalInconsistencyException format:@"IOSurface CPU write lock failed"];return;
    }
    auto plane=[self nativeTextureAtLevel:level slice:slice];
    GPU::Shader::TextureCopyRegion copy;GPU::Shader::textureCopyRegion(plane,region.origin.x,region.origin.y,region.size.width,region.size.height,copy);
    auto *destination = (uint8_t *)(uintptr_t)_backing->_resource.cpuAddress + plane.offset + copy.offset;
    for (NSUInteger y = 0; y < copy.rows; ++y) memcpy(destination+y*plane.pitch,(const uint8_t *)bytes+y*pitch,copy.rowBytes);
    publishCPU();auto status=surface?IOSurfaceUnlock(surface,0,nullptr):KERN_SUCCESS;[owner->_transportLock unlock];
    if (status) [NSException raise:NSInternalInconsistencyException format:@"IOSurface CPU write unlock failed"];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)pitch fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level {
    [self getBytes:bytes bytesPerRow:pitch bytesPerImage:0 fromRegion:region mipmapLevel:level slice:0];
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)pitch {
    [self replaceRegion:region mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:pitch bytesPerImage:0];
}
- (BOOL)blitPlane:(MTLBlitOption)options level:(NSUInteger)level descriptor:(GPU::Shader::Texture *)plane {
    return [self blitPlane:options level:level slice:0 descriptor:plane];
}
- (BOOL)blitPlane:(MTLBlitOption)options level:(NSUInteger)level slice:(NSUInteger)slice descriptor:(GPU::Shader::Texture *)plane {
    if (_framebufferOnly || slice>=textureSliceCount(self)) return NO;
    if (_format==MTLPixelFormatDepth32Float_Stencil8) {
        if (options!=MTLBlitOptionDepthFromDepthStencil && options!=MTLBlitOptionStencilFromDepthStencil) return NO;
        *plane=[self nativePlane:options==MTLBlitOptionStencilFromDepthStencil level:level slice:slice];
    } else {
        if (options!=MTLBlitOptionNone) return NO;
        *plane=[self nativeTextureAtLevel:level slice:slice];
    }
    return plane->resource!=0;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format {
    return [self newTextureViewWithPixelFormat:format textureType:_type levels:NSMakeRange(0,_levels) slices:NSMakeRange(0,textureSliceCount(self))];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type
    levels:(NSRange)levels slices:(NSRange)slices {
    return [[QcomMTLTexture alloc] initWithParent:self format:format type:type levels:levels slices:slices swizzle:0];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type
    levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swizzle {
    return [[QcomMTLTexture alloc] initWithParent:self format:format type:type levels:levels slices:slices swizzle:nativeSwizzle(swizzle)];
}
@end

@implementation QcomMTLSampler
+ (Protocol *)metalProtocol { return @protocol(MTLSamplerState); }
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLSamplerDescriptor *)d {
    if (!(self = [super init])) return nil;
    if (!d || d.minFilter > MTLSamplerMinMagFilterLinear ||
        d.magFilter > MTLSamplerMinMagFilterLinear || d.mipFilter > MTLSamplerMipFilterLinear ||
        !d.maxAnisotropy || d.maxAnisotropy>16 || d.sAddressMode>MTLSamplerAddressModeClampToBorderColor ||
        d.tAddressMode>MTLSamplerAddressModeClampToBorderColor || d.rAddressMode>MTLSamplerAddressModeClampToBorderColor || d.borderColor>MTLSamplerBorderColorOpaqueWhite ||
        d.compareFunction > MTLCompareFunctionAlways || d.supportArgumentBuffers || !__builtin_isfinite(d.lodMinClamp) ||
        !__builtin_isfinite(d.lodMaxClamp) || d.lodMinClamp<0 || d.lodMaxClamp<d.lodMinClamp) return nil;
    _owner = device;_state={uint32_t(d.minFilter)|(uint32_t(d.magFilter)<<1)|(uint32_t(d.mipFilter)<<2)|(uint32_t(d.compareFunction)<<8),
        uint32_t(MIN(d.lodMinClamp,4095.0f/256)*256),uint32_t(MIN(d.lodMaxClamp,4095.0f/256)*256),
        uint32_t(d.sAddressMode)|(uint32_t(d.tAddressMode)<<3)|(uint32_t(d.rAddressMode)<<6),uint32_t(d.maxAnisotropy),uint32_t(d.borderColor),uint32_t(d.normalizedCoordinates)};
    if(!GPU::Shader::validSampler(_state))return nil;
    self.label = d.label; return self;
}
- (id<MTLDevice>)device { return _owner; }
- (MTLResourceID)gpuResourceID { return {0}; } // Bindless samplers are not advertised.
@end

@implementation QcomMTLFunction
+ (Protocol *)metalProtocol { return @protocol(MTLFunction); }
- (instancetype)initWithDevice:(QcomMTLDevice *)device program:(const GPU::Shader::Program *)program binaryKey:(NSData *)key {
    if ((self = [super init])) {
        // Callers already derived or validated this function key. Hashing the
        // ISA here only produced a temporary key they immediately replaced.
        _owner=device;_program=program;_binaryKey=key;
        _maxThreads=(program->flags&QGPU_VARIABLE_LOCAL_SIZE)?GPU::Shader::MaxComputeThreads:64;
    } return self;
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device name:(NSString *)name stage:(unsigned)stage constants:(NSDictionary *)constants {
    if(!(self=[super init]) || stage>GPU::Shader::Fragment)return nil;
    _owner=device;_declaredName=[name copy];_reflectedStage=stage;_constantInfo=[constants copy];_maxThreads=GPU::Shader::MaxComputeThreads;return self;
}
- (id<MTLDevice>)device { return _owner; }
- (NSString *)name { return _declaredName ?: [NSString stringWithUTF8String:_program->name]; }
- (MTLFunctionType)functionType {
    unsigned stage=_program?_program->stage:_reflectedStage;
    return stage == GPU::Shader::Compute ? MTLFunctionTypeKernel : stage == GPU::Shader::Vertex ? MTLFunctionTypeVertex : MTLFunctionTypeFragment;
}
- (MTLPatchType)patchType { return MTLPatchTypeNone; }
- (NSInteger)patchControlPointCount { return 0; }
- (NSArray *)vertexAttributes { return @[]; }
- (NSArray *)stageInputAttributes { return @[]; }
- (NSDictionary *)functionConstantsDictionary { return _constantInfo ?: @{}; }
- (MTLFunctionOptions)options { return _options; }
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index { (void)index; return nil; }
@end

@implementation QcomMTLProgram
- (instancetype)initWithDevice:(QcomMTLDevice *)device image:(const QGPUProgramImage &)image error:(NSError **)error {
    if (!(self = [super init])) return nil;
    _owner = device; _image = [NSData dataWithBytes:&image length:sizeof(image)];
    GPU::JIT::ProgramData handle{};
    [_owner->_transportLock lock]; auto status = _owner->_connection.registerProgram(image,handle); [_owner->_transportLock unlock];
    if (status || handle.version != GPU::JIT::Version || handle.id < GPU::JIT::FirstID) {
        if (error) *error = status ? transportError(status) : shaderFailure(@"Invalid JIT registration response"); return nil;
    }
    const auto *stored = static_cast<const QGPUProgramImage *>(_image.bytes);
    memcpy(&program,&stored->info,sizeof(stored->info)); program.id = handle.id;
    program.code = stored->code; program.immediates = stored->immediates; program.name = stored->name;
    if (program.stage==GPU::Shader::Fragment) {
        memcpy(&_msaaProgram,&stored->msaa.info,sizeof(stored->msaa.info));_msaaProgram.id=handle.id;
        _msaaProgram.code=stored->msaa.code;_msaaProgram.immediates=stored->msaa.immediates;_msaaProgram.name=stored->name;
        program.msaa=&_msaaProgram;
    }
    return self;
}
- (void)dealloc {
    if (program.id) {
        [_owner->_transportLock lock]; _owner->_connection.deleteProgram(program.id); [_owner->_transportLock unlock];
    }
}
@end

@implementation QcomMTLBinaryArchive
+ (Protocol *)metalProtocol {return @protocol(MTLBinaryArchive);}
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error {
    if(![descriptor isKindOfClass:[MTLBinaryArchiveDescriptor class]]) {
        if(error)*error=archiveFailure(@"A native binary archive descriptor is required",MTLBinaryArchiveErrorInvalidFile);return nil;
    }
    NSURL *url=[descriptor.url copy];
    if(!url)return [self initWithDevice:device archiveData:nil error:error];
    if(!url.isFileURL){if(error)*error=archiveFailure(@"Binary archives require a local file URL",MTLBinaryArchiveErrorInvalidFile);return nil;}
    // Explicit MTLBinaryArchive URLs retain their bounded file read. The
    // automatic shader cache supplies an owned CoreFSCache payload instead.
    NSError *readError=nil;NSFileHandle *file=[NSFileHandle fileHandleForReadingFromURL:url error:&readError];
    NSData *data=file?[file readDataUpToLength:Archive::MaxBytes+1 error:&readError]:nil;
    if(file)[file closeAndReturnError:nullptr];
    if(!data){if(error)*error=readError?:archiveFailure(@"Could not read A540 binary archive",MTLBinaryArchiveErrorInvalidFile);return nil;}
    return [self initWithDevice:device archiveData:data error:error];
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device archiveData:(NSData *)data error:(NSError **)error {
    if(!(self=[super init]))return nil;
    _owner=device;_lock=[NSLock new];_registered=[NSMapTable strongToWeakObjectsMapTable];
    if(!data)return self;
    if(data.length<Archive::HeaderBytes || data.length>Archive::MaxBytes) {
        if(error)*error=archiveFailure(@"Invalid or oversized A540 binary archive",MTLBinaryArchiveErrorInvalidFile);return nil;
    }
    Archive::Contents contents;
    if(!Archive::decode(data.bytes,data.length,archiveContext(),GPUPrograms::Registry,archiveFileDigest(data.bytes,data.length),contents)) {
        if(error)*error=archiveFailure(@"Incompatible, stale or corrupt A540 binary archive",MTLBinaryArchiveErrorInvalidFile);return nil;
    }
    for(auto &function:contents.functions) {
        if(![[NSString alloc] initWithBytes:function.name.data() length:function.name.size() encoding:NSUTF8StringEncoding]) {
            if(error)*error=archiveFailure(@"Invalid archive function name",MTLBinaryArchiveErrorInvalidFile);return nil;
        }
        if(function.kind==Archive::AOT) {
            const GPU::Shader::Program *program=nullptr;
            for(const auto &candidate:GPUPrograms::Programs)if(candidate.id==function.aot){program=&candidate;break;}
            if(!program || function.name!=program->name || function.stage!=program->stage ||
                function.maxThreads>((program->flags&QGPU_VARIABLE_LOCAL_SIZE)?GPU::Shader::MaxComputeThreads:64)) {
                if(error)*error=archiveFailure(@"Archive AOT reference does not match this driver",MTLBinaryArchiveErrorInvalidFile);return nil;
            }
        } else {
            const auto *image=reinterpret_cast<const QGPUProgramImage *>(function.image.data());
            if(function.maxThreads>((image->info.flags&QGPU_VARIABLE_LOCAL_SIZE)?GPU::Shader::MaxComputeThreads:64)) {
                if(error)*error=archiveFailure(@"Archive thread limit exceeds its compiled function",MTLBinaryArchiveErrorInvalidFile);return nil;
            }
        }
        _functions.emplace(function.key,std::move(function));
    }
    for(const auto &pipeline:contents.pipelines)_pipelines.emplace(pipeline.key,pipeline);
    return self;
}
- (id<MTLDevice>)device {return _owner;}
- (QcomMTLFunction *)functionForKey:(NSData *)key name:(NSString *)name stage:(unsigned)stage maxThreads:(NSUInteger)limit
    found:(BOOL *)found error:(NSError **)error {
    *found=NO;if(key.length!=32)return nil;
    [_lock lock];
    @try {
        auto at=_functions.find(archiveKey(key));
        if(at==_functions.end() || at->second.name!=name.UTF8String || at->second.stage!=stage || at->second.maxThreads>limit)return nil;
        *found=YES;const auto &record=at->second;QcomMTLFunction *function=nil;
        if(record.kind==Archive::AOT) {
            for(const auto &program:GPUPrograms::Programs)if(program.id==record.aot) {
                function=[[QcomMTLFunction alloc] initWithDevice:_owner program:&program binaryKey:key];break;
            }
        } else {
            QcomMTLProgram *program=[_registered objectForKey:key];
            if(!program) {
                // Normal kernel JIT admission still validates the pointer-free
                // image and retains code for every in-flight native job.
                const auto *image=reinterpret_cast<const QGPUProgramImage *>(record.image.data());
                program=[[QcomMTLProgram alloc] initWithDevice:_owner image:*image error:error];
                if(!program)return nil;
                [_registered setObject:program forKey:key];
            }
            function=[[QcomMTLFunction alloc] initWithDevice:_owner program:&program->program binaryKey:key];if(function)function->_dynamicProgram=program;
        }
        if(!function){if(error)*error=archiveFailure(@"Could not restore the compiled A540 function",MTLBinaryArchiveErrorInternalError);return nil;}
        function->_maxThreads=MIN(function->_maxThreads,record.maxThreads);return function;
    } @finally {[_lock unlock];}
}
- (BOOL)containsPipeline:(const Archive::Pipeline &)pipeline {
    [_lock lock];auto found=_pipelines.find(pipeline.key);
    BOOL match=found!=_pipelines.end() && found->second.kind==pipeline.kind &&
        found->second.functions[0]==pipeline.functions[0] && found->second.functions[1]==pipeline.functions[1];
    [_lock unlock];return match;
}
- (BOOL)addFunctions:(NSArray<QcomMTLFunction *> *)functions pipeline:(const Archive::Pipeline &)pipeline error:(NSError **)error {
    if((pipeline.kind!=Archive::Compute && pipeline.kind!=Archive::Render) ||
        (pipeline.kind==Archive::Compute && Archive::nonzero(pipeline.functions[1])) ||
        functions.count!=(pipeline.kind==Archive::Compute?1u:2u) || !Archive::nonzero(pipeline.key)) {
        if(error)*error=archiveFailure(@"Invalid binary archive pipeline record");return NO;
    }
    std::vector<Archive::Function> records;
    for(QcomMTLFunction *function in functions) {
        if(!function || function->_owner!=_owner || !function->_program || function->_binaryKey.length!=32) {
            if(error)*error=archiveFailure(@"Only compiled functions from this device can be archived");return NO;
        }
        Archive::Function record;record.key=archiveKey(function->_binaryKey);record.stage=function->_program->stage;
        record.name=function->_program->name;record.maxThreads=uint32_t(function->_maxThreads);
        if([function->_dynamicProgram isKindOfClass:[QcomMTLProgram class]]) {
            auto program=(QcomMTLProgram *)function->_dynamicProgram;record.kind=Archive::Compiled;
            const auto *begin=static_cast<const uint8_t *>(program->_image.bytes);record.image.assign(begin,begin+program->_image.length);
        } else {
            bool registered=NO;for(const auto &program:GPUPrograms::Programs)if(function->_program==&program){registered=YES;break;}
            if(!registered){if(error)*error=archiveFailure(@"The AOT function is not in this driver");return NO;}
            record.kind=Archive::AOT;record.aot=function->_program->id;
        }
        if(!Archive::validFunction(record,GPUPrograms::Registry)){if(error)*error=archiveFailure(@"Invalid compiled function metadata");return NO;}
        records.push_back(std::move(record));
    }
    for(NSUInteger i=0;i<records.size();++i)if(records[i].key!=pipeline.functions[i] ||
        records[i].stage!=(pipeline.kind==Archive::Compute?GPU::Shader::Compute:i?GPU::Shader::Fragment:GPU::Shader::Vertex)) {
        if(error)*error=archiveFailure(@"Pipeline functions do not match the archive record");return NO;
    }
    [_lock lock];
    @try {
        size_t added=0;for(const auto &record:records)if(!_functions.count(record.key))++added;
        if(_functions.size()+added>Archive::MaxFunctions || (_pipelines.size()>=Archive::MaxPipelines && !_pipelines.count(pipeline.key))) {
            if(error)*error=archiveFailure(@"Binary archive entry quota exceeded",MTLBinaryArchiveErrorInternalError);return NO;
        }
        for(const auto &record:records) {
            auto at=_functions.find(record.key);
            if(at!=_functions.end() && (at->second.name!=record.name || at->second.stage!=record.stage || at->second.maxThreads!=record.maxThreads)) {
                if(error)*error=archiveFailure(@"Conflicting function identity in binary archive");return NO;
            }
        }
        for(auto &record:records)_functions.emplace(record.key,std::move(record));
        _pipelines.emplace(pipeline.key,pipeline);return YES;
    } @finally {[_lock unlock];}
}
- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)descriptor error:(NSError **)error {
    if(error)*error=nil;
    if(![descriptor isKindOfClass:[MTLComputePipelineDescriptor class]]){if(error)*error=archiveFailure(@"A compute pipeline descriptor is required");return NO;}
    descriptor=[descriptor copy];descriptor.binaryArchives=nil;
    auto pipeline=(QcomMTLComputePipeline *)[_owner newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone reflection:nullptr error:error];
    return pipeline?[self addFunctions:@[pipeline->_function] pipeline:archivePipeline(pipeline) error:error]:NO;
}
- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    if(error)*error=nil;
    if(![descriptor isKindOfClass:[MTLRenderPipelineDescriptor class]]){if(error)*error=archiveFailure(@"A render pipeline descriptor is required");return NO;}
    descriptor=[descriptor copy];descriptor.binaryArchives=nil;
    auto pipeline=(QcomMTLRenderPipeline *)[_owner newRenderPipelineStateWithDescriptor:descriptor error:error];
    return pipeline?[self addFunctions:@[pipeline->_vertex,pipeline->_fragment] pipeline:archivePipeline(pipeline) error:error]:NO;
}
- (BOOL)addTileRenderPipelineFunctionsWithDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    (void)descriptor;if(error)*error=archiveFailure(@"A540 tile pipelines are not supported");return NO;
}
- (BOOL)addMeshRenderPipelineFunctionsWithDescriptor:(MTLMeshRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    (void)descriptor;if(error)*error=archiveFailure(@"A540 mesh pipelines are not supported");return NO;
}
- (BOOL)addLibraryWithDescriptor:(MTLStitchedLibraryDescriptor *)descriptor error:(NSError **)error {
    (void)descriptor;if(error)*error=archiveFailure(@"A540 stitched libraries are not supported");return NO;
}
- (BOOL)addFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor library:(id<MTLLibrary>)library error:(NSError **)error {
    (void)descriptor;(void)library;if(error)*error=archiveFailure(@"Visible/intersection functions are not supported; archive supported stage functions through their pipeline descriptors");return NO;
}
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error {
    if(error)*error=nil;
    if(!url.isFileURL){if(error)*error=archiveFailure(@"Binary archive output requires a local file URL",MTLBinaryArchiveErrorInvalidFile);return NO;}
    Archive::Contents contents;[_lock lock];
    @try {for(const auto &entry:_functions)contents.functions.push_back(entry.second);for(const auto &entry:_pipelines)contents.pipelines.push_back(entry.second);}
    @finally {[_lock unlock];}
    std::vector<uint8_t> bytes;
    if(!Archive::encode(contents,archiveContext(),GPUPrograms::Registry,bytes)) {
        if(error)*error=archiveFailure(@"Could not serialize the bounded A540 binary archive",MTLBinaryArchiveErrorInternalError);return NO;
    }
    auto digest=archiveFileDigest(bytes.data(),bytes.size());memcpy(bytes.data()+Archive::DigestOffset,digest.data(),digest.size());
    return [[NSData dataWithBytes:bytes.data() length:bytes.size()] writeToURL:url options:NSDataWritingAtomic error:error];
}
@end

@implementation QcomMTLLibrary
+ (Protocol *)metalProtocol { return @protocol(MTLLibrary); }
- (instancetype)initWithDevice:(QcomMTLDevice *)device names:(NSArray *)names {
    if ((self = [super init])) {
        _owner = device; _names = [names copy];_maxThreads=GPU::Shader::MaxComputeThreads;
        _binaryIdentity=builtinLibraryIdentity();
        _entryLock=[NSLock new];_programs=[NSMutableDictionary new];_reflectionCache=[NSMutableDictionary new];
    } return self;
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device images:(const std::vector<QGPUProgramImage> &)images error:(NSError **)error {
    if (!(self = [self initWithDevice:device names:@[]])) return nil;
    NSMutableDictionary *programs = [NSMutableDictionary new]; NSMutableArray *names = [NSMutableArray new];
    for (const auto &image : images) {
        NSString *name = [NSString stringWithUTF8String:image.name];
        if (!name) { if (error) *error = shaderFailure(@"AIR entry names must be valid UTF-8"); return nil; }
        QcomMTLProgram *program = [[QcomMTLProgram alloc] initWithDevice:device image:image error:error];
        if (!program) return nil; // ARC rolls back all earlier registrations.
        programs[name] = program; [names addObject:name];
    }
    _programs = programs; _names = [names copy];
    _binaryIdentity=keyData(archiveHash(images.data(),images.size()*sizeof(QGPUProgramImage)));return self;
}
- (instancetype)initWithDevice:(QcomMTLDevice *)device archive:(NSData *)data identity:(NSData *)identity error:(NSError **)error {
    if(!(self=[self initWithDevice:device names:@[]]))return nil;
    std::string diagnostic;
    if(!libraryIndex(data,_archiveFunctions,diagnostic)) {
        if(error)*error=shaderFailure([NSString stringWithUTF8String:diagnostic.c_str()]);return nil;
    }
    NSMutableArray *names=[NSMutableArray new];
    for(const auto &function:_archiveFunctions)[names addObject:[NSString stringWithUTF8String:function.name.c_str()]];
    _archive=data;_names=[names copy];_binaryIdentity=identity;return self;
}
- (QcomMTLLibrary *)withMaxThreads:(NSUInteger)limit {
    if (!limit) return self;
    // The source cache owns generic code. A per-call wrapper carries the
    // option without changing another library's functions or registrations.
    QcomMTLLibrary *copy=[[QcomMTLLibrary alloc] initWithDevice:_owner names:_names];
    copy->_programs=_programs;copy->_entryLock=_entryLock;copy->_archive=_archive;copy->_archiveFunctions=_archiveFunctions;
    copy->_reflectionCache=_reflectionCache;copy->_binaryIdentity=_binaryIdentity;
    copy->_compiledLibrary=self;copy->_maxThreads=MIN(_maxThreads,limit);return copy;
}
- (id<MTLDevice>)device { return _owner; }
- (NSArray<NSString *> *)functionNames { return _names; }
- (MTLLibraryType)type { return MTLLibraryTypeExecutable; }
- (NSString *)installName { return nil; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
    return [self functionWithName:name values:nil specialized:NO error:nullptr];
}
- (id<MTLFunction>)functionWithName:(NSString *)name values:(MTLFunctionConstantValues *)values specialized:(BOOL)specialized error:(NSError **)error {
    return [self functionWithName:name values:values specialized:specialized archives:nil failOnMiss:NO error:error];
}
- (id<MTLFunction>)functionWithName:(NSString *)name values:(MTLFunctionConstantValues *)values specialized:(BOOL)specialized
    archives:(NSArray *)archives failOnMiss:(BOOL)fail error:(NSError **)error {
    NSError *__autoreleasing diagnosticError=nil;if(!error)error=&diagnosticError;*error=nil;
    MetalTimingScope timing(_owner,GPU::MetalTiming::Function,uint64_t(name.hash));
    MetalFailureScope failureScope(_owner,GPU::Diagnostics::Function,error);
    if(!validBinaryArchives(archives,_owner,error))return nil;
    if (![_names containsObject:name]) {if(error)*error=shaderFailure(@"Function is not in this Metal library");return nil;}
    std::vector<GPU::Compiler::FunctionConstantValue> supplied;
    if(specialized && _archive && !snapshotConstants(values,supplied,error))return nil;
    [_entryLock lock];NSString *key=name;NSData *binaryKey=nil;
    if(_archive) {
        const GPU::Compiler::LibraryArchive::Function *selected=nullptr;
        for(const auto &function:_archiveFunctions)if(function.name==name.UTF8String){selected=&function;break;}
        if(!selected){[_entryLock unlock];if(error)*error=shaderFailure(@"Function is absent from the AIR index");return nil;}
        static_assert(GPUSystemShaders::Registry==GPUPrograms::Registry,"System shader registry mismatch");
        // libraryIndex already verifies the full AIR payload digest. The
        // bundled catalog matches that digest AND the entry name, never just
        // a known system function name or a shortened diagnostic hash.
        const auto *system=GPU::SystemShaders::find(GPUSystemShaders::Entries,GPUSystemShaders::Count,selected->hash,selected->name.c_str());
        const char *catalogState=system?"variant-miss":"entry-miss";
        NSDictionary *reflection=_reflectionCache[name];std::string diagnostic;
        if(!reflection) {
            std::vector<GPU::Compiler::FunctionConstant> constants;unsigned stage=UINT32_MAX;
            NSData *reflectionKey=functionBinaryKey(_binaryIdentity,name,_maxThreads);
            bool restored=false;
            if(system){GPU::SystemShaders::reflect(*system,constants,stage);restored=true;timing.record.detail|=0x400;}
            else restored=ProcessAIRCache::shared().reflection(reflectionKey,constants,stage);
            bool ok=restored;
            if(restored){if(!system)timing.record.detail|=64;}
            else {
                const auto inspectWait=mach_absolute_time();[_owner->_compilerLock lock];const auto inspectBegin=mach_absolute_time();
                ok=GPU::Compiler::inspectAIRConstants(
                    static_cast<const uint8_t *>(_archive.bytes)+selected->offset,selected->size,selected->name,constants,stage,diagnostic);
                const auto inspectEnd=mach_absolute_time();[_owner->_compilerLock unlock];
                metalTimingSpan(_owner,GPU::MetalTiming::CompilerLock,inspectWait,inspectBegin,timing.record.identity);
                metalTimingSpan(_owner,GPU::MetalTiming::AIRInspect,inspectBegin,inspectEnd,timing.record.identity);
            }
            unsigned expected=selected->type==0?1:selected->type==1?2:0;
            if(!ok || stage!=expected) {[_entryLock unlock];if(error)*error=shaderFailure(ok?@"MTLB TYPE disagrees with AIR stage":[NSString stringWithUTF8String:diagnostic.c_str()]);return nil;}
            NSDictionary *info=constantReflection(constants,error);
            if(!info){[_entryLock unlock];return nil;}
            // Cache canonical C++ metadata, then construct fresh native
            // reflection objects for each library; clients cannot mutate a
            // different library's cached MTLFunctionConstant objects.
            if(!restored)ProcessAIRCache::shared().rememberReflection(reflectionKey,constants,stage);
            reflection=@{@"stage":@(stage),@"constants":info};_reflectionCache[name]=reflection;
        }
        NSDictionary *info=reflection[@"constants"];
        if(!specialized && info.count) {
            QcomMTLFunction *function=[[QcomMTLFunction alloc] initWithDevice:_owner name:name stage:[reflection[@"stage"] unsignedIntValue] constants:info];
            function->_maxThreads=MIN(function->_maxThreads,_maxThreads);[_entryLock unlock];return function;
        }
        std::vector<GPU::Compiler::FunctionConstant> declarations;
        for(NSString *constantName in info) {
            MTLFunctionConstant *constant=info[constantName];
            declarations.push_back({constantName.UTF8String,uint32_t(constant.index),uint32_t(constant.type),bool(constant.required)});
        }
        std::vector<GPU::Compiler::FunctionConstantValue> effective;
        if(!GPU::Compiler::resolveFunctionConstants(declarations,supplied,effective,diagnostic)) {
            [_entryLock unlock];if(error)*error=shaderFailure([NSString stringWithUTF8String:diagnostic.c_str()]);return nil;
        }
        key=constantCacheKey(name,effective);
        binaryKey=functionBinaryKey(_binaryIdentity,key,_maxThreads);
        memcpy(&timing.record.argument,binaryKey.bytes,sizeof(timing.record.argument));
        for(QcomMTLBinaryArchive *archive in archives) {
            BOOL found=NO;QcomMTLFunction *cached=[archive functionForKey:binaryKey name:name stage:[reflection[@"stage"] unsignedIntValue]
                maxThreads:_maxThreads found:&found error:error];
            if(found){captureArchivedNativeShader(_owner,cached,_archive,*selected,effective);[_entryLock unlock];return cached;}
        }
        if(fail){[_entryLock unlock];if(error)*error=archiveFailure(@"No matching A540 function binary for these AIR constants");return nil;}
        if(!_programs[key] && system && _owner->_jit) {
            if(const auto *variant=GPU::SystemShaders::match(*system,effective)) {
                catalogState="restore-failed";
                auto image=std::make_unique<QGPUProgramImage>();
                if(GPU::SystemShaders::restore(*system,*variant,GPUPrograms::Registry,*image)) {
                    const auto begin=mach_absolute_time();
                    QcomMTLProgram *program=[[QcomMTLProgram alloc] initWithDevice:_owner image:*image error:error];
                    metalTimingSpan(_owner,GPU::MetalTiming::ProgramRegister,begin,mach_absolute_time(),timing.record.identity);
                    if(!program){[_entryLock unlock];return nil;}
                    _programs[key]=program;timing.record.detail|=0x800;
                    if(_owner->_shaderCaptureEnabled) {
                        QcomMTLFunction *captured=[[QcomMTLFunction alloc] initWithDevice:_owner program:&program->program binaryKey:binaryKey];
                        captured->_dynamicProgram=program;
                        captureArchivedNativeShader(_owner,captured,_archive,*selected,effective);
                    }
                }
            }
        }
        if(!_programs[key]) {
            QcomMTLFunction *cached=cachedCompiledFunction(_owner,binaryKey,name,[reflection[@"stage"] unsignedIntValue],_maxThreads,timing.record.detail);
            if(cached) {
                _programs[key]=cached->_dynamicProgram;
                // Function timing detail: 1=persistent hit, 2=stored,
                // 4=store failed, 8=process temporary hit, 16=temporary stored,
                // 32=deterministic AIR failure hit, 64=process reflection hit,
                // 512=native CoreFSCache storage (also on successful writes),
                // 1024=bundled reflection, 2048=bundled exact A540 binary.
                captureArchivedNativeShader(_owner,cached,_archive,*selected,effective);
                [_entryLock unlock];return cached;
            }
        }
        if(!_programs[key]) {
            if(!_owner->_jit){[_entryLock unlock];if(error)*error=shaderFailure(@"A compatible JIT backend is required");return nil;}
            auto rejectRemembered=[&]()->QcomMTLFunction * {
                timing.record.detail|=32;
                diagnostic+="\n"+airFailureContext(*selected,static_cast<const uint8_t *>(_archive.bytes)+selected->offset,effective);
                [_entryLock unlock];if(error)*error=shaderFailure([NSString stringWithUTF8String:diagnostic.c_str()]);return nil;
            };
            if(ProcessAIRCache::shared().failure(binaryKey,diagnostic))return rejectRemembered();
            std::vector<GPU::Compiler::Entry> entries;std::vector<QGPUProgramImage> images;
            const auto compileWait=mach_absolute_time();[_owner->_compilerLock lock];const auto lowerBegin=mach_absolute_time();
            // Another library can finish the same unsupported variant while
            // this caller waits for LLVM. Do not repeat its deterministic work.
            if(ProcessAIRCache::shared().failure(binaryKey,diagnostic)) {
                [_owner->_compilerLock unlock];
                metalTimingSpan(_owner,GPU::MetalTiming::CompilerLock,compileWait,lowerBegin,timing.record.identity);
                return rejectRemembered();
            }
            GPU::Compiler::AIRCompileInfo compileInfo;
            bool ok=GPU::Compiler::compileAIR(static_cast<const uint8_t *>(_archive.bytes)+selected->offset,
                selected->size,GPUPrograms::Registry,images,diagnostic,selected->name,effective,&compileInfo);
            if(!ok && compileInfo.lowerFailed)ProcessAIRCache::shared().rememberFailure(binaryKey,selected->name,diagnostic);
            if(ok && (images.size()!=1 || images[0].info.stage!=[reflection[@"stage"] unsignedIntValue])) {ok=false;diagnostic="MTLB TYPE disagrees with specialized AIR stage";}
            const auto backendEnd=mach_absolute_time();
            const auto lowerEnd=airLowerBoundary(lowerBegin,backendEnd,compileInfo);
            if(compileInfo.route==GPU::Compiler::AIRRoute::NIR)timing.record.detail|=128;
            else if(compileInfo.route==GPU::Compiler::AIRRoute::GLSLFallback)timing.record.detail|=256;
            if(ok && _owner->_shaderCaptureEnabled){std::string captureError;
                if(GPU::Compiler::lowerAIR(static_cast<const uint8_t *>(_archive.bytes)+selected->offset,
                    selected->size,entries,captureError,selected->name,effective) && entries.size()==1)
                    captureNativeShader(_owner,entries[0],images[0],*selected,effective,compileInfo.route==GPU::Compiler::AIRRoute::NIR?"metallib-air-nir":"metallib-air");}
            QcomMTLProgram *program=ok && images.size()==1?[[QcomMTLProgram alloc] initWithDevice:_owner image:images[0] error:error]:nil;
            const auto registerEnd=mach_absolute_time();[_owner->_compilerLock unlock];
            metalTimingSpan(_owner,GPU::MetalTiming::CompilerLock,compileWait,lowerBegin,timing.record.identity);
            metalTimingSpan(_owner,GPU::MetalTiming::AIRLower,lowerBegin,lowerEnd,timing.record.identity);
            metalTimingSpan(_owner,GPU::MetalTiming::Backend,lowerEnd,backendEnd,timing.record.identity);
            metalTimingSpan(_owner,GPU::MetalTiming::ProgramRegister,backendEnd,registerEnd,timing.record.identity);
            recordShaderCacheMiss(_owner,binaryKey,_binaryIdentity,*selected,effective,_maxThreads,
                catalogState,compileInfo,ok,timing.record.identity);
            if(!program) {
                // Keep the actual failure first: a large constant list can
                // exceed the bounded kernel diagnostic message by itself.
                if(!diagnostic.empty())diagnostic+="\n"+airFailureContext(*selected,static_cast<const uint8_t *>(_archive.bytes)+selected->offset,effective);
                [_entryLock unlock];if(!diagnostic.empty() && error)*error=shaderFailure([NSString stringWithUTF8String:diagnostic.c_str()]);return nil;
            }
            _programs[key]=program;
            timing.record.detail|=storeCompiledFunction(program,binaryKey,_maxThreads);
        }
    }
    if(!binaryKey) {
        binaryKey=functionBinaryKey(_binaryIdentity,key,_maxThreads);
        memcpy(&timing.record.argument,binaryKey.bytes,sizeof(timing.record.argument));
        QcomMTLProgram *dynamic=_programs[key];unsigned stage=UINT32_MAX;
        if(dynamic)stage=dynamic->program.stage;
        else for(const auto &program:GPUPrograms::Programs)if([name isEqualToString:[NSString stringWithUTF8String:program.name]]){stage=program.stage;break;}
        for(QcomMTLBinaryArchive *archive in archives) {
            BOOL found=NO;QcomMTLFunction *cached=[archive functionForKey:binaryKey name:name stage:stage maxThreads:_maxThreads found:&found error:error];
            if(found){[_entryLock unlock];return cached;}
        }
        if(fail){[_entryLock unlock];if(error)*error=archiveFailure(@"No matching A540 function binary in the supplied archives");return nil;}
    }
    QcomMTLProgram *dynamic=_programs[key];[_entryLock unlock];
    if(dynamic) {
        QcomMTLFunction *function=[[QcomMTLFunction alloc] initWithDevice:_owner program:&dynamic->program binaryKey:binaryKey];
        function->_dynamicProgram=dynamic;function->_maxThreads=MIN(function->_maxThreads,_maxThreads);return function;
    }
    for(const auto &program:GPUPrograms::Programs)if([name isEqualToString:[NSString stringWithUTF8String:program.name]]) {
        QcomMTLFunction *function=[[QcomMTLFunction alloc] initWithDevice:_owner program:&program binaryKey:binaryKey];
        function->_maxThreads=MIN(function->_maxThreads,_maxThreads);return function;
    }
    return nil;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
    return [self functionWithName:name values:values specialized:YES error:error];
}
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor error:(NSError **)error {
    if(error)*error=nil;
    if(![descriptor isKindOfClass:[MTLFunctionDescriptor class]] ||
        [descriptor isKindOfClass:[MTLIntersectionFunctionDescriptor class]]) {
        if(error)*error=shaderFailure(@"A native vertex, fragment or kernel function descriptor is required",MTLLibraryErrorUnsupported);return nil;
    }
    descriptor=[descriptor copy];
    if(!descriptor.name.length || !descriptor.name.UTF8String ||
        (descriptor.specializedName && (!descriptor.specializedName.length || !descriptor.specializedName.UTF8String))) {
        if(error)*error=shaderFailure(@"Invalid native function descriptor name");return nil;
    }
    if(descriptor.options&~MTLFunctionOptionFailOnBinaryArchiveMiss) {
        if(error)*error=shaderFailure(@"Visible function binaries and pipeline-script recording are not supported",MTLLibraryErrorUnsupported);return nil;
    }
    if(![_names containsObject:descriptor.name]) {
        if(error)*error=shaderFailure(@"Function is not in this Metal library",MTLLibraryErrorFunctionNotFound);return nil;
    }
    QcomMTLFunction *function=(QcomMTLFunction *)[self functionWithName:descriptor.name values:descriptor.constantValues specialized:YES
        archives:descriptor.binaryArchives failOnMiss:bool(descriptor.options&MTLFunctionOptionFailOnBinaryArchiveMiss) error:error];
    if(function) {function->_declaredName=[descriptor.specializedName?:descriptor.name copy];function->_options=descriptor.options;}
    return function;
}
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
    if(!handler)return;descriptor=[descriptor copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{@autoreleasepool {
        NSError *error=nil;id<MTLFunction> function=[self newFunctionWithDescriptor:descriptor error:&error];handler(function,error);
    }});
}
- (void)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
    if(!handler)return;name=[name copy];values=[values copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{@autoreleasepool {
        NSError *error=nil;id<MTLFunction> function=[self newFunctionWithName:name constantValues:values error:&error];handler(function,error);
    }});
}

@end

@implementation QcomMTLComputePipeline
+ (Protocol *)metalProtocol { return @protocol(MTLComputePipelineState); }
- (instancetype)initWithFunction:(QcomMTLFunction *)function {
    if ((self = [super init])) {_function=function;_maxThreads=function->_maxThreads;} return self;
}
- (id<MTLDevice>)device { return _function.device; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return _maxThreads; }
- (NSUInteger)threadExecutionWidth { return _function->_program->subgroupSize; }
- (NSUInteger)staticThreadgroupMemoryLength { return _function->_program->sharedBytes; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (MTLShaderValidation)shaderValidation { return MTLShaderValidationDisabled; }
- (MTLResourceID)gpuResourceID { return {_function->_program->id}; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions { (void)dimensions; return 0; }
@end

@implementation QcomMTLDepthStencilState
+ (Protocol *)metalProtocol { return @protocol(MTLDepthStencilState); }
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLDepthStencilDescriptor *)d {
    if (!(self=[super init]) || !d || d.depthCompareFunction>MTLCompareFunctionAlways) return nil;
    _owner=device;_state=GPU::Shader::defaultDepthStencilState();
    _state.compare=uint32_t(d.depthCompareFunction);_state.write=d.depthWriteEnabled;
    auto face=[](MTLStencilDescriptor *desc,GPU::Shader::StencilFace &f) {
        if (!desc) return true;
        if (desc.stencilCompareFunction>MTLCompareFunctionAlways || desc.stencilFailureOperation>MTLStencilOperationDecrementWrap ||
            desc.depthFailureOperation>MTLStencilOperationDecrementWrap || desc.depthStencilPassOperation>MTLStencilOperationDecrementWrap) return false;
        f={1,uint32_t(desc.stencilCompareFunction),uint32_t(desc.stencilFailureOperation),uint32_t(desc.depthFailureOperation),
            uint32_t(desc.depthStencilPassOperation),desc.readMask&255,desc.writeMask&255};return true;
    };
    if (!face(d.frontFaceStencil,_state.front) || !face(d.backFaceStencil,_state.back)) return nil;
    self.label=d.label;return self;
}
- (id<MTLDevice>)device { return _owner; }
@end

@implementation QcomMTLRenderPipeline
+ (Protocol *)metalProtocol { return @protocol(MTLRenderPipelineState); }
- (instancetype)initWithDevice:(QcomMTLDevice *)device descriptor:(MTLRenderPipelineDescriptor *)d {
    if (!(self = [super init])) return nil;
    QcomMTLFunction *vertex = [d.vertexFunction isKindOfClass:[QcomMTLFunction class]] ? (QcomMTLFunction *)d.vertexFunction : nil;
    QcomMTLFunction *fragment = [d.fragmentFunction isKindOfClass:[QcomMTLFunction class]] ? (QcomMTLFunction *)d.fragmentFunction : nil;
    if(!d.fragmentFunction) {
        bool colors=false;
        for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)colors|=d.colorAttachments[i].pixelFormat!=MTLPixelFormatInvalid;
        if(colors || (d.depthAttachmentPixelFormat==MTLPixelFormatInvalid && d.stencilAttachmentPixelFormat==MTLPixelFormatInvalid))return nil;
        // Keep ordinary IOGPU submission and rasterization for a depth/stencil
        // prepass. This shader exports no color or replacement depth value.
        for(const auto &program:GPUPrograms::Programs)if(!strcmp(program.name,"raster_depth_only")) {
            NSData *key=functionBinaryKey(builtinLibraryIdentity(),[NSString stringWithUTF8String:program.name],GPU::Shader::MaxComputeThreads);
            fragment=[[QcomMTLFunction alloc] initWithDevice:device program:&program binaryKey:key];break;
        }
    }
    if (!vertex || !fragment || !vertex->_program || !fragment->_program || vertex.device != device || fragment.device != device ||
        vertex->_program->stage != GPU::Shader::Vertex || fragment->_program->stage != GPU::Shader::Fragment ||
        (d.rasterSampleCount!=1 && d.rasterSampleCount!=2 && d.rasterSampleCount!=4) || !d.rasterizationEnabled || d.alphaToOneEnabled ||
        (d.depthAttachmentPixelFormat!=MTLPixelFormatInvalid && !depthFormat(d.depthAttachmentPixelFormat)) ||
        (d.stencilAttachmentPixelFormat!=MTLPixelFormatInvalid && !stencilFormat(d.stencilAttachmentPixelFormat)) ||
        d.tessellationPartitionMode != MTLTessellationPartitionModePow2 ||
        d.supportIndirectCommandBuffers) return nil;
    if(d.maxVertexAmplificationCount>1 ||
        (d.inputPrimitiveTopology!=MTLPrimitiveTopologyClassUnspecified && d.inputPrimitiveTopology!=MTLPrimitiveTopologyClassTriangle) ||
        d.vertexPreloadedLibraries.count || d.fragmentPreloadedLibraries.count ||
        !emptyLinkedFunctions(d.vertexLinkedFunctions) || !emptyLinkedFunctions(d.fragmentLinkedFunctions) ||
        d.supportAddingVertexBinaryFunctions || d.supportAddingFragmentBinaryFunctions ||
        d.maxVertexCallStackDepth>1 || d.maxFragmentCallStackDepth>1 || d.shaderValidation==MTLShaderValidationEnabled)return nil;
    GPU::Shader::Linkage linkage;
    if(!GPU::Shader::link(*vertex->_program,GPU::Shader::rasterProgram(*fragment->_program,uint32_t(d.rasterSampleCount)),linkage))return nil;
    bool attachmentPresent=d.depthAttachmentPixelFormat!=MTLPixelFormatInvalid || d.stencilAttachmentPixelFormat!=MTLPixelFormatInvalid;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i) {
        auto color=d.colorAttachments[i];_formats[i]=color.pixelFormat;
        if(color.pixelFormat==MTLPixelFormatInvalid) {
            if(fragment->_program->framebufferMask&(1u<<i))return nil;
            continue;
        }
        if(!renderableColorFormat(color.pixelFormat) || (color.writeMask&~MTLColorWriteMaskAll) ||
            color.sourceRGBBlendFactor>MTLBlendFactorOneMinusBlendAlpha || color.destinationRGBBlendFactor>MTLBlendFactorOneMinusBlendAlpha ||
            color.sourceAlphaBlendFactor>MTLBlendFactorOneMinusBlendAlpha || color.destinationAlphaBlendFactor>MTLBlendFactorOneMinusBlendAlpha ||
            color.rgbBlendOperation>MTLBlendOperationMax || color.alphaBlendOperation>MTLBlendOperationMax)return nil;
        attachmentPresent=true;
        auto &state=_colorStates[i];state.blend=color.blendingEnabled;
        state.writeMask=((color.writeMask&MTLColorWriteMaskRed)?1u:0u)|((color.writeMask&MTLColorWriteMaskGreen)?2u:0u)|
            ((color.writeMask&MTLColorWriteMaskBlue)?4u:0u)|((color.writeMask&MTLColorWriteMaskAlpha)?8u:0u);
        state.rgbOperation=uint32_t(color.rgbBlendOperation);state.alphaOperation=uint32_t(color.alphaBlendOperation);
        state.sourceRGB=uint32_t(color.sourceRGBBlendFactor);state.destinationRGB=uint32_t(color.destinationRGBBlendFactor);
        state.sourceAlpha=uint32_t(color.sourceAlphaBlendFactor);state.destinationAlpha=uint32_t(color.destinationAlphaBlendFactor);
    }
    if(!attachmentPresent)return nil;
    const auto &vp=*vertex->_program;
    for (unsigned i=0;i<QGPU_MAX_ATTRIBUTES;++i) if (vp.attributeMask&(1u<<i)) {
        auto attribute=d.vertexDescriptor.attributes[i];if (!attribute || attribute.bufferIndex>=GPU::Shader::MaxBindings ||
            attribute.offset>2047 || attribute.format>UINT32_MAX) return nil;
        auto layout=d.vertexDescriptor.layouts[attribute.bufferIndex];
        if (!layout || layout.stride>2048 || layout.stepFunction>MTLVertexStepFunctionPerInstance || layout.stepRate>UINT32_MAX) return nil;
        _vertexInput.attributes[i]={uint32_t(attribute.format),uint32_t(attribute.offset),uint32_t(attribute.bufferIndex),0};
        _vertexInput.layouts[attribute.bufferIndex]={uint32_t(layout.stride),uint32_t(layout.stepFunction),uint32_t(layout.stepRate),0};
    }
    uint32_t fetchBindings=0;if (!GPU::Shader::vertexLayout(vp,_vertexInput,fetchBindings)) return nil;
    _state=GPU::Shader::defaultRenderState(1,1);
    const auto &color=_colorStates[0];_state.blend=color.blend;_state.writeMask=color.writeMask;
    _state.rgbOperation=color.rgbOperation;_state.alphaOperation=color.alphaOperation;
    _state.sourceRGB=color.sourceRGB;_state.destinationRGB=color.destinationRGB;_state.sourceAlpha=color.sourceAlpha;_state.destinationAlpha=color.destinationAlpha;
    _vertex=vertex;_fragment=fragment;_samples=uint32_t(d.rasterSampleCount);_alphaToCoverage=d.alphaToCoverageEnabled;
    _depthFormat=d.depthAttachmentPixelFormat;_stencilFormat=d.stencilAttachmentPixelFormat;
    self.label = d.label; return self;
}
- (id<MTLDevice>)device { return _vertex.device; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 0; }
- (NSUInteger)maxTotalThreadsPerObjectThreadgroup { return 0; }
- (NSUInteger)maxTotalThreadsPerMeshThreadgroup { return 0; }
- (NSUInteger)objectThreadExecutionWidth { return 0; }
- (NSUInteger)meshThreadExecutionWidth { return 0; }
- (NSUInteger)maxTotalThreadgroupsPerMeshGrid { return 0; }
- (MTLShaderValidation)shaderValidation { return MTLShaderValidationDisabled; }
- (BOOL)threadgroupSizeMatchesTileSize { return NO; }
- (NSUInteger)imageblockSampleLength { return 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (MTLResourceID)gpuResourceID { return {(uint64_t(_vertex->_program->id)<<32)|_fragment->_program->id}; }
@end

static bool makeBlitDispatch(const GPU::Blit::ComputePlan &plan,GPU::Shader::Dispatch &dispatch) {
    const GPU::Shader::Program *program=nullptr;
    GPU::Blit::VectorFillGeometry geometry;
    const bool vectorFill=GPU::Blit::vectorFillGeometry(plan,geometry);
    const char *name=vectorFill?GPU::Blit::vectorFillKernel(plan):
        plan.pairFill?"blit_fill_pairs":plan.rowFill?"blit_fill_rows":plan.bytewise?(plan.copy?"blit_copy_bytes":"blit_fill_bytes"):(plan.copy?"blit_copy_words":"blit_fill_words");
    for (const auto &p:GPUPrograms::Programs) if (!strcmp(p.name,name)) {program=&p;break;}
    if (!program) return false;
    dispatch={};
    dispatch.registry=GPUPrograms::Registry;dispatch.version=GPU::Shader::Version;dispatch.program=program->id;
    uint32_t local=plan.threads>GPU::Shader::MaxComputeGroups*64?128:64;
    dispatch.groups[0]=(plan.threads+local-1)/local;dispatch.groups[1]=dispatch.groups[2]=1;
    dispatch.local[0]=local;dispatch.local[1]=dispatch.local[2]=1;
    dispatch.grid[0]=plan.threads;dispatch.grid[1]=dispatch.grid[2]=1;
    if(vectorFill)geometry.apply(dispatch);
    uint64_t elements=0;unsigned dimensions=0;
    if (!GPU::Shader::dispatchFootprint(dispatch,*program,elements,dimensions)) return false;
    return true;
}

@implementation QcomMTLCommandQueue
+ (Protocol *)metalProtocol { return @protocol(MTLCommandQueue); }
- (instancetype)initWithDevice:(QcomMTLDevice *)device capacity:(NSUInteger)capacity {
    if (!(self = [super init])) return nil;
    _owner = device; _token = 1; _orderLock = [NSRecursiveLock new]; _pending = [NSMutableArray new];
    // IOGPUMetalCommandQueue starts at normal priority (1); callers such as
    // QuartzCore can explicitly request High without promoting every queue.
    _priority=1;_backgroundPriority=2;
    _executionLock=[NSLock new];_ready=[NSMutableArray new];_retiring=[NSMutableArray new];
    _fenceUpdates=[NSMutableArray new];
    _worker = dispatch_queue_create("QcomMTL.submit", DISPATCH_QUEUE_SERIAL);
    _callbacks = dispatch_queue_create("QcomMTL.callbacks", DISPATCH_QUEUE_SERIAL);
    _cleanup = dispatch_queue_create("QcomMTL.retired-resources", DISPATCH_QUEUE_SERIAL);
    _capacity = dispatch_semaphore_create(capacity);
    [device->_transportLock lock];
    auto status = device->_submissionError;
    const char *stage="device-submission-state";
    if(!status){stage="newQueue";status=device->_connection.newQueue("Metal", _queue);}
    if(!status) {
        stage="setQueuePriority";
        GPU::Native::QueuePriority priority{uint32_t(_priority),uint32_t(_backgroundPriority),0};uint32_t applied=0;
        status=device->_connection.setQueuePriority(uint32_t(_queue.id),priority,applied);
        if(!status)_priority=applied;
    }
    if (!status) {stage="newShmem";status = device->_connection.newShmem(GPU::Native::MaxShmemBytes, _shmem);}
    if (!status) {stage="newNotificationQueue";status = device->_connection.newNotificationQueue(64, _notifications);}
    if (!status) {stage="setNotificationQueue";status = device->_connection.setNotificationQueue((uint32_t)_queue.id, _notifications.id);}
    if (!status && device->_completionNotifications) {
        stage="notification-port";
        _completionPort=IODataQueueAllocateNotificationPort();
        if(!_completionPort)status=kIOReturnNoResources;
        else {
            _completionSource=dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV,_completionPort,0,
                dispatch_get_global_queue(QOS_CLASS_DEFAULT,0));
            if(!_completionSource) {
                mach_port_mod_refs(mach_task_self(),_completionPort,MACH_PORT_RIGHT_RECEIVE,-1);_completionPort=MACH_PORT_NULL;
                status=kIOReturnNoResources;
            } else {
                __weak QcomMTLCommandQueue *weakSelf=self;
                const mach_port_t receivePort=_completionPort;
                dispatch_source_set_event_handler(_completionSource,^{@autoreleasepool {
                    QcomMTLCommandQueue *queue=weakSelf;
                    if(queue)[queue notificationReady];
                }});
                // Never destroy/reuse a receive name while dispatch may be
                // receiving on it. This block owns only the port, not self.
                dispatch_source_set_cancel_handler(_completionSource,^{mach_port_mod_refs(mach_task_self(),receivePort,MACH_PORT_RIGHT_RECEIVE,-1);});
                dispatch_resume(_completionSource);
                stage="setNotificationPort";status=device->_connection.setNotificationPort(_notifications.id,_completionPort);
            }
        }
    }
    if(!status)[device->_nativeQueues addObject:self];
    [device->_transportLock unlock];
    if(status)NSLog(@"QcomMTL: queue initialization failed pid=%d stage=%s IOReturn=0x%08x",getpid(),stage,status);
    return status ? nil : self;
}
- (void)dealloc {
    if(_completionSource)dispatch_source_cancel(_completionSource);
    [_owner->_transportLock lock];
    if (_queue.id) _owner->_connection.deleteQueue((uint32_t)_queue.id);
    if (_blitParameters.id) {
        _owner->_connection.deleteResource(_blitParameters.id);
        _owner->_allocated -= _blitParameters.bytes;
    }
    if (_shmem.id) _owner->_connection.deleteShmem(_shmem.id);
    if (_notifications.id) _owner->_connection.deleteNotificationQueue(_notifications.id);
    [_owner->_transportLock unlock];
}
- (id<MTLDevice>)device { return _owner; }
- (id<MTLCommandBuffer>)commandBufferWithRetainedReferences:(BOOL)retained {
    // Nonblocking bring-up quota; never hold the native connection while waiting.
    if (dispatch_semaphore_wait(_capacity, DISPATCH_TIME_NOW)) return nil;
    QcomMTLCommandBuffer *buffer = [[QcomMTLCommandBuffer alloc] initWithQueue:self retainedReferences:retained];
    if (!buffer) dispatch_semaphore_signal(_capacity);
    return buffer;
}
- (id<MTLCommandBuffer>)commandBuffer {return [self commandBufferWithRetainedReferences:YES];}
- (NSUInteger)getGPUPriority {
    [_orderLock lock];NSUInteger result=_priority;[_orderLock unlock];return result;
}
- (NSUInteger)getBackgroundGPUPriority {
    [_orderLock lock];NSUInteger result=_backgroundPriority;[_orderLock unlock];return result;
}
- (BOOL)_setGPUPriority:(NSUInteger)priority backgroundPriority:(NSUInteger)background {
    if(priority>5 || priority==3 || (background!=2 && background!=3))return NO;
    [_orderLock lock];GPU::Native::QueuePriority args{uint32_t(priority),uint32_t(background),0};uint32_t applied=0;
    [_owner->_transportLock lock];auto status=_owner->_connection.setQueuePriority(uint32_t(_queue.id),args,applied);[_owner->_transportLock unlock];
    if(!status){_priority=applied;_backgroundPriority=background;}
    [_orderLock unlock];return status==KERN_SUCCESS;
}
- (BOOL)setGPUPriority:(NSUInteger)priority {
    if(priority>5 || priority==3)[NSException raise:NSInvalidArgumentException format:@"Invalid native GPU priority"];
    [_orderLock lock];BOOL result=[self _setGPUPriority:priority backgroundPriority:_backgroundPriority];[_orderLock unlock];return result;
}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority {
    if(priority>5)[NSException raise:NSInvalidArgumentException format:@"Invalid native background GPU priority"];
    [_orderLock lock];BOOL result=[self _setGPUPriority:_priority backgroundPriority:priority];[_orderLock unlock];return result;
}
- (BOOL)setGPUPriority:(NSUInteger)priority offset:(unsigned short)offset {
    (void)offset;return [self setGPUPriority:priority]; // original22H374 wrapper also ignores offset
}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority offset:(unsigned short)offset {
    (void)offset;return [self setBackgroundGPUPriority:priority];
}
- (void)setSubmissionQueue:(dispatch_queue_t)queue {
    [_orderLock lock];
    if(_hasSubmitted) {
        [_orderLock unlock];[NSException raise:NSInvalidArgumentException format:@"Set the submission queue before submitting A540 commands"];return;
    }
    // QuartzCore supplies its own dispatch target immediately after creating
    // the Metal queue. Keep our serial executor to preserve native FIFO even
    // when its target is concurrent; configure the target before activation.
    auto worker=dispatch_queue_create("QcomMTL.submit",dispatch_queue_attr_make_initially_inactive(DISPATCH_QUEUE_SERIAL));
    if(queue)dispatch_set_target_queue(worker,queue);
    dispatch_activate(worker);_worker=worker;[_orderLock unlock];
}
- (void)setCompletionQueue:(dispatch_queue_t)queue {
    [_orderLock lock];
    if(_hasSubmitted) {
        [_orderLock unlock];[NSException raise:NSInvalidArgumentException format:@"Set the completion queue before submitting A540 commands"];return;
    }
    auto callbacks=dispatch_queue_create("QcomMTL.callbacks",dispatch_queue_attr_make_initially_inactive(DISPATCH_QUEUE_SERIAL));
    if(queue)dispatch_set_target_queue(callbacks,queue);
    dispatch_activate(callbacks);_callbacks=callbacks;[_orderLock unlock];
}
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)descriptor {
    if(!descriptor || descriptor.errorOptions || descriptor.logState)return nil;
    return [self commandBufferWithRetainedReferences:descriptor.retainedReferences];
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {return [self commandBufferWithRetainedReferences:NO];}
- (void)insertDebugCaptureBoundary {}
- (void)schedule:(QcomMTLCommandBuffer *)buffer commit:(BOOL)commit {
    // A weak load can become the last CPU owner. Release these snapshots
    // after both locks: resource destruction can reenter native transport.
    __attribute__((objc_precise_lifetime)) NSArray *commitResources=nil;
    [_orderLock lock]; [buffer->_condition lock];
    if (buffer->_status >= MTLCommandBufferStatusCommitted) {
        [buffer->_condition unlock]; [_orderLock unlock];
        [NSException raise:NSInvalidArgumentException format:@"Command buffer already committed"]; return;
    }
    if (buffer->_status == MTLCommandBufferStatusNotEnqueued) {
        QcomMTLQueueEntry *entry = [QcomMTLQueueEntry new]; entry.buffer = buffer;
        [_pending addObject:entry]; buffer->_status = MTLCommandBufferStatusEnqueued;
    }
    if (commit) {
        _hasSubmitted=YES;
        if (buffer->_encoderActive && !buffer->_error) buffer->_error = failure(@"endEncoding must precede commit");
        commitResources=[buffer copyResourcesAndResolveFences];
        buffer->_status = MTLCommandBufferStatusCommitted;
        for (QcomMTLQueueEntry *entry in _pending) if (entry.buffer == buffer) { entry.submitted = buffer; break; }
    }
    [buffer->_condition unlock];
    [self pump];
    [_orderLock unlock];
}
- (void)pump {
    // Called under _orderLock. Enqueue reserves FIFO position using a weak
    // reference, so abandoned, uncommitted buffers cannot leak their queue.
    // A temporary strong weak-load may be the last reference; the recursive
    // order lock permits its dealloc to abandon that reservation safely.
    while (_pending.count) {
        QcomMTLQueueEntry *entry = _pending.firstObject;
        QcomMTLCommandBuffer *next = entry.submitted ?: entry.buffer;
        if (!next) { [_pending removeObjectAtIndex:0]; continue; }
        if (next.status < MTLCommandBufferStatusCommitted) break;
        [_pending removeObjectAtIndex:0];
        _hasSubmitted=YES;
        [_ready addObject:next];
        [next->_condition lock];next->_readyForExecution=YES;[next->_condition broadcast];[next->_condition unlock];
    }
    if(_ready.count && !_presentationBarrier && !_ready.firstObject.blocked && !_drainScheduled) {
        _drainScheduled=YES;
        dispatch_async(_worker, ^{ @autoreleasepool { [self drain]; } });
    }
    if(_retiring.count && !_retiring.firstObject.blocked && !_finishScheduled) {
        _finishScheduled=YES;
        dispatch_block_t retire=^{
            @autoreleasepool {[self finishRetirements];}
        };
        if(_owner->_completionNotifications)dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),retire);
        else dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_MSEC),dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),retire);
    }
}
- (void)drain { [self drainThrough:nil]; }
- (void)drainThrough:(QcomMTLCommandBuffer *)limit {
    // Keep a suspended buffer at the FIFO head. Dependency completion wakes
    // this executor without occupying its caller-supplied dispatch target.
    [_orderLock lock];_drainScheduled=NO;[_orderLock unlock];
    if(![_executionLock tryLock])return;
    for(;;) {
        if(limit && limit.status>=MTLCommandBufferStatusScheduled)break;
        [_orderLock lock];
        QcomMTLCommandBuffer *next=_presentationBarrier?nil:_ready.firstObject;
        [_orderLock unlock];if(!next || next.blocked)break;
        BOOL finished=NO;@autoreleasepool {finished=[next run];}
        if(!finished && !next->_awaitingRetirement)break;
        [_orderLock lock];
        if(_ready.firstObject==next) {
            if(!finished) {
                next->_inRetirement=YES;
                [_retiring addObject:next];
                if(next->_presentations.count)_presentationBarrier=next;
            }
            [_ready removeObjectAtIndex:0];
        }
        [_orderLock unlock];
    }
    [_executionLock unlock];
    [_orderLock lock];[self pump];[_orderLock unlock];
}
- (void)finishRetirements {
    // Status/presentation retirement must also run when the caller occupies
    // its private submission target. This executor never encodes new work.
    [_orderLock lock];_finishScheduled=NO;[_orderLock unlock];
    // The owning executor always pumps after unlocking. A failed tryLock
    // needs no polling/retry loop while that executor is encoding a batch.
    if(![_executionLock tryLock])return;
    for(;;) {
        [_orderLock lock];QcomMTLCommandBuffer *next=_retiring.firstObject;[_orderLock unlock];
        if(!next || next.blocked)break;
        BOOL finished=NO;@autoreleasepool {finished=[next run];}
        if(!finished)break;
        [_orderLock lock];
        if(_retiring.firstObject==next)[_retiring removeObjectAtIndex:0];
        if(_presentationBarrier==next)_presentationBarrier=nil;
        [_orderLock unlock];
    }
    [_executionLock unlock];[_orderLock lock];[self pump];[_orderLock unlock];
}
- (void)releaseCleanupBytes:(uint64_t)bytes admission:(MetalRetirement::Budget::Admission)admission nanos:(uint64_t)nanos queued:(uint64_t)queued {
    [_orderLock lock];
    if(!_cleanupBudget.release(bytes,admission))abort();
    ++_cleanupRuns;_cleanupNanos+=nanos;_cleanupMaxNanos=std::max(_cleanupMaxNanos,nanos);_cleanupQueueNanos+=queued;
    if(admission==MetalRetirement::Budget::Admission::Exclusive)++_cleanupExclusiveRuns;
    QcomMTLCommandBuffer *head=_retiring.firstObject;
    [_orderLock unlock];
    // A budget waiter registered _blocked under the same order lock. Waking
    // after dropping that lock avoids a lost wake and never holds transport.
    [head wake];
}
- (NSDictionary *)qcomRetiredCleanupStatistics {
    [_orderLock lock];
    NSDictionary *result=@{@"runs":@(_cleanupRuns),@"release_ns":@(_cleanupNanos),@"max_release_ns":@(_cleanupMaxNanos),
        @"queue_ns":@(_cleanupQueueNanos),@"budget_waits":@(_cleanupWaits),@"exclusive_runs":@(_cleanupExclusiveRuns),
        @"pending":@(_cleanupBudget.count()),@"pending_bytes":@(_cleanupBudget.bytes()),
        @"peak_pending":@(_cleanupBudget.peakCount()),@"peak_bytes":@(_cleanupBudget.peakBytes()),
        @"exclusive":@(_cleanupBudget.exclusive())};
    [_orderLock unlock];return result;
}
- (void)qcomWaitForRetiredCleanup {
    // Project diagnostic only: public Metal completion does not promise that
    // every final resource reference has already been destroyed. A pool
    // allocation test needs an explicit cleanup boundary before accounting.
    dispatch_sync(_cleanup,^{});
}
- (void)wakeForNativeError {
    [_orderLock lock];NSMutableArray *buffers=[_ready mutableCopy];[buffers addObjectsFromArray:_retiring];[_orderLock unlock];
    for(QcomMTLCommandBuffer *buffer in buffers)[buffer wake];
}
- (void)abandon { [_orderLock lock]; [self pump]; [_orderLock unlock]; }
- (void)failNative:(kern_return_t)status {
    if(!_completionError)_completionError=status?status:kIOReturnIOError;
    if(!_owner->_submissionError)_owner->_submissionError=_completionError;
    auto count=_completions.abort();
    if(count<=_owner->_nativeJobs)_owner->_nativeJobs-=count;
    else _owner->_nativeJobs=0;
    for(QcomMTLFencePoint *point in _fenceUpdates)[point finish:_completionError queue:0 token:0];
    [_fenceUpdates removeAllObjects];
}
- (void)armRetirement {
    if(_pollScheduled || (!_completions.pending() && !_completionError))return;
    _pollScheduled=YES;
    const uint64_t delay=_completionSource?50*NSEC_PER_MSEC:NSEC_PER_MSEC;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,delay),dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{
        @autoreleasepool {[self pollNative:YES];}
    });
}
- (void)notificationReady {
    // Bounded, nonblocking drain of wakeups. The shared queue still has one
    // consumer under the device transport lock, including watchdog callbacks.
    kern_return_t status=KERN_SUCCESS;
    for(unsigned n=0;n<64;++n) {
        bool received=false;
        status=GPU::NativeConnection::receiveNotification(_completionPort,0,received);
        if(status || !received)break;
    }
    if(status) {
        dispatch_source_cancel(_completionSource);
        [_owner->_transportLock lock];[self failNative:status];[_owner->_transportLock unlock];
    }
    [self pollNative:NO];
}
- (void)pollNative:(BOOL)watchdog {
    NSArray *wakeups=nil,*failedQueues=nil;
    [_owner->_transportLock lock];
    @try {
        // An event must not clear a still-outstanding watchdog; otherwise
        // every event would create another timer and keep the queue alive.
        if(watchdog)_pollScheduled=NO;
        else ++_eventWakeups;
        uint32_t before=_completions.pending();
        auto status=_completionError;
        if(!status)status=_owner->_connection.pollCompletions(_notifications,_completions);
        const auto retired=before-_completions.pending();
        const auto now=nativeCompletionClock();
        if(retired>_owner->_nativeJobs)status=kIOReturnIOError;
        else _owner->_nativeJobs-=retired;
        _retiredToken=_completions.retired();
        for(NSUInteger i=_fenceUpdates.count;i;--i)
            if([_fenceUpdates[i-1] retireThrough:_retiredToken])[_fenceUpdates removeObjectAtIndex:i-1];
        if(retired)_nativeProgress=now;
        if(_completionSource) {
            if(watchdog)_watchdogRetired+=retired;else _eventRetired+=retired;
            const auto total=_eventRetired+_watchdogRetired;
            if(retired && total>=_nextCompletionReport) {
                fprintf(stderr,"QCOM_MTL_COMPLETION pid=%d queue=%llu events=%llu event_retired=%llu watchdog_retired=%llu pending=%u\n",
                    getpid(),(unsigned long long)_queue.id,(unsigned long long)_eventWakeups,
                    (unsigned long long)_eventRetired,(unsigned long long)_watchdogRetired,_completions.pending());
                _nextCompletionReport=total+1024;
            }
        }
        if(!status && _completions.pending() && (!now || now<_nativeProgress || now-_nativeProgress>=2000000000ULL))status=kIOReturnTimeout;
        if(status)[self failNative:status];
        if(retired || status) {
            wakeups=_owner->_nativeWaiters.allObjects;[_owner->_nativeWaiters removeAllObjects];
        }
        if(status)failedQueues=_owner->_nativeQueues.allObjects;
        if(_completions.pending())[self armRetirement];
    } @finally {[_owner->_transportLock unlock];}
    // Never take a queue order/condition lock while holding the transport
    // lock on this callback path. Waiters recheck their own exact token.
    for(QcomMTLCommandBuffer *buffer in wakeups)[buffer wake];
    for(QcomMTLCommandQueue *queue in failedQueues)[queue wakeForNativeError];
}
- (kern_return_t)waitNative:(uint64_t)token buffer:(QcomMTLCommandBuffer *)buffer allQueues:(BOOL)all {
    if(_completionError)return _completionError;
    if(all?_owner->_nativeJobs!=0:_retiredToken<token) {
        [buffer->_condition lock];buffer->_blocked=YES;[buffer->_condition unlock];
        [_owner->_nativeWaiters addObject:buffer];return kIOReturnNotReady;
    }
    return KERN_SUCCESS;
}
- (kern_return_t)admitNative {
    if(_completionError)return _completionError;
    if(_owner->_submissionError)return _owner->_submissionError;
    if(_token>=UINT64_MAX-2)return kIOReturnNoResources;
    // Bound producer run-ahead at command-buffer boundaries. Once a buffer
    // starts, let its passes enter the native queue together: throttling each
    // small clear/draw separately makes one compositor frame repeatedly wait
    // behind unrelated producers. The kernel's normal job limit still applies.
    if(_owner->_pipelinedAdmission && _completions.pending() && !_executing.nativeAdmitted)
        return kern_return_t(GPU::Native::AdmissionDeferredStatus);
    if(_completions.full() || _owner->_nativeJobs>=GPU::MaxJobs)
        return _owner->_pipelinedAdmission?kern_return_t(GPU::Native::AdmissionDeferredStatus):kIOReturnNotReady;
    return KERN_SUCCESS;
}
- (kern_return_t)deferNativeForBuffer:(QcomMTLCommandBuffer *)buffer {
    const auto now=nativeCompletionClock();
    if(!_admissionPressureSince)_admissionPressureSince=now;
    if(!now || now<_admissionPressureSince || now-_admissionPressureSince>=5000000000ULL)return kIOReturnNoResources;
    [buffer->_condition lock];buffer->_blocked=YES;[buffer->_condition unlock];
    [_owner->_nativeWaiters addObject:buffer];
    if(!_retryScheduled) {
        _retryScheduled=YES;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_MSEC),dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{@autoreleasepool {
            [_owner->_transportLock lock];_retryScheduled=NO;
            NSArray *wakeups=_owner->_nativeWaiters.allObjects;[_owner->_nativeWaiters removeAllObjects];
            [_owner->_transportLock unlock];
            for(QcomMTLCommandBuffer *waiting in wakeups)[waiting wake];
        }});
    }
    return kIOReturnNotReady;
}
- (kern_return_t)submitCommands:(const GPU::Command *)commands count:(uint32_t)count {
    auto available=[self admitNative];if(available)return available;
    auto &connection = _owner->_connection;
    NSData *tasks=_executing?_executing->_executionResponsibleTasks:nil;
    auto status=connection.writeCommands(_shmem,commands,count,static_cast<const uint32_t *>(tasks.bytes),uint32_t(tasks.length/4));
    if(status)return status;
    publishCPU();
    uint64_t endToken=_token+1;
    status=connection.submit((uint32_t)_queue.id,_shmem.id,_token,endToken);
    if(uint32_t(status)==GPU::Native::AdmissionDeferredStatus)return status;
    if(!status && !_completions.admit(_token,endToken))status=kIOReturnIOError;
    _token+=2;
    if(status){[self failNative:status];[self armRetirement];return status;}
    if(_completions.pending()==1)_nativeProgress=nativeCompletionClock();
    _admissionPressureSince=0;++_owner->_nativeJobs;
    [_executing markNativeAdmitted];
    [self armRetirement];return KERN_SUCCESS;
}
- (kern_return_t)submitBlit:(const GPU::Blit::ComputePlan &)plan source:(QcomMTLBuffer *)source destination:(QcomMTLBuffer *)destination {
    // The native admission snapshots every builtin blit layout into its own
    // read-only IB before returning. Reusing this CPU page needs no GPU wait.
    if (_completionError) return _completionError;
    GPU::Shader::Dispatch dispatch{};
    if(!makeBlitDispatch(plan,dispatch))return kIOReturnBadArgument;
    if (!_blitParameters.id) {
        GPU::Native::ResourceData resource{};
        auto status=_owner->_connection.newResource(GPU::PageBytes,resource);
        if (status) return status;
        if (!resource.id || !resource.cpuAddress || resource.bytes<sizeof(plan.parameters)) {
            if (resource.id) _owner->_connection.deleteResource(resource.id);
            return kIOReturnBadArgument;
        }
        _blitParameters=resource;_owner->_allocated+=resource.bytes;
    }
    memcpy((void *)(uintptr_t)_blitParameters.cpuAddress,plan.parameters,sizeof(plan.parameters));
    unsigned destinationSlot=0,parametersSlot=1;
    if (plan.copy) {
        dispatch.buffers[0]={source->_resource.id,plan.sourceOffset,plan.sourceBytes,0};
        destinationSlot=1;parametersSlot=2;
    }
    dispatch.buffers[destinationSlot]={destination->_resource.id,plan.destinationOffset,plan.destinationBytes,0};
    dispatch.buffers[parametersSlot]={_blitParameters.id,0,sizeof(plan.parameters),0};
    return [self submitShader:GPU::ShaderDispatch payload:[NSData dataWithBytes:&dispatch length:sizeof(dispatch)]];
}
- (kern_return_t)execute:(QcomMTLOperation *)operation {
    if(operation.responsibleTaskIDs) {
        if(!_executing)return kIOReturnError;
        _executing->_executionResponsibleTasks=operation.responsibleTaskIDs;
        return KERN_SUCCESS;
    }
    // Admission may pause halfway through a CP blit. Commit a row/byte cursor
    // only after its batch was accepted, so waking never replays GPU writes.
    auto available=[self admitNative];if(available)return available;
    if(operation.barrier) {
        GPU::Command barrier{GPU::Barrier,0,0,0,0,0};return [self submitCommands:&barrier count:1];
    }
    if(operation.shaderOpcode)return [self submitShader:operation.shaderOpcode payload:operation.shaderPayload];
    auto *destination=operation.destination;const bool copy=operation.source!=nil;
    GPU::Blit::ComputePlan plan;
    if(copy) {
        const bool surface=operation.source->_surface || destination->_surface;
        if(GPU::Blit::copyBytePlan(operation.sourceOffset,operation.destinationOffset,operation.bytes,
            operation.sourceStride,operation.destinationStride,MAX(operation.rows,NSUInteger(1)),plan) ||
           GPU::Blit::copyPlan(operation.sourceOffset,operation.destinationOffset,operation.bytes,
            operation.sourceStride,operation.destinationStride,MAX(operation.rows,NSUInteger(1)),plan,!surface))
            return [self submitBlit:plan source:operation.source destination:destination];
    } else {
        uint32_t pattern=operation.wordPattern?operation.wordValue:uint32_t(operation.value)*0x01010101u;
        if(operation.wordPattern) {
            bool valid=operation.pairPattern?
                GPU::Blit::fillPairRowsPlan(operation.destinationOffset,operation.bytes,operation.destinationStride,
                    MAX(operation.rows,NSUInteger(1)),pattern,operation.wordHigh,plan):
                GPU::Blit::fillRowsPlan(operation.destinationOffset,operation.bytes,operation.destinationStride,
                    MAX(operation.rows,NSUInteger(1)),pattern,plan);
            if(!valid)return kIOReturnBadArgument;
            return [self submitBlit:plan source:nil destination:destination];
        }
        if(GPU::Blit::fillBytePlan(operation.destinationOffset,operation.bytes,pattern,plan) ||
           GPU::Blit::fillPlan(operation.destinationOffset,operation.bytes,pattern,plan))
            return [self submitBlit:plan source:nil destination:destination];
    }
    const NSUInteger rows=copy?MAX(operation.rows,NSUInteger(1)):1;
    while(operation->_nativeRow<rows) {
        available=[self admitNative];if(available)return available;
        GPU::Blit::Batch batch;NSUInteger row=operation->_nativeRow,done=operation->_nativeByte;
        while(row<rows && batch.capacity(copy)) {
            const uint32_t bytes=uint32_t(MIN(operation.bytes-done,NSUInteger(batch.capacity(copy))));
            const auto to=operation.destinationOffset+(copy?row*operation.destinationStride:0)+done;
            GPU::Command command=copy?GPU::Native::copyCommand(operation.source->_resource.id,
                uint32_t(operation.sourceOffset+row*operation.sourceStride+done),destination->_resource.id,uint32_t(to),bytes):
                GPU::Command{GPU::Fill32,0,destination->_resource.id,to,bytes,
                    operation.wordPattern?operation.wordValue:uint32_t(operation.value)*0x01010101u};
            if(!batch.append(command))return kIOReturnBadArgument;
            done+=bytes;if(done==operation.bytes){++row;done=0;}
        }
        auto status=[self submitCommands:batch.commands() count:batch.count()];if(status)return status;
        operation->_nativeRow=row;operation->_nativeByte=done;
    }
    return KERN_SUCCESS;
}
- (kern_return_t)executeEvent:(QcomMTLOperation *)operation {
    BOOL valid=NO;__attribute__((objc_precise_lifetime)) NSArray *live=[operation copyLiveResourcesWithValidity:&valid];
    if(!valid)return kIOReturnBadArgument;
    // Called without the device transport lock: another queue must be able
    // to retire its A540 fence and signal the event which this queue awaits.
    if ([operation.event isKindOfClass:[QcomMTLEvent class]]) {
        auto event=(QcomMTLEvent *)operation.event;
        if (operation.signalEvent) {[event signalValue:operation.eventValue];return KERN_SUCCESS;}
        return [event waitForValue:operation.eventValue] ? KERN_SUCCESS : kIOReturnTimeout;
    }
    auto event=(id<MTLSharedEvent>)operation.event;
    if (operation.signalEvent) {event.signaledValue=operation.eventValue;return KERN_SUCCESS;}
    return [event waitUntilSignaledValue:operation.eventValue timeoutMS:5000] ? KERN_SUCCESS : kIOReturnTimeout;
}
- (kern_return_t)submitShader:(uint32_t)opcode payload:(NSData *)payload {
    auto available=[self admitNative];if(available)return available;
    // Legacy kernels require a whole-device index drain. The new capability
    // replaces it with pre-Family checks against all retained physical GPU
    // producers, including other clients, before the CPU snapshot is read.
    if(!_owner->_pipelinedAdmission && opcode==GPU::ShaderDraw && payload.length==sizeof(GPU::Shader::Draw) &&
        static_cast<const GPU::Shader::Draw *>(payload.bytes)->indexSize && _owner->_nativeJobs)return kIOReturnNotReady;
    auto &connection=_owner->_connection;
    NSData *tasks=_executing?_executing->_executionResponsibleTasks:nil;
    auto status=connection.writeShaderCommand(_shmem,opcode,payload.bytes,payload.length,
        static_cast<const uint32_t *>(tasks.bytes),uint32_t(tasks.length/4));
    if(status)return status;
    publishCPU();uint64_t endToken=_token+1;
    status=connection.submit(uint32_t(_queue.id),_shmem.id,_token,endToken);
    if(uint32_t(status)==GPU::Native::AdmissionDeferredStatus)return status;
    if(!status && !_completions.admit(_token,endToken))status=kIOReturnIOError;
    _token+=2;
    if(status){[self failNative:status];[self armRetirement];return status;}
    if(_completions.pending()==1)_nativeProgress=nativeCompletionClock();
    _admissionPressureSince=0;++_owner->_nativeJobs;[_executing markNativeAdmitted];[self armRetirement];return KERN_SUCCESS;
}
- (kern_return_t)submitShaderBatch:(NSArray<QcomMTLOperation *> *)operations {
    NSData *passData=operations.firstObject.renderPass;
    if(operations.count==1 && !passData)return [self submitShader:operations[0].shaderOpcode payload:operations[0].shaderPayload];
    if(passData && (passData.length!=sizeof(GPU::RenderPass::Description) || operations.firstObject.renderPassDraws!=operations.count))return kIOReturnBadArgument;
    const auto *pass=static_cast<const GPU::RenderPass::Description *>(passData.bytes);
    auto available=[self admitNative];if(available)return available;
    if(!operations.count || operations.count>GPU::Native::MaxBatchCommands)return kIOReturnBadArgument;
    uint32_t opcodes[GPU::Native::MaxBatchCommands]{};const void *payloads[GPU::Native::MaxBatchCommands]{};
    size_t sizes[GPU::Native::MaxBatchCommands]{};
    for(NSUInteger i=0;i<operations.count;++i) {
        auto operation=operations[i];opcodes[i]=operation.shaderOpcode;payloads[i]=operation.shaderPayload.bytes;sizes[i]=operation.shaderPayload.length;
        if(!_owner->_pipelinedAdmission && opcodes[i]==GPU::ShaderDraw && sizes[i]==sizeof(GPU::Shader::Draw) &&
            static_cast<const GPU::Shader::Draw *>(payloads[i])->indexSize && _owner->_nativeJobs)return kIOReturnNotReady;
    }
    NSData *tasks=_executing?_executing->_executionResponsibleTasks:nil;
    auto status=_owner->_connection.writeShaderBatch(_shmem,opcodes,payloads,sizes,uint32_t(operations.count),
        static_cast<const uint32_t *>(tasks.bytes),uint32_t(tasks.length/4),pass);
    if(status)return status;
    publishCPU();uint64_t endToken=_token+1;
    status=_owner->_connection.submit(uint32_t(_queue.id),_shmem.id,_token,endToken);
    if(!status && pass)__atomic_add_fetch(&_owner->_gmemPasses,1,__ATOMIC_RELAXED);
    if(uint32_t(status)==GPU::Native::AdmissionDeferredStatus)return status;
    if(!status && !_completions.admit(_token,endToken))status=kIOReturnIOError;
    _token+=2;
    if(status){[self failNative:status];[self armRetirement];return status;}
    if(_completions.pending()==1)_nativeProgress=nativeCompletionClock();
    _admissionPressureSince=0;++_owner->_nativeJobs;[_executing markNativeAdmitted];[self armRetirement];return KERN_SUCCESS;
}
@end

@implementation QcomMTLCommandBuffer
+ (Protocol *)metalProtocol { return @protocol(MTLCommandBuffer); }
- (instancetype)initWithQueue:(QcomMTLCommandQueue *)queue retainedReferences:(BOOL)retained {
    if (!(self = [super init])) return nil;
    static uint64_t timingIdentity=1;
    if(queue->_owner->_metalTimingEnabled)_timingIdentity=__atomic_fetch_add(&timingIdentity,uint64_t(1),__ATOMIC_RELAXED);
    _queue = queue;_retainedReferences=retained;_ownedResources=[NSMutableArray new]; _condition = [NSCondition new]; _operations = [NSMutableArray new];_presentations=[NSMutableArray new];
    _scheduledHandlers = [NSMutableArray new]; _completedHandlers = [NSMutableArray new]; _hasCapacity = YES;
    return self;
}
- (void)dealloc {
    if (_hasCapacity) dispatch_semaphore_signal(_queue->_capacity);
    if (_status == MTLCommandBufferStatusEnqueued) [_queue abandon];
}
- (id<MTLDevice>)device { return _queue.device; }
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (BOOL)retainedReferences { return _retainedReferences; }
- (MTLCommandBufferErrorOption)errorOptions { return MTLCommandBufferErrorOptionNone; }
- (NSUInteger)protectionOptions { return 0; }
- (void)setProtectionOptions:(NSUInteger)options {
    [_condition lock];
    if(_status>=MTLCommandBufferStatusCommitted || _encoderActive) {
        [_condition unlock];
        [NSException raise:NSInvalidArgumentException format:@"Protection options require an uncommitted command buffer without an active encoder"];
        return;
    }
    if(options && !_error)_error=failure(@"Protected GPU command buffers are not supported",MTLCommandBufferErrorInvalidResource);
    [_condition unlock];
}
- (void)setResponsibleTaskIDs:(const uint32_t *)ids count:(uint32_t)count {
    // Original Metal copies DWORD IDs at this point in the command stream.
    // Null clears attribution; the caller's array never survives this call.
    if(!ids)count=0;
    [_condition lock];
    if(_status>=MTLCommandBufferStatusCommitted || _encoderActive) {
        [_condition unlock];
        [NSException raise:NSInvalidArgumentException format:@"Responsible task IDs require an uncommitted command buffer without an active encoder"];
        return;
    }
    if(count>GPU::Native::MaxResponsibleTaskIDs || _operations.count>=256) {
        if(!_error)_error=failure(@"Responsible task metadata exceeds the native command limit",MTLCommandBufferErrorOutOfMemory);
    } else if(!_error) {
        auto operation=[QcomMTLOperation new];
        operation.responsibleTaskIDs=count?[NSData dataWithBytes:ids length:count*4]:[NSData data];
        if(!operation || !operation.responsibleTaskIDs)_error=failure(@"Responsible task metadata allocation failed",MTLCommandBufferErrorOutOfMemory);
        else {[operation configureRetainedReferences:_retainedReferences];[_operations addObject:operation];}
    }
    [_condition unlock];
}
- (MTLCommandBufferStatus)status { [_condition lock]; auto status = _status; [_condition unlock]; return status; }
- (NSError *)error { [_condition lock]; NSError *error = _error; [_condition unlock]; return error; }
// No calibrated hardware clock exists yet. Zero means timing is unavailable.
- (CFTimeInterval)GPUStartTime { return 0; }
- (CFTimeInterval)GPUEndTime { return 0; }
- (CFTimeInterval)kernelStartTime { return 0; }
- (CFTimeInterval)kernelEndTime { return 0; }
- (id<MTLLogContainer>)logs { return nil; }
- (void)fail:(NSError *)error {
    [_condition lock]; if (!_error && _status < MTLCommandBufferStatusCommitted) _error = error; [_condition unlock];
    [_queue->_owner reportMetalFailure:GPU::Diagnostics::Encoder error:error];
}
- (void)retainInternalResource:(id)resource {
    if(!resource)return;[_condition lock];
    if(_status<MTLCommandBufferStatusCommitted)[_ownedResources addObject:resource];
    [_condition unlock];
}
- (QcomMTLBuffer *)newInlineSnapshot:(const void *)bytes length:(NSUInteger)length {
    // Pinned AGXA10 setBytes/VertexBytes/FragmentBytes likewise advance an
    // upload cursor with 16-byte alignment and grow only at the buffer end.
    constexpr NSUInteger pageBytes=4096, alignment=16, maxPages=16;
    if (!bytes || !length || length>pageBytes) return nil;
    QcomMTLBuffer *allocated=nil;
    [_condition lock];
    if (_status>=MTLCommandBufferStatusCommitted || !_encoderActive || _error) {[_condition unlock];return nil;}
    const auto offset=(_inlineUsed+alignment-1)&~(alignment-1);
    const bool needsPage=!_inlinePage || length>pageBytes-offset;
    const bool standalone=needsPage && _inlinePages>=maxPages;
    [_condition unlock];
    // Native allocation enters the transport lock. Never hold _condition
    // across it: execution holds transport while publishing Scheduled.
    if (needsPage) {
        allocated=[[QcomMTLBuffer alloc] initWithDevice:_queue->_owner length:standalone?length:pageBytes
            options:MTLResourceStorageModeShared inlinePage:!standalone];
        if (!allocated) return nil;
    }
    [_condition lock];
    QcomMTLBuffer *snapshot=nil;
    @try {
        if (_status>=MTLCommandBufferStatusCommitted || !_encoderActive || _error) return nil;
        auto at=(_inlineUsed+alignment-1)&~(alignment-1);
        if (!_inlinePage || length>pageBytes-at) {
            if (!allocated) return nil; // Concurrent encoding is not supported.
            if (_inlinePages>=maxPages) {
                snapshot=allocated.length==length?allocated:
                    [[QcomMTLBuffer alloc] initWithInlineRoot:allocated offset:0 length:length];
            } else {
                _inlinePage=allocated;_inlineUsed=0;at=0;++_inlinePages;
                [_ownedResources addObject:allocated];
            }
        }
        if (!snapshot) {
            snapshot=[[QcomMTLBuffer alloc] initWithInlineRoot:_inlinePage offset:at length:length];
            if (snapshot) _inlineUsed=at+length;
        }
        if (snapshot) {
            // Commit cannot freeze the stream until this immediate copy and
            // the strong internal retain finish, including unretained CBs.
            memcpy(snapshot.contents,bytes,length);
            [_ownedResources addObject:snapshot];
        }
    } @finally {[_condition unlock];}
    return snapshot;
}
- (void)record:(QcomMTLOperation *)operation {
    [_condition lock];
    if (_status >= MTLCommandBufferStatusCommitted || !_encoderActive) {
        [_condition unlock]; [NSException raise:NSInvalidArgumentException format:@"Command buffer is no longer encoding"]; return;
    }
    if (_operations.count >= 256) { if (!_error) _error = failure(@"Metal bring-up command limit exceeded", MTLCommandBufferErrorOutOfMemory); }
    else if(!_error){
        // Keep this decision in the immutable encoded stream, before borrowed
        // references are converted. Only real drawable destinations qualify.
        for(id resource in operation.resources)if([resource isKindOfClass:[QcomMTLTexture class]]) {
            auto *texture=(QcomMTLTexture *)resource;
            if(!texture->_isDrawable || !texture->_backing->_surface)continue;
            const uint32_t id=texture->_backing->_resource.id;
            BOOL writes=operation.destination && operation.destination->_resource.id==id;
            if(operation.shaderOpcode==GPU::ShaderDraw && operation.shaderPayload.length==sizeof(GPU::Shader::Draw)) {
                const auto &draw=*static_cast<const GPU::Shader::Draw *>(operation.shaderPayload.bytes);
                for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)writes|=GPU::Shader::colorTarget(draw,i).resource==id;
                writes|=draw.depth.resource==id || draw.stencil.resource==id;
            } else if(operation.shaderOpcode==GPU::ShaderDispatch && operation.shaderPayload.length==sizeof(GPU::Shader::Dispatch) &&
                      (texture.usage&MTLTextureUsageShaderWrite)) {
                const auto &dispatch=*static_cast<const GPU::Shader::Dispatch *>(operation.shaderPayload.bytes);
                for(const auto &image:dispatch.images)writes|=image.resource==id;
            }
            _writesDrawable|=writes;
        }
        [operation configureRetainedReferences:_retainedReferences];[_operations addObject:operation];
    }
    [_condition unlock];
}
- (void)recordBarrierWithResources:(NSArray *)resources {
    for (id<MTLResource> resource in resources) {
        if ((![(id)resource isKindOfClass:[QcomMTLBuffer class]] && ![(id)resource isKindOfClass:[QcomMTLTexture class]]) || resource.device!=self.device) {
            [self fail:failure(@"Invalid barrier resource",MTLCommandBufferErrorInvalidResource)];return;
        }
    }
    auto operation=[QcomMTLOperation new];operation.barrier=YES;operation.resources=resources;[self record:operation];
}
- (NSArray *)copyResourcesAndResolveFences {
    // Called with _condition held at commit. Capture individual producer
    // points in device commit order, never an encoding-time maximum value.
    auto device=_queue->_owner;NSMutableArray *commitResources=[NSMutableArray new];[device->_dependencyLock lock];
    for(QcomMTLOperation *operation in _operations) {
        BOOL valid=NO;NSArray *live=[operation copyLiveResourcesWithValidity:&valid];[commitResources addObjectsFromArray:live];
        if(!valid){if(!_error)_error=failure(@"An unretained command resource expired before commit",MTLCommandBufferErrorInvalidResource);continue;}
        for (id resource in live) if ([resource isKindOfClass:[QcomMTLBuffer class]] || [resource isKindOfClass:[QcomMTLTexture class]]) {
            if ([resource setPurgeableState:MTLPurgeableStateKeepCurrent]!=MTLPurgeableStateNonVolatile && !_error)
                _error=failure(@"A command resource is volatile or empty at commit",MTLCommandBufferErrorInvalidResource);
        }
        if(!operation.fence)continue;
        auto fence=operation.fence;
        if (operation.signalFence) {
            for(NSUInteger i=fence->_producers.count;i;--i)if(!fence->_producers[i-1].pending)[fence->_producers removeObjectAtIndex:i-1];
            operation.fencePoint=[QcomMTLFencePoint new];if(operation.fencePoint)operation.fencePoint->_producer=self;fence->_broken=operation.fencePoint==nil;
            if(operation.fencePoint)[fence->_producers addObject:operation.fencePoint];
            if(!operation.fencePoint && !_error)_error=failure(@"Fence allocation failed",MTLCommandBufferErrorOutOfMemory);
        } else {
            operation.fencePoints=[fence->_producers copy];
            if ((!operation.fencePoints.count || fence->_broken) && !_error) _error=failure(@"Fence wait requires previously committed producing updates",MTLCommandBufferErrorInvalidResource);
        }
    }
    [device->_dependencyLock unlock];
    if (_error) for (QcomMTLOperation *operation in _operations) if (operation.signalFence)
        [operation.fencePoint finish:kIOReturnAborted queue:0 token:0];
    return commitResources;
}
- (BOOL)blocked { [_condition lock];BOOL result=_blocked;[_condition unlock];return result; }
- (void)wake {
    [_condition lock];BOOL active=_status<MTLCommandBufferStatusCompleted;
    if(active){_blocked=NO;[_condition broadcast];}[_condition unlock];if(active)[_queue abandon];
}
- (void)resumeEvent:(kern_return_t)status {
    [_condition lock];BOOL active=_status<MTLCommandBufferStatusCompleted;
    if(active){_eventResult=status;_eventReady=YES;_blocked=NO;}[_condition unlock];if(active)[_queue abandon];
}
- (void)enqueue { [_queue schedule:self commit:NO]; }
- (BOOL)canFlushDirectDrawable {
    // Original Mercury commits then calls the native drawable's present on
    // its transaction thread. Unlike presentDrawable:, that call cannot wait
    // for our asynchronous CPU executor to publish the producer access.
    [_condition lock];BOOL needed=_writesDrawable && !_presentations.count && _status<MTLCommandBufferStatusScheduled;[_condition unlock];
    if(!needed || !_queue->_owner->_surfaceFences)return NO;
    BOOL eligible=YES,found=NO;
    [_queue->_orderLock lock];
    @try {
        if(!_readyForExecution || _queue->_presentationBarrier)return NO;
        for(QcomMTLCommandBuffer *prior in _queue->_ready) {
            [prior->_condition lock];NSArray *operations=prior->_operations;
            BOOL presents=prior->_presentations.count!=0;[prior->_condition unlock];
            if(presents){eligible=NO;break;}
            for(QcomMTLOperation *operation in operations) {
                // Do not turn a future GPU event or another queue's pending
                // fence into a blocking commit. Those paths remain async.
                if(operation.event && !operation.signalEvent){eligible=NO;break;}
                if(operation.fence && !operation.signalFence)for(QcomMTLFencePoint *point in operation.fencePoints) {
                    auto producer=point->_producer;
                    if(point.pending && (!producer || producer->_queue!=_queue)){eligible=NO;break;}
                }
                if(!eligible)break;
            }
            if(!eligible)break;
            if(prior==self){found=YES;break;}
        }
    } @finally {[_queue->_orderLock unlock];}
    return eligible && found;
}
- (void)flushDirectDrawable {
    // A caller may occupy the configured serial submission target. Use the
    // ordinary FIFO executor here, with its original execution lock and real
    // notification-driven dependencies. No marker substitutes for pixel
    // admission, and no Scheduled/completed/fence state is manufactured.
    for(;;) {
        if(self.status>=MTLCommandBufferStatusScheduled)return;
        [_queue drainThrough:self];
        if(self.status>=MTLCommandBufferStatusScheduled)return;
        [_queue finishRetirements];
        [_condition lock];
        if(_status<MTLCommandBufferStatusScheduled)
            [_condition waitUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
        [_condition unlock];
    }
}
- (void)commitWithDirectDrawableAdmission:(BOOL)direct {
    MetalTimingScope timing(_queue->_owner,GPU::MetalTiming::Commit,_timingIdentity);
    [_queue schedule:self commit:YES];
    if(direct && [self canFlushDirectDrawable]) {
        timing.record.detail=1;[self flushDirectDrawable];timing.record.argument=self.status;
    }
}
- (void)commit { [self commitWithDirectDrawableAdmission:YES]; }
- (BOOL)commitAndWaitUntilSubmitted {
    // This BOOL API can defer cleanup to Scheduled. It must also avoid the
    // public commit's special wait for Mercury's immediate raw presentation.
    [self commitWithDirectDrawableAdmission:NO];if(self.error)return NO;
    // Native _submitAvailableCommandBuffers returns NO when an earlier
    // enqueued buffer is not committed. The caller can commit it only after
    // we return, particularly on QuartzCore's serial submission target.
    [_condition lock];BOOL ready=_readyForExecution && !_error;[_condition unlock];
    if(!ready)return NO;
    // QuartzCore may occupy this queue's serial submission target while it
    // calls us. Run the ordinary FIFO executor now if its execution lock is
    // available. It admits real operations, preserves dependency suspension
    // and publishes Scheduled only after the complete operation list. Native
    // retirement and callbacks remain on their existing independent executor.
    // A busy executor, a future dependency or admission pressure leaves the
    // buffer committed. Return NO so the native caller attaches its cleanup
    // to Scheduled and releases the submission target. A speculative marker
    // cannot make that cleanup safe and must not wait for GPU capacity here.
    [_queue drainThrough:self];
    // QuartzCore recycles its encoding resources immediately on YES, or
    // attaches that cleanup to addScheduledHandler: on NO. A private marker
    // proves neither that all pixel operations were admitted nor that their
    // borrowed resources may be recycled. Normal execution publishes
    // Scheduled only after the entire operation list has been admitted.
    auto current=self.status;
    return current>=MTLCommandBufferStatusScheduled && current!=MTLCommandBufferStatusError;
}
- (void)recordEvent:(id<MTLEvent>)event value:(uint64_t)value signal:(BOOL)signal {
    [_condition lock];
    if (_status>=MTLCommandBufferStatusCommitted) {
        [_condition unlock];[NSException raise:NSInvalidArgumentException format:@"Command buffer already committed"];return;
    }
    BOOL local=[event isKindOfClass:[QcomMTLEvent class]] && event.device==self.device;
    BOOL shared=[event conformsToProtocol:@protocol(MTLSharedEvent)] && !event.device &&
        [event respondsToSelector:@selector(waitUntilSignaledValue:timeoutMS:)];
    if ((!local && !shared) || _encoderActive || _operations.count>=256) {
        if(!_error)_error=failure(@"Invalid event, active encoder or command limit",MTLCommandBufferErrorInvalidResource);
    } else if (!_error) {
        auto operation=[QcomMTLOperation new];operation.event=event;operation.eventValue=value;operation.signalEvent=signal;
        [operation configureRetainedReferences:_retainedReferences];[_operations addObject:operation];
    }
    [_condition unlock];
}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value { [self recordEvent:event value:value signal:NO]; }
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value { [self recordEvent:event value:value signal:YES]; }
- (void)recordPresentation:(id<MTLDrawable>)drawable mode:(NSUInteger)mode time:(CFTimeInterval)time options:(NSDictionary *)options {
    [_condition lock];
    if (_status>=MTLCommandBufferStatusCommitted) {
        [_condition unlock];[NSException raise:NSInvalidArgumentException format:@"Command buffer already committed"];return;
    }
    id<MTLTexture> texture=[drawable respondsToSelector:@selector(texture)] ? [(id<QcomNativeDrawableOptions>)drawable texture] : nil;
    BOOL valid=drawable && [drawable conformsToProtocol:@protocol(MTLDrawable)] &&
        [texture isKindOfClass:[QcomMTLTexture class]] && texture.device==self.device &&
        std::isfinite(time) && time>=0 && mode<=3 && _presentations.count<16;
    if(mode==3)valid=valid && [options isKindOfClass:[NSDictionary class]] && [drawable respondsToSelector:@selector(presentWithOptions:)];
    for(QcomMTLPresentation *pending in _presentations)if(pending.drawable==drawable)valid=NO;
    if(!valid) {if(!_error)_error=failure(@"Invalid or duplicate drawable presentation",MTLCommandBufferErrorInvalidResource);}
    else if(!_error) {
        auto pending=[QcomMTLPresentation new];pending.drawable=drawable;pending.texture=texture;pending.mode=mode;pending.time=time;pending.options=options;
        [_presentations addObject:pending];
    }
    [_condition unlock];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable { [self recordPresentation:drawable mode:0 time:0 options:nil]; }
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)time { [self recordPresentation:drawable mode:1 time:time options:nil]; }
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)duration { [self recordPresentation:drawable mode:2 time:duration options:nil]; }
- (void)presentDrawable:(id<MTLDrawable>)drawable options:(NSDictionary *)options { [self recordPresentation:drawable mode:3 time:0 options:options]; }
- (void)submitPresentations {
    if(_presentationsSubmitted)return;
    _presentationsSubmitted=YES;
    if(!self.error)for(QcomMTLPresentation *pending in _presentations) {
        @try {
            MetalTimingScope timing(_queue->_owner,GPU::MetalTiming::Stage(GPU::MetalTiming::Present+pending.mode),_timingIdentity);
            if(_queue->_owner->_metalTimingEnabled) {
                IOSurfaceRef surface=((QcomMTLTexture *)pending.texture)->_backing->_surface;
                if(surface)timing.record.detail=(uint64_t(IOSurfaceGetSeed(surface))<<32)|IOSurfaceGetID(surface);
                const auto target=pending.time;memcpy(&timing.record.argument,&target,sizeof(target));
            }
            [pending present];
        }
        @catch(NSException *exception) {
            [_condition lock];if(!_error)_error=failure(exception.reason ?: @"Drawable presentation failed");[_condition unlock];break;
        }
    }
}
- (void)waitUntilScheduled {
    [_condition lock];while(_status<MTLCommandBufferStatusScheduled || _scheduledCallbacks)[_condition wait];[_condition unlock];
}
- (void)waitUntilCompleted {
    [_condition lock];while(_status<MTLCommandBufferStatusCompleted || _scheduledCallbacks || _completedCallbacks)[_condition wait];[_condition unlock];
}
- (void)dispatchHandlers:(NSArray *)handlers scheduled:(BOOL)scheduled {
    // Called with the condition held. Public waits include registered handler
    // delivery, while native retirement itself stays off the submission target.
    if(!handlers.count)return;
    if(scheduled)_scheduledCallbacks+=handlers.count;else _completedCallbacks+=handlers.count;
    dispatch_async(_queue->_callbacks,^{
        @try {for(MTLCommandBufferHandler handler in handlers)handler(self);}
        @finally {
            [_condition lock];if(scheduled)_scheduledCallbacks-=handlers.count;else _completedCallbacks-=handlers.count;
            [_condition broadcast];[_condition unlock];
        }
    });
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)handler {
    [_condition lock];
    BOOL scheduled = _status >= MTLCommandBufferStatusScheduled && _status != MTLCommandBufferStatusError;
    if (_status < MTLCommandBufferStatusScheduled) [_scheduledHandlers addObject:[handler copy]];
    else if (scheduled) [self dispatchHandlers:@[[handler copy]] scheduled:YES];
    [_condition unlock];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
    [_condition lock];
    if (_status < MTLCommandBufferStatusCompleted) [_completedHandlers addObject:[handler copy]];
    else [self dispatchHandlers:@[[handler copy]] scheduled:NO];
    [_condition unlock];
}
- (void)markScheduled {
    [_condition lock];
    _nativeAdmitted=YES;
    if (_status==MTLCommandBufferStatusCommitted && !_error) {
        NSArray *scheduled=[_scheduledHandlers copy];[_scheduledHandlers removeAllObjects];
        _status = MTLCommandBufferStatusScheduled; [_condition broadcast];
        [self dispatchHandlers:scheduled scheduled:YES];
    }
    [_condition unlock];
}
- (BOOL)nativeAdmitted {[_condition lock];BOOL admitted=_nativeAdmitted;[_condition unlock];return admitted;}
- (void)markNativeAdmitted {[_condition lock];_nativeAdmitted=YES;[_condition broadcast];[_condition unlock];}
- (BOOL)run {
    MetalTimingScope timing(_queue->_owner,GPU::MetalTiming::QueueRun,_timingIdentity);
    if(self.status>=MTLCommandBufferStatusCompleted)return YES;
    if(_cleanupStarted) {
        [_condition lock];BOOL ready=_cleanupFinished;if(!ready)_blocked=YES;[_condition unlock];
        return ready?[self completeRetirement]:NO;
    }
    BOOL paused=NO,inlineRetired=NO;kern_return_t status=KERN_SUCCESS;
    // Native admission owns the transport lock. Keep borrowed snapshots alive
    // until run returns after unlocking, including partial failed snapshots.
    __attribute__((objc_precise_lifetime)) NSMutableArray *cpuResources=[NSMutableArray new];
    [_queue->_owner->_transportLock lock];publishCPU();_queue->_executing=self;
    @try {
      @try {
        if(!self.error && !_awaitingRetirement) {
            status=_queue->_completionError?:_queue->_owner->_submissionError;
            QcomMTLOperation *first=_operations.firstObject;
            if(!status && !self.nativeAdmitted && (!first || first.event || first.fence)) {
                _queue->_markerOnly=(first.event && !first.signalEvent) || (first.fence && !first.signalFence);
                GPU::Command barrier{GPU::Barrier,0,0,0,0,0};status=[_queue submitCommands:&barrier count:1];
                _queue->_markerOnly=NO;
            }
            while(!status && _nextOperation<_operations.count) {
                QcomMTLOperation *operation=_operations[_nextOperation];
                BOOL valid=NO;NSArray *live=[operation copyLiveResourcesWithValidity:&valid];[cpuResources addObjectsFromArray:live];
                if(!valid){status=kIOReturnBadArgument;break;}
                if(operation.fence) {
                    if(operation.signalFence) {
                        auto point=operation.fencePoint;
                        if(!point){status=kIOReturnBadArgument;break;}
                        // The native FIFO orders subsequent passes on this
                        // queue. Publish that dependency without blocking CPU
                        // admission; other queues still need real retirement.
                        if(![point admitForQueue:_queue->_queue.id token:_queue->_completions.issued()]) {
                            status=kIOReturnIOError;break;
                        }
                        if(![point retireThrough:_queue->_retiredToken])[_queue->_fenceUpdates addObject:point];
                    } else {
                        auto points=operation.fencePoints;
                        if(!points.count){status=kIOReturnBadArgument;break;}
                        auto read=[&](uint64_t i) {return [points[NSUInteger(i)] observationForQueue:_queue->_queue.id];};
                        auto dependencies=MetalSync::dependencies(points.count,read);
                        if(dependencies.state==MetalSync::DependencyState::Pending) {
                            [_condition lock];_blocked=YES;[_condition unlock];
                            if(!operation.fenceSubscribed) {
                                operation.fenceSubscribed=YES;__weak QcomMTLCommandBuffer *weak=self;
                                for(QcomMTLFencePoint *point in points)[point notifyWhenFinished:^{[weak wake];}];
                            }
                            dependencies=MetalSync::dependencies(points.count,read);
                            if(dependencies.state==MetalSync::DependencyState::Pending){paused=YES;break;}
                            [_condition lock];_blocked=NO;[_condition unlock];
                        }
                        status=kern_return_t(dependencies.error);
                    }
                } else if(operation.event && !operation.signalEvent) {
                    [_condition lock];BOOL ready=_eventPending && _eventReady,launch=!_eventPending;
                    if(ready){status=_eventResult;_eventPending=_eventReady=NO;}
                    else {_eventPending=YES;_blocked=YES;}
                    [_condition unlock];
                    if(!ready) {
                        if(launch) {
                            QcomMTLCommandQueue *queue=_queue;__weak QcomMTLCommandBuffer *weak=self;
                            dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{@autoreleasepool {
                                kern_return_t result=kIOReturnError;
                                @try {result=[queue executeEvent:operation];}
                                @catch(NSException *exception){(void)exception;}
                                [weak resumeEvent:result];
                            }});
                        }
                        paused=YES;break;
                    }
                } else if(operation.event) {
                    status=[_queue waitNative:_queue->_completions.issued() buffer:self allQueues:NO];
                    if(status==kIOReturnNotReady && !_queue->_completionError){paused=YES;status=0;break;}
                    if(status)break;
                    _queue->_executing=nil;publishCPU();[_queue->_owner->_transportLock unlock];
                    @try {status=[_queue executeEvent:operation];}
                    @finally {[_queue->_owner->_transportLock lock];_queue->_executing=self;publishCPU();}
                } else if(operation.renderPass) {
                    NSMutableArray<QcomMTLOperation *> *pass=[NSMutableArray array];
                    if(!operation.renderPassDraws || operation.renderPassDraws>_operations.count-_nextOperation)status=kIOReturnBadArgument;
                    for(NSUInteger i=0;!status && i<operation.renderPassDraws;++i) {
                        auto draw=_operations[_nextOperation+i];BOOL valid=NO;
                        NSArray *live=[draw copyLiveResourcesWithValidity:&valid];
                        if(!valid){status=kIOReturnBadArgument;break;}
                        [cpuResources addObjectsFromArray:live];[pass addObject:draw];
                    }
                    ++timing.record.detail;
                    if(!status)status=[_queue submitShaderBatch:pass];
                    if(!status)_nextOperation+=pass.count-1;
                } else if(operation.shaderOpcode) {
                    // Keep fence/event/barrier and attribution boundaries.
                    // Borrowed references survive the whole native admission.
                    NSMutableArray<QcomMTLOperation *> *batch=[NSMutableArray arrayWithObject:operation];
                    GPU::MetalBatch::Plan plan(_queue->_shmem.bytes,uint32_t(_executionResponsibleTasks.length/4));
                    const bool planned=plan.append(operation.shaderOpcode,operation.shaderPayload.bytes,operation.shaderPayload.length);
                    auto batchableShader=[](QcomMTLOperation *op)->bool {
                        if(op.shaderOpcode==GPU::ShaderDraw)return op.shaderPayload.length==sizeof(GPU::Shader::Draw);
                        if(op.shaderOpcode!=GPU::ShaderDispatch || op.shaderPayload.length!=sizeof(GPU::Shader::Dispatch))return false;
                        const auto &d=*static_cast<const GPU::Shader::Dispatch *>(op.shaderPayload.bytes);
                        const auto *p=GPUPrograms::lookup(d.program);
                        // These builtins freeze layout parameters on the CPU.
                        // Keep them outside a batch with GPU producers.
                        return op.immutableFill || !p || (strcmp(p->name,"blit_copy_words") && strcmp(p->name,"blit_fill_words") &&
                            strcmp(p->name,"blit_copy_bytes") && strcmp(p->name,"blit_fill_bytes") && strcmp(p->name,"blit_fill_rows") &&
                            strcmp(p->name,"blit_fill_pairs") && strcmp(p->name,"blit_fill_rows_2d") && strcmp(p->name,"blit_fill_pairs_2d") &&
                            strcmp(p->name,"blit_fill_rows_2d_tail") && strcmp(p->name,"blit_fill_pairs_2d_tail"));
                    };
                    const bool mixed=_queue->_owner->_mixedBatchShaders && batchableShader(operation);
                    for(NSUInteger next=_nextOperation+1;planned && (mixed || operation.shaderOpcode==GPU::ShaderDraw) && _queue->_owner->_batchShaders && next<_operations.count && batch.count<GPU::Native::MaxBatchCommands;++next) {
                        auto candidate=_operations[next];
                        if(candidate.renderPass || candidate.event || candidate.fence || candidate.barrier || candidate.responsibleTaskIDs) {timing.record.detail+=uint64_t(1)<<32;break;}
                        if(mixed?!batchableShader(candidate):candidate.shaderOpcode!=operation.shaderOpcode) {timing.record.detail+=uint64_t(1)<<32;break;}
                        BOOL nextValid=NO;NSArray *nextLive=[candidate copyLiveResourcesWithValidity:&nextValid];
                        [cpuResources addObjectsFromArray:nextLive];
                        if(!nextValid){status=kIOReturnBadArgument;break;}
                        BOOL boundary=NO;
                        if(!mixed)for(QcomMTLOperation *prior in batch)if(prior.shaderBufferWrites){boundary=YES;break;}
                        if(candidate.shaderOpcode==GPU::ShaderDraw && candidate.shaderPayload.length==sizeof(GPU::Shader::Draw)) {
                            const auto &draw=*static_cast<const GPU::Shader::Draw *>(candidate.shaderPayload.bytes);
                            QcomMTLBuffer *indices=nil;
                            // Retained buffers already own their resources;
                            // nextLive only snapshots borrowed references.
                            NSArray *indexResources=candidate->_borrowed?nextLive:candidate.resources;
                            if(draw.indexSize)for(id resource in indexResources)
                                if([resource isKindOfClass:[QcomMTLBuffer class]] && ((QcomMTLBuffer *)resource)->_resource.id==draw.indices.resource)
                                    {indices=(QcomMTLBuffer *)resource;break;}
                            if(draw.indexSize)for(QcomMTLOperation *prior in batch) {
                                if(prior.immutableFill) {
                                    auto destination=prior.destination;
                                    auto a=destination->_allocationRoot?:destination,b=indices?(indices->_allocationRoot?:indices):nil;
                                    // A private driver allocation exposes no CPU
                                    // storage that a no-copy/IOSurface index buffer
                                    // could alias. Shared/imported destinations keep
                                    // the conservative boundary; the kernel still
                                    // checks physical aliases before snapshotting.
                                    if(!b || a->_resource.id==b->_resource.id || a->_surface || a->_clientBytes ||
                                        ((b->_surface || b->_clientBytes) && a.storageMode!=MTLStorageModePrivate))
                                        {boundary=YES;break;}
                                    if(b->_surface || b->_clientBytes)timing.record.argument+=uint64_t(1)<<16;
                                    continue;
                                }
                                if(prior.shaderOpcode!=GPU::ShaderDraw || prior.shaderBufferWrites){boundary=YES;break;}
                                const auto &before=*static_cast<const GPU::Shader::Draw *>(prior.shaderPayload.bytes);
                                for(unsigned slot=0;slot<QGPU_COLOR_TARGET_COUNT;++slot)
                                    if(GPU::Shader::colorTarget(before,slot).resource==draw.indices.resource)boundary=YES;
                                if(before.depth.resource==draw.indices.resource || before.stencil.resource==draw.indices.resource)boundary=YES;
                            }
                        }
                        if(boundary) {timing.record.detail+=uint64_t(1)<<16;break;}
                        if(!plan.append(candidate.shaderOpcode,candidate.shaderPayload.bytes,candidate.shaderPayload.length)) {timing.record.detail+=uint64_t(1)<<48;break;}
                        [batch addObject:candidate];
                    }
                    ++timing.record.detail;
                    if(batch.count==GPU::Native::MaxBatchCommands)++timing.record.argument;
                    if(plan.resources()>(timing.record.argument>>32))
                        timing.record.argument=(timing.record.argument&UINT32_MAX)|(uint64_t(plan.resources())<<32);
                    if(!status)status=[_queue submitShaderBatch:batch];
                    if(!status)_nextOperation+=batch.count-1;
                } else status=[_queue execute:operation];
                if(uint32_t(status)==GPU::Native::AdmissionDeferredStatus) {
                    status=[_queue deferNativeForBuffer:self];
                    if(status==kIOReturnNotReady){paused=YES;status=0;break;}
                }
                if(status==kIOReturnNotReady && !_queue->_completionError && !_queue->_owner->_submissionError) {
                    status=[_queue waitNative:0 buffer:self allQueues:YES];
                    if(status==kIOReturnNotReady){paused=YES;status=0;break;}
                    if(!status)status=kIOReturnIOError; // no producer could release the requested slot/read boundary
                }
                if(!status)++_nextOperation;
            }
            // A first scheduling marker may also encounter the bounded window.
            if(uint32_t(status)==GPU::Native::AdmissionDeferredStatus) {
                status=[_queue deferNativeForBuffer:self];
                if(status==kIOReturnNotReady){paused=YES;status=0;}
            }
            if(status==kIOReturnNotReady && !_queue->_completionError && !_queue->_owner->_submissionError) {
                status=[_queue waitNative:0 buffer:self allQueues:YES];
                if(status==kIOReturnNotReady){paused=YES;status=0;}
                else if(!status)status=kIOReturnIOError;
            }
        }
      } @catch(NSException *exception) {(void)exception;status=kIOReturnError;paused=NO;}
      if(!paused) {
        if(status){[_condition lock];if(!_error)_error=transportError(status);[_condition unlock];}
        if(!self.error && self.nativeAdmitted)[self markScheduled];
        if(!self.error && _queue->_owner->_surfaceFences && !_presentationsSubmitted && _presentations.count) {
            BOOL nativeSurfaces=YES;
            for(QcomMTLPresentation *pending in _presentations)
                if(!((QcomMTLTexture *)pending.texture)->_backing->_surface)nativeSurfaces=NO;
            if(nativeSurfaces) {
                // Whole-list admission registered real IOSurface access
                // fences. Native display preparation now waits on the GPU
                // producer while CPU retirement proceeds independently.
                // Keep drawable/resource references and the FIFO presentation
                // barrier until real end-token retirement as before.
                _queue->_executing=nil;publishCPU();[_queue->_owner->_transportLock unlock];
                @try {[self submitPresentations];}
                @finally {[_queue->_owner->_transportLock lock];_queue->_executing=self;publishCPU();}
            }
        }
        if(!_awaitingRetirement) {
            // Include all preceding work on this actual native queue.
            _completionToken=_queue->_completions.issued();_awaitingRetirement=YES;
        }
        auto retired=[_queue waitNative:_completionToken buffer:self allQueues:NO];
        inlineRetired=!retired && !_queue->_completionError && _queue->_retiredToken>=_completionToken;
        if(retired==kIOReturnNotReady && !_queue->_completionError){paused=YES;status=0;}
        else if(retired)status=retired;
        if(!_inRetirement) {
            if(status){[_condition lock];if(!_error)_error=transportError(status);[_condition unlock];}
            // Every terminal buffer, including an already-retired or failed
            // one, enters the same FIFO before delivering completed handlers.
            paused=YES;status=0;
        }
      }
    } @finally {_queue->_markerOnly=NO;_queue->_executing=nil;publishCPU();[_queue->_owner->_transportLock unlock];}
    if(paused && !status)return NO;
    if(status){[_condition lock];if(!_error)_error=transportError(status);[_condition unlock];}
    if(self.error) for(QcomMTLOperation *operation in _operations)if(operation.signalFence)
        [operation.fencePoint finish:status?status:kIOReturnAborted queue:0 token:0];
    // Legacy kernels and non-IOSurface drawables retain completed-only
    // presentation. Native fenced drawables were queued once at Scheduled.
    [self submitPresentations];
    const uint64_t cleanupBytes=retiredStorageBytes(_operations,_ownedResources,_presentations);
    [_queue->_orderLock lock];auto admission=_queue->_cleanupBudget.acquire(cleanupBytes);
    if(admission==MetalRetirement::Budget::Admission::Wait) {
        ++_queue->_cleanupWaits;[_condition lock];_blocked=YES;[_condition unlock];
        [_queue->_orderLock unlock];return NO;
    }
    [_queue->_orderLock unlock];
    [_condition lock];
    // Transfer existing ownership without releasing a resource under either
    // the condition or the queue's submission execution lock. Only successful
    // physical retirement, never public status, makes inline roots reusable.
    NSMutableArray *retiredOperations=_operations,*retiredOwned=_ownedResources,*retiredPresentations=_presentations;
    _operations=[NSMutableArray new];_ownedResources=[NSMutableArray new];_presentations=[NSMutableArray new];
    if(inlineRetired && !_error)for(id resource in retiredOwned)if([resource isKindOfClass:[QcomMTLBuffer class]]) {
        auto *page=(QcomMTLBuffer *)resource;
        if(page->_inlinePoolPage && !page->_allocationRoot)page->_inlinePoolRetired=YES;
    }
    // The native ID is recycled only by the root's LAST release, even if an
    // old encoder still retains a snapshot. No pool/transport lock under this
    // condition; failed/cancelled buffers keep the ordinary delete/quarantine.
    _inlinePage=nil;_inlineUsed=_inlinePages=0;
    BOOL exclusive=admission==MetalRetirement::Budget::Admission::Exclusive;
    if(exclusive){_cleanupStarted=YES;_blocked=YES;}
    [_condition unlock];
    const uint64_t enqueued=nativeCompletionClock();QcomMTLCommandQueue *queue=_queue;
    // Only an oversized buffer needs to survive until cleanup. Normal blocks
    // retain the detached arrays and the queue, not the completed buffer.
    QcomMTLCommandBuffer *pending=exclusive?self:nil;
    dispatch_async(queue->_cleanup,^{
        const uint64_t begin=nativeCompletionClock();
        @autoreleasepool {
            [retiredPresentations removeAllObjects];[retiredOperations removeAllObjects];[retiredOwned removeAllObjects];
        }
        if(pending){[pending->_condition lock];pending->_cleanupFinished=YES;[pending->_condition unlock];}
        [queue releaseCleanupBytes:cleanupBytes admission:admission nanos:nativeCompletionClock()-begin queued:begin-enqueued];
    });
    // Oversized storage never joins the completed-buffer backlog: keep its
    // FIFO retirement/capacity reservation until the exclusive release ends.
    return exclusive?NO:[self completeRetirement];
}
- (BOOL)completeRetirement {
    [_condition lock];
    _blocked=NO;_status = _error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
    if(_error)[_queue->_owner reportMetalFailure:GPU::Diagnostics::CommandBuffer error:_error];
    [_scheduledHandlers removeAllObjects];
    NSArray *completed = [_completedHandlers copy]; [_completedHandlers removeAllObjects];
    if (_hasCapacity) { _hasCapacity = NO; dispatch_semaphore_signal(_queue->_capacity); }
    [_condition broadcast];
    [self dispatchHandlers:completed scheduled:NO];
    [_condition unlock];
    return YES;
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
    [_condition lock];
    if (_encoderActive || _status >= MTLCommandBufferStatusCommitted || _error) {
        if (!_error && _status < MTLCommandBufferStatusCommitted) _error = failure(@"Only one command encoder may be active");
        [_condition unlock]; return nil;
    }
    _encoderActive = YES; [_condition unlock];
    return [[QcomMTLBlitCommandEncoder alloc] initWithBuffer:self];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)descriptor {
    // Counter attachments would require commands not present in this backend.
    for (NSUInteger i = 0; i < 4; ++i) if (descriptor.sampleBufferAttachments[i].sampleBuffer) {
        [self fail:failure(@"Blit counter attachments are not implemented")]; return nil;
    }
    return [self blitCommandEncoder];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)descriptor {
    [_condition lock];
    if (_encoderActive || _status >= MTLCommandBufferStatusCommitted || _error) {
        if (!_error && _status < MTLCommandBufferStatusCommitted) _error = failure(@"Only one command encoder may be active");
        [_condition unlock]; return nil;
    }
    _encoderActive = YES; [_condition unlock];
    NSString *rejection=nil;
    QcomMTLRenderEncoder *encoder = [[QcomMTLRenderEncoder alloc] initWithBuffer:self descriptor:descriptor rejection:&rejection];
    if (!encoder) { [_condition lock]; _encoderActive = NO; [_condition unlock];
        NSMutableString *detail=[NSMutableString stringWithFormat:@"Unsupported render pass: reason=%@ size=%lux%lu layers=%lu visibility=%u rate=%u sample_positions=%lu",
            rejection?:@"encoder-allocation",(unsigned long)descriptor.renderTargetWidth,(unsigned long)descriptor.renderTargetHeight,
            (unsigned long)descriptor.renderTargetArrayLength,descriptor.visibilityResultBuffer!=nil,
            descriptor.rasterizationRateMap!=nil,(unsigned long)[descriptor getSamplePositions:nullptr count:0]];
        for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT+2;++i) {
            MTLRenderPassAttachmentDescriptor *a=i<QGPU_COLOR_TARGET_COUNT?descriptor.colorAttachments[i]:
                i==QGPU_COLOR_TARGET_COUNT?(MTLRenderPassAttachmentDescriptor *)descriptor.depthAttachment:descriptor.stencilAttachment;
            if(!a.texture)continue;
            [detail appendFormat:@" a%u={native=%u format=%lu size=%lux%lu samples=%lu usage=%lu level=%lu slice=%lu depth=%lu load=%lu store=%lu opts=%lu resolve=%u}",
                i,[a.texture isKindOfClass:[QcomMTLTexture class]],(unsigned long)a.texture.pixelFormat,
                (unsigned long)a.texture.width,(unsigned long)a.texture.height,(unsigned long)a.texture.sampleCount,
                (unsigned long)a.texture.usage,(unsigned long)a.level,(unsigned long)a.slice,(unsigned long)a.depthPlane,
                (unsigned long)a.loadAction,(unsigned long)a.storeAction,(unsigned long)a.storeActionOptions,a.resolveTexture!=nil];
            [detail appendFormat:@" storage=%lu same_device=%u levels=%lu array=%lu",
                (unsigned long)a.texture.storageMode,a.texture.device==self.device,
                (unsigned long)a.texture.mipmapLevelCount,(unsigned long)a.texture.arrayLength];
            if(i<QGPU_COLOR_TARGET_COUNT) {
                auto c=descriptor.colorAttachments[i].clearColor;
                [detail appendFormat:@" clear={%a,%a,%a,%a}",c.red,c.green,c.blue,c.alpha];
            }
        }
        [self fail:failure(detail)]; }
    return encoder;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
    [_condition lock];
    if (_encoderActive || _status >= MTLCommandBufferStatusCommitted || _error) {
        if (!_error && _status < MTLCommandBufferStatusCommitted) _error = failure(@"Only one command encoder may be active");
        [_condition unlock]; return nil;
    }
    _encoderActive = YES; [_condition unlock]; return [[QcomMTLComputeEncoder alloc] initWithBuffer:self];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)descriptor {
    if (descriptor.dispatchType != MTLDispatchTypeSerial) { [self fail:failure(@"Concurrent compute is not implemented")]; return nil; }
    for (NSUInteger i = 0; i < 4; ++i) if (descriptor.sampleBufferAttachments[i].sampleBuffer) {
        [self fail:failure(@"Compute counter attachments are not implemented")]; return nil;
    }
    return [self computeCommandEncoder];
}
- (void)forwardInvocation:(NSInvocation *)invocation {
    if (self.status >= MTLCommandBufferStatusCommitted) { [super forwardInvocation:invocation]; return; }
    [self fail:failure([@"Unsupported Metal command: " stringByAppendingString:NSStringFromSelector(invocation.selector)])];
    clearReturn(invocation);
}
- (void)pushDebugGroup:(NSString *)string { (void)string; }
- (void)popDebugGroup {}
@end

namespace {
void finishEncoder(QcomMTLCommandBuffer *buffer, BOOL &ended) {
    [buffer->_condition lock];
    if (ended || buffer->_status >= MTLCommandBufferStatusCommitted) {
        [buffer->_condition unlock]; [NSException raise:NSInvalidArgumentException format:@"Encoder ended or command buffer committed"]; return;
    }
    ended = YES; buffer->_encoderActive = NO; [buffer->_condition unlock];
}
void checkEncoder(BOOL ended) {
    if (ended) [NSException raise:NSInvalidArgumentException format:@"Encoder already ended"];
}
bool renderFenceStages(QcomMTLCommandBuffer *buffer,MTLRenderStages stages) {
    static_assert((MTLRenderStageVertex|MTLRenderStageFragment)==3,"native render stage bits");
    if(MetalSync::renderStages(stages))return true;
    [buffer fail:failure(@"Unsupported render fence/barrier stages",MTLCommandBufferErrorInvalidResource)];return false;
}
void scopeBarrier(QcomMTLCommandBuffer *buffer,MTLBarrierScope scope) {
    static_assert((MTLBarrierScopeBuffers|MTLBarrierScopeTextures)==3,"native iOS barrier scope bits");
    if(!MetalSync::barrierScope(scope)){[buffer fail:failure(@"Unsupported memory barrier scope",MTLCommandBufferErrorInvalidResource)];return;}
    [buffer recordBarrierWithResources:nil];
}
void resourceBarrier(QcomMTLCommandBuffer *buffer,const id<MTLResource> resources[],NSUInteger count) {
    if((count && !resources) || count>GPU::Native::Memory::ResourcesPerJob){[buffer fail:failure(@"Invalid memory barrier resources",MTLCommandBufferErrorInvalidResource)];return;}
    NSMutableArray *retained=[NSMutableArray arrayWithCapacity:count];
    for(NSUInteger i=0;i<count;++i){if(!resources[i]){[buffer fail:failure(@"Nil barrier resource",MTLCommandBufferErrorInvalidResource)];return;}[retained addObject:resources[i]];}
    [buffer recordBarrierWithResources:retained];
}
}

@implementation QcomMTLComputeEncoder
- (void)clearBindings {
    _pipeline.set(nil,false);
    for(auto &reference:_buffers)reference.set(nil,false);
    for(auto &reference:_textures)reference.set(nil,false);
    for(auto &reference:_samplers)reference.set(nil,false);
}
- (NSArray *)copyLiveBindings {
    if(_buffer.retainedReferences)return @[];
    NSMutableArray *live=[NSMutableArray new];if(!captureReference(_pipeline,live))return nil;
    for(const auto &reference:_buffers)if(!captureReference(reference,live))return nil;
    for(const auto &reference:_textures)if(!captureReference(reference,live))return nil;
    for(const auto &reference:_samplers)if(!captureReference(reference,live))return nil;
    return live;
}
+ (Protocol *)metalProtocol { return @protocol(MTLComputeCommandEncoder); }
- (instancetype)initWithBuffer:(QcomMTLCommandBuffer *)buffer {
    if ((self = [super init])) {_buffer = buffer;_scope=[[QcomMTLEncoderScope alloc] initWithBuffer:buffer];} return self;
}
- (id<MTLDevice>)device { return _buffer.device; }
- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }
- (void)endEncoding { checkEncoder(_ended);[_scope finish];finishEncoder(_buffer, _ended);[self clearBindings]; }
- (void)updateFence:(id<MTLFence>)fence { checkEncoder(_ended);[_scope fence:fence signal:YES]; }
- (void)waitForFence:(id<MTLFence>)fence { checkEncoder(_ended);[_scope fence:fence signal:NO]; }
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope {checkEncoder(_ended);scopeBarrier(_buffer,scope);}
- (void)memoryBarrierWithResources:(const id<MTLResource>[])resources count:(NSUInteger)count {checkEncoder(_ended);resourceBarrier(_buffer,resources,count);}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state {
    checkEncoder(_ended);
    if (![state isKindOfClass:[QcomMTLComputePipeline class]] || state.device != self.device) {
        [_buffer fail:failure(@"Invalid compute pipeline", MTLCommandBufferErrorInvalidResource)]; return;
    }
    _pipeline.set((QcomMTLComputePipeline *)state,_buffer.retainedReferences);
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if (index >= GPU::Shader::MaxBindings || (offset & 3) ||
        (buffer && (![buffer isKindOfClass:[QcomMTLBuffer class]] || buffer.device != self.device || offset > buffer.length))) {
        [_buffer fail:failure(@"Invalid compute buffer binding", MTLCommandBufferErrorInvalidResource)]; return;
    }
    _buffers[index].set((QcomMTLBuffer *)buffer,_buffer.retainedReferences); _offsets[index] = offset;
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if (index>=GPU::Shader::MaxBindings || length>4096 || (length && !bytes)) {
        [_buffer fail:failure(@"Invalid inline compute data",MTLCommandBufferErrorInvalidResource)];return;
    }
    if (!length) { [self setBuffer:nil offset:0 atIndex:index];return; }
    id<MTLBuffer> snapshot=[_buffer newInlineSnapshot:bytes length:length];
    if (!snapshot) { [_buffer fail:failure(@"Inline compute buffer allocation failed",MTLCommandBufferErrorOutOfMemory)];return; }
    [self setBuffer:snapshot offset:0 atIndex:index];
}
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= GPU::Shader::MaxBindings || !_buffers[index].get()) { [_buffer fail:failure(@"Compute buffer not bound")]; return; }
    [self setBuffer:_buffers[index].get() offset:offset atIndex:index];
}
- (void)setBuffers:(const id<MTLBuffer>[])buffers offsets:(const NSUInteger[])offsets withRange:(NSRange)range {
    checkEncoder(_ended);
    if (!buffers || !offsets || !validRange(GPU::Shader::MaxBindings,range.location,range.length)) {
        [_buffer fail:failure(@"Invalid compute binding range")]; return;
    }
    for (NSUInteger i = 0; i < range.length; ++i) [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location+i];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if (index>=GPU::Shader::MaxBindings || length>QGPU_MAX_THREADGROUP_BYTES || (length&15)) {
        [_buffer fail:failure(@"Invalid threadgroup memory length/index",MTLCommandBufferErrorInvalidResource)];return;
    }
    _threadgroupLengths[index]=(uint32_t)length;
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if (index>=QGPU_COMPUTE_IMAGE_COUNT || (texture && (![texture isKindOfClass:[QcomMTLTexture class]] ||
        texture.device!=self.device || texture.storageMode==MTLStorageModeMemoryless || texture.framebufferOnly || (!colorFormat(texture.pixelFormat) && !depthFormat(texture.pixelFormat))))) {
        [_buffer fail:failure(@"Invalid compute texture binding",MTLCommandBufferErrorInvalidResource)];return;
    }
    _textures[index].set((QcomMTLTexture *)texture,_buffer.retainedReferences);
}
- (void)setTextures:(const id<MTLTexture>[])textures withRange:(NSRange)range {
    checkEncoder(_ended);
    if (!textures || !validRange(QGPU_COMPUTE_IMAGE_COUNT,range.location,range.length)) {[_buffer fail:failure(@"Invalid compute texture range")];return;}
    for (NSUInteger i=0;i<range.length;++i)[self setTexture:textures[i] atIndex:range.location+i];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if(index>=QGPU_COMPUTE_IMAGE_COUNT || (sampler && (![sampler isKindOfClass:[QcomMTLSampler class]] || sampler.device!=self.device))) {
        [_buffer fail:failure(@"Invalid compute sampler",MTLCommandBufferErrorInvalidResource)];return;
    }
    _samplers[index].set((QcomMTLSampler *)sampler,_buffer.retainedReferences);
    _samplerStates[index]=_samplers[index].get()?_samplers[index].get()->_state:GPU::Shader::Sampler{};
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler lodMinClamp:(float)minimum lodMaxClamp:(float)maximum atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if(index>=QGPU_COMPUTE_IMAGE_COUNT || !__builtin_isfinite(minimum) || !__builtin_isfinite(maximum) || minimum<0 || maximum<minimum) {
        [_buffer fail:failure(@"Invalid compute sampler LOD clamps",MTLCommandBufferErrorInvalidResource)];return;
    }
    [self setSamplerState:sampler atIndex:index];
    if(_samplers[index].get()){_samplerStates[index].minLOD=uint32_t(MIN(minimum,4095.0f/256)*256);_samplerStates[index].maxLOD=uint32_t(MIN(maximum,4095.0f/256)*256);}
}
- (void)setSamplerStates:(const id<MTLSamplerState>[])samplers withRange:(NSRange)range {
    checkEncoder(_ended);
    if(!samplers || !validRange(QGPU_COMPUTE_IMAGE_COUNT,range.location,range.length)){[_buffer fail:failure(@"Invalid compute sampler range")];return;}
    for(NSUInteger i=0;i<range.length;++i)[self setSamplerState:samplers[i] atIndex:range.location+i];
}
- (void)setSamplerStates:(const id<MTLSamplerState>[])samplers lodMinClamps:(const float[])minimum lodMaxClamps:(const float[])maximum withRange:(NSRange)range {
    checkEncoder(_ended);
    if(!samplers || !minimum || !maximum || !validRange(QGPU_COMPUTE_IMAGE_COUNT,range.location,range.length)){[_buffer fail:failure(@"Invalid compute sampler/clamp range")];return;}
    for(NSUInteger i=0;i<range.length;++i)[self setSamplerState:samplers[i] lodMinClamp:minimum[i] lodMaxClamp:maximum[i] atIndex:range.location+i];
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)local {
    [self encodeDispatch:groups local:local grid:MTLSizeMake(0,0,0)];
}
- (void)encodeDispatch:(MTLSize)groups local:(MTLSize)local grid:(MTLSize)grid {
    checkEncoder(_ended);
    __attribute__((objc_precise_lifetime)) NSArray *live=[self copyLiveBindings];
    if(!live){[_buffer fail:failure(@"An unretained compute binding expired",MTLCommandBufferErrorInvalidResource)];return;}
    if (!_pipeline.get() || !local.width || !local.height || !local.depth ||
        local.width>GPU::Shader::MaxComputeThreads || local.height>GPU::Shader::MaxComputeThreads || local.depth>64 ||
        local.width*local.height*local.depth>_pipeline.get().maxTotalThreadsPerThreadgroup ||
        (_pipeline.get()->_threadgroupMultiple && local.width*local.height*local.depth%_pipeline.get().threadExecutionWidth) ||
        groups.width>GPU::Shader::MaxComputeGroups || groups.height>GPU::Shader::MaxComputeGroups || groups.depth>GPU::Shader::MaxComputeGroups) {
        [_buffer fail:failure(@"Invalid A540 workgroup dimensions or thread count")]; return;
    }
    if (!groups.width || !groups.height || !groups.depth) return;
    const auto *program = _pipeline.get()->_function->_program;
    GPU::Shader::Dispatch dispatch{}; dispatch.registry = GPUPrograms::Registry;
    dispatch.version = GPU::Shader::Version; dispatch.program = program->id;
    dispatch.groups[0]=(uint32_t)groups.width;dispatch.groups[1]=(uint32_t)groups.height;dispatch.groups[2]=(uint32_t)groups.depth;
    dispatch.local[0]=(uint32_t)local.width;dispatch.local[1]=(uint32_t)local.height;dispatch.local[2]=(uint32_t)local.depth;
    dispatch.grid[0]=(uint32_t)grid.width;dispatch.grid[1]=(uint32_t)grid.height;dispatch.grid[2]=(uint32_t)grid.depth;
    for (unsigned i=0;i<GPU::Shader::MaxBindings;++i) if (program->threadgroupMask&(1u<<i)) dispatch.threadgroupLengths[i]=_threadgroupLengths[i];
    NSMutableArray *retained = [NSMutableArray arrayWithObject:_pipeline.get()];
    for(unsigned i=0;i<QGPU_COMPUTE_IMAGE_COUNT;++i) if((program->imageMask|program->sampleMask)&(1u<<i)) {
        auto *texture=_textures[i].get();MTLTextureUsage required=MTLTextureUsageUnknown;
        if((program->imageReadMask|program->sampleMask)&(1u<<i))required|=MTLTextureUsageShaderRead;
        if(program->imageWriteMask&(1u<<i))required|=MTLTextureUsageShaderWrite;
        if (!texture || (texture.usage && (texture.usage&required)!=required)) {
            [_buffer fail:failure(@"Missing compute texture or incompatible usage",MTLCommandBufferErrorInvalidResource)];return;
        }
        dispatch.images[i]=[texture nativeTexture];[retained addObject:texture];
    }
    for(unsigned i=0;i<QGPU_COMPUTE_IMAGE_COUNT;++i)if(program->samplerMask&(1u<<i)) {
        if(!_samplers[i].get()){[_buffer fail:failure(@"Missing compute sampler",MTLCommandBufferErrorInvalidResource)];return;}
        dispatch.samplers[i]=_samplerStates[i];[retained addObject:_samplers[i].get()];
    }
    uint64_t elements=0;unsigned dimensions=0;
    if (!GPU::Shader::dispatchFootprint(dispatch,*program,elements,dimensions)) {
        [_buffer fail:failure(@"Invalid compute grid, shared memory or texture bindings")];return;
    }
    for (NSUInteger i = 0; i < GPU::Shader::MaxBindings; ++i) if (program->bufferMask & (1u << i)) {
        QcomMTLBuffer *buffer = _buffers[i].get(); NSUInteger offset = _offsets[i];
        uint64_t required=0;
        if (!buffer || !GPU::Shader::bindingBytes(*program,(unsigned)i,elements,0,required) || !validRange(buffer.length,offset,required)) {
            [_buffer fail:failure(@"Compute grid exceeds a bound buffer", MTLCommandBufferErrorInvalidResource)]; return;
        }
        dispatch.buffers[i] = {buffer->_resource.id, (uint32_t)(buffer->_nativeOffset+offset), (uint32_t)(buffer.length-offset), 0};
        [retained addObject:buffer];
    }
    QcomMTLOperation *operation = [QcomMTLOperation new]; operation.shaderOpcode = GPU::ShaderDispatch;
    operation.shaderPayload = [NSData dataWithBytes:&dispatch length:sizeof(dispatch)]; operation.resources = retained;
    [_buffer record:operation];
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)local {
    checkEncoder(_ended);
    if (!local.width || !local.height || !local.depth ||
        grid.width>UINT32_MAX || grid.height>UINT32_MAX || grid.depth>UINT32_MAX) {
        [_buffer fail:failure(@"Invalid compute thread grid")]; return;
    }
    MTLSize groups=MTLSizeMake(grid.width/local.width+NSUInteger(grid.width%local.width!=0),
        grid.height/local.height+NSUInteger(grid.height%local.height!=0),grid.depth/local.depth+NSUInteger(grid.depth%local.depth!=0));
    [self encodeDispatch:groups local:local grid:grid];
}
- (void)forwardInvocation:(NSInvocation *)invocation {
    if (_ended || _buffer.status >= MTLCommandBufferStatusCommitted) { [super forwardInvocation:invocation]; return; }
    [_buffer fail:failure([@"Unsupported compute command: " stringByAppendingString:NSStringFromSelector(invocation.selector)])];
    clearReturn(invocation);
}
- (void)insertDebugSignpost:(NSString *)string { (void)string; }
- (void)pushDebugGroup:(NSString *)string { (void)string; }
- (void)popDebugGroup {}
@end

@implementation QcomMTLRenderEncoder
- (void)clearBindings {
    _pipeline.set(nil,false);
    _depth.set(nil,false);
    _stencil.set(nil,false);
    for(auto &reference:_targets)reference.set(nil,false);
    for(auto &reference:_resolveTargets)reference.set(nil,false);
    for(auto &reference:_buffers)reference.set(nil,false);
    for(auto &reference:_fragmentBuffers)reference.set(nil,false);
    for(auto &stage:_sampledTextures)for(auto &reference:stage)reference.set(nil,false);
    for(auto &stage:_samplers)for(auto &reference:stage)reference.set(nil,false);
    for(auto &parameters:_resolveParameters)parameters=nil;
}
- (NSArray *)copyLiveBindings {
    if(_buffer.retainedReferences)return @[];
    NSMutableArray *live=[NSMutableArray new];
    if(!captureReference(_pipeline,live) || !captureReference(_depth,live) || !captureReference(_stencil,live))return nil;
    for(const auto &reference:_targets)if(!captureReference(reference,live))return nil;
    for(const auto &reference:_resolveTargets)if(!captureReference(reference,live))return nil;
    for(const auto &reference:_buffers)if(!captureReference(reference,live))return nil;
    for(const auto &reference:_fragmentBuffers)if(!captureReference(reference,live))return nil;
    for(const auto &stage:_sampledTextures)for(const auto &reference:stage)if(!captureReference(reference,live))return nil;
    for(const auto &stage:_samplers)for(const auto &reference:stage)if(!captureReference(reference,live))return nil;
    return live;
}
+ (Protocol *)metalProtocol { return @protocol(MTLRenderCommandEncoder); }
- (NSUInteger)tileWidth { return 0; }
- (NSUInteger)tileHeight { return 0; }
- (instancetype)initWithBuffer:(QcomMTLCommandBuffer *)buffer descriptor:(MTLRenderPassDescriptor *)d rejection:(NSString *__autoreleasing *)rejection {
    auto reject=[&](NSString *reason)->QcomMTLRenderEncoder * {
        if(rejection)*rejection=reason;return nil;
    };
    if(!(self=[super init]))return reject(@"self-allocation");
    _buffer=buffer;_scope=[[QcomMTLEncoderScope alloc] initWithBuffer:buffer];
    _passStart=buffer->_operations.count;
    if(!d || d.visibilityResultBuffer || d.rasterizationRateMap || d.renderTargetArrayLength>1 || [d getSamplePositions:nullptr count:0])return reject(@"descriptor");
    constexpr unsigned count=QGPU_COLOR_TARGET_COUNT+2;
    MTLRenderPassAttachmentDescriptor *__strong attachments[count]={};
    QcomMTLTexture *__strong planes[count]={};
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)attachments[i]=d.colorAttachments[i];
    attachments[QGPU_COLOR_TARGET_COUNT]=d.depthAttachment;attachments[QGPU_COLOR_TARGET_COUNT+1]=d.stencilAttachment;
    unsigned frame=count;
    for(unsigned i=0;i<count;++i) {
        auto attachment=attachments[i];if(!attachment.texture)continue;
        if(![attachment.texture isKindOfClass:[QcomMTLTexture class]] || attachment.texture.device!=self.device ||
            (attachment.texture.usage && !(attachment.texture.usage&MTLTextureUsageRenderTarget)) ||
            attachment.level>=attachment.texture.mipmapLevelCount || attachment.slice>=textureSliceCount(attachment.texture) || attachment.depthPlane ||
            attachment.loadAction>MTLLoadActionClear || attachment.storeActionOptions)return reject(@"attachment");
        bool resolve=attachment.storeAction==MTLStoreActionMultisampleResolve || attachment.storeAction==MTLStoreActionStoreAndMultisampleResolve;
        if(!resolve && attachment.storeAction!=MTLStoreActionStore && attachment.storeAction!=MTLStoreActionDontCare)return reject(@"store-action");
        if(bool(attachment.resolveTexture)!=resolve || (resolve && attachment.texture.sampleCount==1))return reject(@"resolve-action");
        auto format=attachment.texture.pixelFormat;
        if(i<QGPU_COLOR_TARGET_COUNT?!renderableColorFormat(format):i==QGPU_COLOR_TARGET_COUNT?!depthFormat(format):!stencilFormat(format))return reject(@"format");
        planes[i]=(QcomMTLTexture *)attachment.texture;
        if(planes[i]->_memoryless) {
            if(attachment.loadAction==MTLLoadActionLoad || attachment.storeAction!=MTLStoreActionDontCare)return reject(@"memoryless-action");
            for(unsigned j=0;j<i;++j)if(attachments[j].texture==attachment.texture)return reject(@"memoryless-alias");
            planes[i]=[planes[i] newPassBackingForQueue:buffer->_queue];if(!planes[i])return reject(@"memoryless-allocation");
            // Internal pass storage survives unretained client encoders too.
            [_buffer retainInternalResource:planes[i]];
        }
        _attachments[i]=i<QGPU_COLOR_TARGET_COUNT?[planes[i] nativeTextureAtLevel:attachment.level slice:attachment.slice]:
            [planes[i] nativePlane:i==QGPU_COLOR_TARGET_COUNT+1 level:attachment.level slice:attachment.slice];
        if(!_attachments[i].resource)return reject(@"native-plane");
        if(resolve) {
            auto target=attachment.resolveTexture;
            if(![target isKindOfClass:[QcomMTLTexture class]] || target.device!=self.device || target.sampleCount!=1 ||
                target.pixelFormat!=format || (target.usage && !(target.usage&MTLTextureUsageRenderTarget)) || attachment.resolveDepthPlane ||
                attachment.resolveLevel>=target.mipmapLevelCount || attachment.resolveSlice>=textureSliceCount(target))return reject(@"resolve-target");
            _resolveTargets[i].set((QcomMTLTexture *)target,_buffer.retainedReferences);
            _resolvePlanes[i]=i<QGPU_COLOR_TARGET_COUNT?[_resolveTargets[i].get() nativeTextureAtLevel:attachment.resolveLevel slice:attachment.resolveSlice]:
                [_resolveTargets[i].get() nativePlane:i==QGPU_COLOR_TARGET_COUNT+1 level:attachment.resolveLevel slice:attachment.resolveSlice];
            if(!_resolvePlanes[i].resource || _resolvePlanes[i].width!=_attachments[i].width || _resolvePlanes[i].height!=_attachments[i].height)return reject(@"resolve-plane");
        }
        if(frame==count)frame=i;
    }
    if(frame==count)return reject(@"no-attachment");
    for(unsigned i=0;i<4;++i)if(d.sampleBufferAttachments[i].sampleBuffer)return reject(@"sample-counter");
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)_targets[i].set(planes[i],_buffer.retainedReferences);
    _depth.set(planes[QGPU_COLOR_TARGET_COUNT],_buffer.retainedReferences);_stencil.set(planes[QGPU_COLOR_TARGET_COUNT+1],_buffer.retainedReferences);
    _width=_attachments[frame].width;_height=_attachments[frame].height;_samples=GPU::Shader::textureSamples(_attachments[frame]);
    // Check all aliases before any clear operation is recorded. Native
    // admission additionally checks distinct imports of the same physical pages.
    for(unsigned i=0;i<count;++i)if(planes[i]) {
        if(_attachments[i].width!=_width || _attachments[i].height!=_height || GPU::Shader::textureSamples(_attachments[i])!=_samples)return reject(@"attachment-extent");
        for(unsigned j=i+1;j<count;++j)if(planes[j]) {
            auto *a=planes[i]->_backing,*b=planes[j]->_backing;
            bool alias=a==b || a->_resource.id==b->_resource.id || (a->_surface && b->_surface && IOSurfaceGetID(a->_surface)==IOSurfaceGetID(b->_surface));
            auto x=_attachments[i],y=_attachments[j];x.resource=y.resource=1;
            if(alias && GPU::Shader::overlap(x,y))return reject(@"attachment-alias");
        }
    }
    // A resolve destination must not overwrite any attachment or another
    // resolve result, including aliased views/imports of the same storage.
    for(unsigned i=0;i<count;++i)if(_resolveTargets[i].get()) {
        auto *a=_resolveTargets[i].get()->_backing;
        for(unsigned j=0;j<2*count;++j) {
            auto texture=j<count?planes[j]:_resolveTargets[j-count].get();
            if(!texture || j==count+i)continue;
            auto *b=texture->_backing;
            bool alias=a==b || a->_resource.id==b->_resource.id || (a->_surface && b->_surface && IOSurfaceGetID(a->_surface)==IOSurfaceGetID(b->_surface));
            auto x=_resolvePlanes[i],y=j<count?_attachments[j]:_resolvePlanes[j-count];x.resource=y.resource=1;
            if(alias && GPU::Shader::overlap(x,y))return reject(@"resolve-alias");
        }
    }
    // Render extents constrain the pass; attachment pitches/layouts continue
    // to describe the full storage. Clear/resolve and default raster state
    // must all use the constrained extent.
    if(d.renderTargetWidth>_width || d.renderTargetHeight>_height)return reject(@"render-extent");
    if(d.renderTargetWidth)_width=d.renderTargetWidth;
    if(d.renderTargetHeight)_height=d.renderTargetHeight;
    if(d.depthAttachment.depthResolveFilter>MTLMultisampleDepthResolveFilterMax ||
        d.stencilAttachment.stencilResolveFilter>MTLMultisampleStencilResolveFilterDepthResolvedSample)return reject(@"resolve-filter");
    for(unsigned i=0;i<2;++i)if(_resolveTargets[QGPU_COLOR_TARGET_COUNT+i].get()) {
        auto &plan=_planeResolve[i];unsigned slot=QGPU_COLOR_TARGET_COUNT+i;
        if(!GPU::Shader::depthStencilResolvePlan(_attachments[slot],_resolvePlanes[slot],_depth.get()?&_attachments[QGPU_COLOR_TARGET_COUNT]:nullptr,
            uint32_t(d.depthAttachment.depthResolveFilter),i?uint32_t(d.stencilAttachment.stencilResolveFilter):0,plan))return reject(@"depth-stencil-plan");
        plan.parameters.width=uint32_t(_width);plan.parameters.height=uint32_t(_height);
        plan.width=i?(uint32_t(_width)+3)/4:uint32_t(_width);plan.height=uint32_t(_height);
        for(const auto &program:GPUPrograms::Programs)if(!strcmp(program.name,i?"resolve_stencil":"resolve_depth")){_planeResolvePrograms[i]=&program;break;}
        if(!_planeResolvePrograms[i])return reject(@"resolve-program");
        _resolveParameters[i]=(QcomMTLBuffer *)[self.device newBufferWithBytes:&plan.parameters length:sizeof(plan.parameters) options:MTLResourceStorageModeShared];
        if(!_resolveParameters[i])return reject(@"resolve-parameters");[_buffer retainInternalResource:_resolveParameters[i]];
    }
    _scissor={0,0,_width,_height};_state=GPU::Shader::defaultRenderState(uint32_t(_width),uint32_t(_height));
    _depthStencil=GPU::Shader::defaultDepthStencilState();_viewport={0,0,double(_width),double(_height),0,1};
    uint32_t clearValues[count]={},clearHigh[count]={};
    for(unsigned slot=0;slot<QGPU_COLOR_TARGET_COUNT;++slot)if(planes[slot] && attachments[slot].loadAction==MTLLoadActionClear) {
        auto color=d.colorAttachments[slot];double components[]={color.clearColor.red,color.clearColor.green,color.clearColor.blue,color.clearColor.alpha};uint8_t packed[4];
        if(planes[slot].pixelFormat==MTLPixelFormatRG32Float) {
            if(!std::isfinite(components[0]) || !std::isfinite(components[1]))return reject(@"rg32-clear");
            float rg[]={float(components[0]),float(components[1])};
            memcpy(&clearValues[slot],&rg[0],4);memcpy(&clearHigh[slot],&rg[1],4);continue;
        }
        if(planes[slot].pixelFormat==MTLPixelFormatRGBA16Float) {
            for(double value:components)if(!std::isfinite(value))return reject(@"rgba16f-clear");
            GPU::MetalColor::packRGBA16Float(components,clearValues[slot],clearHigh[slot]);continue;
        }
        for(unsigned i=0;i<4;++i) {
            if(!__builtin_isfinite(components[i]))return reject(@"color-clear");
            double value=components[i]<0?0:components[i]>1?1:components[i];
            if(i<3 && GPU::Shader::srgbFormat(nativeColorFormat(planes[slot].pixelFormat)))value=value<=0.0031308?value*12.92:1.055*std::pow(value,1.0/2.4)-0.055;
            packed[i]=uint8_t(value*255+0.5);
        }
        if(GPU::Shader::bgraFormat(nativeColorFormat(planes[slot].pixelFormat)))std::swap(packed[0],packed[2]);
        uint32_t cpp=GPU::Shader::formatBytes(nativeColorFormat(planes[slot].pixelFormat));
        if(cpp==1)clearValues[slot]=uint32_t(packed[0])*0x01010101u;
        else if(cpp==2)clearValues[slot]=(uint32_t(packed[0])|(uint32_t(packed[1])<<8))*0x00010001u;
        else memcpy(&clearValues[slot],packed,4);
    }
    if(_depth.get() && d.depthAttachment.loadAction==MTLLoadActionClear) {
        double value=d.depthAttachment.clearDepth;if(!__builtin_isfinite(value))return reject(@"depth-clear");
        float depth=value<=0?0:value>=1?1:float(value);memcpy(&clearValues[QGPU_COLOR_TARGET_COUNT],&depth,4);
    }
    clearValues[QGPU_COLOR_TARGET_COUNT+1]=(d.stencilAttachment.clearStencil&255)*0x01010101u;
    _renderPass.registry=GPUPrograms::Registry;_renderPass.width=uint32_t(_width);_renderPass.height=uint32_t(_height);
    _gmemCandidate=__atomic_load_n(&((QcomMTLDevice *)self.device)->_gmem,__ATOMIC_RELAXED) && !_depth.get() && !_stencil.get() && _samples==1;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(planes[i]) {
        auto &a=_renderPass.colors[i];a.texture=_attachments[i];_renderPass.colorMask|=1u<<i;
        a.load=uint32_t(attachments[i].loadAction);a.store=uint32_t(attachments[i].storeAction);
        if(a.load==GPU::RenderPass::Clear) {
            a.clear[0]=clearValues[i];a.clear[1]=clearHigh[i];
            // FAST_CLEAR packs the color before the MRT's channel swap.
            if(GPU::Shader::bgraFormat(a.texture.format))
                a.clear[0]=(a.clear[0]&0xff00ff00)|((a.clear[0]&255)<<16)|((a.clear[0]>>16)&255);
        }
        if(_resolveTargets[i].get())_gmemCandidate=NO;
    }
    _gmemCandidate&=GPU::RenderPass::valid(_renderPass);
    // Ordinary color draws share the native batch with following draws. For
    // exact binary colors this avoids separate full-screen compute fills and
    // their submissions, without changing arbitrary-color or MSAA conversion.
    const GPU::Shader::Program *clearVertex=nullptr,*clearFragment=nullptr;
    if(_samples==1 && [(QcomMTLDevice *)self.device qcomRenderClearEnabled]) {
        for(const auto &program:GPUPrograms::Programs) {
            if(!strcmp(program.name,"composition_vertex"))clearVertex=&program;
            if(!strcmp(program.name,"composition_clear"))clearFragment=&program;
        }
    }
    struct ClearPacket {float positions[3][4],uv[3][2],padding[2],colors[QGPU_COLOR_TARGET_COUNT][4];};
    static_assert(offsetof(ClearPacket,colors)==80,"Clear bindings must remain 16-byte aligned");
    ClearPacket packet={{{-1,-1,0,1},{3,-1,0,1},{-1,3,0,1}},{},{},{}};
    bool renderClear[QGPU_COLOR_TARGET_COUNT]={};bool anyRenderClear=false;
    if(clearVertex && clearFragment)for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)
        if(planes[i] && attachments[i].loadAction==MTLLoadActionClear && GPU::Shader::renderableColorFormat(_attachments[i].format)) {
            const auto color=d.colorAttachments[i].clearColor;
            const double components[]={color.red,color.green,color.blue,color.alpha};
            bool exact=true;
            for(unsigned c=0;c<4;++c) {
                // Preserve signed zero and every non-binary float bit pattern
                // through the existing raw-word clear path.
                exact&=(components[c]==0 || components[c]==1) && !std::signbit(components[c]);
                packet.colors[i][c]=float(components[c]);
            }
            renderClear[i]=exact;anyRenderClear|=exact;
        }
    QcomMTLBuffer *clearPacket=anyRenderClear?[_buffer newInlineSnapshot:&packet length:sizeof(packet)]:nil;
    for(unsigned i=0;i<count;++i)if(planes[i] && attachments[i].loadAction==MTLLoadActionClear) {
        if(i<QGPU_COLOR_TARGET_COUNT && renderClear[i] && clearPacket) {
            GPU::Shader::Draw draw{};draw.registry=GPUPrograms::Registry;draw.version=GPU::Shader::Version;
            draw.vertex=clearVertex->id;draw.fragment=clearFragment->id;
            draw.primitive=uint32_t(MTLPrimitiveTypeTriangle);draw.count=3;draw.instances=1;
            draw.target=_attachments[i];draw.state=GPU::Shader::defaultRenderState(uint32_t(_width),uint32_t(_height));
            draw.depthStencil=GPU::Shader::defaultDepthStencilState();
            draw.scissor[2]=uint32_t(_width);draw.scissor[3]=uint32_t(_height);
            const auto resource=clearPacket->_resource.id;const auto offset=uint32_t(clearPacket->_nativeOffset);
            draw.buffers[0]={resource,offset,48,0};draw.buffers[1]={resource,offset+48,24,0};
            draw.fragmentBuffers[0]={resource,offset+uint32_t(offsetof(ClearPacket,colors)+i*16),16,0};
            auto clear=[QcomMTLOperation new];clear.shaderOpcode=GPU::ShaderDraw;
            clear.shaderPayload=[NSData dataWithBytes:&draw length:sizeof(draw)];
            clear.resources=@[planes[i],clearPacket];[_buffer record:clear];continue;
        }
        auto texture=planes[i];auto plane=_attachments[i];QcomMTLOperation *clear=[QcomMTLOperation new];
        clear.destination=texture->_backing;clear.destinationOffset=plane.offset;
        clear.bytes=_width*GPU::Shader::formatBytes(plane.format)*GPU::Shader::textureSamples(plane);
        clear.rows=_height;clear.destinationStride=plane.pitch;
        clear.wordPattern=YES;clear.wordValue=clearValues[i];clear.wordHigh=clearHigh[i];
        clear.pairPattern=plane.format==GPU::Shader::RG32Float || plane.format==GPU::Shader::RGBA16Float;
        // Freeze this CPU-owned layout while encoding, so the existing
        // vector fill can share a native submission with surrounding passes.
        // Generic blits retain their per-submission parameter page.
        GPU::Blit::ComputePlan plan;
        const bool valid=clear.pairPattern?
            GPU::Blit::fillPairRowsPlan(clear.destinationOffset,clear.bytes,clear.destinationStride,clear.rows,clear.wordValue,clear.wordHigh,plan):
            GPU::Blit::fillRowsPlan(clear.destinationOffset,clear.bytes,clear.destinationStride,clear.rows,clear.wordValue,plan);
        GPU::Shader::Dispatch dispatch{};
        if(!valid || !makeBlitDispatch(plan,dispatch))return reject(@"clear-plan");
        auto parameters=[_buffer newInlineSnapshot:plan.parameters length:sizeof(plan.parameters)];
        if(!parameters)return reject(@"clear-parameters");
        dispatch.buffers[0]={texture->_backing->_resource.id,plan.destinationOffset,plan.destinationBytes,0};
        dispatch.buffers[1]={parameters->_resource.id,uint32_t(parameters->_nativeOffset),sizeof(plan.parameters),0};
        clear.shaderOpcode=GPU::ShaderDispatch;clear.immutableFill=YES;
        clear.shaderPayload=[NSData dataWithBytes:&dispatch length:sizeof(dispatch)];
        clear.resources=@[texture,parameters];[_buffer record:clear];
    }
    _passDrawStart=_buffer->_operations.count;
    return self;
}
- (id<MTLDevice>)device { return _buffer.device; }
- (void)preserveGMEMPass {
    if(!_gmemCandidate)return;
    [_buffer->_condition lock];
    const NSUInteger end=_buffer->_operations.count;
    GPU::RenderPass::TileLayout tiles;
    bool eligible=!_buffer->_error && end>_passDrawStart && GPU::RenderPass::tileLayout(_renderPass,tiles) &&
        GPU::RenderPass::rootWords(_renderPass,tiles,uint32_t(end-_passDrawStart))<=16384;
    bool attachmentReads=false;
    GPU::MetalBatch::Plan plan(GPU::Native::MaxShmemBytes,GPU::Native::MaxResponsibleTaskIDs,GPU::Native::RenderPassCommandVersion);
    for(NSUInteger i=_passDrawStart;eligible && i<end;++i) {
        auto op=_buffer->_operations[i];
        attachmentReads|=op.framebufferReads;
        eligible=op.gmemEligible && op.shaderOpcode==GPU::ShaderDraw && !op.event && !op.fence && !op.barrier && !op.responsibleTaskIDs &&
            op.shaderPayload.length==sizeof(GPU::Shader::Draw) &&
            GPU::RenderPass::drawMatches(_renderPass,*static_cast<const GPU::Shader::Draw *>(op.shaderPayload.bytes)) &&
            plan.append(op.shaderOpcode,op.shaderPayload.bytes,op.shaderPayload.length);
    }
    // Attachment consumers reuse the producer's on-chip plane. Ordinary
    // sysmem passes keep their established path until they need that reuse.
    if(eligible && attachmentReads) {
        auto first=_buffer->_operations[_passDrawStart];
        first.renderPass=[NSData dataWithBytes:&_renderPass length:sizeof(_renderPass)];first.renderPassDraws=end-_passDrawStart;
        // Draws keep their original retained/borrowed resource ownership.
        // Clear operations are replaced only after the complete pass qualifies.
        [_buffer->_operations removeObjectsInRange:NSMakeRange(_passStart,_passDrawStart-_passStart)];
    }
    [_buffer->_condition unlock];
}
- (void)endEncoding {
    checkEncoder(_ended);
    __attribute__((objc_precise_lifetime)) NSArray *live=[self copyLiveBindings];
    if(!live) {
        [_buffer fail:failure(@"An unretained render binding expired",MTLCommandBufferErrorInvalidResource)];
        [_scope finish];finishEncoder(_buffer,_ended);[self clearBindings];return;
    }
    [self preserveGMEMPass];
    const GPU::Shader::Program *program=nullptr;
    for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,"resolve_color")){program=&p;break;}
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(_resolveTargets[i].get()) {
        if(!program){[_buffer fail:failure(@"Missing native color resolve program")];break;}
        GPU::Shader::Dispatch dispatch{};dispatch.registry=GPUPrograms::Registry;dispatch.version=GPU::Shader::Version;dispatch.program=program->id;
        dispatch.local[0]=dispatch.local[1]=8;dispatch.local[2]=1;
        dispatch.groups[0]=(uint32_t(_width)+7)/8;dispatch.groups[1]=(uint32_t(_height)+7)/8;dispatch.groups[2]=1;
        dispatch.grid[0]=uint32_t(_width);dispatch.grid[1]=uint32_t(_height);dispatch.grid[2]=1;
        dispatch.images[0]=_attachments[i];dispatch.images[0].swizzle=0;dispatch.images[7]=_resolvePlanes[i];
        uint64_t elements=0;unsigned dimensions=0;
        if(!GPU::Shader::dispatchFootprint(dispatch,*program,elements,dimensions)){[_buffer fail:failure(@"Invalid native color resolve plan")];break;}
        QcomMTLOperation *operation=[QcomMTLOperation new];operation.shaderOpcode=GPU::ShaderDispatch;
        operation.shaderPayload=[NSData dataWithBytes:&dispatch length:sizeof(dispatch)];operation.resources=@[_targets[i].get(),_resolveTargets[i].get()];
        [_buffer record:operation];
    }
    for(unsigned i=0;i<2;++i)if(_resolveTargets[QGPU_COLOR_TARGET_COUNT+i].get()) {
        const auto &plan=_planeResolve[i];const auto *p=_planeResolvePrograms[i];auto parameters=_resolveParameters[i];
        GPU::Shader::Dispatch dispatch{};dispatch.registry=GPUPrograms::Registry;dispatch.version=GPU::Shader::Version;dispatch.program=p->id;
        dispatch.local[0]=dispatch.local[1]=8;dispatch.local[2]=1;
        dispatch.grid[0]=plan.width;dispatch.grid[1]=plan.height;dispatch.grid[2]=1;
        dispatch.groups[0]=(plan.width+7)/8;dispatch.groups[1]=(plan.height+7)/8;dispatch.groups[2]=1;
        memcpy(dispatch.buffers,plan.buffers,sizeof(plan.buffers));
        dispatch.buffers[3]={parameters->_resource.id,0,sizeof(plan.parameters),0};
        uint64_t elements=0;unsigned dimensions=0;
        if(!GPU::Shader::dispatchFootprint(dispatch,*p,elements,dimensions)){[_buffer fail:failure(@"Invalid native depth/stencil resolve plan")];break;}
        unsigned slot=QGPU_COLOR_TARGET_COUNT+i;NSMutableArray *retained=[NSMutableArray arrayWithObjects:i?_stencil.get():_depth.get(),_resolveTargets[slot].get(),parameters,nil];
        if(i && plan.parameters.stencilFilter)[retained addObject:_depth.get()];
        QcomMTLOperation *operation=[QcomMTLOperation new];operation.shaderOpcode=GPU::ShaderDispatch;
        operation.shaderPayload=[NSData dataWithBytes:&dispatch length:sizeof(dispatch)];operation.resources=retained;[_buffer record:operation];
    }
    [_scope finish];finishEncoder(_buffer,_ended);[self clearBindings];
}
- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages {
    checkEncoder(_ended);if(renderFenceStages(_buffer,stages))[_scope fence:fence signal:YES];
}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages {
    checkEncoder(_ended);if(renderFenceStages(_buffer,stages))[_scope fence:fence signal:NO];
}
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope afterStages:(MTLRenderStages)after beforeStages:(MTLRenderStages)before {
    checkEncoder(_ended);if(renderFenceStages(_buffer,after) && renderFenceStages(_buffer,before))scopeBarrier(_buffer,scope);
}
- (void)memoryBarrierWithResources:(const id<MTLResource>[])resources count:(NSUInteger)count afterStages:(MTLRenderStages)after beforeStages:(MTLRenderStages)before {
    checkEncoder(_ended);if(renderFenceStages(_buffer,after) && renderFenceStages(_buffer,before))resourceBarrier(_buffer,resources,count);
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state {
    checkEncoder(_ended);
    if(![state isKindOfClass:[QcomMTLRenderPipeline class]] || state.device!=self.device) {
        [_buffer fail:failure(@"Invalid render pipeline",MTLCommandBufferErrorInvalidResource)];return;
    }
    auto *pipeline=(QcomMTLRenderPipeline *)state;
    bool compatible=pipeline->_samples==_samples && pipeline->_depthFormat==_depth.get().pixelFormat && pipeline->_stencilFormat==_stencil.get().pixelFormat;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(pipeline->_formats[i]!=_targets[i].get().pixelFormat)compatible=false;
    if(!compatible) {
        NSMutableString *detail=[NSMutableString stringWithFormat:@"Render attachment mismatch extent=%lux%lu samples=%u/%lu depth=%lu/%lu stencil=%lu/%lu",
            (unsigned long)_width,(unsigned long)_height,pipeline->_samples,(unsigned long)_samples,
            (unsigned long)pipeline->_depthFormat,(unsigned long)_depth.get().pixelFormat,
            (unsigned long)pipeline->_stencilFormat,(unsigned long)_stencil.get().pixelFormat];
        for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(pipeline->_formats[i]!=_targets[i].get().pixelFormat)
            [detail appendFormat:@" color%u=%lu/%lu",i,(unsigned long)pipeline->_formats[i],(unsigned long)_targets[i].get().pixelFormat];
        [_buffer fail:failure(detail,MTLCommandBufferErrorInvalidResource)];return;
    }
    _pipeline.set(pipeline,_buffer.retainedReferences);
}
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if (index >= GPU::Shader::MaxBindings || (offset & 3) ||
        (buffer && (![buffer isKindOfClass:[QcomMTLBuffer class]] || buffer.device != self.device || offset > buffer.length))) {
        [_buffer fail:failure(@"Invalid vertex buffer binding", MTLCommandBufferErrorInvalidResource)]; return;
    }
    _buffers[index].set((QcomMTLBuffer *)buffer,_buffer.retainedReferences); _offsets[index] = offset;
}
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if (index >= GPU::Shader::MaxBindings || !_buffers[index].get()) { [_buffer fail:failure(@"Vertex buffer not bound")]; return; }
    [self setVertexBuffer:_buffers[index].get() offset:offset atIndex:index];
}
- (void)setVertexBuffers:(const id<MTLBuffer>[])buffers offsets:(const NSUInteger[])offsets withRange:(NSRange)range {
    checkEncoder(_ended);
    if (!buffers || !offsets || !validRange(GPU::Shader::MaxBindings,range.location,range.length)) {
        [_buffer fail:failure(@"Invalid vertex binding range")];return;
    }
    for (NSUInteger i=0;i<range.length;++i) [self setVertexBuffer:buffers[i] offset:offsets[i] atIndex:range.location+i];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if (index>=GPU::Shader::MaxBindings || length>4096 || (length && !bytes)) {
        [_buffer fail:failure(@"Invalid inline vertex data",MTLCommandBufferErrorInvalidResource)];return;
    }
    if (!length) { [self setVertexBuffer:nil offset:0 atIndex:index];return; }
    id<MTLBuffer> snapshot=[_buffer newInlineSnapshot:bytes length:length];
    if (!snapshot) { [_buffer fail:failure(@"Inline vertex buffer allocation failed",MTLCommandBufferErrorOutOfMemory)];return; }
    [self setVertexBuffer:snapshot offset:0 atIndex:index];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if (index>=GPU::Shader::MaxBindings || (offset&3) ||
        (buffer && (![buffer isKindOfClass:[QcomMTLBuffer class]] || buffer.device!=self.device || offset>buffer.length))) {
        [_buffer fail:failure(@"Invalid fragment buffer binding",MTLCommandBufferErrorInvalidResource)];return;
    }
    _fragmentBuffers[index].set((QcomMTLBuffer *)buffer,_buffer.retainedReferences);_fragmentOffsets[index]=offset;
}
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if (index>=GPU::Shader::MaxBindings || !_fragmentBuffers[index].get()) {
        [_buffer fail:failure(@"Fragment buffer not bound")];return;
    }
    [self setFragmentBuffer:_fragmentBuffers[index].get() offset:offset atIndex:index];
}
- (void)setFragmentBuffers:(const id<MTLBuffer>[])buffers offsets:(const NSUInteger[])offsets withRange:(NSRange)range {
    checkEncoder(_ended);
    if (!buffers || !offsets || !validRange(GPU::Shader::MaxBindings,range.location,range.length)) {
        [_buffer fail:failure(@"Invalid fragment binding range")];return;
    }
    for (NSUInteger i=0;i<range.length;++i) [self setFragmentBuffer:buffers[i] offset:offsets[i] atIndex:range.location+i];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    checkEncoder(_ended);
    if (index>=GPU::Shader::MaxBindings || length>4096 || (length && !bytes)) {
        [_buffer fail:failure(@"Invalid inline fragment data",MTLCommandBufferErrorInvalidResource)];return;
    }
    if (!length) { [self setFragmentBuffer:nil offset:0 atIndex:index];return; }
    id<MTLBuffer> snapshot=[_buffer newInlineSnapshot:bytes length:length];
    if (!snapshot) { [_buffer fail:failure(@"Inline fragment buffer allocation failed",MTLCommandBufferErrorOutOfMemory)];return; }
    [self setFragmentBuffer:snapshot offset:0 atIndex:index];
}
- (void)bindTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index stage:(NSUInteger)stage {
    checkEncoder(_ended);
    // The original release AGX graphics binder accepts the descriptor's
    // framebufferOnly flag. Our linear storage has a valid sampled view;
    // retain the real ownership, declared usage and draw-time alias checks.
    if(index>=QGPU_METAL_TEXTURE_BINDING_COUNT || (texture && (![texture isKindOfClass:[QcomMTLTexture class]] || texture.device!=self.device || texture.storageMode==MTLStorageModeMemoryless ||
        (texture.usage && !(texture.usage&MTLTextureUsageShaderRead))))) {
        [_buffer fail:failure([NSString stringWithFormat:@"Invalid graphics texture: stage=%lu index=%lu present=%u native=%u owner=%u framebuffer=%u usage=%lu format=%lu size=%lux%lu",
            (unsigned long)stage,(unsigned long)index,texture!=nil,[texture isKindOfClass:[QcomMTLTexture class]],
            texture.device==self.device,texture.framebufferOnly,(unsigned long)texture.usage,(unsigned long)texture.pixelFormat,
            (unsigned long)texture.width,(unsigned long)texture.height])];return;}
    _sampledTextures[stage][index].set((QcomMTLTexture *)texture,_buffer.retainedReferences);
}
- (void)bindTextures:(id<MTLTexture> const __unsafe_unretained *)textures withRange:(NSRange)range stage:(NSUInteger)stage {
    checkEncoder(_ended);
    if(!validRange(QGPU_METAL_TEXTURE_BINDING_COUNT,range.location,range.length) || (range.length && !textures)) {[_buffer fail:failure(@"Invalid graphics texture range")];return;}
    for(NSUInteger i=0;i<range.length;++i)[self bindTexture:textures[i] atIndex:range.location+i stage:stage];
}
- (void)bindSampler:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index stage:(NSUInteger)stage
    clamp:(BOOL)clamp minimum:(float)minimum maximum:(float)maximum {
    checkEncoder(_ended);
    if(index>=QGPU_STAGE_SAMPLER_COUNT || (sampler && (![sampler isKindOfClass:[QcomMTLSampler class]] || sampler.device!=self.device)) ||
        (clamp && (!__builtin_isfinite(minimum) || !__builtin_isfinite(maximum) || minimum<0 || maximum<minimum))) {
        [_buffer fail:failure(@"Invalid graphics sampler or LOD clamps")];return;
    }
    auto *bound=(QcomMTLSampler *)sampler;_samplers[stage][index].set(bound,_buffer.retainedReferences);
    auto state=bound?bound->_state:GPU::Shader::Sampler{};
    if(clamp && bound){state.minLOD=uint32_t(MIN(minimum,4095.0f/256)*256);state.maxLOD=uint32_t(MIN(maximum,4095.0f/256)*256);}
    _samplerStates[stage][index]=state;
}
- (void)bindSamplers:(id<MTLSamplerState> const __unsafe_unretained *)samplers withRange:(NSRange)range stage:(NSUInteger)stage
    minimum:(const float *)minimum maximum:(const float *)maximum clamp:(BOOL)clamp {
    checkEncoder(_ended);
    if(!validRange(QGPU_STAGE_SAMPLER_COUNT,range.location,range.length) || (range.length && (!samplers || (clamp && (!minimum || !maximum))))) {
        [_buffer fail:failure(@"Invalid graphics sampler range")];return;
    }
    for(NSUInteger i=0;i<range.length;++i)[self bindSampler:samplers[i] atIndex:range.location+i stage:stage
        clamp:clamp minimum:clamp?minimum[i]:0 maximum:clamp?maximum[i]:0];
}
- (void)setVertexTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    [self bindTexture:texture atIndex:index stage:0];
}
- (void)setVertexTextures:(id<MTLTexture> const __unsafe_unretained *)textures withRange:(NSRange)range {
    [self bindTextures:textures withRange:range stage:0];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    [self bindSampler:sampler atIndex:index stage:0 clamp:NO minimum:0 maximum:0];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)sampler lodMinClamp:(float)minimum lodMaxClamp:(float)maximum atIndex:(NSUInteger)index {
    [self bindSampler:sampler atIndex:index stage:0 clamp:YES minimum:minimum maximum:maximum];
}
- (void)setVertexSamplerStates:(id<MTLSamplerState> const __unsafe_unretained *)samplers withRange:(NSRange)range {
    [self bindSamplers:samplers withRange:range stage:0 minimum:nullptr maximum:nullptr clamp:NO];
}
- (void)setVertexSamplerStates:(id<MTLSamplerState> const __unsafe_unretained *)samplers lodMinClamps:(const float *)minimum lodMaxClamps:(const float *)maximum withRange:(NSRange)range {
    [self bindSamplers:samplers withRange:range stage:0 minimum:minimum maximum:maximum clamp:YES];
}
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    [self bindTexture:texture atIndex:index stage:1];
}
- (void)setFragmentTextures:(id<MTLTexture> const __unsafe_unretained *)textures withRange:(NSRange)range {
    [self bindTextures:textures withRange:range stage:1];
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    [self bindSampler:sampler atIndex:index stage:1 clamp:NO minimum:0 maximum:0];
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler lodMinClamp:(float)minimum lodMaxClamp:(float)maximum atIndex:(NSUInteger)index {
    [self bindSampler:sampler atIndex:index stage:1 clamp:YES minimum:minimum maximum:maximum];
}
- (void)setFragmentSamplerStates:(id<MTLSamplerState> const __unsafe_unretained *)samplers withRange:(NSRange)range {
    [self bindSamplers:samplers withRange:range stage:1 minimum:nullptr maximum:nullptr clamp:NO];
}
- (void)setFragmentSamplerStates:(id<MTLSamplerState> const __unsafe_unretained *)samplers lodMinClamps:(const float *)minimum lodMaxClamps:(const float *)maximum withRange:(NSRange)range {
    [self bindSamplers:samplers withRange:range stage:1 minimum:minimum maximum:maximum clamp:YES];
}
- (void)setViewport:(MTLViewport)viewport {
    checkEncoder(_ended);
    if (!__builtin_isfinite(viewport.originX) || !__builtin_isfinite(viewport.originY) ||
        !__builtin_isfinite(viewport.width) || !__builtin_isfinite(viewport.height) ||
        viewport.originX < -16384 || viewport.originX > 16384 || viewport.originY < -16384 || viewport.originY > 16384 ||
        viewport.width < 0 || viewport.width > 16384 || viewport.height < 0 || viewport.height > 16384 ||
        !__builtin_isfinite(viewport.znear) || !__builtin_isfinite(viewport.zfar) ||
        viewport.znear<0 || viewport.znear>1 || viewport.zfar<0 || viewport.zfar>1) {
        [_buffer fail:failure(@"Viewport must be finite, within the A540 coordinate limits, and use depth endpoints in 0..1")]; return;
    }
    _emptyViewport = viewport.width == 0 || viewport.height == 0;
    if (_emptyViewport) { _viewport = viewport; return; }
    float values[] = {float(viewport.originX+viewport.width*0.5),float(viewport.width*0.5),
        float(viewport.originY+viewport.height*0.5),float(-viewport.height*0.5)};
    float depth[]={viewport.znear<=0 ? 0.0f : float(viewport.znear),float(viewport.zfar-viewport.znear)};
    auto state = _state; memcpy(state.viewport,values,sizeof(values));memcpy(state.depthRange,depth,sizeof(depth));
    if (!GPU::Shader::validRenderState(state)) { [_buffer fail:failure(@"Viewport scale is below the A540 render profile")]; return; }
    memcpy(_state.viewport,values,sizeof(values));memcpy(_state.depthRange,depth,sizeof(depth));
    _viewport = viewport;
}
- (void)setScissorRect:(MTLScissorRect)rect {
    checkEncoder(_ended);
    if (rect.x > _width || rect.width > _width-rect.x ||
        rect.y > _height || rect.height > _height-rect.y) {
        [_buffer fail:failure(@"Scissor is outside the render target")]; return;
    }
    _scissor = rect;
}
- (void)setViewports:(const MTLViewport *)viewports count:(NSUInteger)count {
    checkEncoder(_ended);
    if (!viewports || count != 1) { [_buffer fail:failure(@"Only one viewport is supported")]; return; }
    [self setViewport:viewports[0]];
}
- (void)setScissorRects:(const MTLScissorRect *)rects count:(NSUInteger)count {
    checkEncoder(_ended);
    if (!rects || count != 1) { [_buffer fail:failure(@"Only one scissor rectangle is supported")]; return; }
    [self setScissorRect:rects[0]];
}
- (void)setCullMode:(MTLCullMode)mode {
    checkEncoder(_ended); if (mode > MTLCullModeBack) { [_buffer fail:failure(@"Invalid cull mode")]; return; }
    _state.cull = uint32_t(mode);
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state {
    checkEncoder(_ended);
    if (!state) { auto defaults=GPU::Shader::defaultDepthStencilState();
        defaults.frontReference=_depthStencil.frontReference;defaults.backReference=_depthStencil.backReference;
        _depthStencil=defaults;return; }
    if (![state isKindOfClass:[QcomMTLDepthStencilState class]] || state.device!=self.device) {
        [_buffer fail:failure(@"Invalid depth/stencil state",MTLCommandBufferErrorInvalidResource)];return;
    }
    auto copy=((QcomMTLDepthStencilState *)state)->_state;
    copy.frontReference=_depthStencil.frontReference;copy.backReference=_depthStencil.backReference;
    _depthStencil=copy;
}
- (void)setStencilReferenceValue:(uint32_t)value { [self setStencilFrontReferenceValue:value backReferenceValue:value]; }
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back {
    checkEncoder(_ended);_depthStencil.frontReference=front&255;_depthStencil.backReference=back&255;
}
- (void)setFrontFacingWinding:(MTLWinding)winding {
    checkEncoder(_ended); if (winding > MTLWindingCounterClockwise) { [_buffer fail:failure(@"Invalid front winding")]; return; }
    _state.winding = uint32_t(winding);
}
- (void)setBlendColorRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha {
    checkEncoder(_ended); float values[] = {red,green,blue,alpha};
    for (auto &value : values) {
        if (!__builtin_isfinite(value)) { [_buffer fail:failure(@"Blend constants must be finite")]; return; }
        value = value <= 0 ? 0 : value >= 1 ? 1 : value;
    }
    memcpy(_state.blendColor,values,sizeof(values));
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)mode {
    checkEncoder(_ended); if (mode != MTLTriangleFillModeFill) [_buffer fail:failure(@"Wireframe is outside this render profile")];
}
- (void)encodeDraw:(MTLPrimitiveType)primitive start:(NSUInteger)start count:(NSUInteger)count
    instances:(NSUInteger)instances baseInstance:(NSUInteger)baseInstance
    indices:(QcomMTLBuffer *)indices indexSize:(NSUInteger)indexSize offset:(NSUInteger)indexOffset baseVertex:(NSInteger)baseVertex {
    checkEncoder(_ended);
    __attribute__((objc_precise_lifetime)) NSArray *live=[self copyLiveBindings];
    if(!live){[_buffer fail:failure(@"An unretained render binding expired",MTLCommandBufferErrorInvalidResource)];return;}
    if (!_pipeline.get() || (primitive != MTLPrimitiveTypeLine && primitive != MTLPrimitiveTypeTriangle && primitive != MTLPrimitiveTypeTriangleStrip) ||
        start > UINT32_MAX || count > GPU::Shader::MaxDrawVertices || start+count > UINT32_MAX ||
        instances > GPU::Shader::MaxDrawInstances || uint64_t(count)*instances > GPU::Shader::MaxDrawInvocations ||
        baseInstance > UINT32_MAX || baseInstance+instances > UINT32_MAX ||
        baseVertex < INT32_MIN || baseVertex > INT32_MAX ||
        (indexSize && (![indices isKindOfClass:[QcomMTLBuffer class]] || indices.device != self.device ||
            (indexOffset & (indexSize-1)) || indexOffset > UINT32_MAX ||
            !validRange(indices.length,indexOffset,uint64_t(count)*indexSize)))) {
        [_buffer fail:failure([NSString stringWithFormat:@"Invalid draw: pipeline=%u primitive=%lu start=%lu count=%lu instances=%lu base_instance=%lu base_vertex=%lld index_size=%lu index_offset=%lu index_length=%lu",
            _pipeline.get()!=nil,(unsigned long)primitive,(unsigned long)start,(unsigned long)count,(unsigned long)instances,
            (unsigned long)baseInstance,(long long)baseVertex,(unsigned long)indexSize,(unsigned long)indexOffset,(unsigned long)indices.length])]; return;
    }
    if (!count || !instances || !_scissor.width || !_scissor.height || _emptyViewport) return;
    // Intersect pixel-center coverage with the viewport as well as the
    // explicit scissor. Guard-band rasterization must not escape the Metal
    // viewport when the vertex shader emits oversized clip-space geometry.
    int64_t left = std::max<int64_t>(_scissor.x,(int64_t)__builtin_ceil(_viewport.originX-0.5));
    int64_t top = std::max<int64_t>(_scissor.y,(int64_t)__builtin_ceil(_viewport.originY-0.5));
    int64_t right = std::min<int64_t>(_scissor.x+_scissor.width,(int64_t)__builtin_ceil(_viewport.originX+_viewport.width-0.5));
    int64_t bottom = std::min<int64_t>(_scissor.y+_scissor.height,(int64_t)__builtin_ceil(_viewport.originY+_viewport.height-0.5));
    if (left >= right || top >= bottom) return;
    GPU::Shader::Draw draw{}; draw.registry = GPUPrograms::Registry; draw.version = GPU::Shader::Version;
    draw.vertex = _pipeline.get()->_vertex->_program->id; draw.fragment = _pipeline.get()->_fragment->_program->id;
    draw.primitive = (uint32_t)primitive; draw.start = (uint32_t)start; draw.count = (uint32_t)count;
    draw.instances = (uint32_t)instances; draw.baseInstance = (uint32_t)baseInstance; draw.baseVertex = (int32_t)baseVertex;
    draw.indexSize = (uint32_t)indexSize;
    if (indexSize) draw.indices = {indices->_resource.id,(uint32_t)(indices->_nativeOffset+indexOffset),(uint32_t)(count*indexSize),0};
    draw.target=_attachments[0];draw.depth=_attachments[QGPU_COLOR_TARGET_COUNT];draw.stencil=_attachments[QGPU_COLOR_TARGET_COUNT+1];
    for(unsigned i=1;i<QGPU_COLOR_TARGET_COUNT;++i)if(_targets[i].get()){draw.colorTargets[i-1]=_attachments[i];draw.colorStates[i-1]=_pipeline.get()->_colorStates[i];}
    draw.depthStencil=_depthStencil;
    memcpy(draw.attributes,_pipeline.get()->_vertexInput.attributes,sizeof(draw.attributes));
    memcpy(draw.layouts,_pipeline.get()->_vertexInput.layouts,sizeof(draw.layouts));
    draw.state = _pipeline.get()->_state;draw.rasterSamples=_samples>1?_samples:0;draw.alphaToCoverage=_pipeline.get()->_alphaToCoverage;
    memcpy(draw.state.viewport,_state.viewport,sizeof(draw.state.viewport));
    memcpy(draw.state.depthRange,_state.depthRange,sizeof(draw.state.depthRange));
    memcpy(draw.state.blendColor,_state.blendColor,sizeof(draw.state.blendColor));
    draw.state.cull = _state.cull; draw.state.winding = _state.winding;
    draw.scissor[0] = (uint32_t)left; draw.scissor[1] = (uint32_t)top;
    draw.scissor[2] = (uint32_t)(right-left); draw.scissor[3] = (uint32_t)(bottom-top);
    NSMutableArray *retained = [NSMutableArray arrayWithObject:_pipeline.get()];
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(_targets[i].get())[retained addObject:_targets[i].get()];if (_depth.get()) [retained addObject:_depth.get()];if (_stencil.get()) [retained addObject:_stencil.get()];
    if (indices) [retained addObject:indices];
    const auto *vertex = _pipeline.get()->_vertex->_program;uint32_t fetchBindings=0;
    if (!GPU::Shader::vertexLayout(*vertex,draw,fetchBindings)) { [_buffer fail:failure(@"Invalid vertex layout")];return; }
    for (NSUInteger i = 0; i < GPU::Shader::MaxBindings; ++i) if ((vertex->bufferMask|fetchBindings) & (1u<<i)) {
        QcomMTLBuffer *buffer = _buffers[i].get(); NSUInteger offset = _offsets[i];
        uint64_t elements = (vertex->instanceMask&(1u<<i)) ? uint64_t(baseInstance)+instances : indexSize ? 0 : uint64_t(start)+count;
        uint64_t required=0;
        if (!buffer || !GPU::Shader::bindingBytes(*vertex,(unsigned)i,elements,uint64_t(baseInstance)+instances,required) || !validRange(buffer.length,offset,required)) {
            [_buffer fail:failure(@"Draw exceeds a vertex buffer", MTLCommandBufferErrorInvalidResource)]; return;
        }
        draw.buffers[i] = {buffer->_resource.id,(uint32_t)(buffer->_nativeOffset+offset),(uint32_t)(buffer.length-offset),0}; [retained addObject:buffer];
    }
    if (!indexSize && !GPU::Shader::vertexFootprint(*vertex,draw,uint64_t(start)+count,fetchBindings)) {
        [_buffer fail:failure(@"Vertex attributes exceed bound buffer ranges",MTLCommandBufferErrorInvalidResource)];return;
    }
    const auto *fragment=&GPU::Shader::rasterProgram(*_pipeline.get()->_fragment->_program,draw.rasterSamples);
    for (NSUInteger i=0;i<GPU::Shader::MaxBindings;++i) if (fragment->bufferMask&(1u<<i)) {
        QcomMTLBuffer *buffer=_fragmentBuffers[i].get();NSUInteger offset=_fragmentOffsets[i];uint64_t required=0;
        if (!buffer || !GPU::Shader::bindingBytes(*fragment,(unsigned)i,0,0,required) || !validRange(buffer.length,offset,required)) {
            [_buffer fail:failure(@"Draw exceeds a fragment buffer",MTLCommandBufferErrorInvalidResource)];return;
        }
        draw.fragmentBuffers[i]={buffer->_resource.id,(uint32_t)(buffer->_nativeOffset+offset),(uint32_t)(buffer.length-offset),0};
        [retained addObject:buffer];
    }
    const GPU::Shader::Program *programs[]={vertex,fragment};
    GPU::Shader::Texture *texturePlanes[]={draw.vertexTextures,draw.textures};
    GPU::Shader::Sampler *samplerStates[]={draw.vertexSamplers,draw.samplers};
    constexpr unsigned attachmentCount=QGPU_COLOR_TARGET_COUNT+2;
    QcomMTLTexture *__strong attachmentTextures[attachmentCount]={};GPU::Shader::Texture attachmentPlanes[attachmentCount]={};
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i){attachmentTextures[i]=_targets[i].get();attachmentPlanes[i]=GPU::Shader::colorTarget(draw,i);}
    attachmentTextures[QGPU_COLOR_TARGET_COUNT]=_depth.get();attachmentTextures[QGPU_COLOR_TARGET_COUNT+1]=_stencil.get();
    attachmentPlanes[QGPU_COLOR_TARGET_COUNT]=draw.depth;attachmentPlanes[QGPU_COLOR_TARGET_COUNT+1]=draw.stencil;
    for(unsigned stage=0;stage<2;++stage) {
        const auto *program=programs[stage];
        for(unsigned slot=0;slot<QGPU_STAGE_SAMPLER_COUNT;++slot)if(program->samplerMask&(1u<<slot)) {
            if(!_samplers[stage][slot].get()){[_buffer fail:failure(@"Missing graphics sampler")];return;}
            samplerStates[stage][slot]=_samplerStates[stage][slot];[retained addObject:_samplers[stage][slot].get()];
        }
        for(unsigned slot=0;slot<QGPU_STAGE_TEXTURE_COUNT;++slot)if(GPU::Shader::graphicsTextureMask(*program)&(1u<<slot)) {
            const auto logical=GPU::Shader::textureBinding(*program,slot);
            auto *texture=_sampledTextures[stage][logical].get();
            if(!texture){[_buffer fail:failure([NSString stringWithFormat:@"Missing graphics texture: stage=%u slot=%u binding=%u function=%@",
                stage,slot,logical,stage?_pipeline.get()->_fragment.name:_pipeline.get()->_vertex.name])];return;}
            if((GPU::Shader::graphicsReadMask(*program)&(1u<<slot)) && texture.usage && !(texture.usage&MTLTextureUsageShaderRead)) {
                [_buffer fail:failure(@"Graphics texture usage excludes shader reads",MTLCommandBufferErrorInvalidResource)];return;
            }
            texturePlanes[stage][slot]=[texture nativeTexture];[retained addObject:texture];
            // Native admission repeats feedback checks against pinned pages.
            for(unsigned i=0;i<attachmentCount;++i)if(attachmentTextures[i] && (GPU::Shader::graphicsReadMask(*program)&(1u<<slot))) {
                auto *sampled=texture->_backing,*backing=attachmentTextures[i]->_backing;
                bool alias=sampled==backing || sampled->_resource.id==backing->_resource.id || (sampled->_surface && backing->_surface && IOSurfaceGetID(sampled->_surface)==IOSurfaceGetID(backing->_surface));
                auto a=texturePlanes[stage][slot],b=attachmentPlanes[i];a.resource=b.resource=1;
                if(alias && GPU::Shader::overlap(a,b)){[_buffer fail:failure(@"Render-target feedback")];return;}
            }
        }
        if(!GPU::Shader::graphicsTextures(*program,texturePlanes[stage],samplerStates[stage])) {
            [_buffer fail:failure(@"Graphics texture dimensions disagree with shader")];return;
        }
    }
    QcomMTLOperation *operation = [QcomMTLOperation new]; operation.shaderOpcode = GPU::ShaderDraw;
    operation.shaderBufferWrites=(vertex->writeMask|fragment->writeMask)!=0;
    operation.gmemEligible=GPU::RenderPass::programsEligible(*vertex,*fragment);
    operation.framebufferReads=fragment->framebufferMask!=0;
    operation.shaderPayload = [NSData dataWithBytes:&draw length:sizeof(draw)]; operation.resources = retained;
    [_buffer record:operation];
}
- (void)drawPrimitives:(MTLPrimitiveType)primitive vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count {
    [self encodeDraw:primitive start:start count:count instances:1 baseInstance:0 indices:nil indexSize:0 offset:0 baseVertex:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances {
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
    instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)baseInstance {
    [self encodeDraw:type start:start count:count instances:instances baseInstance:baseInstance indices:nil indexSize:0 offset:0 baseVertex:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType
    indexBuffer:(id<MTLBuffer>)indices indexBufferOffset:(NSUInteger)offset {
    [self drawIndexedPrimitives:type indexCount:count indexType:indexType indexBuffer:indices indexBufferOffset:offset instanceCount:1];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType
    indexBuffer:(id<MTLBuffer>)indices indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances {
    [self drawIndexedPrimitives:type indexCount:count indexType:indexType indexBuffer:indices indexBufferOffset:offset
        instanceCount:instances baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType
    indexBuffer:(id<MTLBuffer>)indices indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances
    baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance {
    if (indexType != MTLIndexTypeUInt16 && indexType != MTLIndexTypeUInt32) {
        [_buffer fail:failure(@"Only UInt16 and UInt32 indices are supported")]; return;
    }
    [self encodeDraw:type start:0 count:count instances:instances baseInstance:baseInstance
        indices:(QcomMTLBuffer *)indices indexSize:indexType == MTLIndexTypeUInt16 ? 2 : 4 offset:offset baseVertex:baseVertex];
}
- (void)forwardInvocation:(NSInvocation *)invocation {
    if (_ended || _buffer.status >= MTLCommandBufferStatusCommitted) { [super forwardInvocation:invocation]; return; }
    [_buffer fail:failure([@"Unsupported render command: " stringByAppendingString:NSStringFromSelector(invocation.selector)])];
    clearReturn(invocation);
}
- (void)insertDebugSignpost:(NSString *)string { (void)string; }
- (void)pushDebugGroup:(NSString *)string { (void)string; }
- (void)popDebugGroup {}
@end

@implementation QcomMTLBlitCommandEncoder
+ (Protocol *)metalProtocol { return @protocol(MTLBlitCommandEncoder); }
- (instancetype)initWithBuffer:(QcomMTLCommandBuffer *)buffer {
    if ((self = [super init])) {_buffer = buffer;_scope=[[QcomMTLEncoderScope alloc] initWithBuffer:buffer];} return self;
}
- (id<MTLDevice>)device { return _buffer.device; }
- (void)endEncoding {
    checkEncoder(_ended);[_scope finish];finishEncoder(_buffer,_ended);
}
- (void)updateFence:(id<MTLFence>)fence {checkEncoder(_ended);[_scope fence:fence signal:YES];}
- (void)waitForFence:(id<MTLFence>)fence {checkEncoder(_ended);[_scope fence:fence signal:NO];}
- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value {
    if (_ended) { [NSException raise:NSInvalidArgumentException format:@"Encoder already ended"]; return; }
    QcomMTLBuffer *destination = [buffer isKindOfClass:[QcomMTLBuffer class]] ? (QcomMTLBuffer *)buffer : nil;
    if (!destination || destination.device != self.device || !validRange(destination.length, range.location, range.length)) {
        [_buffer fail:failure(@"Invalid Metal fill buffer or range", MTLCommandBufferErrorInvalidResource)]; return;
    }
    if (!range.length) return;
    QcomMTLOperation *operation = [QcomMTLOperation new]; operation.destination = destination;
    operation.destinationOffset = destination->_nativeOffset+range.location; operation.bytes = range.length; operation.value = value;
    [_buffer record:operation];
}
- (void)copyFromBuffer:(id<MTLBuffer>)sourceBuffer sourceOffset:(NSUInteger)sourceOffset
    toBuffer:(id<MTLBuffer>)destinationBuffer destinationOffset:(NSUInteger)destinationOffset size:(NSUInteger)size {
    [self copyRows:sourceBuffer offset:sourceOffset stride:size to:destinationBuffer offset:destinationOffset stride:size bytes:size rows:1];
}
- (QcomMTLOperation *)copyRows:(id<MTLBuffer>)sourceBuffer offset:(NSUInteger)sourceOffset stride:(NSUInteger)sourceStride
    to:(id<MTLBuffer>)destinationBuffer offset:(NSUInteger)destinationOffset stride:(NSUInteger)destinationStride
    bytes:(NSUInteger)size rows:(NSUInteger)rows {
    if (_ended) { [NSException raise:NSInvalidArgumentException format:@"Encoder already ended"]; return nil; }
    QcomMTLBuffer *source = [sourceBuffer isKindOfClass:[QcomMTLBuffer class]] ? (QcomMTLBuffer *)sourceBuffer : nil;
    QcomMTLBuffer *destination = [destinationBuffer isKindOfClass:[QcomMTLBuffer class]] ? (QcomMTLBuffer *)destinationBuffer : nil;
    if (!source || !destination || source.device!=self.device || destination.device!=self.device) {
        [_buffer fail:failure(@"Invalid Metal copy buffers, range or overlap", MTLCommandBufferErrorInvalidResource)]; return nil;
    }
    if (!size || !rows) return nil;
    if (!GPU::stridedRange(source.length,sourceOffset,size,sourceStride,rows) ||
        !GPU::stridedRange(destination.length,destinationOffset,size,destinationStride,rows)) {
        [_buffer fail:failure(@"Invalid Metal copy row range",MTLCommandBufferErrorInvalidResource)];return nil;
    }
    sourceOffset+=source->_nativeOffset;destinationOffset+=destination->_nativeOffset;
    bool alias=source==destination || source->_resource.id==destination->_resource.id || (source->_surface && destination->_surface &&
        IOSurfaceGetID(source->_surface)==IOSurfaceGetID(destination->_surface));
    if (alias && GPU::stridedOverlap({sourceOffset,size,sourceStride,rows},{destinationOffset,size,destinationStride,rows})) {
        [_buffer fail:failure(@"Overlapping Metal copy backing",MTLCommandBufferErrorInvalidResource)];return nil;
    }
    if (sourceStride==size && destinationStride==size) {size*=rows;rows=1;sourceStride=destinationStride=size;}
    QcomMTLOperation *operation = [QcomMTLOperation new]; operation.source = source; operation.destination = destination;
    operation.sourceOffset = sourceOffset; operation.destinationOffset = destinationOffset; operation.bytes = size;
    operation.rows=rows;operation.sourceStride=sourceStride;operation.destinationStride=destinationStride;
    [_buffer record:operation];
    return operation;
}
- (void)copyFromTexture:(id<MTLTexture>)source sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
    sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)destination
    destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)pitch destinationBytesPerImage:(NSUInteger)imagePitch {
    [self copyFromTexture:source sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size toBuffer:destination
        destinationOffset:offset destinationBytesPerRow:pitch destinationBytesPerImage:imagePitch options:MTLBlitOptionNone];
}
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
    sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)destination
    destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)pitch destinationBytesPerImage:(NSUInteger)imagePitch
    options:(MTLBlitOption)options {
    (void)imagePitch; checkEncoder(_ended); GPU::Shader::Texture plane{};
    QcomMTLTexture *source=[sourceTexture isKindOfClass:[QcomMTLTexture class]] ? (QcomMTLTexture *)sourceTexture : nil;
    if (!source || source.sampleCount!=1 || ![source blitPlane:options level:level slice:slice descriptor:&plane] || source.device!=self.device ||
        ![destination isKindOfClass:[QcomMTLBuffer class]] || destination.device!=self.device ||
        origin.z || size.depth!=1 || !validRange(plane.width,origin.x,size.width) ||
        !validRange(plane.height,origin.y,size.height)) {
        [_buffer fail:failure(@"Invalid texture-to-buffer plane or region",MTLCommandBufferErrorInvalidResource)];return;
    }
    GPU::Shader::TextureCopyRegion copy;
    if(!GPU::Shader::textureCopyRegion(plane,origin.x,origin.y,size.width,size.height,copy)) {
        [_buffer fail:failure(@"Invalid compressed texture-to-buffer region",MTLCommandBufferErrorInvalidResource)];return;
    }
    NSUInteger row=copy.rowBytes;
    if (pitch<row || offset>destination.length ||
        (copy.rows && (row>destination.length-offset || (copy.rows-1)>(destination.length-offset-row)/MAX(pitch,(NSUInteger)1)))) {
        [_buffer fail:failure(@"Invalid texture-to-buffer range",MTLCommandBufferErrorInvalidResource)];return;
    }
    if (!row || !copy.rows) return;
    auto *operation=[self copyRows:source->_backing offset:plane.offset-source->_backing->_nativeOffset+copy.offset
        stride:plane.pitch to:destination offset:offset stride:pitch bytes:row rows:copy.rows];
    operation.resources=@[source];
}
- (void)copyFromBuffer:(id<MTLBuffer>)source sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)pitch
    sourceBytesPerImage:(NSUInteger)imagePitch sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)destination
    destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin {
    [self copyFromBuffer:source sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:imagePitch sourceSize:size
        toTexture:destination destinationSlice:slice destinationLevel:level destinationOrigin:origin options:MTLBlitOptionNone];
}
- (void)copyFromBuffer:(id<MTLBuffer>)source sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)pitch
    sourceBytesPerImage:(NSUInteger)imagePitch sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)destinationTexture
    destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin options:(MTLBlitOption)options {
    (void)imagePitch; checkEncoder(_ended); GPU::Shader::Texture plane{};
    QcomMTLTexture *destination=[destinationTexture isKindOfClass:[QcomMTLTexture class]] ? (QcomMTLTexture *)destinationTexture : nil;
    if (!destination || destination.sampleCount!=1 || ![destination blitPlane:options level:level slice:slice descriptor:&plane] || destination.device!=self.device ||
        ![source isKindOfClass:[QcomMTLBuffer class]] || source.device!=self.device ||
        origin.z || size.depth!=1 || !validRange(plane.width,origin.x,size.width) ||
        !validRange(plane.height,origin.y,size.height)) {
        [_buffer fail:failure(@"Invalid buffer-to-texture plane or region",MTLCommandBufferErrorInvalidResource)];return;
    }
    GPU::Shader::TextureCopyRegion copy;
    if(!GPU::Shader::textureCopyRegion(plane,origin.x,origin.y,size.width,size.height,copy)) {
        [_buffer fail:failure(@"Invalid compressed buffer-to-texture region",MTLCommandBufferErrorInvalidResource)];return;
    }
    NSUInteger row=copy.rowBytes;
    if (pitch<row || offset>source.length ||
        (copy.rows && (row>source.length-offset || (copy.rows-1)>(source.length-offset-row)/MAX(pitch,(NSUInteger)1)))) {
        [_buffer fail:failure(@"Invalid buffer-to-texture range",MTLCommandBufferErrorInvalidResource)];return;
    }
    if (!row || !copy.rows) return;
    auto *operation=[self copyRows:source offset:offset stride:pitch to:destination->_backing
        offset:plane.offset-destination->_backing->_nativeOffset+copy.offset
        stride:plane.pitch bytes:row rows:copy.rows];
    operation.resources=@[destination];
}
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
    sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)destinationTexture
    destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel destinationOrigin:(MTLOrigin)destinationOrigin {
    checkEncoder(_ended);
    QcomMTLTexture *source=[sourceTexture isKindOfClass:[QcomMTLTexture class]] ? (QcomMTLTexture *)sourceTexture : nil;
    QcomMTLTexture *destination=[destinationTexture isKindOfClass:[QcomMTLTexture class]] ? (QcomMTLTexture *)destinationTexture : nil;
    if (!source || !destination || source.device!=self.device || destination.device!=self.device ||
        source.pixelFormat!=destination.pixelFormat || source.sampleCount!=destination.sampleCount || sourceSlice>=textureSliceCount(source) || sourceLevel>=source.mipmapLevelCount || destinationSlice>=textureSliceCount(destination) || destinationLevel>=destination.mipmapLevelCount ||
        sourceOrigin.z || destinationOrigin.z || size.depth!=1 ||
        !validRange(MAX(NSUInteger(1),source.width>>sourceLevel),sourceOrigin.x,size.width) || !validRange(MAX(NSUInteger(1),source.height>>sourceLevel),sourceOrigin.y,size.height) ||
        !validRange(MAX(NSUInteger(1),destination.width>>destinationLevel),destinationOrigin.x,size.width) || !validRange(MAX(NSUInteger(1),destination.height>>destinationLevel),destinationOrigin.y,size.height)) {
        [_buffer fail:failure(@"Invalid texture copy or overlap",MTLCommandBufferErrorInvalidResource)];return;
    }
    BOOL combined=source.pixelFormat==MTLPixelFormatDepth32Float_Stencil8;
    for (unsigned aspect=0;aspect<(combined?2u:1u);++aspect) {
        MTLBlitOption option=combined ? (aspect?MTLBlitOptionStencilFromDepthStencil:MTLBlitOptionDepthFromDepthStencil) : MTLBlitOptionNone;
        GPU::Shader::Texture from{},to{};
        if (![source blitPlane:option level:sourceLevel slice:sourceSlice descriptor:&from] ||
            ![destination blitPlane:option level:destinationLevel slice:destinationSlice descriptor:&to]) {[_buffer fail:failure(@"Invalid texture mip plane")];return;}
        GPU::Shader::TextureCopyRegion read,write;
        if(!GPU::Shader::textureCopyRegion(from,sourceOrigin.x,sourceOrigin.y,size.width,size.height,read) ||
            !GPU::Shader::textureCopyRegion(to,destinationOrigin.x,destinationOrigin.y,size.width,size.height,write) ||
            read.rowBytes!=write.rowBytes || read.rows!=write.rows) {
            [_buffer fail:failure(@"Invalid compressed texture copy region",MTLCommandBufferErrorInvalidResource)];return;
        }
        if(!read.rowBytes || !read.rows)continue;
        auto *operation=[self copyRows:source->_backing offset:from.offset-source->_backing->_nativeOffset+read.offset stride:from.pitch
            to:destination->_backing offset:to.offset-destination->_backing->_nativeOffset+write.offset stride:to.pitch
            bytes:read.rowBytes rows:read.rows];
        operation.resources=@[source,destination];
    }
}
- (void)copyFromTexture:(id<MTLTexture>)source sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
    toTexture:(id<MTLTexture>)destination destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
    sliceCount:(NSUInteger)sliceCount levelCount:(NSUInteger)levelCount {
    checkEncoder(_ended);
    if (![source isKindOfClass:[QcomMTLTexture class]] || ![destination isKindOfClass:[QcomMTLTexture class]] ||
        source.device!=self.device || destination.device!=self.device || source.storageMode==MTLStorageModeMemoryless || destination.storageMode==MTLStorageModeMemoryless || source.pixelFormat!=destination.pixelFormat || source.sampleCount!=destination.sampleCount ||
        !validRange(textureSliceCount(source),sourceSlice,sliceCount) || !validRange(textureSliceCount(destination),destinationSlice,sliceCount) || !validRange(source.mipmapLevelCount,sourceLevel,levelCount) ||
        !validRange(destination.mipmapLevelCount,destinationLevel,levelCount)) {[_buffer fail:failure(@"Invalid texture mip range")];return;}
    if (!sliceCount || !levelCount) return;
    if (MAX(NSUInteger(1),source.width>>sourceLevel)!=MAX(NSUInteger(1),destination.width>>destinationLevel) ||
        MAX(NSUInteger(1),source.height>>sourceLevel)!=MAX(NSUInteger(1),destination.height>>destinationLevel)) {[_buffer fail:failure(@"Texture mip dimensions differ")];return;}
    auto *from=(QcomMTLTexture *)source,*to=(QcomMTLTexture *)destination;
    bool alias=from->_backing==to->_backing || from->_backing->_resource.id==to->_backing->_resource.id || (from->_backing->_surface && to->_backing->_surface &&
        IOSurfaceGetID(from->_backing->_surface)==IOSurfaceGetID(to->_backing->_surface));
    // Include both D32S8 planes and sort by actual backing byte offsets.
    // Check every source/destination interval before recording any copy.
    if (alias) {
        std::vector<std::pair<uint64_t,uint64_t>> ranges[2];
        for (unsigned side=0;side<2;++side) for (NSUInteger slice=0;slice<sliceCount;++slice) for (NSUInteger level=0;level<levelCount;++level) {
            auto texture=side?to:from;NSUInteger mip=(side?destinationLevel:sourceLevel)+level,index=(side?destinationSlice:sourceSlice)+slice;
            bool combined=texture.pixelFormat==MTLPixelFormatDepth32Float_Stencil8;
            for(unsigned aspect=0;aspect<(combined?2u:1u);++aspect) {
                GPU::Shader::Texture plane{};MTLBlitOption option=combined?(aspect?MTLBlitOptionStencilFromDepthStencil:MTLBlitOptionDepthFromDepthStencil):MTLBlitOptionNone;
                uint64_t bytes=0;if(![texture blitPlane:option level:mip slice:index descriptor:&plane] || !GPU::Shader::textureBytes(plane,bytes)) {[_buffer fail:failure(@"Invalid texture slice/mip backing")];return;}
                ranges[side].push_back({plane.offset,uint64_t(plane.offset)+bytes});
            }
        }
        std::sort(ranges[0].begin(),ranges[0].end());std::sort(ranges[1].begin(),ranges[1].end());
        size_t a=0,b=0;
        while(a<ranges[0].size() && b<ranges[1].size()) {
            const auto &x=ranges[0][a],&y=ranges[1][b];
            if(x.first<y.second && y.first<x.second){[_buffer fail:failure(@"Overlapping texture slice/mip backing")];return;}
            if(x.second<=y.first)++a;else ++b;
        }
    }
    for (NSUInteger slice=0;slice<sliceCount;++slice) for (NSUInteger level=0;level<levelCount;++level) [self copyFromTexture:source sourceSlice:sourceSlice+slice sourceLevel:sourceLevel+level sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(MAX(NSUInteger(1),source.width>>(sourceLevel+level)),MAX(NSUInteger(1),source.height>>(sourceLevel+level)),1) toTexture:destination destinationSlice:destinationSlice+slice destinationLevel:destinationLevel+level
        destinationOrigin:MTLOriginMake(0,0,0)];
}
- (void)copyFromTexture:(id<MTLTexture>)source toTexture:(id<MTLTexture>)destination {
    checkEncoder(_ended);
    if (![source isKindOfClass:[QcomMTLTexture class]] || ![destination isKindOfClass:[QcomMTLTexture class]] || textureSliceCount(source)!=textureSliceCount(destination)) {[_buffer fail:failure(@"Invalid texture copy")];return;}
    NSUInteger from=0,to=0;bool found=false;
    for (NSUInteger level=0;level<source.mipmapLevelCount;++level)
        if (MAX(NSUInteger(1),source.width>>level)==destination.width && MAX(NSUInteger(1),source.height>>level)==destination.height) {from=level;found=true;break;}
    if (!found) for (NSUInteger level=0;level<destination.mipmapLevelCount;++level)
        if (MAX(NSUInteger(1),destination.width>>level)==source.width && MAX(NSUInteger(1),destination.height>>level)==source.height) {to=level;found=true;break;}
    if (!found) {[_buffer fail:failure(@"No matching texture mip dimensions")];return;}
    [self copyFromTexture:source sourceSlice:0 sourceLevel:from toTexture:destination destinationSlice:0 destinationLevel:to
        sliceCount:textureSliceCount(source) levelCount:MIN(source.mipmapLevelCount-from,destination.mipmapLevelCount-to)];
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)texture {
    checkEncoder(_ended);
    QcomMTLTexture *target=[texture isKindOfClass:[QcomMTLTexture class]]?(QcomMTLTexture *)texture:nil;
    if (!target || target.sampleCount!=1 || target.device!=self.device || !renderableColorFormat(target.pixelFormat)) {
        [_buffer fail:failure(@"Invalid mipmap generation texture",MTLCommandBufferErrorInvalidResource)];return;
    }
    if (target.mipmapLevelCount<2) return;
    const GPU::Shader::Program *program=nullptr;
    for (const auto &p:GPUPrograms::Programs) if (!strcmp(p.name,"mipmap_rgba8")) {program=&p;break;}
    if (!program) {[_buffer fail:failure(@"Missing mipmap compute program")];return;}
    for (NSUInteger slice=0;slice<textureSliceCount(target);++slice) for (NSUInteger level=1;level<target.mipmapLevelCount;++level) {
        auto source=[target nativeTextureAtLevel:level-1 slice:slice],destination=[target nativeTextureAtLevel:level slice:slice];
        // Mipmap generation filters stored color, not a shader read view.
        // Reapplying a channel permutation per level would alter the data.
        source.swizzle=destination.swizzle=0;
        GPU::Shader::Dispatch dispatch{};dispatch.registry=GPUPrograms::Registry;dispatch.version=GPU::Shader::Version;dispatch.program=program->id;
        dispatch.groups[0]=(destination.width*destination.height+63)/64;dispatch.groups[1]=dispatch.groups[2]=1;
        dispatch.local[0]=64;dispatch.local[1]=dispatch.local[2]=1;
        dispatch.images[0]=source;dispatch.images[7]=destination;
        dispatch.samplers[0]={3,0,0}; // Linear min/mag, clamped edges, one level.
        uint64_t elements=0;unsigned dimensions=0;
        if (!GPU::Shader::dispatchFootprint(dispatch,*program,elements,dimensions)) {[_buffer fail:failure(@"Invalid mipmap compute dimensions")];return;}
        QcomMTLOperation *operation=[QcomMTLOperation new];operation.shaderOpcode=GPU::ShaderDispatch;
        operation.shaderPayload=[NSData dataWithBytes:&dispatch length:sizeof(dispatch)];operation.resources=@[target];
        // The serial command queue waits on the real CP fence before the
        // next level consumes this output; no CPU image generation occurs.
        [_buffer record:operation];
    }
}
- (void)forwardInvocation:(NSInvocation *)invocation {
    if (_ended || _buffer.status >= MTLCommandBufferStatusCommitted) { [super forwardInvocation:invocation]; return; }
    [_buffer fail:failure([@"Unsupported Metal blit: " stringByAppendingString:NSStringFromSelector(invocation.selector)])];
    clearReturn(invocation);
}
- (void)insertDebugSignpost:(NSString *)string { (void)string; }
- (void)pushDebugGroup:(NSString *)string { (void)string; }
- (void)popDebugGroup {}
@end
#pragma clang diagnostic pop

id<MTLDevice> QcomMTLCreateDevice(uint32_t service, NSError **error) {
    if (error) *error = nil;
    return [[QcomMTLDevice alloc] initWithService:service error:error];
}
id<MTLDevice> QcomMTLCreateSystemDefaultDevice(NSError **error) {
    if (error) *error = nil;
    auto matching = IOServiceMatching("QcomGPU");
    mach_port_t iterator = MACH_PORT_NULL;
    auto status = matching ? IOServiceGetMatchingServices(MACH_PORT_NULL, matching, &iterator) : kIOReturnNoMemory;
    if (status) { if (error) *error = transportError(status); return nil; }
    auto service = IOIteratorNext(iterator), duplicate = IOIteratorNext(iterator); IOObjectRelease(iterator);
    if (!service || duplicate) {
        if (service) IOObjectRelease(service); if (duplicate) IOObjectRelease(duplicate);
        if (error) *error = failure(@"Exactly one QcomGPU accelerator is required"); return nil;
    }
    id<MTLDevice> device = QcomMTLCreateDevice(service, error); IOObjectRelease(service); return device;
}

extern "C" BOOL QcomMTLEncodeComposition(id<MTLCommandBuffer> commands,id<MTLTexture> output,
    const QcomMTLCompositionLayer *input,NSUInteger count,MTLClearColor clearColor,NSError **error) {
    auto reject=[&](NSString *message) {if(error)*error=failure(message,MTLCommandBufferErrorInvalidResource);return NO;};
    if(![commands isKindOfClass:[QcomMTLCommandBuffer class]] || ![output isKindOfClass:[QcomMTLTexture class]] ||
        commands.device!=output.device || output.textureType!=MTLTextureType2D || output.sampleCount!=1 ||
        !colorFormat(output.pixelFormat) || (output.usage && !(output.usage&MTLTextureUsageRenderTarget)) ||
        count>8 || (count && !input))return reject(@"Composition requires a2D color target and at most8 layers from the same device");
    auto buffer=(QcomMTLCommandBuffer *)commands;auto target=(QcomMTLTexture *)output;
    [buffer->_condition lock];BOOL available=buffer->_status<MTLCommandBufferStatusCommitted && !buffer->_encoderActive && !buffer->_error;
    [buffer->_condition unlock];if(!available)return reject(@"Composition requires an uncommitted command buffer without an active encoder");
    const double clear[]={clearColor.red,clearColor.green,clearColor.blue,clearColor.alpha};
    for(double component:clear)if(!std::isfinite(component) || component<0 || component>1)return reject(@"Invalid composition clear color");
    QcomMTLCompositionLayer layers[8]{};if(count)memcpy(layers,input,count*sizeof(*input));
    NSMutableArray<id<MTLTexture>> *textures=[NSMutableArray arrayWithCapacity:count];
    auto rectangle=[](CGRect rect) {return std::isfinite(rect.origin.x) && std::isfinite(rect.origin.y) &&
        std::isfinite(rect.size.width) && std::isfinite(rect.size.height) && rect.size.width>=0 && rect.size.height>=0;};
    for(NSUInteger i=0;i<count;++i) {
        const auto &layer=layers[i];id<MTLTexture> texture=layer.texture;
        if(![texture isKindOfClass:[QcomMTLTexture class]] || texture.device!=output.device || texture.framebufferOnly ||
            texture.textureType!=MTLTextureType2D || texture.sampleCount!=1 || !colorFormat(texture.pixelFormat) ||
            (texture.usage && !(texture.usage&MTLTextureUsageShaderRead)) || !rectangle(layer.frame) || !rectangle(layer.contentsRect) ||
            layer.contentsRect.origin.x<0 || layer.contentsRect.origin.y<0 ||
            layer.contentsRect.size.width>1-layer.contentsRect.origin.x || layer.contentsRect.size.height>1-layer.contentsRect.origin.y ||
            !std::isfinite(layer.opacity) || layer.opacity<0 || layer.opacity>1)return reject(@"Invalid composition texture, rectangle or opacity");
        const double affine[]={layer.transform.a,layer.transform.b,layer.transform.c,layer.transform.d,layer.transform.tx,layer.transform.ty};
        for(double component:affine)if(!std::isfinite(component))return reject(@"Invalid composition transform");
        const auto &clip=layer.scissor;
        if(clip.x>output.width || clip.y>output.height || clip.width>output.width-clip.x || clip.height>output.height-clip.y)
            return reject(@"Composition scissor exceeds the target");
        auto source=(QcomMTLTexture *)texture;
        bool alias=source->_backing==target->_backing || source->_backing->_resource.id==target->_backing->_resource.id ||
            (source.iosurface && target.iosurface && IOSurfaceGetID(source.iosurface)==IOSurfaceGetID(target.iosurface));
        auto sampled=[source nativeTexture],attachment=[target nativeTexture];sampled.resource=attachment.resource=1;
        if(alias && GPU::Shader::overlap(sampled,attachment))return reject(@"Composition cannot sample overlapping output storage");
        [textures addObject:texture];
    }
    // CPU work builds transformed quad geometry only. Clearing, sampling,
    // rasterization, clipping, opacity and premultiplied blending use A540.
    const NSUInteger vertices=6*(count+1),uvBase=vertices*4;
    std::vector<float> geometry(vertices*6);
    const float corners[6][2]={{0,0},{1,0},{0,1},{0,1},{1,0},{1,1}};
    for(NSUInteger q=0;q<=count;++q)for(NSUInteger i=0;i<6;++i) {
        double u=corners[i][0],v=corners[i][1],x=u*output.width,y=v*output.height;
        if(q) {
            const auto &layer=layers[q-1];double px=layer.frame.origin.x+u*layer.frame.size.width,py=layer.frame.origin.y+v*layer.frame.size.height;
            x=layer.transform.a*px+layer.transform.c*py+layer.transform.tx;y=layer.transform.b*px+layer.transform.d*py+layer.transform.ty;
            u=layer.contentsRect.origin.x+u*layer.contentsRect.size.width;v=layer.contentsRect.origin.y+v*layer.contentsRect.size.height;
        }
        NSUInteger index=q*6+i;float nx=float(2*x/output.width-1),ny=float(1-2*y/output.height);
        if(!std::isfinite(nx) || !std::isfinite(ny))return reject(@"Composition transform exceeds float32 coordinates");
        geometry[index*4]=nx;geometry[index*4+1]=ny;geometry[index*4+3]=1;
        geometry[uvBase+index*2]=float(u);geometry[uvBase+index*2+1]=float(v);
    }
    id<MTLDevice> device=commands.device;
    id<MTLLibrary> library=loadLibrary((QcomMTLDevice *)device,
        [NSData dataWithBytes:GPUPrograms::Library length:sizeof(GPUPrograms::Library)-1],nullptr);
    id<MTLFunction> vertex=[library newFunctionWithName:@"composition_vertex"];
    id<MTLFunction> color=[library newFunctionWithName:@"composition_clear"],image=[library newFunctionWithName:@"composition_fragment"];
    MTLRenderPipelineDescriptor *descriptor=[MTLRenderPipelineDescriptor new];descriptor.vertexFunction=vertex;descriptor.fragmentFunction=color;
    auto attachment=descriptor.colorAttachments[0];attachment.pixelFormat=output.pixelFormat;
    NSError *pipelineError=nil;id<MTLRenderPipelineState> clearPipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&pipelineError];
    descriptor.fragmentFunction=image;attachment.blendingEnabled=YES;
    attachment.sourceRGBBlendFactor=attachment.sourceAlphaBlendFactor=MTLBlendFactorOne;
    attachment.destinationRGBBlendFactor=attachment.destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
    id<MTLRenderPipelineState> imagePipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&pipelineError];
    if(!clearPipeline || !imagePipeline) {if(error)*error=pipelineError ?: failure(@"Composition shader pipelines are unavailable");return NO;}
    id<MTLBuffer> vertexData=[device newBufferWithBytes:geometry.data() length:geometry.size()*sizeof(float) options:MTLResourceStorageModeShared];
    MTLSamplerDescriptor *sampler=[MTLSamplerDescriptor new];sampler.minFilter=sampler.magFilter=MTLSamplerMinMagFilterLinear;
    id<MTLSamplerState> filtering=[device newSamplerStateWithDescriptor:sampler];
    if(!vertexData || !filtering)return reject(@"Composition resource allocation failed");
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=output;
    // Draw the background in one real GPU draw instead of thousands of small
    // CP fills. Every pixel is covered before premultiplied layers are drawn.
    pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder=[commands renderCommandEncoderWithDescriptor:pass];
    if(!encoder)return reject(@"Composition render pass could not be recorded");
    // The helper owns these implementation objects; the caller cannot retain
    // them on behalf of an unretained command buffer. Input textures and the
    // output retain the command buffer's chosen external ownership mode.
    for(id resource in @[clearPipeline,imagePipeline,vertexData,filtering])[buffer retainInternalResource:resource];
    [encoder setRenderPipelineState:clearPipeline];[encoder setVertexBuffer:vertexData offset:0 atIndex:0];
    [encoder setVertexBuffer:vertexData offset:uvBase*sizeof(float) atIndex:1];
    float background[4]={float(clearColor.red),float(clearColor.green),float(clearColor.blue),float(clearColor.alpha)};
    [encoder setFragmentBytes:background length:sizeof(background) atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [encoder setRenderPipelineState:imagePipeline];[encoder setFragmentSamplerState:filtering atIndex:0];
    for(NSUInteger i=0;i<count;++i) {
        const auto &layer=layers[i];if(!layer.opacity || !layer.frame.size.width || !layer.frame.size.height)continue;
        MTLScissorRect clip=layer.scissor;if(!clip.x && !clip.y && !clip.width && !clip.height)clip={0,0,output.width,output.height};
        if(!clip.width || !clip.height)continue;
        [encoder setScissorRect:clip];[encoder setVertexBuffer:vertexData offset:(i+1)*6*4*sizeof(float) atIndex:0];
        [encoder setVertexBuffer:vertexData offset:uvBase*sizeof(float)+(i+1)*6*2*sizeof(float) atIndex:1];
        [encoder setFragmentTexture:textures[i] atIndex:0];float opacity=layer.opacity;
        [encoder setFragmentBytes:&opacity length:sizeof(opacity) atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    }
    [encoder endEncoding];if(commands.error) {if(error)*error=commands.error;return NO;}return YES;
}
