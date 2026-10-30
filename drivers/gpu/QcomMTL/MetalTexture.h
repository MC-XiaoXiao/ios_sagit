// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUShaderProtocol.h>
#include <string.h>

namespace GPU { namespace MetalTexture {
constexpr uint32_t LinearTextureAlignment=64;
inline uint32_t linearTextureAlignment(uint32_t format) {
    return Shader::colorFormat(format)?LinearTextureAlignment:0;
}
// The buffer owns the allocation and its resource ID. A linear view reserves
// no new IOVA or storage; the same plane reaches the normal kernel validator.
inline bool bufferTexture(uint32_t resource,uint64_t bufferBytes,uint64_t offset,
    uint64_t width,uint64_t height,uint64_t pitch,uint32_t format,uint32_t swizzle,
    Shader::Texture &out,Shader::TextureLayout &layout,uint64_t imagePitch=0) {
    out={};layout={};
    const auto alignment=linearTextureAlignment(format);
    if(!alignment || !bufferBytes || bufferBytes>MaxBufferBytes || offset>bufferBytes ||
        offset>UINT32_MAX || width>UINT32_MAX || height>UINT32_MAX || pitch>UINT32_MAX ||
        (offset&(alignment-1)) || (pitch&(alignment-1)))return false;
    Shader::Texture candidate{resource,uint32_t(offset),uint32_t(width),uint32_t(height),uint32_t(pitch),format,0,swizzle};
    Shader::TextureLayout checked;
    if(!Shader::textureLayout(candidate,checked,false) || checked.accessBytes>bufferBytes-offset ||
        (imagePitch && (imagePitch<checked.accessBytes || imagePitch>bufferBytes-offset)))return false;
    out=candidate;layout=checked;return true;
}

// MTLTextureLayout describes a CPU-owned image, not an allocated MTLResource.
// Its texels use the exact kernel/sampler layout, followed by a 16-byte layout
// watermark (the native 22H374 protocol stores it at size-sizeof(watermark)).
// Version this identifier whenever the storage interpretation changes. Never
// use the AGX watermark: those bytes describe a different GPU's tiled image.
struct LayoutWatermark { uint64_t words[2]; };
constexpr LayoutWatermark LinearLayoutWatermark={{0x51434f4d4c494e31ull,0x413534304d495031ull}};
struct CopyRegion { uint64_t x,y,z,width,height,depth; };
class MemoryLayout {
    Shader::Texture _texture{};
    Shader::TextureLayout _layout{};
    struct CopyPlan {uint64_t textureOffset=0,linearOffset=0,texturePitch=0,linearPitch=0,rowBytes=0,rows=0,planes=0,textureStride=0,linearStride=0;};
    bool plan(uint64_t slice,uint64_t level,CopyRegion region,uint64_t offset,uint64_t pitch,
        uint64_t imagePitch,CopyPlan &out) const {
        out={};Shader::Texture selected,mip;Shader::TextureTransfer copy;
        if(level>=_layout.levels || slice>=Shader::textureSlices(_texture) ||
            !Shader::textureSlice(_texture,uint32_t(slice),selected) || !Shader::textureLevel(selected,uint32_t(level),mip) ||
            !Shader::textureTransfer(mip,region.x,region.y,region.z,region.width,region.height,region.depth,pitch,imagePitch,copy) ||
            offset>UINT64_MAX-copy.linearSpan)return false;
        out={mip.offset+copy.offset,offset,mip.pitch,pitch,copy.rowBytes,copy.rows,copy.planes,copy.sliceStride,copy.linearStride};return true;
    }
    static bool pointerSpan(const void *pointer,uint64_t offset,uint64_t span) {
        auto base=reinterpret_cast<uintptr_t>(pointer);
        return pointer && offset<=UINTPTR_MAX-base && span<=UINTPTR_MAX-base-offset;
    }
    static bool copyRows(void *destination,uint64_t destinationOffset,uint64_t destinationPitch,
        const void *source,uint64_t sourceOffset,uint64_t sourcePitch,const CopyPlan &p,bool toTexture) {
        if(!p.rows || !p.planes)return true;
        auto sourceStride=toTexture?p.linearStride:p.textureStride,destinationStride=toTexture?p.textureStride:p.linearStride;
        uint64_t sourceSpan=(p.planes-1)*sourceStride+(p.rows-1)*sourcePitch+p.rowBytes;
        uint64_t destinationSpan=(p.planes-1)*destinationStride+(p.rows-1)*destinationPitch+p.rowBytes;
        if(!pointerSpan(source,sourceOffset,sourceSpan) ||
            !pointerSpan(destination,destinationOffset,destinationSpan))return false;
        auto from=reinterpret_cast<uintptr_t>(source)+sourceOffset;
        auto to=reinterpret_cast<uintptr_t>(destination)+destinationOffset;
        if(from==to && (p.rows==1 || sourcePitch==destinationPitch) && (p.planes==1 || sourceStride==destinationStride))return true;
        // The SPI supplies no scratch storage and promises no overlapping
        // conversion. Reject overlap before writing any row.
        if(from<to+destinationSpan && to<from+sourceSpan)return false;
        for(uint64_t z=0;z<p.planes;++z)for(uint64_t y=0;y<p.rows;++y)
            memcpy(reinterpret_cast<void *>(to+z*destinationStride+y*destinationPitch),
                reinterpret_cast<const void *>(from+z*sourceStride+y*sourcePitch),size_t(p.rowBytes));
        return true;
    }
public:
    bool initialize(const Shader::Texture &texture) {
        _texture={};_layout={};Shader::TextureLayout checked;
        // Depth/stencil and MSAA memory have no color-image copy contract
        // here. Do not silently reinterpret them as single-sample color data.
        if(texture.offset || texture.samples || !Shader::textureLayout(texture,checked))return false;
        _texture=texture;_layout=checked;return true;
    }
    uint64_t dataSize() const {return _layout.bytes;}
    uint64_t size() const {return _layout.levels?uint64_t(_layout.bytes)+sizeof(LayoutWatermark):0;}
    uint64_t alignment() const {return _layout.levels?LinearTextureAlignment:0;}
    uint32_t copyGranularity() const {return Shader::formatBlockSize(_texture.format);}
    bool initializeMemory(void *memory) const {
        if(!size() || !pointerSpan(memory,0,size()))return false;
        memset(memory,0,size_t(size()));return true;
    }
    bool finalizeMemory(void *memory) const {
        if(!size() || !pointerSpan(memory,0,size()))return false;
        memcpy(static_cast<uint8_t *>(memory)+_layout.bytes,&LinearLayoutWatermark,sizeof(LayoutWatermark));return true;
    }
    bool copyToTexture(const void *linear,uint64_t offset,uint64_t pitch,uint64_t imagePitch,
        void *memory,uint64_t slice,uint64_t level,CopyRegion region) const {
        CopyPlan p;
        return plan(slice,level,region,offset,pitch,imagePitch,p) &&
            copyRows(memory,p.textureOffset,p.texturePitch,linear,p.linearOffset,p.linearPitch,p,true);
    }
    bool copyFromTexture(const void *memory,uint64_t slice,uint64_t level,CopyRegion region,
        void *linear,uint64_t offset,uint64_t pitch,uint64_t imagePitch) const {
        CopyPlan p;
        return plan(slice,level,region,offset,pitch,imagePitch,p) &&
            copyRows(linear,p.linearOffset,p.linearPitch,memory,p.textureOffset,p.texturePitch,p,false);
    }
};
} }
