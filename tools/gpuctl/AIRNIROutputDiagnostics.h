// SPDX-License-Identifier: BSD-2-Clause
// Diagnostic only: frozen production programs, real A540 work and readbacks.
#pragma once
#include <GPUJITProtocol.h>
#include <algorithm>
#include <array>
#include <vector>

@interface NSObject (QcomFrozenProgramDiagnostic)
- (id)initWithDevice:(id<MTLDevice>)device images:(const std::vector<QGPUProgramImage> &)images error:(NSError **)error;
@end

[[maybe_unused]] static const char airNIRDiagnosticVertexSource[] = R"QGPU(
#include <metal_stdlib>
using namespace metal;
struct AirNIRVertex {float4 position [[position]];float2 uv [[user(v0)]];float4 color [[user(v1)]];};
vertex AirNIRVertex air_nir_vertex(uint id [[vertex_id]],constant float4 *config [[buffer(0)]]) {
 float2 p=id==0?float2(-1,-1):id==1?float2(3,-1):float2(-1,3);
 AirNIRVertex out;out.position=float4(p,0.25,1);
 out.uv=float2((p.x+1)*0.5*config[0].x,(1-p.y)*0.5);out.color=float4(out.uv,0.25,0.75);return out;
}
)QGPU";
static const char airNIRDiagnosticFragmentSource[] = R"QGPU(
#include <metal_stdlib>
using namespace metal;
fragment float4 air_nir_initialize(float4 p [[position]],uint sample [[sample_id]],constant uint4 *config [[buffer(0)]]) {
 uint base=uint(p.x)*17+uint(p.y)*31+config[0].y*43+sample*37+config[0].x*7;
 float4 value=float4(base&255u,(base+61u)&255u,(base+122u)&255u,(base+183u)&255u)/255.0;
 if(config[0].z==0)value.w=0;if(config[0].z==1)value.w=1;return value;
}
fragment float4 air_nir_read(float4 p [[position]],texture2d<float,access::read> input [[texture(0)]]) {
 return float4(input.read(uint2(p.xy)));
}
fragment float4 air_nir_read_ms(float4 p [[position]],texture2d_ms<float,access::read> input [[texture(0)]]) {
 return float4(input.read(uint2(uint(p.x)%32u,uint(p.y)),uint(p.x)/32u));
}
)QGPU";

namespace AirNIRHardware {
static id<MTLLibrary> library(id<MTLDevice> device, NSString *path, bool window, NSError **error,
                              QGPUProgramImage *description = nullptr) {
    NSData *bytes = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:error];
    if ([path.pathExtension isEqualToString:@"deflate"]) {
        if (!bytes.length || bytes.length > sizeof(QGPUProgramImage))
            return nil;
        bytes = [bytes decompressedDataUsingAlgorithm:NSDataCompressionAlgorithmZlib error:error];
    }
    if (bytes.length != sizeof(QGPUProgramImage))
        return nil;
    std::vector<QGPUProgramImage> images(1);
    memcpy(images.data(), bytes.bytes, bytes.length);
    if (!GPU::JIT::valid(images[0], images[0].registry))
        return nil;
    if (description)
        *description = images[0];
    if (!window) {
        images[0].constantWindow = {};
        images[0].msaaConstantWindow = {};
    }
    if (!GPU::JIT::valid(images[0], images[0].registry))
        return nil;
    Class type = NSClassFromString(@"QcomMTLLibrary");
    return type ? [[type alloc] initWithDevice:device images:images error:error] : nil;
}
static id<MTLRenderPipelineState> pipeline(id<MTLDevice> device, id<MTLFunction> vertex, id<MTLFunction> fragment,
                                           MTLPixelFormat format, unsigned samples, bool second, NSError **error) {
    auto d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vertex;
    d.fragmentFunction = fragment;
    d.rasterSampleCount = samples;
    d.colorAttachments[0].pixelFormat = format;
    if (second) {
        d.colorAttachments[1].pixelFormat = MTLPixelFormatRG32Float;
        d.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
    }
    return [device newRenderPipelineStateWithDescriptor:d error:error];
}
static id<MTLTexture> target(id<MTLDevice> device, MTLPixelFormat format, unsigned samples) {
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:32 height:32 mipmapped:NO];
    d.sampleCount = samples;
    d.textureType = samples == 1 ? MTLTextureType2D : MTLTextureType2DMultisample;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:d];
}
template <class T> static void put(std::vector<uint8_t> &b, unsigned at, T value) {
    if (at + sizeof(value) <= b.size())
        memcpy(b.data() + at, &value, sizeof(value));
}
static std::array<std::vector<uint8_t>, 8> uniforms(bool welcome, unsigned seed) {
    std::array<std::vector<uint8_t>, 8> buffers;
    for (unsigned s = 0; s < 8; ++s) {
        buffers[s].resize(512);
        for (unsigned i = 0; i < 512; ++i)
            buffers[s][i] = (i * 13 + (8 + s) * 37 + seed * 19) & 255;
    }
    if (welcome) {
        std::fill(buffers[1].begin(), buffers[1].end(), 0);
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned c = 0; c < 4; ++c)
                put<uint16_t>(buffers[1], 176 + 8 * i + 2 * c, i == c ? 0x3c00 : 0);
        put<float>(buffers[2], 0, float(int(seed % 5) - 2) * 0.5f);
        put<uint16_t>(buffers[5], 0, 0x3000);
        if (seed % 7 == 6)
            buffers[1].resize(180 + 2 * (seed % 4));
    } else {
        auto &b = buffers[0];
        for (unsigned i = 0; i < 64; ++i)
            put<float>(b, i * 4, 0.25f + float((seed + i) % 5) * 0.1f);
        const float first[] = {0.1f, float(seed) * 0.03f, 0.2f, 0.3f, 0.7f, 0.1f, 0.4f, 0.2f, 0.6f, -0.15f};
        memcpy(b.data(), first, sizeof(first));
        put<uint16_t>(b, 40, seed % 5);
        put<uint16_t>(b, 42, (seed + 1) % 5);
        put<float>(b, 44, (seed % 5) * 0.25f);
        for (unsigned at = 48; at < 120; at += 12) {
            put<float>(b, at, 0.05f);
            put<float>(b, at + 4, 0.95f);
            put<float>(b, at + 8, 1.2f);
        }
        put<uint16_t>(b, 120, seed % 5);
        put<uint16_t>(b, 122, (seed + 2) % 5);
        for (unsigned i = 0; i < 16; ++i)
            put<float>(b, 144 + i * 4, i % 5 == 0 ? 1.f : 0.f);
        for (unsigned i = 0; i < 4; ++i)
            put<float>(b, 208 + i * 4, 0.05f);
        if (seed % 13 == 12)
            b.resize(180 + (seed % 4) * 2);
    }
    return buffers;
}
static id<MTLTexture> texture(id<MTLDevice> device, unsigned seed, unsigned image) {
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                width:32
                                                               height:32
                                                            mipmapped:YES];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    auto result = [device newTextureWithDescriptor:d];
    for (unsigned level = 0; result && level < 6; ++level) {
        unsigned edge = 32 >> level;
        std::vector<uint8_t> b(edge * edge * 4);
        for (unsigned y = 0; y < edge; ++y)
            for (unsigned x = 0; x < edge; ++x)
                for (unsigned c = 0; c < 4; ++c)
                    b[(x + y * edge) * 4 + c] =
                        c == 3 && seed % 3 != 2
                            ? (seed % 3 == 0 ? 0 : 255)
                            : uint8_t(x * 17 + y * 31 + image * 43 + level * 29 + c * 61 + seed * 7);
        [result replaceRegion:MTLRegionMake2D(0, 0, edge, edge)
                  mipmapLevel:level
                    withBytes:b.data()
                  bytesPerRow:edge * 4];
    }
    return result;
}
static id<MTLSamplerState> sampler(id<MTLDevice> device, unsigned seed) {
    auto d = [MTLSamplerDescriptor new];
    d.minFilter = d.magFilter = seed & 1 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = seed & 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
    d.lodMaxClamp = 5;
    d.sAddressMode = seed & 1 ? MTLSamplerAddressModeMirrorClampToEdge : MTLSamplerAddressModeRepeat;
    d.tAddressMode = seed & 2 ? MTLSamplerAddressModeMirrorClampToEdge : MTLSamplerAddressModeMirrorRepeat;
    return [device newSamplerStateWithDescriptor:d];
}
static bool initialize(id<MTLCommandBuffer> commands, id<MTLTexture> image, id<MTLRenderPipelineState> state,
                       id<MTLBuffer> vertex, unsigned seed, unsigned index) {
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = image;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    auto r = [commands renderCommandEncoderWithDescriptor:pass];
    if (!r)
        return false;
    const uint32_t config[] = {seed, index, seed % 3, 0};
    [r setRenderPipelineState:state];
    [r setVertexBuffer:vertex offset:0 atIndex:0];
    [r setFragmentBytes:config length:sizeof(config) atIndex:0];
    [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [r endEncoding];
    return true;
}
static NSData *readback(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> image,
                        id<MTLRenderPipelineState> reader, id<MTLBuffer> vertex, bool &guards) {
    NSUInteger pitch = 32 * image.sampleCount * 8, bytes = pitch * 32, offset = 256;
    auto backing = [device newBufferWithLength:offset + bytes + 256 options:MTLResourceStorageModeShared];
    if (!backing.contents)
        return nil;
    memset(backing.contents, 0xa5, backing.length);
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                width:32 * image.sampleCount
                                                               height:32
                                                            mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget;
    auto atlas = [backing newTextureWithDescriptor:d offset:offset bytesPerRow:pitch];
    if (!atlas)
        return nil;
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = atlas;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    auto commands = [queue commandBuffer];
    auto r = [commands renderCommandEncoderWithDescriptor:pass];
    if (!r)
        return nil;
    [r setRenderPipelineState:reader];
    [r setVertexBuffer:vertex offset:0 atIndex:0];
    [r setFragmentTexture:image atIndex:0];
    [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [r endEncoding];
    if (!completed(commands))
        return nil;
    guards = true;
    const auto *b = (const uint8_t *)backing.contents;
    for (NSUInteger i = 0; i < backing.length; ++i)
        if ((i < offset || i >= offset + bytes) && b[i] != 0xa5)
            guards = false;
    return [NSData dataWithBytes:b + offset length:bytes];
}
static bool save(NSData *bytes, NSString *folder, NSString *name) {
    return bytes && [bytes writeToFile:[folder stringByAppendingPathComponent:name] atomically:YES];
}
} // namespace AirNIRHardware

static int airNIROutputs(id<MTLDevice> device) {
    using namespace AirNIRHardware;
    @try {
        NSString *folder = [driverBundlePath() stringByAppendingPathComponent:@"air-nir-output"];
        NSData *manifestData = [NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"manifest.json"]];
        NSDictionary *manifest =
            manifestData ? [NSJSONSerialization JSONObjectWithData:manifestData options:0 error:nullptr] : nil;
        NSArray *cases = manifest[@"cases"];
        if ([manifest[@"version"] unsignedIntValue] != 1 || ![cases isKindOfClass:[NSArray class]] ||
            cases.count != 10) {
            fputs("AIR_NIR_OUTPUT frozen fixture manifest missing or invalid\n", stderr);
            return 1;
        }
        NSString *output = @"/private/var/tmp/air-nir-hardware";
        if (![[NSFileManager defaultManager] createDirectoryAtPath:output
                                       withIntermediateDirectories:YES
                                                        attributes:nil
                                                             error:nullptr])
            return 1;
        NSMutableArray *rows = [NSMutableArray new];
        NSMutableDictionary *readbacks = [NSMutableDictionary new];
        NSError *error = nil;
        auto queue = [device newCommandQueue];
        if (!queue)
            return 1;
        auto common = [device newLibraryWithSource:[NSString stringWithUTF8String:airNIRDiagnosticFragmentSource]
                                           options:nil
                                             error:&error];
        if (!common) {
            fprintf(stderr, "AIR_NIR_OUTPUT helper compiler: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        unsigned failures = 0, computeCases = 0, renderCases = 0;
        uint64_t totalValues = 0, totalMismatches = 0;
        auto report = [&](bool complete) {
            NSDictionary *r = @{
                @"hardware_tested" : @YES,
                @"complete" : @(complete),
                @"passed" : @(complete && !failures),
                @"device" : device.name,
                @"fixture_manifest" : manifest,
                @"compute_cases" : @(computeCases),
                @"render_cases" : @(renderCases),
                @"values" : @(totalValues),
                @"bit_or_byte_mismatches" : @(totalMismatches),
                @"failed_cases" : @(failures),
                @"cases" : rows,
                @"readbacks" : readbacks,
                @"limits" : @[
                    @"Frozen production JIT images test A540 output, not device AIR compilation time.",
                    @"Fragment paths share authored helpers and driver; equality is not an independent reference for all pixels.",
                    @"Confirm selected window variants against native operation hashes and constantWindowState."
                ]
            };
            NSData *data = [NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nullptr];
            return save(data, output, @"result.json") &&
                   save(data, @"/private/var/tmp", @"setup-air-nir-hardware-result.txt");
        };
        if (!report(false))
            return 1;
        for (NSDictionary *item in cases)
            @autoreleasepool {
                NSString *label = item[@"label"];
                unsigned profile = [item[@"profile"] unsignedIntValue];
                bool compute = [label isEqualToString:@"compute"], welcome = [label isEqualToString:@"welcome"];
                if (!compute && !welcome && ![label isEqualToString:@"fluidity"])
                    return 1;
                NSMutableData *bundle = [NSMutableData new];
                NSString *bundleName = [NSString stringWithFormat:@"setup-air-nir-hardware-%@-%u.txt", label, profile];
                auto append = [&](NSData *bytes, NSString *name) {
                    if (!bytes || bytes.length > 8 * 1024 * 1024 - bundle.length)
                        return false;
                    readbacks[name] =
                        @{@"bundle" : bundleName,
                          @"offset" : @(bundle.length),
                          @"bytes" : @(bytes.length)};
                    [bundle appendData:bytes];
                    return true;
                };
                if (compute) {
                    id<MTLComputePipelineState> states[2];
                    for (unsigned route = 0; route < 2; ++route) {
                        QGPUProgramImage image{};
                        auto lib =
                            library(device, [folder stringByAppendingPathComponent:item[route ? @"nir" : @"glsl"]],
                                    false, &error, &image);
                        auto f = [lib newFunctionWithName:[NSString stringWithUTF8String:image.name]];
                        states[route] = f ? [device newComputePipelineStateWithFunction:f error:&error] : nil;
                        if (!states[route])
                            return 1;
                    }
                    for (unsigned seed = 0; seed < 16; ++seed)
                        @autoreleasepool {
                            unsigned inputBytes = seed % 4 == 0   ? 4
                                                  : seed % 4 == 1 ? 256
                                                                  : 512,
                                     outputBytes = seed % 5 == 0 ? 512 : inputBytes;
                            NSData *results[2];
                            uint64_t referenceMismatches = 0;
                            bool guards = true;
                            for (unsigned route = 0; route < 2; ++route) {
                                auto input = [device newBufferWithLength:inputBytes
                                                                 options:MTLResourceStorageModeShared],
                                     target = [device newBufferWithLength:outputBytes
                                                                  options:MTLResourceStorageModeShared];
                                if (!input.contents || !target.contents || input.allocatedSize < inputBytes + 64 ||
                                    target.allocatedSize < outputBytes + 64)
                                    return 1;
                                memset(input.contents, 0xa5, inputBytes + 64);
                                memset(target.contents, 0xa5, outputBytes + 64);
                                for (unsigned i = 0; i < inputBytes / 4; ++i)
                                    ((uint32_t *)input.contents)[i] = i * 0x10203u + seed;
                                std::vector<uint8_t> inputExpected((const uint8_t *)input.contents,
                                                                   (const uint8_t *)input.contents + inputBytes + 64);
                                auto cb = [queue commandBuffer];
                                auto ce = [cb computeCommandEncoder];
                                [ce setComputePipelineState:states[route]];
                                [ce setBuffer:input offset:0 atIndex:0];
                                [ce setBuffer:target offset:0 atIndex:1];
                                [ce dispatchThreadgroups:MTLSizeMake(3, 1, 1)
                                    threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                                [ce endEncoding];
                                if (!completed(cb))
                                    return 1;
                                for (unsigned i = 0; i < outputBytes / 4; ++i)
                                    if (((uint32_t *)target.contents)[i] !=
                                        (i < inputBytes / 4 ? i * 0x10203u + seed : 0) + 7)
                                        ++referenceMismatches;
                                guards &= !memcmp(input.contents, inputExpected.data(), inputExpected.size());
                                for (unsigned i = 0; i < 64; ++i)
                                    if (((const uint8_t *)target.contents)[outputBytes + i] != 0xa5)
                                        guards = false;
                                results[route] = [NSData dataWithBytes:target.contents length:outputBytes + 64];
                                if (!append(results[route], [NSString stringWithFormat:@"compute-%u-%s.bin", seed,
                                                                                       route ? "nir" : "glsl"]))
                                    return 1;
                            }
                            uint64_t mismatch = 0;
                            for (NSUInteger i = 0; i < results[0].length; ++i)
                                if (((const uint8_t *)results[0].bytes)[i] != ((const uint8_t *)results[1].bytes)[i])
                                    ++mismatch;
                            bool passed = !mismatch && !referenceMismatches && guards;
                            failures += !passed;
                            ++computeCases;
                            totalValues += outputBytes / 4;
                            totalMismatches += mismatch;
                            [rows addObject:@{
                                @"label" : label,
                                @"seed" : @(seed),
                                @"input_bytes" : @(inputBytes),
                                @"output_bytes" : @(outputBytes),
                                @"byte_mismatches" : @(mismatch),
                                @"reference_mismatches" : @(referenceMismatches),
                                @"guards" : @(guards),
                                @"passed" : @(passed)
                            }];
                        }
                } else {
                    QGPUProgramImage vi{};
                    auto vl =
                        library(device, [folder stringByAppendingPathComponent:item[@"vertex"]], false, &error, &vi);
                    auto vertex = [vl newFunctionWithName:[NSString stringWithUTF8String:vi.name]];
                    if (!vertex)
                        return 1;
                    uint64_t nonzeroProfile = 0;
                    for (unsigned window = 0; window < 2; ++window)
                        @autoreleasepool {
                            id<MTLLibrary> libs[2];
                            id<MTLFunction> fragments[2];
                            QGPUProgramDescription descriptions[2]{};
                            for (unsigned route = 0; route < 2; ++route) {
                                QGPUProgramImage image{};
                                libs[route] = library(
                                    device, [folder stringByAppendingPathComponent:item[route ? @"nir" : @"glsl"]],
                                    window, &error, &image);
                                descriptions[route] = image.info;
                                fragments[route] =
                                    [libs[route] newFunctionWithName:[NSString stringWithUTF8String:image.name]];
                                if (!fragments[route])
                                    return 1;
                            }
                            for (unsigned samples : {1u, 2u, 4u})
                                @autoreleasepool {
                                    if (![device supportsTextureSampleCount:samples])
                                        return 1;
                                    id<MTLRenderPipelineState> states[2];
                                    for (unsigned route = 0; route < 2; ++route)
                                        states[route] = pipeline(device, vertex, fragments[route],
                                                                 MTLPixelFormatRGBA16Float, samples, !welcome, &error);
                                    auto init =
                                        pipeline(device, vertex, [common newFunctionWithName:@"air_nir_initialize"],
                                                 MTLPixelFormatRGBA16Float, samples, false, &error);
                                    auto initSecond = welcome
                                                          ? nil
                                                          : pipeline(device, vertex,
                                                                     [common newFunctionWithName:@"air_nir_initialize"],
                                                                     MTLPixelFormatRG32Float, samples, false, &error);
                                    auto reader =
                                        pipeline(device, vertex,
                                                 [common newFunctionWithName:samples == 1 ? @"air_nir_read"
                                                                                          : @"air_nir_read_ms"],
                                                 MTLPixelFormatRGBA16Float, 1, false, &error);
                                    if (!states[0] || !states[1] || !init || (!welcome && !initSecond) || !reader) {
                                        fprintf(stderr, "AIR_NIR_OUTPUT pipeline %s/%u/%u/%u: %s\n", label.UTF8String,
                                                profile, window, samples, error.localizedDescription.UTF8String);
                                        return 1;
                                    }
                                    for (unsigned seed : {0u, 1u, 2u, 6u, 12u, 13u})
                                        @autoreleasepool {
                                            auto data = uniforms(welcome, seed);
                                            id<MTLBuffer> uniformsGPU[8];
                                            for (unsigned s = 0; s < 8; ++s)
                                                uniformsGPU[s] =
                                                    [device newBufferWithBytes:data[s].data()
                                                                        length:data[s].size()
                                                                       options:MTLResourceStorageModeShared];
                                            const float config[] = {seed & 1 ? -1.f : 1.f, 0, 0, 0};
                                            auto vc = [device newBufferWithBytes:config
                                                                          length:sizeof(config)
                                                                         options:MTLResourceStorageModeShared];
                                            id<MTLTexture> textures[8];
                                            for (unsigned s = 0; s < 8; ++s)
                                                textures[s] = texture(device, seed, s);
                                            auto sampling = sampler(device, seed);
                                            if (!vc || !sampling)
                                                return 1;
                                            for (unsigned s = 0; s < 8; ++s)
                                                if (!textures[s] || !uniformsGPU[s].contents)
                                                    return 1;
                                            NSData *results[2];
                                            bool guards = true, uniformsUnchanged = true;
                                            for (unsigned route = 0; route < 2; ++route)
                                                @autoreleasepool {
                                                    auto color = target(device, MTLPixelFormatRGBA16Float, samples),
                                                         second =
                                                             welcome ? nil
                                                                     : target(device, MTLPixelFormatRG32Float, samples);
                                                    if (!color || (!welcome && !second))
                                                        return 1;
                                                    auto cb = [queue commandBuffer];
                                                    if (!initialize(cb, color, init, vc, seed, 16) ||
                                                        (!welcome && !initialize(cb, second, initSecond, vc, seed, 17)))
                                                        return 1;
                                                    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
                                                    pass.colorAttachments[0].texture = color;
                                                    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
                                                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                                                    if (!welcome) {
                                                        pass.colorAttachments[1].texture = second;
                                                        pass.colorAttachments[1].loadAction = MTLLoadActionLoad;
                                                        pass.colorAttachments[1].storeAction = MTLStoreActionStore;
                                                    }
                                                    auto r = [cb renderCommandEncoderWithDescriptor:pass];
                                                    if (!r)
                                                        return 1;
                                                    [r setRenderPipelineState:states[route]];
                                                    [r setVertexBuffer:vc offset:0 atIndex:0];
                                                    for (unsigned s = 0; s < 8; ++s) {
                                                        [r setFragmentBuffer:uniformsGPU[s] offset:0 atIndex:s];
                                                        if (GPU::Shader::graphicsTextureMask(descriptions[route]) &
                                                            (1u << s))
                                                            [r setFragmentTexture:textures[s]
                                                                          atIndex:GPU::Shader::textureBinding(
                                                                                      descriptions[route], s)];
                                                        [r setFragmentSamplerState:sampling atIndex:s];
                                                    }
                                                    [r drawPrimitives:MTLPrimitiveTypeTriangle
                                                          vertexStart:0
                                                          vertexCount:3];
                                                    [r endEncoding];
                                                    if (!completed(cb))
                                                        return 1;
                                                    bool g = false;
                                                    results[route] = readback(device, queue, color, reader, vc, g);
                                                    guards &= g;
                                                    if (!results[route])
                                                        return 1;
                                                    for (unsigned s = 0; s < 8; ++s)
                                                        if (memcmp(uniformsGPU[s].contents, data[s].data(),
                                                                   data[s].size()))
                                                            uniformsUnchanged = false;
                                                    if (!append(
                                                            results[route],
                                                            [NSString stringWithFormat:@"%@-%u-w%u-ms%u-seed%u-%s.bin",
                                                                                       label, profile, window, samples,
                                                                                       seed, route ? "nir" : "glsl"]))
                                                        return 1;
                                                }
                                            if (results[0].length != results[1].length)
                                                return 1;
                                            uint64_t mismatch = 0, nonzero = 0;
                                            double maxError = 0;
                                            bool nonfinite = false;
                                            for (NSUInteger i = 0; i < results[0].length / 2; ++i) {
                                                uint16_t a = ((const uint16_t *)results[0].bytes)[i],
                                                         b = ((const uint16_t *)results[1].bytes)[i];
                                                nonzero += (a & 0x7fff) != 0;
                                                if (a != b) {
                                                    ++mismatch;
                                                    __fp16 x, y;
                                                    memcpy(&x, &a, 2);
                                                    memcpy(&y, &b, 2);
                                                    if (std::isfinite(float(x)) && std::isfinite(float(y)))
                                                        maxError =
                                                            std::max(maxError, double(std::abs(float(x) - float(y))));
                                                    else
                                                        nonfinite = true;
                                                }
                                            }
                                            bool passed = !mismatch && guards && uniformsUnchanged;
                                            failures += !passed;
                                            ++renderCases;
                                            totalMismatches += mismatch;
                                            totalValues += results[0].length / 2;
                                            nonzeroProfile += nonzero;
                                            [rows addObject:@{
                                                @"label" : label,
                                                @"profile" : @(profile),
                                                @"window_variants_provided" : @(window),
                                                @"samples" : @(samples),
                                                @"seed" : @(seed),
                                                @"format" : @"RGBA16Float raw samples via common half reader",
                                                @"values" : @(results[0].length / 2),
                                                @"bit_mismatches" : @(mismatch),
                                                @"max_absolute_error" : @(maxError),
                                                @"nonfinite_difference" : @(nonfinite),
                                                @"nonzero_components" : @(nonzero),
                                                @"guards" : @(guards),
                                                @"uniforms_unchanged" : @(uniformsUnchanged),
                                                @"passed" : @(passed)
                                            }];
                                        }
                                }
                        }
                    if (!nonzeroProfile)
                        ++failures;
                }
                if (!save(bundle, @"/private/var/tmp", bundleName) || !report(false))
                    return 1;
                printf("AIR_NIR_OUTPUT label=%s profile=%u compute=%u render=%u failures=%u\n", label.UTF8String,
                       profile, computeCases, renderCases, failures);
                fflush(stdout);
            }
        if (!report(true))
            return 1;
        printf("AIR_NIR_OUTPUT complete compute=%u render=%u values=%llu mismatches=%llu failures=%u "
               "result=%s/result.json\n",
               computeCases, renderCases, (unsigned long long)totalValues, (unsigned long long)totalMismatches,
               failures, output.UTF8String);
        fflush(stdout);
        return failures ? 1 : 0;
    } @catch (NSException *exception) {
        fprintf(stderr, "AIR_NIR_OUTPUT exception=%s reason=%s\n", exception.name.UTF8String,
                exception.reason.UTF8String);
        return 1;
    }
}
