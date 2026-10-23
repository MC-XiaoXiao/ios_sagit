// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#import <Metal/Metal.h>
#include <stddef.h>
#include <stdint.h>

// iOS 18 Metal.framework projection, audited against 22H374 arm64.
// See drivers/abi-22H374/metal-plugin-abi.json. No Apple instance variables are declared:
// the non-fragile ObjC ABI places our storage after the real superclass.
struct QcomMTLDeviceLimits {
    uint32_t maxFramebufferStorageBits;
    uint32_t linearTextureArrayAlignmentBytes, linearTextureArrayAlignmentSlice;
    uint32_t maxTileBuffers, maxTileTextures, maxTileSamplers, maxTileInlineDataSize, minTilePixels;
    uint32_t maxColorAttachments, maxVertexAttributes, maxVertexBuffers, maxVertexTextures;
    uint32_t maxVertexSamplers, maxVertexInlineDataSize, maxInterpolants;
    uint32_t maxFragmentBuffers, maxFragmentTextures, maxFragmentSamplers, maxFragmentInlineDataSize;
    uint32_t maxComputeBuffers, maxComputeTextures, maxComputeSamplers, maxComputeInlineDataSize;
    uint32_t maxComputeLocalMemorySizes, maxTotalComputeThreadsPerThreadgroup, maxComputeThreadgroupMemory;
    float maxLineWidth, maxPointSize;
    uint32_t maxVisibilityQueryOffset, padmaxBufferLength;
    uint32_t minConstantBufferAlignmentBytes, minBufferNoCopyAlignmentBytes;
    uint32_t maxTextureWidth1D, maxTextureWidth2D, maxTextureHeight2D;
    uint32_t maxTextureWidth3D, maxTextureHeight3D, maxTextureDepth3D, maxTextureDimensionCube, maxTextureLayers;
    uint32_t linearTextureAlignmentBytes, iosurfaceTextureAlignmentBytes, iosurfaceReadOnlyTextureAlignmentBytes;
    uint32_t deviceLinearTextureAlignmentBytes, deviceLinearReadOnlyTextureAlignmentBytes;
    uint32_t maxFunctionConstantIndices, maxComputeThreadgroupMemoryAlignmentBytes, maxInterpolatedComponents;
    uint32_t maxTessellationFactor, maxIndirectBuffers, maxIndirectTextures, maxIndirectSamplers;
    uint32_t maxIndirectSamplersPerDevice, maxFenceInstances, maxViewportCount, maxCustomSamplePositions;
    uint32_t maxVertexAmplificationFactor, maxVertexAmplificationCount, maxTextureBufferWidth;
    uint32_t maxComputeAttributes, maxIOCommandsInFlight, maxPredicatedNestingDepth;
    uint32_t maxAccelerationStructureLevels, maxConstantBufferArguments;
    uint64_t maxBufferLength;
};
static_assert(sizeof(QcomMTLDeviceLimits) == 264 && alignof(QcomMTLDeviceLimits) == 8, "Metal limits ABI");
static_assert(offsetof(QcomMTLDeviceLimits, maxTextureWidth2D) == 132 &&
              offsetof(QcomMTLDeviceLimits, maxBufferLength) == 256, "Metal limits fields");

// This class and its metaclass are exported by the target Metal.framework.
// The concrete device adopts the framework's registered MTLDeviceSPI protocol
// after ABI validation; the abstract base has no protocol list in22H374.
// Do not introduce a local protocol with the same private name.
@interface _MTLDevice : NSObject <MTLDevice>
- (instancetype)initWithAcceleratorPort:(uint32_t)service;
- (void)initLimits;
- (const QcomMTLDeviceLimits *)limits;
- (void)initFeatureQueries;
- (NSUInteger)featureProfile;
@end

// Structural projection of the original MTLTextureLayout protocol in22H374.
// Use a private adapter name so loading this bundle cannot register a second
// Apple protocol. The anonymous watermark and MTLRegion retain the native
// 16-byte return / indirect 48-byte region ABI.
typedef struct { uint64_t words[2]; } QcomNativeTextureLayoutWatermark;
@protocol QcomNativeTextureLayout <NSObject>
- (NSUInteger)size;
- (NSUInteger)alignment;
- (MTLSize)copyGranularity;
- (QcomNativeTextureLayoutWatermark)watermark;
- (void)initializeTextureMemory:(void *)memory;
- (void)finalizeTextureMemory:(void *)memory;
- (void)copyFromLinearBytes:(const void *)bytes linearOffset:(NSUInteger)offset
    linearBytesPerRow:(NSUInteger)rowBytes linearBytesPerImage:(NSUInteger)imageBytes
    toTextureMemory:(void *)memory textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level
    textureRegion:(MTLRegion)region;
- (void)copyFromTextureMemory:(const void *)memory textureSlice:(NSUInteger)slice
    textureLevel:(NSUInteger)level textureRegion:(MTLRegion)region toLinearBytes:(void *)bytes
    linearOffset:(NSUInteger)offset linearBytesPerRow:(NSUInteger)rowBytes linearBytesPerImage:(NSUInteger)imageBytes;
@end
@interface _MTLDevice (QcomNativeTextureLayout)
- (id<QcomNativeTextureLayout>)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor
    isHeapOrBufferBacked:(BOOL)heapOrBufferBacked;
@end

// CAMetalLayer in22H374 sets these existing descriptor properties before
// newTextureWithDescriptor:iosurface:plane:. Only zero rotation is supported.
// This category declares selectors; it does not replace their implementation.
@interface MTLTextureDescriptor (QcomNativePresentation)
- (BOOL)isDrawable;
- (BOOL)framebufferOnly;
- (NSUInteger)rotation;
- (NSUInteger)protectionOptions;
@end

// Original descriptor/resource protection getters use an unsigned64 value
// in22H374. All allocations supported by this adapter are unprotected.
@interface MTLHeapDescriptor (QcomNativeProtection)
- (NSUInteger)protectionOptions;
@end

@protocol QcomNativeDrawableOptions <MTLDrawable>
@property (readonly) id<MTLTexture> texture;
- (void)presentWithOptions:(NSDictionary *)options;
@end

// Read-only projections of actual22H374 Metal objects. Values are copied by
// Metal's own implementation; no constant-storage or private ivars are read.
@interface MTLFunctionConstantValues (QcomNativeConstants)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end
@protocol QcomNativeConstantData <NSObject>
- (NSUInteger)dataType;
- (void *)data;
@end
@protocol QcomNativeIndexedConstantData <QcomNativeConstantData>
- (NSUInteger)index;
@end
@protocol QcomNativeNamedConstantData <QcomNativeConstantData>
- (NSString *)name;
@end
@interface MTLFunctionConstant (QcomNativeReflection)
- (instancetype)initWithName:(NSString *)name type:(MTLDataType)type index:(NSUInteger)index required:(BOOL)required;
@end
