// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUShaderProtocol.h>

namespace GPU { namespace RenderPass {
constexpr uint32_t Version=1;
enum LoadAction : uint32_t { DontCare=0,Load=1,Clear=2 };
enum StoreAction : uint32_t { Discard=0,Store=1 };
struct Attachment {
    Shader::Texture texture{};
    uint32_t load=DontCare,store=Discard,clear[4]{};
};
// A complete encoder's attachment lifetime. Resource IDs have the same
// task-local meaning as Draw; callers never supply a GMEM or GPU address.
struct Description {
    uint64_t registry=0;
    uint32_t version=Version,width=0,height=0,colorMask=0;
    uint32_t reserved[2]{};
    Attachment colors[QGPU_COLOR_TARGET_COUNT]{};
};
inline bool valid(const Description &p) {
    if(!p.registry || p.version!=Version || p.reserved[0] || p.reserved[1] || !p.width || !p.height ||
        p.width>MaxTextureDimension || p.height>MaxTextureDimension ||
        !p.colorMask || p.colorMask>255)return false;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i) {
        const auto &a=p.colors[i];const auto &t=a.texture;
        if(!(p.colorMask&(1u<<i))) {
            const Attachment empty{};
            if(__builtin_memcmp(&a,&empty,sizeof(a)))return false;
            continue;
        }
        uint64_t bytes=0;
        if(!t.resource || t.width!=p.width || t.height!=p.height ||
            t.samples || t.layers || t.layerStride || t.cube || t.mipLevels || t.swizzle ||
            !Shader::renderableColorFormat(t.format) || Shader::formatBytes(t.format)<4 ||
            !Shader::textureBytes(t,bytes) || a.load>Clear || a.store>Store)return false;
        // A5xx GMEM transfers use the texture block layout, unlike the exact
        // rectangle 2D fill. Only transfer layouts that contain whole blocks.
        // A transient attachment needs no system-memory load or store.
        if((a.load==Load || a.store==Store) &&
            ((t.height&31) || t.pitch%(64*Shader::formatBytes(t.format))))return false;
        if(a.load!=Clear && (a.clear[0] || a.clear[1] || a.clear[2] || a.clear[3]))return false;
    }
    return true;
}
inline bool drawMatches(const Description &p,const Shader::Draw &d) {
    if(d.registry!=p.registry || d.version!=Shader::Version || !Shader::validDraw(d) ||
        d.depth.resource || d.stencil.resource || d.rasterSamples || d.alphaToCoverage ||
        Shader::colorTargetMask(d)!=p.colorMask)return false;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)
        if(__builtin_memcmp(&Shader::colorTarget(d,i),&p.colors[i].texture,sizeof(Shader::Texture)))return false;
    return true;
}
struct TileLayout {
    uint32_t width=0,height=0,columns=0,rows=0,bytes=0;
    uint32_t offsets[QGPU_COLOR_TARGET_COUNT]{},pitches[QGPU_COLOR_TARGET_COUNT]{};
};
inline bool tileLayout(const Description &pass,TileLayout &out) {
    out={};if(!valid(pass))return false;
    uint32_t cpp=0;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(pass.colorMask&(1u<<i))
        cpp+=Shader::formatBytes(pass.colors[i].texture.format);
    const uint32_t maxWidth=pass.width<1024?(pass.width+63)&~63u:1024;
    for(uint32_t width=64;width<=maxWidth;width+=64) {
        uint32_t height=(GPU::GMEMBytes/(width*cpp))&~31u;
        const uint32_t maxHeight=pass.height<4064?(pass.height+31)&~31u:4064;
        if(height>maxHeight)height=maxHeight;
        for(;height;height-=32) {
            TileLayout candidate{};candidate.width=width;candidate.height=height;
            for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(pass.colorMask&(1u<<i)) {
                candidate.offsets[i]=(candidate.bytes+16383)&~16383u;
                candidate.pitches[i]=width*Shader::formatBytes(pass.colors[i].texture.format);
                candidate.bytes=candidate.offsets[i]+candidate.pitches[i]*height;
            }
            if(candidate.bytes>GPU::GMEMBytes)continue;
            candidate.columns=(pass.width+width-1)/width;candidate.rows=(pass.height+height-1)/height;
            const uint32_t tiles=candidate.columns*candidate.rows,oldTiles=out.columns*out.rows;
            // Replay all geometry once per tile. Minimize that count first,
            // then padded pixels and the number of exact right-edge stores.
            const uint64_t pixels=uint64_t(tiles)*width*height,oldPixels=uint64_t(oldTiles)*out.width*out.height;
            if(!out.bytes || tiles<oldTiles || (tiles==oldTiles &&
                (pixels<oldPixels || (pixels==oldPixels && candidate.rows<out.rows))))out=candidate;
            break;
        }
    }
    return out.bytes!=0;
}
// Bound the root command stream before replacing the original clear ops.
// 264 fixed words/tile (including optional timestamps), 14 per load/store, 15 per clear and 4 per child IB.
// A whole readonly draw fits one child; adjacent draws may share it.
inline uint64_t rootWords(const Description &pass,const TileLayout &tile,uint32_t draws) {
    uint64_t perTile=264+4ull*draws;
    unsigned stores=0;
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if(pass.colorMask&(1u<<i)) {
        const auto &a=pass.colors[i];
        perTile+=a.load==Load?14:a.load==Clear?15:0;
        if(a.store==Store){perTile+=14;++stores;}
    }
    return 64+uint64_t(tile.columns)*tile.rows*perTile+
        ((pass.width&31)?uint64_t(tile.rows)*stores*160:0);
}
// Four always-on timestamps per tile, outside ordinary operation cells and
// the 64-byte tile origin at 2048. Larger passes simply omit this diagnostic.
constexpr uint32_t TimingOffset=2112,TimingCapacity=(PageBytes-TimingOffset)/32;
inline uint32_t timingTiles(const TileLayout &tile) {
    const uint32_t count=tile.columns*tile.rows;
    return count<=TimingCapacity?count:0;
}
inline uint32_t scratchBytes(const Description &pass,const TileLayout &tile) {
    uint32_t edge=0;
    if(pass.width&31)for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)
        if((pass.colorMask&(1u<<i)) && pass.colors[i].store==Store && tile.pitches[i]*tile.height>edge)
            edge=tile.pitches[i]*tile.height;
    return PageBytes+edge;
}
inline bool programsEligible(const Shader::Program &vs,const Shader::Program &fs) {
    return !vs.writeMask && !fs.writeMask && !vs.imageWriteMask && !fs.imageWriteMask &&
        !(fs.framebufferMask&fs.colorMask) &&
        (!fs.framebufferMask || (fs.flags&QGPU_FRAMEBUFFER_TILE_ORIGIN));
}
static_assert(sizeof(Attachment)==72 && sizeof(Description)==608,"native render pass metadata ABI");
} }
