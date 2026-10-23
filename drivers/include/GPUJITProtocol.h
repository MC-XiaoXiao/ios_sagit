// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include "GPUShaderInterface.h"
// Pointer-free compiler output. The vendor selectors extend, rather than
// replace, Apple's resource, queue and submission selectors.
#define QGPU_PROGRAM_FIELDS \
    uint32_t id, stage, codeWords, instrlen, constlen, fullRegs, halfRegs, \
        branchStack, threadSize, subgroupSize, immediateBase, immediateCount, \
        driverBase, driverCount, localID, groupID, vertexID, position, \
        color, fragCoord, fragMask, bufferMask, writeMask, textureCount, flags, \
        instanceID, instanceMask, varyingCount, totalIn, perspective, linear, centerRHW, sharedBytes, threadgroupMask, imageMask, imageReadMask, imageWriteMask, imageUintMask, sampleMask, samplerMask, explicitSamplerMask, cubeSamplerMask, fetchMask, queryMask; \
    uint32_t strides[8]; \
    QGPUVarying varyings[QGPU_MAX_VARYINGS]; \
    uint32_t attributeMask; \
    QGPUAttribute attributes[QGPU_MAX_ATTRIBUTES]; \
    uint32_t implicitSamplerMask,constantSamplerMask,constantImplicitSamplerMask,constantExplicitSamplerMask,constantCubeSamplerMask,framebufferMask; \
    uint32_t constantSamplerSlots[QGPU_MAX_CONSTANT_SAMPLERS],constantSamplerLevelSlots[QGPU_MAX_CONSTANT_SAMPLERS]; \
    QGPUConstantSampler constantSamplers[QGPU_MAX_CONSTANT_SAMPLERS]; \
    uint32_t colorMask,colorRegisters[QGPU_COLOR_TARGET_COUNT],depthReg; \
    uint32_t multisampleMask,depthTextureMask; \
    uint32_t sampleFlags,sampleID,sampleMaskIn,sampleMaskOut, \
        perspectiveCentroid,linearCentroid,perspectiveSample,linearSample; \
    uint32_t textureBindings; \
    uint32_t constantWindowCount; \
    QGPUConstantWindow constantWindows[QGPU_MAX_CONSTANT_WINDOWS]
typedef struct QGPUProgramDescription { QGPU_PROGRAM_FIELDS; } QGPUProgramDescription;
typedef struct QGPUProgramVariant {
    QGPUProgramDescription info;
    uint32_t code[QGPU_MAX_CODE_WORDS], immediates[512];
} QGPUProgramVariant;
typedef struct QGPUProgramImage {
    uint64_t registry;
    uint32_t version, gpu;
    QGPUProgramDescription info;
    uint32_t code[QGPU_MAX_CODE_WORDS], immediates[512];
    char name[96];
    uint32_t reserved, reserved2;
    // Every fragment has a second, independently allocated IR3 variant.
    // It shares its owner's function ID and lifetime. Other stages use zeros.
    QGPUProgramVariant msaa;
    // Optional full-window specializations. Logical interfaces must match the
    // ordinary variants; their IR3 register allocations are independent.
    QGPUProgramVariant constantWindow, msaaConstantWindow;
} QGPUProgramImage;
#ifdef __cplusplus
#include <GPUShaderProtocol.h>
#include <stddef.h>
namespace GPU { namespace JIT {
constexpr uint32_t Version = 39, MaxPrograms = 256, FirstID = 0x80000000;
constexpr uint32_t UploadChunkBytes=64*1024, UploadChunks=2;
constexpr uint32_t uploadChunkBytes(unsigned index) {
    return index>=UploadChunks || index*UploadChunkBytes>=sizeof(QGPUProgramImage)?0:
        sizeof(QGPUProgramImage)-index*UploadChunkBytes<UploadChunkBytes?
        sizeof(QGPUProgramImage)-index*UploadChunkBytes:UploadChunkBytes;
}
enum Selector : uint32_t { RegisterProgram = 0x1000, DeleteProgram = 0x1001 };
struct ProgramData { uint32_t version, id; };
static_assert(sizeof(QGPUVarying)==40 && sizeof(QGPUProgramDescription) == 2468 && sizeof(QGPUProgramImage) == 83720, "JIT image ABI");
static_assert(sizeof(QGPUProgramImage)<=UploadChunks*UploadChunkBytes && uploadChunkBytes(1)>0,"bounded two-part shader upload");
static_assert(offsetof(Shader::Program, strides) == offsetof(QGPUProgramDescription, strides) &&
              offsetof(Shader::Program, code) >= sizeof(QGPUProgramDescription), "program prefix ABI");
inline bool validDescription(const QGPUProgramDescription &p) {
    if(p.constantWindowCount>QGPU_MAX_CONSTANT_WINDOWS)return false;
    if (p.id || p.stage > Shader::Fragment || !p.codeWords || p.codeWords > QGPU_MAX_CODE_WORDS || (p.codeWords & 31) ||
        !p.instrlen || p.instrlen > QGPU_MAX_INSTRUCTION_UNITS || p.codeWords < p.instrlen * 32 || p.codeWords > (p.instrlen + 1) * 32 ||
        p.constlen > 128 || (p.constlen & 3) || !p.fullRegs || p.fullRegs > 63 || p.halfRegs > 63 ||
        p.branchStack > Shader::MaxBranchStack || p.threadSize > 1 || (p.subgroupSize != 32 && p.subgroupSize != 64) ||
        p.immediateBase > p.constlen * 4 || p.immediateCount > p.constlen * 4 - p.immediateBase ||
        p.driverBase > p.constlen * 4 || p.driverCount > p.constlen * 4 - p.driverBase ||
        p.driverCount > (p.stage == Shader::Vertex ? 40u : p.stage == Shader::Compute ? 16u : 4u) ||
        p.bufferMask > 255 || (p.writeMask & ~p.bufferMask) || p.textureCount > (p.stage==Shader::Compute?QGPU_STAGE_TEXTURE_COUNT:QGPU_GRAPHICS_TEXTURE_COUNT) || (p.flags & ~(16777215u|QGPU_FRAGMENT_FLAGS|QGPU_BUFFER_SIZE_CONSTANTS|QGPU_CONSTANT_WINDOWS_REQUIRED|QGPU_FRAMEBUFFER_TILE_ORIGIN)) ||
        ((p.flags&QGPU_FRAMEBUFFER_TILE_ORIGIN) && (p.stage!=Shader::Fragment || !p.framebufferMask)) ||
        (p.stage==Shader::Compute ? (p.flags&19u) : p.stage==Shader::Vertex ? (p.flags&119u) : (p.flags&100u)) ||
        p.localID > 252 || p.groupID > 252 || p.vertexID > 252 || p.position > 252 ||
        p.color > 252 || p.fragCoord > 252 || p.fragMask > 15 || p.instanceID > 252 ||
        (p.instanceMask & ~p.bufferMask) || (p.stage != Shader::Vertex && p.instanceMask)) return false;
    if (p.immediateCount && p.driverCount && p.immediateBase < p.driverBase + p.driverCount &&
        p.driverBase < p.immediateBase + p.immediateCount) return false;
    uint32_t prefix=Shader::memoryConstantWords(p);
    if (prefix && (p.constlen*4<prefix || (p.immediateCount && p.immediateBase<prefix) ||
        (p.driverCount && p.driverBase<prefix))) return false;
    if ((p.stage == Shader::Vertex && p.writeMask) ||
        (p.stage == Shader::Fragment && p.writeMask && !(p.flags&2u)) ||
        (p.stage == Shader::Fragment && p.bufferMask &&
            (p.flags & (QGPU_BOUNDED_BUFFERS|QGPU_FRAGMENT_BUFFER_BANK)) != (QGPU_BOUNDED_BUFFERS|QGPU_FRAGMENT_BUFFER_BANK)) ||
        (p.stage == Shader::Vertex && p.position == 252)) return false;
    for (unsigned i = 0; i < 8; ++i) {
        if (p.bufferMask & (1u << i)) { if (p.strides[i] != 2 && p.strides[i] != 4 && p.strides[i] != 8 && p.strides[i] != 16) return false; }
        else if (p.strides[i]) return false;
    }
    return Shader::validInterface(p);
}
inline bool sameBytes(const void *a,const void *b,unsigned count) {
    const auto *x=static_cast<const uint8_t *>(a),*y=static_cast<const uint8_t *>(b);
    for(unsigned i=0;i<count;++i)if(x[i]!=y[i])return false;
    return true;
}
inline bool emptyVariant(const QGPUProgramVariant &variant) {
    const auto *bytes=reinterpret_cast<const uint8_t *>(&variant);
    for(unsigned i=0;i<sizeof(variant);++i)if(bytes[i])return false;
    return true;
}
inline bool compatibleConstantWindow(const QGPUProgramDescription &p,const QGPUProgramDescription &w) {
    if(!validDescription(w) || p.stage!=Shader::Fragment || !p.constantWindowCount ||
        w.flags!=(p.flags|QGPU_CONSTANT_WINDOWS_REQUIRED))return false;
    const uint32_t a[]={p.stage,p.bufferMask,p.writeMask,p.imageUintMask,p.instanceMask,p.varyingCount,p.framebufferMask,
        p.textureBindings,p.textureCount,p.sampleMask,p.samplerMask,p.explicitSamplerMask,p.cubeSamplerMask,
        p.fetchMask,p.queryMask,p.implicitSamplerMask,p.constantSamplerMask,p.constantImplicitSamplerMask,
        p.constantExplicitSamplerMask,p.constantCubeSamplerMask,p.colorMask,p.multisampleMask,p.depthTextureMask,
        p.sampleFlags,p.attributeMask,p.constantWindowCount};
    const uint32_t b[]={w.stage,w.bufferMask,w.writeMask,w.imageUintMask,w.instanceMask,w.varyingCount,w.framebufferMask,
        w.textureBindings,w.textureCount,w.sampleMask,w.samplerMask,w.explicitSamplerMask,w.cubeSamplerMask,
        w.fetchMask,w.queryMask,w.implicitSamplerMask,w.constantSamplerMask,w.constantImplicitSamplerMask,
        w.constantExplicitSamplerMask,w.constantCubeSamplerMask,w.colorMask,w.multisampleMask,w.depthTextureMask,
        w.sampleFlags,w.attributeMask,w.constantWindowCount};
    if(!sameBytes(a,b,sizeof(a)) || !sameBytes(p.strides,w.strides,sizeof(p.strides)) ||
        !sameBytes(p.constantWindows,w.constantWindows,sizeof(p.constantWindows)) ||
        !sameBytes(p.constantSamplerSlots,w.constantSamplerSlots,sizeof(p.constantSamplerSlots)) ||
        !sameBytes(p.constantSamplerLevelSlots,w.constantSamplerLevelSlots,sizeof(p.constantSamplerLevelSlots)) ||
        !sameBytes(p.constantSamplers,w.constantSamplers,sizeof(p.constantSamplers)))return false;
    for(unsigned i=0;i<p.varyingCount;++i)
        if(!Shader::sameSemantic(p.varyings[i],w.varyings[i]) || p.varyings[i].components!=w.varyings[i].components ||
            p.varyings[i].mode!=w.varyings[i].mode)return false;
    return true;
}
inline bool valid(const QGPUProgramImage &image, uint64_t registry) {
    const auto &p=image.info,&m=image.msaa.info;
    if (image.registry!=registry || image.version!=Version || image.gpu!=540 || image.reserved || image.reserved2 ||
        !image.name[0] || image.name[sizeof(image.name)-1] || (p.sampleFlags&QGPU_SAMPLE_MSAA) ||
        (p.flags&QGPU_CONSTANT_WINDOWS_REQUIRED) || !validDescription(p)) return false;
    if (p.stage!=Shader::Fragment) {
        return emptyVariant(image.msaa) && emptyVariant(image.constantWindow) && emptyVariant(image.msaaConstantWindow);
    }
    if (m.stage!=p.stage || !(m.sampleFlags&QGPU_SAMPLE_MSAA) || (m.flags&QGPU_CONSTANT_WINDOWS_REQUIRED) || !validDescription(m) ||
        m.bufferMask!=p.bufferMask || m.writeMask!=p.writeMask || m.instanceMask!=p.instanceMask || m.varyingCount!=p.varyingCount || m.framebufferMask!=p.framebufferMask || m.textureBindings!=p.textureBindings ||
        ((m.flags^p.flags)&QGPU_FRAMEBUFFER_TILE_ORIGIN)) return false;
    for (unsigned i=0;i<8;++i) if (m.strides[i]!=p.strides[i]) return false;
    if(m.constantWindowCount!=p.constantWindowCount)return false;
    for(unsigned i=0;i<QGPU_MAX_CONSTANT_WINDOWS;++i)
        if(m.constantWindows[i].buffer!=p.constantWindows[i].buffer ||
            m.constantWindows[i].byteOffset!=p.constantWindows[i].byteOffset)return false;
    for (unsigned i=0;i<p.varyingCount;++i)
        if (!Shader::sameSemantic(m.varyings[i],p.varyings[i]) || m.varyings[i].components!=p.varyings[i].components ||
            m.varyings[i].mode!=p.varyings[i].mode) return false;
    if(emptyVariant(image.constantWindow))return emptyVariant(image.msaaConstantWindow);
    return compatibleConstantWindow(p,image.constantWindow.info) &&
        compatibleConstantWindow(m,image.msaaConstantWindow.info);
}
} }
#endif
#undef QGPU_PROGRAM_FIELDS
