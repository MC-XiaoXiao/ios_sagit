// SPDX-License-Identifier: BSD-2-Clause AND MIT
// VPC linkage follows Mesa Freedreno/IR3, Copyright (c) 2016 Rob Clark.
// The MIT permission notice is retained in tools/gpu-compiler/NOTICE.txt.
#pragma once
#include <GPUUserProtocol.h>
#include <GPUShaderInterface.h>
namespace GPU { namespace Shader {
constexpr uint32_t Version = 44, MaxBindings = QGPU_STAGE_BUFFER_COUNT;
constexpr uint32_t MaxComputeThreads = 1024, MaxComputeGroups = 65535;
// Mesa's A5xx compiler provisions 64 reconvergence entries; the SP register
// encodes pairs. Barrier shaders retain the prior three-pair admission bound
// so all waves of an admitted 1024-thread group can remain resident.
constexpr uint32_t MaxBranchStack=32, MaxBarrierBranchStack=3;
constexpr uint64_t MaxComputeInvocations = 16777216;
constexpr uint32_t MaxDrawVertices = 1048576, MaxDrawInstances = 65535;
constexpr uint64_t MaxDrawInvocations = 16777216;
enum Stage : uint32_t { Compute, Vertex, Fragment };
enum Format : uint32_t { RGBA8 = 1, BGRA8 = 2, Depth32 = 3, Stencil8 = 4, RGBA8SRGB = 5, BGRA8SRGB = 6, R8 = 7, RG8 = 8, A8 = 9, RGBA16Float = 10, RG32Float = 11, RGBA16Unorm = 12, ASTC4x4 = 13, ASTC4x4SRGB = 14, R32Uint = 15 };
inline bool unsignedIntegerFormat(uint32_t format) {return format==R32Uint;}
inline bool compressedFormat(uint32_t format) {return format==ASTC4x4 || format==ASTC4x4SRGB;}
inline bool sampledOnlyFormat(uint32_t format) { return format==A8 || format==RGBA16Unorm || compressedFormat(format); }
inline bool renderableColorFormat(uint32_t format) { return format==RGBA8 || format==BGRA8 || format==RGBA8SRGB || format==BGRA8SRGB || format==R8 || format==RG8 || format==RGBA16Float || format==RG32Float; }
inline bool colorFormat(uint32_t format) { return renderableColorFormat(format) || sampledOnlyFormat(format) || unsignedIntegerFormat(format); }
// Storage bytes per block. Uncompressed formats have a one-texel block.
inline uint32_t formatBytes(uint32_t format) { return compressedFormat(format)?16u:format==RGBA16Float || format==RGBA16Unorm || format==RG32Float?8u:format==R8 || format==A8 || format==Stencil8?1u:format==RG8?2u:colorFormat(format) || format==Depth32?4u:0u; }
inline uint32_t formatBlockSize(uint32_t format) {return compressedFormat(format)?4u:1u;}
inline uint32_t formatBlocks(uint32_t extent,uint32_t format) {return extent/formatBlockSize(format)+bool(extent%formatBlockSize(format));}
inline bool srgbFormat(uint32_t format) { return format==RGBA8SRGB || format==BGRA8SRGB || format==ASTC4x4SRGB; }
inline bool bgraFormat(uint32_t format) { return format==BGRA8 || format==BGRA8SRGB; }
inline uint32_t linearFormat(uint32_t format) { return format==RGBA8SRGB?RGBA8:format==BGRA8SRGB?BGRA8:format==ASTC4x4SRGB?ASTC4x4:format; }
struct Binding { uint32_t resource, offset, bytes, reserved; };
constexpr uint32_t MaxMipLevels=15;
enum TextureChannel : uint32_t { ChannelZero,ChannelOne,ChannelRed,ChannelGreen,ChannelBlue,ChannelAlpha };
// Zero is identity, otherwise four semantic 3-bit selectors plus one. This
// keeps an empty descriptor all-zero and represents an all-zero swizzle.
inline uint32_t packSwizzle(uint32_t r,uint32_t g,uint32_t b,uint32_t a) {
    if (r>ChannelAlpha || g>ChannelAlpha || b>ChannelAlpha || a>ChannelAlpha) return UINT32_MAX;
    return r==ChannelRed && g==ChannelGreen && b==ChannelBlue && a==ChannelAlpha?0:1+r+(g<<3)+(b<<6)+(a<<9);
}
inline bool validSwizzle(uint32_t value) {
    if (!value) return true;
    uint32_t bits=value-1;if (bits&~4095u) return false;
    for (unsigned i=0;i<4;++i) if (((bits>>(i*3))&7)>ChannelAlpha) return false;
    return true;
}
inline uint32_t swizzleChannel(uint32_t value,unsigned channel) {return value?((value-1)>>(channel*3))&7:channel+ChannelRed;}
inline uint32_t composeSwizzle(uint32_t parent,uint32_t view) {
    if (!validSwizzle(parent) || !validSwizzle(view)) return UINT32_MAX;
    uint32_t channels[4];
    for (unsigned i=0;i<4;++i) {uint32_t c=swizzleChannel(view,i);channels[i]=c<ChannelRed?c:swizzleChannel(parent,c-ChannelRed);}
    return packSwizzle(channels[0],channels[1],channels[2],channels[3]);
}
struct Texture {
    uint32_t resource, offset, width, height, pitch, format;
    uint32_t mipLevels=0; // Zero is the canonical single-level plane encoding.
    uint32_t swizzle=0; // Read/sample only; never changes writes or storage.
    uint32_t layers=0,layerStride=0; // Zero layers denotes a single2D/cube texture; arrays retain the original mip-chain stride in views.
    uint32_t cube=0; // Boolean dimension tag; layers counts cubes for cube arrays, zero denotes a single cube.
    uint32_t samples=0; // Canonical 0 means one sample; 2/4 are linear per-pixel sample arrays.
};
inline uint32_t textureSamples(const Texture &t) {return t.samples?t.samples:1;}
inline bool validSamples(uint32_t samples) {return !samples || samples==2 || samples==4;}

struct TextureCopyRegion {uint64_t offset=0,rowBytes=0,rows=0;};
inline bool textureCopyRegion(const Texture &t,uint64_t x,uint64_t y,uint64_t width,uint64_t height,TextureCopyRegion &out) {
    out={};
    const auto block=formatBlockSize(t.format),bytes=formatBytes(t.format)*textureSamples(t);
    if(!bytes || !validSamples(t.samples) || (compressedFormat(t.format) && t.samples) ||
        t.width>MaxTextureDimension || t.height>MaxTextureDimension ||
        x>t.width || width>t.width-x || y>t.height || height>t.height-y)return false;
    if(!width || !height)return true;
    if(x%block || y%block || (width%block && x+width!=t.width) || (height%block && y+height!=t.height))return false;
    out.offset=(y/block)*t.pitch+(x/block)*bytes;
    out.rowBytes=(width/block+bool(width%block))*bytes;
    out.rows=height/block+bool(height%block);return true;
}

inline uint32_t textureSlices(const Texture &t) {return (t.layers?t.layers:1)*(t.cube?6u:1u);}
enum AddressMode : uint32_t { ClampToEdge,MirrorClampToEdge,Repeat,MirrorRepeat,ClampToZero,ClampToBorderColor };
struct Sampler {
    uint32_t filters=0,minLOD=0,maxLOD=32; // LOD is unsigned 8.8 fixed point.
    uint32_t addressModes=0,maxAnisotropy=1,borderColor=0,normalized=1;
};
template<class S> inline uint32_t addressMode(const S &s,unsigned axis) {return (s.addressModes>>(axis*3))&7;}
template<class S> inline uint32_t samplerMirrorFlags(const S &s) {
    return uint32_t(addressMode(s,0)==MirrorClampToEdge)|(uint32_t(addressMode(s,1)==MirrorClampToEdge)<<1);
}
template<class S> inline uint32_t samplerInfoFlags(const S &s) {return samplerMirrorFlags(s);}
template<class S> inline bool validSampler(const S &s) {
    if ((s.filters&~0x70fu) || ((s.filters>>2)&3)>=3 || s.minLOD>s.maxLOD || s.maxLOD>4095 ||
        (s.addressModes&~511u) || !s.maxAnisotropy || s.maxAnisotropy>16 || s.borderColor>2 || s.normalized>1) return false;
    for(unsigned axis=0;axis<3;++axis)if(addressMode(s,axis)>ClampToBorderColor)return false;
    if(!s.normalized && (s.addressModes || s.maxAnisotropy!=1 || (s.filters&12u) || (s.filters&1u)!=((s.filters>>1)&1u)))return false;
    // A5xx has one border color per sampler. Two different axis colors need
    // a more general sampling lowering; do not silently select one of them.
    bool zero=addressMode(s,0)==ClampToZero || addressMode(s,1)==ClampToZero;
    bool border=addressMode(s,0)==ClampToBorderColor || addressMode(s,1)==ClampToBorderColor;
    return !(zero && border && s.borderColor);
}
inline bool emptySampler(const Sampler &s) {return !s.filters && !s.minLOD && s.maxLOD==32 && !s.addressModes && s.maxAnisotropy==1 && !s.borderColor && s.normalized==1;}
enum BlendFactor : uint32_t {
    Zero, One, SourceColor, OneMinusSourceColor, SourceAlpha, OneMinusSourceAlpha,
    DestinationColor, OneMinusDestinationColor, DestinationAlpha, OneMinusDestinationAlpha,
    SourceAlphaSaturated, BlendColor, OneMinusBlendColor, BlendAlpha, OneMinusBlendAlpha
};
enum BlendOperation : uint32_t { Add, Subtract, ReverseSubtract, Min, Max };
enum CullMode : uint32_t { CullNone, CullFront, CullBack };
enum Winding : uint32_t { Clockwise, CounterClockwise };
enum Interpolation : uint32_t { Perspective, Linear, Flat, PerspectiveCentroid, LinearCentroid, PerspectiveSample, LinearSample };
enum Compare : uint32_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum StencilOperation : uint32_t { Keep, Clear, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap };
struct StencilFace { uint32_t enabled,compare,fail,depthFail,pass,readMask,writeMask; };
struct DepthStencilState {
    uint32_t compare,write;
    StencilFace front,back;
    uint32_t frontReference,backReference;
};
inline DepthStencilState defaultDepthStencilState() {
    DepthStencilState s{};s.compare=Always;s.front.compare=s.back.compare=Always;
    s.front.readMask=s.front.writeMask=s.back.readMask=s.back.writeMask=255;return s;
}
inline bool validDepthStencilState(const DepthStencilState &s) {
    if (s.compare>Always || s.write>1 || s.frontReference>255 || s.backReference>255) return false;
    const StencilFace faces[]={s.front,s.back};
    for (const auto &f:faces) if (f.enabled>1 || f.compare>Always || f.fail>DecrementWrap || f.depthFail>DecrementWrap ||
        f.pass>DecrementWrap || f.readMask>255 || f.writeMask>255) return false;
    return true;
}
// Semantic state, never user-supplied register encodings. Floats cross the
// boundary as IEEE-754 bits; validation/encoding in the Kext uses integers.
struct RenderState {
    uint32_t blend, writeMask, rgbOperation, alphaOperation;
    uint32_t sourceRGB, destinationRGB, sourceAlpha, destinationAlpha;
    uint32_t blendColor[4]; // normalized float32 RGBA, default zero
    uint32_t viewport[4]; // float32 x offset/scale, y offset/scale
    uint32_t cull, winding;
    uint32_t depthRange[2]; // float32 Z offset/scale
};
struct ColorState {
    uint32_t blend,writeMask,rgbOperation,alphaOperation;
    uint32_t sourceRGB,destinationRGB,sourceAlpha,destinationAlpha;
};
template<class S> inline bool validColorState(const S &s) {
    return s.blend<=1 && s.writeMask<=15 && s.rgbOperation<=Max && s.alphaOperation<=Max &&
        s.sourceRGB<=OneMinusBlendAlpha && s.destinationRGB<=OneMinusBlendAlpha &&
        s.sourceAlpha<=OneMinusBlendAlpha && s.destinationAlpha<=OneMinusBlendAlpha;
}
template<class S> inline ColorState copyColorState(const S &s) {
    return {s.blend,s.writeMask,s.rgbOperation,s.alphaOperation,s.sourceRGB,s.destinationRGB,s.sourceAlpha,s.destinationAlpha};
}
inline uint32_t halfDimension(uint32_t value) {
    if (!value || value > MaxTextureDimension) return 0;
    uint32_t bit = 31 - __builtin_clz(value);
    return ((bit + 126) << 23) | ((value - (1u << bit)) << (23 - bit));
}
inline RenderState defaultRenderState(uint32_t width, uint32_t height) {
    RenderState s{}; s.writeMask = 15; s.sourceRGB = s.sourceAlpha = One;
    s.viewport[0] = s.viewport[1] = halfDimension(width);
    s.viewport[2] = halfDimension(height); s.viewport[3] = s.viewport[2] | 0x80000000;
    s.depthRange[1]=0x3f800000;
    return s;
}
inline bool validRenderState(const RenderState &s) {
    if (s.blend > 1 || s.writeMask > 15 || s.rgbOperation > Max || s.alphaOperation > Max ||
        s.sourceRGB > OneMinusBlendAlpha || s.destinationRGB > OneMinusBlendAlpha ||
        s.sourceAlpha > OneMinusBlendAlpha || s.destinationAlpha > OneMinusBlendAlpha ||
        s.cull > CullBack || s.winding > CounterClockwise) return false;
    for (auto color : s.blendColor) if (color > 0x3f800000) return false; // finite [0,1]
    if (s.depthRange[0]>0x3f800000 || (s.depthRange[1]&0x7fffffff)>0x3f800000) return false;
    // Window/scissor bounds still constrain all render-target memory writes.
    // Restrict offsets to +/-32768 and scales to [2^-16,8192], with Metal's
    // top-left viewport (negative Y scale). Denormals/NaNs/infinities fail.
    return (s.viewport[0] & 0x7fffffff) <= 0x47000000 &&
        s.viewport[1] >= 0x37800000 && s.viewport[1] <= 0x46000000 &&
        (s.viewport[2] & 0x7fffffff) <= 0x47000000 &&
        (s.viewport[3] & 0x80000000) &&
        (s.viewport[3] & 0x7fffffff) >= 0x37800000 && (s.viewport[3] & 0x7fffffff) <= 0x46000000;
}
struct Dispatch {
    uint64_t registry;
    uint32_t version, program, groups[3], local[3];
    uint32_t grid[3]={},reserved=0; // All-zero grid is the uniform group form.
    Binding buffers[MaxBindings];
    uint32_t threadgroupLengths[MaxBindings];
    Texture images[QGPU_COMPUTE_IMAGE_COUNT];
    Sampler samplers[QGPU_COMPUTE_IMAGE_COUNT];
};
struct Draw {
    uint64_t registry;
    uint32_t version, vertex, fragment, primitive, start, count;
    Texture target, textures[QGPU_STAGE_TEXTURE_COUNT];
    Sampler samplers[QGPU_STAGE_SAMPLER_COUNT];
    uint32_t scissor[4], reserved;
    Binding buffers[MaxBindings];
    RenderState state;
    Binding indices; // resource/offset/bytes; copied into job-private storage
    uint32_t indexSize, instances, baseInstance;
    int32_t baseVertex;
    Texture depth,stencil;
    DepthStencilState depthStencil;
    struct VertexAttribute { uint32_t format,offset,buffer,reserved; } attributes[QGPU_MAX_ATTRIBUTES];
    struct VertexLayout { uint32_t stride,step,rate,reserved; } layouts[MaxBindings];
    Binding fragmentBuffers[MaxBindings];
    Texture vertexTextures[QGPU_STAGE_TEXTURE_COUNT];
    Sampler vertexSamplers[QGPU_STAGE_SAMPLER_COUNT];
    uint32_t vertexReserved=0,textureReserved=0;
    Texture colorTargets[QGPU_COLOR_TARGET_COUNT-1];
    ColorState colorStates[QGPU_COLOR_TARGET_COUNT-1];
    uint32_t rasterSamples=0,alphaToCoverage=0;
};
inline const Texture &colorTarget(const Draw &d,unsigned slot) {return slot?d.colorTargets[slot-1]:d.target;}
inline ColorState colorState(const Draw &d,unsigned slot) {return slot?d.colorStates[slot-1]:copyColorState(d.state);}
inline uint32_t colorTargetMask(const Draw &d) {
    uint32_t mask=0;for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(colorTarget(d,i).resource)mask|=1u<<i;return mask;
}
inline bool validDraw(const Draw &d) {
    if ((d.primitive!=1 && d.primitive!=3 && d.primitive!=4) || d.reserved || d.vertexReserved || d.textureReserved || !validSamples(d.rasterSamples) || d.alphaToCoverage>1 || !d.count || d.count > MaxDrawVertices || !d.instances || d.instances > MaxDrawInstances ||
        uint64_t(d.count)*d.instances > MaxDrawInvocations ||
        uint64_t(d.baseInstance)+d.instances > UINT32_MAX || !validRenderState(d.state)) return false;
    for(unsigned i=1;i<QGPU_COLOR_TARGET_COUNT;++i) {
        const auto &s=d.colorStates[i-1];if(!validColorState(s))return false;
        if(!colorTarget(d,i).resource && (s.blend || s.writeMask || s.rgbOperation || s.alphaOperation ||
            s.sourceRGB || s.destinationRGB || s.sourceAlpha || s.destinationAlpha))return false;
    }
    if (!d.indexSize) return !d.indices.resource && !d.indices.offset && !d.indices.bytes &&
        !d.indices.reserved && !d.baseVertex && uint64_t(d.start)+d.count <= UINT32_MAX;
    return (d.indexSize == 2 || d.indexSize == 4) && !d.start && d.indices.resource &&
        !d.indices.reserved && !(d.indices.offset & (d.indexSize-1)) &&
        d.indices.bytes == uint64_t(d.count)*d.indexSize;
}
// Scan only the immutable kernel copy. When staging in cached CPU memory,
// publish these exact validated bytes to the private CP buffer without reading
// the caller's index memory again.
// Strip restart is enabled for indexed Metal triangle strips; restart markers
// do not fetch a vertex and therefore do not contribute to buffer bounds.
template<unsigned IndexSize>
inline bool indexFootprintScan(const Draw &d, const void *snapshot, uint64_t &elements) {
    const auto *bytes = static_cast<const uint8_t *>(snapshot);
    const uint32_t restart = IndexSize == 2 ? 0xffffu : UINT32_MAX;
    for (uint32_t i=0;i<d.count;++i,bytes+=IndexSize) {
        // Fixed-width little-endian reads fold to one ARM64 load, including
        // an unaligned cached snapshot, without a byte loop for every index.
        uint32_t value = uint32_t(bytes[0]) | (uint32_t(bytes[1])<<8);
        if constexpr(IndexSize == 4) value |= (uint32_t(bytes[2])<<16) | (uint32_t(bytes[3])<<24);
        if (d.primitive == 4 && value == restart) continue;
        int64_t vertex = int64_t(value)+d.baseVertex;
        if (vertex < 0 || vertex > UINT32_MAX) return false;
        if (uint64_t(vertex)+1 > elements) elements = uint64_t(vertex)+1;
    }
    return true;
}
inline bool indexFootprint(const Draw &d, const void *snapshot, uint64_t &elements) {
    elements = 0;
    if (!snapshot || !d.indexSize || !validDraw(d)) return false;
    return d.indexSize == 2 ? indexFootprintScan<2>(d,snapshot,elements) : indexFootprintScan<4>(d,snapshot,elements);
}
// Compiler-owned data: embedded AOT programs or immutable kernel copies of
// administrator-registered JIT images. Submission never accepts user PM4.
struct Program {
    uint32_t id, stage, codeWords, instrlen, constlen, fullRegs, halfRegs,
        branchStack, threadSize, subgroupSize, immediateBase, immediateCount,
        driverBase, driverCount, localID, groupID, vertexID, position,
        color, fragCoord, fragMask, bufferMask, writeMask, textureCount, flags,
        instanceID, instanceMask, varyingCount, totalIn, perspective, linear, centerRHW, sharedBytes, threadgroupMask, imageMask, imageReadMask, imageWriteMask, imageUintMask, sampleMask, samplerMask, explicitSamplerMask, cubeSamplerMask, fetchMask, queryMask;
    uint32_t strides[MaxBindings];
    QGPUVarying varyings[QGPU_MAX_VARYINGS];
    uint32_t attributeMask;
    QGPUAttribute attributes[QGPU_MAX_ATTRIBUTES];
    uint32_t implicitSamplerMask,constantSamplerMask,constantImplicitSamplerMask,constantExplicitSamplerMask,constantCubeSamplerMask,framebufferMask;
    uint32_t constantSamplerSlots[QGPU_MAX_CONSTANT_SAMPLERS],constantSamplerLevelSlots[QGPU_MAX_CONSTANT_SAMPLERS];
    QGPUConstantSampler constantSamplers[QGPU_MAX_CONSTANT_SAMPLERS];
    uint32_t colorMask,colorRegisters[QGPU_COLOR_TARGET_COUNT],depthReg;
    uint32_t multisampleMask,depthTextureMask;
    uint32_t sampleFlags,sampleID,sampleMaskIn,sampleMaskOut,
        perspectiveCentroid,linearCentroid,perspectiveSample,linearSample;
    uint32_t textureBindings; // Zero is identity; otherwise eight logical-index nibbles.
    uint32_t constantWindowCount;
    QGPUConstantWindow constantWindows[QGPU_MAX_CONSTANT_WINDOWS];
    const uint32_t *code, *immediates;
    const char *name;
    const Program *msaa;
    const Program *constantWindow=nullptr;
};
inline const Program &rasterProgram(const Program &p,uint32_t samples) {
    return samples>1 && p.msaa?*p.msaa:p;
}
template<class P> inline uint32_t graphicsReadMask(const P &p) {return p.sampleMask|p.fetchMask;}
template<class P> inline unsigned textureBinding(const P &p,unsigned physical) {
    return p.textureBindings?(p.textureBindings>>(physical*4))&15u:physical;
}
template<class P> inline uint32_t graphicsTextureMask(const P &p) {return graphicsReadMask(p)|p.queryMask;}
// Keep launch validation shared by Metal, native admission and PM4. The X
// count remains the legacy buffer footprint; guarded programs use bindingBytes
// and checks of the actual SSBO range in their compiler-generated code.
inline bool imageFootprint(const Dispatch &,const Program &,uint32_t (&)[QGPU_IMAGE_CONSTANT_WORDS]);
inline bool threadgroupFootprint(const Dispatch &d,const Program &p,uint32_t (&constants)[QGPU_THREADGROUP_CONSTANT_WORDS],uint32_t &total) {
    total=p.sharedBytes;
    if (p.stage!=Compute || total>QGPU_MAX_THREADGROUP_BYTES || (total&3) || p.threadgroupMask>255 ||
        bool(p.threadgroupMask)!=bool(p.flags&QGPU_DYNAMIC_THREADGROUP) || (p.threadgroupMask && (total&15))) return false;
    for (unsigned i=0;i<MaxBindings;++i) {
        constants[i*2]=constants[i*2+1]=0;uint32_t length=d.threadgroupLengths[i];
        if (!(p.threadgroupMask&(1u<<i))) {if (length) return false;continue;}
        if (!length || (length&15) || length>QGPU_MAX_THREADGROUP_BYTES) return false;
        total=(total+15)&~15u;if (length>QGPU_MAX_THREADGROUP_BYTES-total) return false;
        constants[i*2]=total-p.sharedBytes;constants[i*2+1]=length;total+=length;
    }
    return true;
}
inline bool dispatchGeometry(const Dispatch &d,uint64_t &elements,unsigned &dimensions) {
    elements=0;dimensions=1;
    bool explicitGrid=d.grid[0] || d.grid[1] || d.grid[2];
    if (d.reserved) return false;
    uint64_t threads=1,invocations=1;
    for (unsigned i=0;i<3;++i) {
        const uint32_t limit=i==2?64u:MaxComputeThreads;
        if (!d.local[i] || d.local[i]>limit || !d.groups[i] || d.groups[i]>MaxComputeGroups ||
            threads>MaxComputeThreads/d.local[i]) return false;
        threads*=d.local[i];
        uint64_t size=uint64_t(d.groups[i])*d.local[i];
        if (explicitGrid) {
            if (!d.grid[i] || d.groups[i]!=d.grid[i]/d.local[i]+uint32_t(d.grid[i]%d.local[i]!=0)) return false;
            size=d.grid[i];
        }
        if (invocations>MaxComputeInvocations/size) return false;
        invocations*=size;
        if (i && size>1) dimensions=i+1;
    }
    elements=explicitGrid?d.grid[0]:uint64_t(d.local[0])*d.groups[0];return true;
}
struct DispatchTile {
    uint32_t groups[3],local[3],baseGroup[3],baseThread[3];
};
inline bool dispatchTiles(const Dispatch &d,DispatchTile (&tiles)[8],uint32_t &count) {
    count=0;uint64_t elements;unsigned dimensions;
    if (!dispatchGeometry(d,elements,dimensions)) return false;
    for (unsigned mask=0;mask<(d.grid[0]?8u:1u);++mask) {
        DispatchTile tile{};bool active=true;
        for (unsigned axis=0;axis<3;++axis) {
            uint32_t full=d.grid[0]?d.grid[axis]/d.local[axis]:d.groups[axis];
            uint32_t tail=d.grid[0]?d.grid[axis]%d.local[axis]:0;
            if (mask&(1u<<axis)) {
                if (!tail) {active=false;break;}
                tile.groups[axis]=1;tile.local[axis]=tail;
                tile.baseGroup[axis]=full;tile.baseThread[axis]=full*d.local[axis];
            } else {
                if (!full) {active=false;break;}
                tile.groups[axis]=full;tile.local[axis]=d.local[axis];
            }
        }
        if (active) tiles[count++]=tile;
    }
    return count!=0;
}
inline void dispatchConstants(const Dispatch &d,const DispatchTile &tile,uint32_t (&out)[QGPU_DISPATCH_CONSTANT_WORDS]) {
    for (unsigned i=0;i<3;++i) {
        out[i]=tile.baseThread[i];out[4+i]=tile.baseGroup[i];
        out[8+i]=d.grid[0]?d.grid[i]:d.groups[i]*d.local[i];out[12+i]=d.groups[i];
    }
    out[3]=d.local[0];out[7]=d.local[1];out[11]=d.local[2];out[15]=0;
}
inline bool dispatchFootprint(const Dispatch &d,const Program &p,uint64_t &elements,unsigned &dimensions) {
    if (p.stage!=Compute || p.sharedBytes>QGPU_MAX_THREADGROUP_BYTES || (p.sharedBytes&3) ||
        !dispatchGeometry(d,elements,dimensions)) return false;
    uint32_t memory[QGPU_THREADGROUP_CONSTANT_WORDS],total;
    if (!threadgroupFootprint(d,p,memory,total)) return false;
    uint32_t images[QGPU_IMAGE_CONSTANT_WORDS];if (!imageFootprint(d,p,images)) return false;
    if (!(p.flags&QGPU_VARIABLE_LOCAL_SIZE) &&
        (d.local[0]!=64 || d.local[1]!=1 || d.local[2]!=1 || d.groups[1]!=1 || d.groups[2]!=1 || (d.grid[0] && d.grid[0]%64))) return false;
    return true;
}
inline bool bindingBytes(const Program &p,unsigned slot,uint64_t elements,uint64_t instances,uint64_t &bytes) {
    bytes=0;if (slot>=MaxBindings) return false;
    if (!(p.bufferMask&(1u<<slot))) return true;
    const uint32_t stride=p.strides[slot];
    if (stride!=2 && stride!=4 && stride!=8 && stride!=16) return false;
    const uint64_t count=(p.flags&QGPU_BOUNDED_BUFFERS)?1:(p.instanceMask&(1u<<slot))?instances:elements;
    if (count>UINT64_MAX/stride) return false;
    bytes=count*stride;return true;
}
inline bool sameSemantic(const QGPUVarying &a, const QGPUVarying &b) {
    for (unsigned i=0;i<4;++i) if (a.semantic[i]!=b.semantic[i]) return false;
    return true;
}
template<class P> inline bool validInterface(const P &p) {
    if(p.branchStack>MaxBranchStack ||
        ((p.flags&QGPU_THREADGROUP_BARRIER) && p.branchStack>MaxBarrierBranchStack))return false;
    if(p.constantWindowCount>QGPU_MAX_CONSTANT_WINDOWS)return false;
    if((p.flags&QGPU_FRAMEBUFFER_TILE_ORIGIN) && (p.stage!=Fragment || !p.framebufferMask ||
        p.constlen*4<QGPU_FRAMEBUFFER_TILE_MEMORY_CONSTANT_WORDS ||
        (p.immediateCount && p.immediateBase<QGPU_FRAMEBUFFER_TILE_MEMORY_CONSTANT_WORDS) ||
        (p.driverCount && p.driverBase<QGPU_FRAMEBUFFER_TILE_MEMORY_CONSTANT_WORDS)))return false;
    if((p.flags&QGPU_CONSTANT_WINDOWS_REQUIRED) &&
        (!p.constantWindowCount || p.stage!=Fragment || p.writeMask))return false;
    if(p.constantWindowCount && (p.stage!=Fragment ||
        (p.flags&(QGPU_BOUNDED_BUFFERS|QGPU_FRAGMENT_BUFFER_BANK|QGPU_BUFFER_SIZE_CONSTANTS))!=
            (QGPU_BOUNDED_BUFFERS|QGPU_FRAGMENT_BUFFER_BANK|QGPU_BUFFER_SIZE_CONSTANTS) ||
        p.constlen*4<qgpu_constant_memory_words(p.constantWindowCount)))return false;
    if(p.constantWindowCount) {
        const uint32_t prefix=qgpu_constant_memory_words(p.constantWindowCount);
        if((p.immediateCount && p.immediateBase<prefix) || (p.driverCount && p.driverBase<prefix))return false;
    }
    for(unsigned i=0;i<QGPU_MAX_CONSTANT_WINDOWS;++i) {
        const auto &window=p.constantWindows[i];
        if(i>=p.constantWindowCount) {if(window.buffer || window.byteOffset)return false;continue;}
        if(window.buffer>=QGPU_STAGE_BUFFER_COUNT || !(p.bufferMask&(1u<<window.buffer)) ||
            (p.writeMask&(1u<<window.buffer)) ||
            (window.byteOffset&15) || window.byteOffset>UINT32_MAX-QGPU_CONSTANT_WINDOW_BYTES ||
            (i && (p.constantWindows[i-1].buffer>window.buffer ||
                (p.constantWindows[i-1].buffer==window.buffer &&
                 (uint64_t)p.constantWindows[i-1].byteOffset+QGPU_CONSTANT_WINDOW_BYTES>window.byteOffset))))return false;
    }
    if (p.stage==Compute && p.textureBindings) return false;
    uint32_t logicalTextures=0;
    for(unsigned slot=0;slot<QGPU_STAGE_TEXTURE_COUNT;++slot)if((p.sampleMask|p.fetchMask|p.queryMask)&(1u<<slot)) {
        const unsigned logical=textureBinding(p,slot);
        if(logicalTextures&(1u<<logical))return false;
        logicalTextures|=1u<<logical;
    }
    if ((p.sampleFlags&~QGPU_SAMPLE_FLAGS) || (p.stage!=Fragment && p.sampleFlags) ||
        ((p.sampleFlags&QGPU_SAMPLE_EXECUTION) && !(p.sampleFlags&QGPU_SAMPLE_ID))) return false;
    const uint32_t sampleRegs[]={p.sampleID,p.sampleMaskIn,p.sampleMaskOut,
        p.perspectiveCentroid,p.linearCentroid,p.perspectiveSample,p.linearSample};
    for (unsigned i=0;i<7;++i) {
        bool active=p.sampleFlags&(2u<<i);
        unsigned limit=(i? p.fullRegs:p.halfRegs)*4,span=i>=3?2:1;
        if (active ? (p.stage!=Fragment || sampleRegs[i]>=252 || sampleRegs[i]+span>limit) : bool(sampleRegs[i])) return false;
    }
    if(p.depthTextureMask>255 || (p.depthTextureMask&(~(p.imageMask|p.sampleMask|p.fetchMask|p.queryMask)|p.imageWriteMask|((p.flags&QGPU_CUBE_TEXTURE_MASK)>>QGPU_CUBE_TEXTURE_SHIFT))))return false;
    uint32_t descriptors=p.sampleMask|(p.fetchMask<<QGPU_FETCH_TEXTURE_BASE);
    if(p.framebufferMask>255 || (p.framebufferMask && (p.stage!=Fragment ||
        (p.sampleFlags&(QGPU_SAMPLE_EXECUTION|QGPU_SAMPLE_ID))!=(QGPU_SAMPLE_EXECUTION|QGPU_SAMPLE_ID))))return false;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(p.framebufferMask&(1u<<i)) {
        uint32_t slot=qgpu_framebuffer_slot(p.sampleMask,p.fetchMask,p.framebufferMask,i);
        if(slot==UINT32_MAX)return false;
        descriptors|=1u<<slot;
    }
    if(p.multisampleMask>255 || (p.multisampleMask&p.sampleMask) ||
        (p.stage==Compute?(p.multisampleMask&(~p.imageMask|p.imageWriteMask)):
            (p.multisampleMask&~(p.fetchMask|p.queryMask))))return false;
    if(p.multisampleMask&((p.flags&QGPU_CUBE_TEXTURE_MASK)>>QGPU_CUBE_TEXTURE_SHIFT))return false;
    if(p.colorMask>255 || (p.stage!=Fragment && (p.colorMask || (p.flags&QGPU_FRAGMENT_FLAGS))))return false;
    if(p.flags&QGPU_FRAGMENT_DEPTH) {
        if(p.depthReg>=252 || p.depthReg>=p.fullRegs*4)return false;
    } else if(p.depthReg)return false;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i) {
        uint32_t reg=p.colorRegisters[i];
        if(p.stage!=Fragment) {if(reg)return false;continue;}
        if(!(p.colorMask&(1u<<i))) {if(reg!=252)return false;continue;}
        if(reg>=252 || reg+4>p.fullRegs*4)return false;
    }
    if(p.stage==Fragment && p.color!=p.colorRegisters[0])return false;
    if (((p.implicitSamplerMask|p.explicitSamplerMask)!=p.samplerMask) || p.constantSamplerMask>65535 ||
        (p.constantImplicitSamplerMask|p.constantExplicitSamplerMask)!=p.constantSamplerMask ||
        (p.constantCubeSamplerMask&~p.constantSamplerMask)) return false;
    uint32_t occupied=p.implicitSamplerMask|(p.explicitSamplerMask<<QGPU_STAGE_SAMPLER_COUNT);
    for(unsigned i=0;i<QGPU_MAX_CONSTANT_SAMPLERS;++i) {
        const auto &s=p.constantSamplers[i];uint32_t bit=1u<<i;
        if (!(p.constantSamplerMask&bit)) {
            if(s.filters || s.minLOD || s.maxLOD || s.addressModes || s.maxAnisotropy || s.borderColor || s.normalized) return false;
        } else if(!validSampler(s) || ((p.constantCubeSamplerMask&bit) && !s.normalized)) return false;
        const uint32_t slots[]={p.constantSamplerSlots[i],p.constantSamplerLevelSlots[i]};
        const uint32_t masks[]={p.constantImplicitSamplerMask,p.constantExplicitSamplerMask};
        for(unsigned n=0;n<2;++n) {
            if(!(masks[n]&bit)) {if(slots[n])return false;continue;}
            if(slots[n]>=QGPU_HARDWARE_SAMPLER_COUNT || (occupied&(1u<<slots[n])))return false;
            occupied|=1u<<slots[n];
        }
    }
    if (p.sampleMask>255 || p.fetchMask>255 || p.queryMask>255 || (p.fetchMask&~p.queryMask) ||
        (p.stage==Compute && (p.fetchMask || p.queryMask)) || p.samplerMask>255 || (p.explicitSamplerMask&~p.samplerMask) || (p.cubeSamplerMask&~p.samplerMask) ||
        bool(((p.flags&QGPU_CUBE_TEXTURE_MASK)>>QGPU_CUBE_TEXTURE_SHIFT)&p.sampleMask)!=bool(p.cubeSamplerMask|p.constantCubeSamplerMask) || (p.sampleMask&p.imageWriteMask) ||
        bool(p.sampleMask)!=bool(p.samplerMask|p.constantSamplerMask) ||
        p.textureCount!=(descriptors?32u-__builtin_clz(descriptors):0u)) return false;
    if (p.attributeMask>0xffff || (p.stage!=Vertex && p.attributeMask)) return false;
    for (unsigned i=0;i<QGPU_MAX_ATTRIBUTES;++i) {
        const auto &a=p.attributes[i];
        if (!(p.attributeMask&(1u<<i))) { if (a.components || a.type || a.regid || a.mask) return false;continue; }
        if (!a.components || a.components>4 || a.type>2 || !a.mask || (a.mask&~((1u<<a.components)-1)) ||
            a.regid>=252 || a.regid+(32-__builtin_clz(a.mask))>p.fullRegs*4) return false;
    }
    if (p.stage>Fragment || !p.fullRegs || p.fullRegs>63 || p.varyingCount>QGPU_MAX_VARYINGS || p.totalIn>124 ||
        p.sharedBytes>QGPU_MAX_THREADGROUP_BYTES || (p.sharedBytes&3) || (p.stage!=Compute && (p.sharedBytes || p.threadgroupMask)) ||
        p.threadgroupMask>255 || bool(p.threadgroupMask)!=bool(p.flags&QGPU_DYNAMIC_THREADGROUP) || p.imageMask>255 ||
        ((p.imageReadMask|p.imageWriteMask|p.imageUintMask)&~p.imageMask) ||
        (p.imageUintMask&(p.sampleMask|p.multisampleMask|p.depthTextureMask)) ||
        (p.stage!=Compute && (p.imageMask || p.imageReadMask || p.imageWriteMask || p.imageUintMask)) ||
        (p.imageMask && !(p.flags&QGPU_BOUNDED_BUFFERS)) || ((p.flags&QGPU_DISPATCH_GEOMETRY) && p.stage!=Compute) ||
        (((p.flags&QGPU_ARRAY_TEXTURE_MASK)>>QGPU_ARRAY_TEXTURE_SHIFT)&~(p.imageMask|p.sampleMask|p.fetchMask|p.queryMask)) ||
        (((p.flags&QGPU_CUBE_TEXTURE_MASK)>>QGPU_CUBE_TEXTURE_SHIFT)&~(p.imageMask|p.sampleMask|p.fetchMask|p.queryMask)) ||
        (p.threadgroupMask && (p.sharedBytes&15)) ||
        (p.stage==Compute && p.varyingCount) || (p.stage!=Fragment && p.totalIn)) return false;
    const uint32_t regs[]={p.perspective,p.linear,p.centerRHW};
    for (auto reg : regs)
        if (reg!=252 && (p.stage!=Fragment || reg>=p.fullRegs*4 || reg>251)) return false;
    if ((p.perspective!=252 && p.perspective+1>=p.fullRegs*4) ||
        (p.linear!=252 && p.linear+1>=p.fullRegs*4)) return false;
    uint32_t used[4]={}, components=0;
    for (unsigned i=0;i<QGPU_MAX_VARYINGS;++i) {
        const auto &v=p.varyings[i];
        if (i>=p.varyingCount) {
            for (auto word:v.semantic) if (word) return false;
            if (v.components || v.mode || v.regid || v.mask || v.inloc || v.reserved) return false;
            continue;
        }
        if (!(v.semantic[0]|v.semantic[1]|v.semantic[2]|v.semantic[3]) || !v.components || v.components>4 ||
            v.mode>LinearSample || v.reserved || (v.mask & ~((1u<<v.components)-1))) return false;
        for (unsigned j=0;j<i;++j) if (sameSemantic(v,p.varyings[j])) return false;
        if (p.stage==Vertex) {
            if (v.inloc || (v.mask ? (v.regid>=252 || v.regid+v.components>p.fullRegs*4 ||
                v.mask!=((1u<<v.components)-1)) : v.regid!=252)) return false;
        } else if (p.stage==Fragment) {
            if (v.regid!=252) return false;
            const unsigned count=__builtin_popcount(v.mask);
            if (!count) { if (v.inloc) return false; continue; }
            if ((v.mode==Perspective && p.perspective==252) || (v.mode==Linear && p.linear==252)) return false;
            if (v.mode>=PerspectiveCentroid) {
                if (p.sampleFlags&QGPU_SAMPLE_MSAA) {
                    if (!(p.sampleFlags&(QGPU_CENTROID_PERSPECTIVE<<(v.mode-PerspectiveCentroid)))) return false;
                } else if (((v.mode-PerspectiveCentroid)&1)?p.linear==252:p.perspective==252) return false;
            }
            if (v.inloc>=p.totalIn || count>p.totalIn-v.inloc) return false;
            for (unsigned n=0;n<count;++n) {
                unsigned loc=v.inloc+n;
                if (used[loc/32]&(1u<<(loc%32))) return false;
                used[loc/32]|=1u<<(loc%32);
            }
            components+=count;
        } else return false;
    }
    return p.stage!=Fragment || components==p.totalIn;
}
// Semantic formats use public Metal numbers, never user-provided VFD bits.
struct VertexFormatInfo { uint32_t bytes=0,alignment=0,components=0,type=0,hardware=0,swap=0; };
inline VertexFormatInfo vertexFormat(uint32_t f) {
    VertexFormatInfo v;
    if (f>=1 && f<=27) {
        unsigned group=(f-1)/3;v.components=(f-1)%3+2;
        static const uint8_t sizes[]={1,1,1,1,2,2,2,2,2};
        static const uint8_t types[]={2,1,0,0,2,1,0,0,0};
        static const uint8_t formats[9][3]={{0x11,0x23,0x33},{0x12,0x24,0x34},
            {0x0f,0x21,0x30},{0x10,0x22,0x32},{0x46,0x5b,0x63},{0x47,0x5c,0x64},
            {0x43,0x58,0x60},{0x44,0x59,0x61},{0x45,0x5a,0x62}};
        v.alignment=sizes[group];v.bytes=v.components*v.alignment;v.type=types[group];v.hardware=formats[group][v.components-2];
    } else if (f>=28 && f<=39) {
        unsigned group=(f-28)/4;v.components=(f-28)%4+1;v.alignment=4;v.bytes=v.components*4;v.type=group;
        static const uint8_t formats[3][4]={{0x4a,0x67,0x74,0x82},{0x4c,0x69,0x73,0x84},{0x4b,0x68,0x72,0x83}};
        v.hardware=formats[group][v.components-1];
    } else if (f==40 || f==41 || f==42) {
        v={4,f==42?1u:4u,4,0,f==40?0x39u:f==41?0x36u:0x30u,f==42?1u:0u};
    } else if (f>=45 && f<=53) {
        static const uint8_t formats[]={5,6,3,4,0x18,0x19,0x15,0x16,0x17};
        static const uint8_t types[]={2,1,0,0,2,1,0,0,0};
        v.alignment=f<49?1:2;v.bytes=v.alignment;v.components=1;v.type=types[f-45];v.hardware=formats[f-45];
    } else if (f==54) v={4,4,3,0,0x42,0};
    return v;
}
// Validate layouts independently of actual draw size at pipeline creation.
inline bool vertexLayout(const Program &p,const Draw &d,uint32_t &bindings) {
    bindings=0;
    for (unsigned i=0;i<QGPU_MAX_ATTRIBUTES;++i) {
        const auto &a=d.attributes[i];
        if (!(p.attributeMask&(1u<<i))) { if (a.format || a.offset || a.buffer || a.reserved) return false;continue; }
        auto f=vertexFormat(a.format);
        if (!f.bytes || f.type!=p.attributes[i].type || a.buffer>=MaxBindings || a.reserved ||
            a.offset>2047 || a.offset%f.alignment) return false;
        const auto &l=d.layouts[a.buffer];
        if (l.reserved || l.stride>2048 || l.stride%f.alignment || l.step>2 ||
            (l.step==1 && l.rate!=1) || (l.step==2 && !l.rate)) return false;
        bindings|=1u<<a.buffer;
    }
    for (unsigned i=0;i<MaxBindings;++i) if (!(bindings&(1u<<i))) {
        const auto &l=d.layouts[i];if (l.stride || l.step || l.rate || l.reserved) return false;
    }
    return true;
}
inline bool vertexFootprint(const Program &p,const Draw &d,uint64_t elements,uint32_t &bindings) {
    if (!vertexLayout(p,d,bindings)) return false;
    for (unsigned i=0;i<QGPU_MAX_ATTRIBUTES;++i) if (p.attributeMask&(1u<<i)) {
        const auto &a=d.attributes[i];const auto &l=d.layouts[a.buffer];const auto &b=d.buffers[a.buffer];auto f=vertexFormat(a.format);
        // The instance start is applied to per-instance fetches in addition to the divisor.
        uint64_t index=l.step==0 ? 0 : l.step==1 ? (elements?elements-1:0) : uint64_t(d.baseInstance)+(d.instances-1)/l.rate;
        uint64_t last=uint64_t(a.offset)+index*(l.step==0?0:l.stride);
        if (!b.resource || b.reserved || b.offset%f.alignment || last>b.bytes || f.bytes>b.bytes-last) return false;
    }
    return true;
}
// FS packed component locations drive VPC placement. Position is appended as
// in Mesa's A5xx linker; it is not part of the FS non-position scalar count.
struct Linkage {
    struct Output { uint32_t regid,mask,loc; } outputs[QGPU_MAX_VARYINGS+1];
    uint32_t count=0,stride=0,mask[4]={},interpolation[8]={};
};
inline bool link(const Program &vs,const Program &fs,Linkage &out) {
    out={};
    if (vs.stage!=Vertex || fs.stage!=Fragment || !validInterface(vs) || !validInterface(fs) ||
        vs.position>=252 || vs.position+4>vs.fullRegs*4) return false;
    for (unsigned i=0;i<fs.varyingCount;++i) {
        const auto &input=fs.varyings[i];
        if (!input.mask) continue; // dead inputs do not consume vertex outputs
        const QGPUVarying *output=nullptr;
        for (unsigned j=0;j<vs.varyingCount;++j) if (sameSemantic(input,vs.varyings[j])) { output=&vs.varyings[j]; break; }
        if (!output || output->components!=input.components) return false;
        if ((input.mask&~output->mask) || output->regid==252) return false;
        unsigned span=32-__builtin_clz(input.mask),end=input.inloc+span;
        if (end+4>127 || out.count>=QGPU_MAX_VARYINGS-1) return false;
        out.outputs[out.count++]={output->regid,input.mask,input.inloc};
        if (end>out.stride) out.stride=end;
        for (unsigned c=0;c<span;++c) out.mask[(input.inloc+c)/32]|=1u<<((input.inloc+c)%32);
        if (input.mode==Flat) for (unsigned c=0;c<unsigned(__builtin_popcount(input.mask));++c) {
            unsigned loc=input.inloc+c;out.interpolation[loc/16]|=1u<<((loc%16)*2);
        }
    }
    out.outputs[out.count++]={vs.position,15,out.stride};out.stride+=4;
    return true;
}
struct TextureLayout {
    uint32_t levels=0,bytes=0,accessBytes=0,layerStride=0;
    struct Level {uint32_t offset,width,height,pitch,bytes;} level[MaxMipLevels]={};
};
inline uint32_t mipLevelCount(uint32_t width,uint32_t height) {
    uint32_t size=width>height?width:height;return size?32-__builtin_clz(size):0;
}
// A5xx linear color/D32/S8 storage follows pinned Mesa fdl5_layout/fdl_pitch:
// minify the *aligned base pitch*, reserve 32 rows in the last mip, and
// page-align the complete layer. No user-supplied per-level address table.
// Copyright (c) 2018 Rob Clark; Copyright (c) 2018-2019 Google, Inc. (MIT).
inline bool linearTextureLayout(const Texture &t,TextureLayout &out,bool allocate=true) {
    out={};uint32_t levels=t.mipLevels?t.mipLevels:1;
    const bool plane=t.format==Depth32 || t.format==Stencil8;
    const uint32_t cpp=formatBytes(t.format),alignment=64u*cpp;
    if (!t.resource || !t.width || !t.height || t.width > MaxTextureDimension || t.height > MaxTextureDimension ||
        (t.pitch & 63) || t.pitch > MaxTextureRowBytes || t.pitch < uint64_t(formatBlocks(t.width,t.format)) * cpp * textureSamples(t) || (t.offset & 63) ||
        (compressedFormat(t.format) && (t.pitch&(alignment-1))) ||
        (!colorFormat(t.format) && !plane) || !validSamples(t.samples) || ((sampledOnlyFormat(t.format) || unsignedIntegerFormat(t.format)) && t.samples) || (t.samples && (levels!=1 || t.cube || (t.pitch%(alignment*textureSamples(t))))) || !validSwizzle(t.swizzle) || (plane && (t.swizzle || t.cube)) || levels>MaxMipLevels || t.layers>2048 ||
        t.cube>1 || (t.cube && t.width!=t.height) || (!t.layers && !t.cube && t.layerStride) || (t.layerStride&4095) ||
        levels>mipLevelCount(t.width,t.height) || (levels>1 && (t.pitch&(alignment-1)))) return false;
    uint64_t bytes=0;
    for (uint32_t i=0;i<levels;++i) {
        uint32_t width=t.width>>i,height=t.height>>i,pitch=t.pitch>>i;
        if (!width) width=1;
        if (!height) height=1;
        if (!pitch) pitch=1;
        if (levels>1) pitch=(pitch+alignment-1)&~(alignment-1);
        uint32_t blockRows=formatBlocks(height,t.format);
        uint32_t rows=(levels>1 || compressedFormat(t.format)) && i+1==levels?(blockRows+31)&~31u:blockRows;
        uint64_t size=uint64_t(pitch)*rows;
        uint64_t access=uint64_t(pitch)*blockRows;
        if (access>MaxBufferBytes || bytes>MaxBufferBytes-access ||
            (allocate && (size>MaxBufferBytes || bytes>MaxBufferBytes-size))) return false;
        out.accessBytes=uint32_t(bytes+access);
        out.level[i]={uint32_t(bytes),width,height,pitch,uint32_t(size)};bytes+=size;
    }
    if (levels>1) bytes=(bytes+4095)&~uint64_t(4095);
    uint64_t stride=t.layerStride?t.layerStride:(bytes+4095)&~uint64_t(4095);
    if (stride<out.accessBytes || (allocate && stride<bytes) || stride>MaxBufferBytes) return false;
    uint32_t slices=textureSlices(t);
    if (slices>1 || (plane && t.layers)) {
        uint64_t access=uint64_t(slices-1)*stride+out.accessBytes;
        uint64_t allocation=uint64_t(slices)*stride;
        if (access>MaxBufferBytes || (allocate && allocation>MaxBufferBytes)) return false;
        out.accessBytes=uint32_t(access);bytes=allocation;
    }
    if (bytes>UINT32_MAX || (allocate && bytes>MaxBufferBytes)) return false;
    out.levels=levels;out.bytes=uint32_t(bytes);out.layerStride=uint32_t(stride);return true;
}
inline bool textureLayout(const Texture &t,TextureLayout &out,bool allocate=true) {
    if(!colorFormat(t.format)){out={};return false;}return linearTextureLayout(t,out,allocate);
}
inline bool planeLayout(const Texture &t,uint32_t format,TextureLayout &out,bool allocate=true) {
    if((format!=Depth32 && format!=Stencil8) || t.format!=format){out={};return false;}
    return linearTextureLayout(t,out,allocate);
}
inline bool textureStorageLayout(const Texture &t,TextureLayout &out,bool allocate=true) {
    return linearTextureLayout(t,out,allocate);
}
inline bool textureSize(const Texture &t,uint64_t &bytes) {
    // Padding belongs to the owning allocation. A 2D view can end before
    // the parent's padded last mip; shaders cannot address that padding.
    TextureLayout layout;bytes=0;if (!textureLayout(t,layout,false)) return false;bytes=layout.accessBytes;return true;
}
inline bool textureLevel(const Texture &t,uint32_t level,Texture &out) {
    TextureLayout layout;out={};if (!textureStorageLayout(t,layout,false) || level>=layout.levels) return false;
    const auto &l=layout.level[level];if (uint64_t(t.offset)+l.offset>UINT32_MAX) return false;
    out={t.resource,t.offset+l.offset,l.width,l.height,l.pitch,t.format,0,t.swizzle,t.layers,(t.layers || t.cube)?layout.layerStride:0,t.cube,t.samples};return true;
}
inline bool textureSlice(const Texture &t,uint32_t slice,Texture &out) {
    TextureLayout layout;out={};
    if (!textureStorageLayout(t,layout,false) || slice>=textureSlices(t)) return false;
    uint64_t offset=uint64_t(t.offset)+uint64_t(slice)*layout.layerStride;
    if (offset>UINT32_MAX) return false;
    out=t;out.offset=uint32_t(offset);out.layers=out.layerStride=out.cube=0;return true;
}
inline bool textureSubview(const Texture &source,uint32_t first,uint32_t count,uint32_t format,Texture &out,uint32_t swizzle=0) {
    out={};TextureLayout parent;
    if (!(colorFormat(source.format)?(colorFormat(format) && formatBytes(source.format)==formatBytes(format)):(format==source.format && !swizzle)) || !count || !textureStorageLayout(source,parent,false) ||
        first>=parent.levels || count>parent.levels-first) return false;
    Texture selected;if (!textureLevel(source,first,selected)) return false;
    selected.format=format;selected.mipLevels=count>1?count:0;selected.swizzle=composeSwizzle(source.swizzle,swizzle);
    TextureLayout child;
    if (!textureStorageLayout(selected,child,false) || child.accessBytes>uint64_t(parent.accessBytes)-parent.level[first].offset) return false;
    out=selected;return true;
}
inline bool textureView(const Texture &source,uint32_t first,uint32_t count,uint32_t slice,uint32_t slices,bool array,
    uint32_t format,Texture &out,uint32_t swizzle=0,bool cube=false) {
    TextureLayout parent;out={};uint32_t layers=textureSlices(source);
    if (!slices || slice>=layers || slices>layers-slice || (cube ? ((slices%6) || (!array && slices!=6)) : (!array && slices!=1)) ||
        !textureStorageLayout(source,parent,false) || (source.samples && cube)) return false;
    Texture selected;if (!textureSubview(source,first,count,format,selected,swizzle)) return false;
    uint64_t offset=uint64_t(selected.offset)+uint64_t(slice)*parent.layerStride;
    if (offset>UINT32_MAX) return false;
    selected.offset=uint32_t(offset);selected.cube=cube;selected.layers=array?(cube?slices/6:slices):0;selected.layerStride=(array || cube)?parent.layerStride:0;
    TextureLayout child;if (!textureStorageLayout(selected,child,false) || uint64_t(selected.offset)-source.offset+child.accessBytes>parent.accessBytes) return false;
    out=selected;return true;
}
inline bool emptyTexture(const Texture &t) { return !t.resource && !t.offset && !t.width && !t.height && !t.pitch && !t.format && !t.mipLevels && !t.swizzle && !t.layers && !t.layerStride && !t.cube && !t.samples; }
inline bool shaderTextureSize(const Program &p,unsigned slot,const Texture &t,uint64_t &bytes) {
    // Integer storage is bit-exact SSBO access. It must never reach a float
    // image helper or the unimplemented integer hardware sampler path.
    if(bool(p.imageUintMask&(1u<<slot))!=unsignedIntegerFormat(t.format) ||
        (unsignedIntegerFormat(t.format) && (p.stage!=Compute || t.samples ||
            !(p.imageMask&(1u<<slot)) || (p.sampleMask&(1u<<slot)))))return false;
    if(bool(p.depthTextureMask&(1u<<slot))!=(t.format==Depth32))return false;
    if(t.format==Depth32){TextureLayout layout;if(!planeLayout(t,Depth32,layout,false))return false;bytes=layout.accessBytes;return true;}
    return textureSize(t,bytes);
}
inline bool graphicsTextures(const Program &p,const Texture *textures,const Sampler *samplers) {
    if ((p.stage!=Vertex && p.stage!=Fragment) || !validInterface(p)) return false;
    for (unsigned i=0;i<QGPU_STAGE_TEXTURE_COUNT;++i) {
        uint64_t bytes=0;
        if (graphicsTextureMask(p)&(1u<<i)) {
            if (!shaderTextureSize(p,i,textures[i],bytes) || bool(textures[i].layers)!=bool(p.flags&(1u<<(QGPU_ARRAY_TEXTURE_SHIFT+i))) ||
                bool(textures[i].cube)!=bool(p.flags&(1u<<(QGPU_CUBE_TEXTURE_SHIFT+i))) ||
                bool(textures[i].samples)!=bool(p.multisampleMask&(1u<<i))) return false;
        } else if (!emptyTexture(textures[i])) return false;
    }
    for (unsigned i=0;i<QGPU_STAGE_SAMPLER_COUNT;++i) {
        if (p.samplerMask&(1u<<i)) {if (!validSampler(samplers[i]) || ((p.cubeSamplerMask&(1u<<i)) && !samplers[i].normalized)) return false;}
        else if (!emptySampler(samplers[i])) return false;
    }
    return true;
}
template<unsigned N> inline void graphicsConstants(const Program &p,const Texture *textures,const Sampler *samplers,
                              uint32_t (&out)[N],const Draw *draw=nullptr) {
    static_assert(N>=QGPU_FRAMEBUFFER_MEMORY_CONSTANT_WORDS,"graphics metadata capacity");
    for(auto &word:out)word=0;
    for(unsigned i=0;i<QGPU_STAGE_SAMPLER_COUNT;++i)
        if(p.samplerMask&(1u<<i))out[i]=samplerInfoFlags(samplers[i]);
    for(unsigned i=0;i<QGPU_STAGE_TEXTURE_COUNT;++i)if(p.queryMask&(1u<<i)) {
        const auto &t=textures[i];auto *info=out+QGPU_GRAPHICS_TEXTURE_CONSTANT_BASE+i*4;
        info[0]=t.width;info[1]=t.height;info[2]=t.layers?t.layers:1;
        info[3]=t.format|((t.mipLevels?t.mipLevels:1)<<8)|(t.swizzle<<16)|((t.samples==4?2u:t.samples==2?1u:0u)<<28);
    }
    if(draw)for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(p.framebufferMask&(1u<<i)) {
        const auto &t=colorTarget(*draw,i);auto *info=out+QGPU_FRAMEBUFFER_CONSTANT_BASE+i*4;
        info[0]=t.width;info[1]=t.height;info[2]=textureSamples(t);info[3]=t.format;
    }
}
inline bool planeSize(const Texture &t,uint32_t format,uint64_t &bytes) {
    TextureLayout layout;bytes=0;if(!planeLayout(t,format,layout,false))return false;bytes=layout.accessBytes;return true;
}
inline bool textureBytes(const Texture &t,uint64_t &bytes) {
    return colorFormat(t.format)?textureSize(t,bytes):planeSize(t,t.format,bytes);
}
template<class P> inline uint32_t memoryConstantWords(const P &p) {
    if(p.constantWindowCount)return qgpu_constant_memory_words(p.constantWindowCount);
    if(p.flags&QGPU_BUFFER_SIZE_CONSTANTS)return QGPU_MEMORY_CONSTANT_WORDS;
    if(p.flags&QGPU_FRAMEBUFFER_TILE_ORIGIN)return QGPU_FRAMEBUFFER_TILE_MEMORY_CONSTANT_WORDS;
    if(p.stage!=Compute)return p.framebufferMask?QGPU_FRAMEBUFFER_MEMORY_CONSTANT_WORDS:p.queryMask?QGPU_GRAPHICS_MEMORY_CONSTANT_WORDS:p.samplerMask?QGPU_SAMPLER_CONSTANT_WORDS:0;
    return p.flags&(QGPU_ARRAY_TEXTURE_MASK|QGPU_CUBE_TEXTURE_MASK)?QGPU_ARRAY_MEMORY_CONSTANT_WORDS:p.flags&QGPU_DISPATCH_GEOMETRY?QGPU_DISPATCH_MEMORY_CONSTANT_WORDS:p.samplerMask?QGPU_SAMPLER_MEMORY_CONSTANT_WORDS:p.imageMask?QGPU_IMAGE_MEMORY_CONSTANT_WORDS:p.threadgroupMask?QGPU_THREADGROUP_CONSTANT_WORDS:0;
}
inline bool imageFootprint(const Dispatch &d,const Program &p,uint32_t (&constants)[QGPU_IMAGE_CONSTANT_WORDS]) {
    if (p.stage!=Compute || p.imageMask>255 || p.sampleMask>255 || p.samplerMask>255 ||
        bool(p.sampleMask)!=bool(p.samplerMask|p.constantSamplerMask) || (p.sampleMask&p.imageWriteMask) ||
        ((p.imageReadMask|p.imageWriteMask|p.imageUintMask)&~p.imageMask) ||
        (p.imageUintMask&(p.sampleMask|p.multisampleMask|p.depthTextureMask))) return false;
    for (unsigned i=0;i<QGPU_COMPUTE_IMAGE_COUNT;++i) {
        const auto &t=d.images[i];uint64_t bytes=0;
        for(unsigned c=0;c<4;++c)constants[i*4+c]=0;
        if (p.samplerMask&(1u<<i)) {if (!validSampler(d.samplers[i]) || ((p.cubeSamplerMask&(1u<<i)) && !d.samplers[i].normalized)) return false;}
        else if (!emptySampler(d.samplers[i])) return false;
        if (!((p.imageMask|p.sampleMask)&(1u<<i))) {if (!emptyTexture(t)) return false;continue;}
        if (!shaderTextureSize(p,i,t,bytes) || bool(t.layers)!=bool(p.flags&(1u<<(QGPU_ARRAY_TEXTURE_SHIFT+i))) ||
            bool(t.cube)!=bool(p.flags&(1u<<(QGPU_CUBE_TEXTURE_SHIFT+i))) ||
            bool(t.samples)!=bool(p.multisampleMask&(1u<<i))) return false;
        if (!(p.imageMask&(1u<<i))) continue;
        // These formats use native A5xx texture conversion. The raw SSBO
        // image encoder still implements only the existing formats.
        if (sampledOnlyFormat(t.format)) return false;
        constants[i*4]=t.width;constants[i*4+1]=t.height;constants[i*4+2]=t.pitch;
        constants[i*4+3]=t.format|((t.mipLevels?t.mipLevels:1)<<8)|(t.swizzle<<16)|((t.samples==4?2u:t.samples==2?1u:0u)<<28);
    }
    return true;
}
inline bool overlap(const Texture &a,const Texture &b) {
    uint64_t ab=0,bb=0;if (!textureBytes(a,ab) || !textureBytes(b,bb)) return false;
    if(!a.resource || a.resource!=b.resource)return false;
    Texture as=a,bs=b;as.layers=as.layerStride=as.cube=bs.layers=bs.layerStride=bs.cube=0;
    if(!textureBytes(as,ab) || !textureBytes(bs,bb))return false;
    TextureLayout al,bl;uint64_t astride=0,bstride=0;
    if(a.layers || a.cube){if(!textureStorageLayout(a,al,false))return false;astride=al.layerStride;}
    if(b.layers || b.cube){if(!textureStorageLayout(b,bl,false))return false;bstride=bl.layerStride;}
    unsigned i=0,j=0,na=textureSlices(a),nb=textureSlices(b);
    while(i<na && j<nb) {
        uint64_t x=uint64_t(a.offset)+i*astride,y=uint64_t(b.offset)+j*bstride;
        if(x<y+bb && y<x+ab)return true;
        if(x+ab<=y)++i;else ++j;
    }
    return false;
}
// Internal resolve uniforms use the ordinary bounded buffer ABI. Pixel data
// remains D32/S8; it is never re-labelled as a public color image descriptor.
struct DepthStencilResolveParameters {
    uint32_t width,height,sourcePitch,destinationPitch,samples,depthFilter,depthPitch,stencilFilter;
};
static_assert(sizeof(DepthStencilResolveParameters)==32,"resolve constant layout");
struct DepthStencilResolvePlan {
    DepthStencilResolveParameters parameters{};
    Binding buffers[3]{};
    uint32_t width=0,height=0;
};
inline bool depthStencilResolvePlan(const Texture &source,const Texture &destination,const Texture *depth,
    uint32_t depthFilter,uint32_t stencilFilter,DepthStencilResolvePlan &out) {
    out={};uint64_t sourceBytes=0,destinationBytes=0,depthBytes=0;
    bool stencil=source.format==Stencil8;
    if (depthFilter>2 || stencilFilter>1 || (!stencil && stencilFilter) ||
        (source.samples!=2 && source.samples!=4) || destination.samples || source.layers || destination.layers ||
        source.mipLevels>1 || destination.mipLevels>1 || source.format!=destination.format ||
        source.width!=destination.width || source.height!=destination.height ||
        !planeSize(source,source.format,sourceBytes) || !planeSize(destination,destination.format,destinationBytes) ||
        overlap(source,destination)) return false;
    out.parameters={source.width,source.height,source.pitch,destination.pitch,source.samples,depthFilter,0,stencilFilter};
    out.buffers[0]={source.resource,source.offset,uint32_t(sourceBytes),0};
    out.buffers[1]={destination.resource,destination.offset,uint32_t(destinationBytes),0};
    if (stencil) {
        // Sample0 never reads depth, but the static shader interface has a
        // read-only depth slot. Reuse its source as an inactive read binding.
        out.buffers[2]=out.buffers[0];
        if (stencilFilter) {
            if (!depth || depth->layers || depth->mipLevels>1 || !planeSize(*depth,Depth32,depthBytes) || depth->samples!=source.samples ||
                depth->width!=source.width || depth->height!=source.height || overlap(*depth,destination)) return false;
            out.parameters.depthPitch=depth->pitch;
            out.buffers[2]={depth->resource,depth->offset,uint32_t(depthBytes),0};
        }
    }
    out.width=stencil?(source.width+3)/4:source.width;out.height=source.height;return true;
}
inline bool validAttachments(const Draw &d,uint32_t &width,uint32_t &height) {
    const Texture *frame=d.depth.resource?&d.depth:&d.stencil;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(colorTarget(d,i).resource){frame=&colorTarget(d,i);break;}
    width=frame->width;height=frame->height;uint64_t bytes=0;
    if (!width || !height || !validDepthStencilState(d.depthStencil)) return false;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i) {
        const auto &t=colorTarget(d,i);
        if(t.resource ? (!renderableColorFormat(t.format) || t.layers || t.layerStride || t.cube || t.mipLevels>1 || !textureSize(t,bytes)) : !emptyTexture(t))return false;
    }
    if (d.depth.resource ? (d.depth.layers || d.depth.mipLevels>1 || !planeSize(d.depth,Depth32,bytes)) : !emptyTexture(d.depth)) return false;
    if (d.stencil.resource ? (d.stencil.layers || d.stencil.mipLevels>1 || !planeSize(d.stencil,Stencil8,bytes)) : !emptyTexture(d.stencil)) return false;
    Texture planes[QGPU_COLOR_TARGET_COUNT+2];
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)planes[i]=colorTarget(d,i);
    planes[QGPU_COLOR_TARGET_COUNT]=d.depth;planes[QGPU_COLOR_TARGET_COUNT+1]=d.stencil;
    for (const auto &p:planes) if (p.resource && (p.width!=width || p.height!=height || p.samples!=d.rasterSamples)) return false;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT+2;++i)for(unsigned j=i+1;j<QGPU_COLOR_TARGET_COUNT+2;++j)if(overlap(planes[i],planes[j]))return false;
    return true;
}
// Keep the flush timestamp page separate from color data. The entire private
// allocation remains bounded by the mapper's ordinary single-buffer limit.
struct FramebufferLayout { uint32_t bytes=GPU::PageBytes,offsets[QGPU_COLOR_TARGET_COUNT]={}; };
inline bool framebufferLayout(const Draw &d,const Program &p,FramebufferLayout &out) {
    out={};if(p.framebufferMask>255 || (p.framebufferMask && p.stage!=Fragment) || (p.framebufferMask&~colorTargetMask(d)))return false;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(p.framebufferMask&(1u<<i)) {
        const auto &t=colorTarget(d,i);uint64_t bytes=0,allocation=0;
        if(t.layers || t.cube || t.mipLevels>1 || t.swizzle || !textureSize(t,bytes) ||
            !GPU::allocationSize(bytes,allocation) || allocation>GPU::MaxBufferBytes-out.bytes)return false;
        out.offsets[i]=out.bytes;out.bytes+=uint32_t(allocation);
    }
    return true;
}
struct Primitive { uint32_t first=0,windingXor=0,count=3; };
class PrimitiveIterator {
    const Draw &d;const uint8_t *indices;uint32_t cursor=0,run=0;
public:
    PrimitiveIterator(const Draw &draw,const void *snapshot):d(draw),indices(static_cast<const uint8_t *>(snapshot)){}
    bool next(Primitive &out) {
        if(d.primitive==1 || d.primitive==3) {
            const uint32_t count=d.primitive==1?2:3;
            if(cursor>d.count || d.count-cursor<count)return false;
            out={cursor,0,count};cursor+=count;return true;
        }
        if(d.primitive!=4 || (d.indexSize && !indices))return false;
        while(cursor<d.count) {
            uint32_t current=cursor++,value=0;
            if(d.indexSize) {
                for(unsigned i=0;i<d.indexSize;++i)value|=uint32_t(indices[uint64_t(current)*d.indexSize+i])<<(8*i);
                if(value==(d.indexSize==2?0xffffu:UINT32_MAX)){run=0;continue;}
            }
            if(++run>=3){out={current-2,(run-3)&1u};return true;}
        }
        return false;
    }
};
static_assert(sizeof(Binding) == 16 && sizeof(Texture) == 48, "shader resource ABI");
static_assert(sizeof(Texture)==48 && sizeof(Sampler)==28 && sizeof(ColorState)==32 && sizeof(RenderState) == 80 && sizeof(DepthStencilState)==72 && sizeof(Dispatch) == 824 && sizeof(Draw) == 2816, "shader submission ABI");
} }
