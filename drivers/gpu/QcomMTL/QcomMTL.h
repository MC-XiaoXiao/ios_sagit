// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#import <Metal/Metal.h>
#import <CoreGraphics/CGAffineTransform.h>
#import <CoreGraphics/CGGeometry.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN
// Explicit bring-up entry points, returning a retained device for the
// AOT/JIT shader, render/compute, blend/raster and linear texture profile.
// These remain project-owned entry points. System Metal loads the principal
// _MTLDevice subclass through -initWithAcceleratorPort: instead.
#ifdef __cplusplus
extern "C" {
#endif
__attribute__((visibility("default")))
id<MTLDevice> _Nullable QcomMTLCreateDevice(uint32_t service, NSError **error) NS_RETURNS_RETAINED;
__attribute__((visibility("default")))
id<MTLDevice> _Nullable QcomMTLCreateSystemDefaultDevice(NSError **error) NS_RETURNS_RETAINED;

// Ordered back-to-front2D layers. Textures contain premultiplied RGBA;
// opacity scales RGB and alpha together. frame/transform use target pixels,
// contentsRect uses normalized coordinates. An all-zero scissor means the
// complete target. The input records are copied before commands are recorded.
typedef struct {
    __unsafe_unretained id<MTLTexture> texture;
    CGRect frame,contentsRect;
    CGAffineTransform transform;
    MTLScissorRect scissor;
    float opacity;
} QcomMTLCompositionLayer;

// Record a GPU clear and up to8 transformed, cropped and clipped layers in
// an existing command buffer. The target may be an imported IOSurface or a
// native CAMetalDrawable.texture. The caller presents/commits that buffer.
// clearColor uses premultiplied alpha. This does not replace CARenderServer.
__attribute__((visibility("default")))
BOOL QcomMTLEncodeComposition(id<MTLCommandBuffer> commands,id<MTLTexture> target,
    const QcomMTLCompositionLayer * _Nullable layers,NSUInteger count,MTLClearColor clearColor,NSError **error);
#ifdef __cplusplus
}
#endif
NS_ASSUME_NONNULL_END
