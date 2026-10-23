// SPDX-License-Identifier: BSD-2-Clause AND MIT
// A5xx tile limits and layout follow Mesa Freedreno; see NOTICE.txt.
#pragma once
#include <GPURenderPass.h>
#include "AdrenoSubmit.h"
#include <A5xxRegs.h>

namespace Adreno {
constexpr uint32_t GMEMTextureBase=0x100000;
// Replay waits must never reuse the watchdog's monotonic cell at offset16.
constexpr uint32_t GMEMReplayCells=64,GMEMOrigin=2048;
using GMEMLayout=GPU::RenderPass::TileLayout;
inline bool gmemLayout(const GPU::RenderPass::Description &pass,GMEMLayout &out) {
    return GPU::RenderPass::tileLayout(pass,out);
}
inline bool gmemPrograms(const GPU::Shader::Program &vs,const GPU::Shader::Program &fs) {
    return GPU::RenderPass::programsEligible(vs,fs);
}
inline uint32_t gmemColorFormat(uint32_t format) {
    using namespace GPU::Shader;
    return format==RGBA16Float?0x62:format==RGBA16Unorm?0x60:format==RG32Float?0x67:0x30;
}
// Only this trusted root emits tile transfers and calls immutable draw IBs.
// Layout and resource addresses were validated by the ordinary native gate.
class GMEMEncoder {
    Encoder &e;
    uint64_t scratch;
    GPU::Result status=GPU::Result::Success;
    uint32_t progress=0;
    bool timed=false;
    void r(uint32_t reg,uint32_t value){if(status==GPU::Result::Success)status=e.reg(reg,value);}
    template<unsigned N> void rr(uint32_t reg,const uint32_t (&v)[N]){if(status==GPU::Result::Success)status=e.regs(reg,v,N);}
    void p(uint32_t op,const uint32_t *v,uint32_t n){if(status==GPU::Result::Success)status=e.packet(op,v,n);}
    void idle(){p(0x26,nullptr,0);}
    void event(uint32_t code){p(0x46,&code,1);}
    void mode(bool gmem){const uint32_t v[]={gmem?3u:1u,0,0,gmem?16u:0u,0};p(0x6c,v,5);}
    void blit(){const uint32_t v[]={30,uint32_t(scratch+32),uint32_t((scratch+32)>>32),0};p(0x46,v,4);idle();}
    void flush() {
        idle();const uint64_t address=scratch+16;
        const uint32_t v[]={29,uint32_t(address),uint32_t(address>>32),++progress};p(0x46,v,4);
        const uint32_t wait[]={0x13,uint32_t(address),uint32_t(address>>32),progress,0xffffffff,16};p(0x3c,wait,6);idle();
    }
    void timestamp(uint32_t tile,uint32_t phase) {
        if(!timed)return;
        const uint64_t address=scratch+GPU::RenderPass::TimingOffset+uint64_t(tile)*32+phase*8;
        const uint32_t v[]={0x4d2u|(1u<<30)|(2u<<18),uint32_t(address),uint32_t(address>>32)};
        p(0x3e,v,3);
    }
    void storeEdge(uint64_t destination,uint32_t pitch,uint32_t width,uint32_t height,
        uint32_t temporaryPitch,uint32_t temporaryHeight) {
        // GMEM resolve accesses complete 32-pixel blocks. Resolve the edge
        // into a private linear tile, then copy only its valid words through
        // the exact-rectangle 2D path. Never overwrite a view's row padding.
        flush();mode(false);r(REG_A5XX_RB_CCU_CNTL,0x10000000);r(REG_A5XX_RB_CNTL,A5XX_RB_CNTL_BYPASS);
        event(25);event(24);idle();
        r(REG_A5XX_UCHE_CACHE_INVALIDATE_MIN_LO,0);r(REG_A5XX_UCHE_CACHE_INVALIDATE_MIN_HI,0);
        r(REG_A5XX_UCHE_CACHE_INVALIDATE_MAX_LO,0);r(REG_A5XX_UCHE_CACHE_INVALIDATE_MAX_HI,0);
        r(REG_A5XX_UCHE_CACHE_INVALIDATE,0x12);idle();
        r(REG_A5XX_RB_2D_BLIT_CNTL,0x86000000);r(REG_A5XX_GRAS_2D_BLIT_CNTL,0x86000000);
        r(REG_A5XX_UNKNOWN_2184,9);r(REG_A5XX_RB_MODE_CNTL,4);r(REG_A5XX_SP_MODE_CNTL,12);
        r(REG_A5XX_TPL1_MODE_CNTL,0x344);r(REG_A5XX_HLSQ_MODE_CNTL,2);r(REG_A5XX_GRAS_CL_CNTL,0x181);
        uint32_t renderMode=5;p(0x6c,&renderMode,1);
        const uint64_t source=scratch+GPU::PageBytes;
        r(REG_A5XX_RB_2D_SRC_INFO,75);r(REG_A5XX_RB_2D_SRC_LO,uint32_t(source));r(REG_A5XX_RB_2D_SRC_HI,uint32_t(source>>32));
        r(REG_A5XX_RB_2D_SRC_SIZE,A5XX_RB_2D_SRC_SIZE_PITCH(temporaryPitch)|A5XX_RB_2D_SRC_SIZE_ARRAY_PITCH(temporaryPitch*temporaryHeight));
        for(unsigned i=4;i<9;++i)r(REG_A5XX_RB_2D_SRC_INFO+i,0);
        r(REG_A5XX_GRAS_2D_SRC_INFO,75);
        r(REG_A5XX_RB_2D_DST_INFO,75);r(REG_A5XX_RB_2D_DST_LO,uint32_t(destination));r(REG_A5XX_RB_2D_DST_HI,uint32_t(destination>>32));
        r(REG_A5XX_RB_2D_DST_SIZE,A5XX_RB_2D_DST_SIZE_PITCH(pitch)|A5XX_RB_2D_DST_SIZE_ARRAY_PITCH((pitch*height+127)&~127u));
        for(unsigned i=4;i<9;++i)r(REG_A5XX_RB_2D_DST_INFO+i,0);
        r(REG_A5XX_GRAS_2D_DST_INFO,75);
        const uint32_t end=(width-1)|((height-1)<<16),copy[]={1,0,end,0,end};p(0x2c,copy,5);
        renderMode=8;p(0x6c,&renderMode,1);idle();flush();
        mode(true);r(REG_A5XX_RB_CCU_CNTL,0x7c13c080);r(REG_A5XX_RB_MODE_CNTL,0x44);r(REG_A5XX_GRAS_CL_CNTL,0x80);
    }
    void mrt(const GPU::RenderPass::Description &pass,const GMEMLayout &tile,const uint64_t *addresses,bool gmem) {
        for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i) {
            const bool active=pass.colorMask&(1u<<i);const auto &t=pass.colors[i].texture;
            const uint32_t pitch=active?(gmem?tile.pitches[i]:t.pitch):0;
            const uint64_t base=active?(gmem?tile.offsets[i]:addresses[i]):0;
            const uint32_t info=active?A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(gmemColorFormat(t.format))|
                A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(GPU::Shader::bgraFormat(t.format)?1:0)|
                (GPU::Shader::srgbFormat(t.format)?A5XX_RB_MRT_BUF_INFO_COLOR_SRGB:0)|
                (gmem?A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(2)|0x800:0):0;
            const uint32_t v[]={info,A5XX_RB_MRT_PITCH(pitch),A5XX_RB_MRT_ARRAY_PITCH(pitch*(gmem?tile.height:t.height)),uint32_t(base),uint32_t(base>>32)};
            rr(REG_A5XX_RB_MRT_BUF_INFO(i),v);
            const uint32_t flags[4]={};rr(REG_A5XX_RB_MRT_FLAG_BUFFER(i),flags);
        }
    }
public:
    GMEMEncoder(Encoder &encoder,uint64_t completion,bool profile=false):e(encoder),scratch(completion),timed(profile){}
    uint32_t sequence()const{return progress;}
    GPU::Result encode(const GPU::RenderPass::Description &pass,const GMEMLayout &tile,const uint64_t *addresses,
        const IndirectBuffer *draws,uint32_t count) {
        if(!count || !draws || !deviceRange(scratch,GPU::PageBytes))return GPU::Result::Invalid;
        timed=timed && GPU::RenderPass::timingTiles(tile);
        idle();event(25);event(24);r(REG_A5XX_RB_CCU_CNTL,0x7c13c080);mode(true);
        const uint32_t flags[4]={};rr(REG_A5XX_RB_BLIT_FLAG_DST_LO,flags);
        r(REG_A5XX_RB_ADDR_MODE_CNTL,1);r(REG_A5XX_RB_CLEAR_CNTL,0);
        r(REG_A5XX_RB_MODE_CNTL,0x44);r(REG_A5XX_GRAS_CL_CNTL,0x80);
        r(REG_A5XX_RB_RENDER_CNTL,8);r(REG_A5XX_GRAS_SC_CNTL,8);
        r(REG_A5XX_RB_RAS_MSAA_CNTL,0);r(REG_A5XX_RB_DEST_MSAA_CNTL,A5XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE);
        r(REG_A5XX_GRAS_SC_RAS_MSAA_CNTL,0);r(REG_A5XX_GRAS_SC_DEST_MSAA_CNTL,A5XX_GRAS_SC_DEST_MSAA_CNTL_MSAA_DISABLE);
        r(REG_A5XX_TPL1_TP_RAS_MSAA_CNTL,0);r(REG_A5XX_TPL1_TP_DEST_MSAA_CNTL,A5XX_TPL1_TP_DEST_MSAA_CNTL_MSAA_DISABLE);
        const uint32_t bin=A5XX_RB_CNTL_WIDTH(tile.width)|A5XX_RB_CNTL_HEIGHT(tile.height);
        for(uint32_t y=0;y<pass.height && status==GPU::Result::Success;y+=tile.height)
        for(uint32_t x=0;x<pass.width && status==GPU::Result::Success;x+=tile.width) {
            const uint32_t index=(y/tile.height)*tile.columns+x/tile.width;
            timestamp(index,0);
            const uint32_t width=pass.width-x<tile.width?pass.width-x:tile.width;
            const uint32_t height=pass.height-y<tile.height?pass.height-y:tile.height;
            const uint32_t bounds[]={x|(y<<16),(x+width-1)|((y+height-1)<<16)};
            rr(REG_A5XX_GRAS_SC_WINDOW_SCISSOR_TL,bounds);rr(REG_A5XX_RB_RESOLVE_CNTL_1,bounds);
            r(REG_A5XX_RB_WINDOW_OFFSET,bounds[0]);
            mrt(pass,tile,addresses,false);r(REG_A5XX_RB_CNTL,bin|A5XX_RB_CNTL_BYPASS);
            for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if((pass.colorMask&(1u<<i)) && pass.colors[i].load==GPU::RenderPass::Load) {
                const uint32_t dst[]={0,tile.offsets[i],0,A5XX_RB_BLIT_DST_PITCH(tile.pitches[i]),A5XX_RB_BLIT_DST_ARRAY_PITCH(tile.pitches[i]*tile.height)};
                rr(REG_A5XX_RB_RESOLVE_CNTL_3,dst);r(REG_A5XX_RB_BLIT_CNTL,A5XX_RB_BLIT_CNTL_BUF(i));blit();
            }
            mrt(pass,tile,addresses,true);r(REG_A5XX_RB_CNTL,bin);
            for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if((pass.colorMask&(1u<<i)) && pass.colors[i].load==GPU::RenderPass::Clear) {
                r(REG_A5XX_RB_BLIT_CNTL,A5XX_RB_BLIT_CNTL_BUF(i));
                r(REG_A5XX_RB_CLEAR_CNTL,A5XX_RB_CLEAR_CNTL_FAST_CLEAR|A5XX_RB_CLEAR_CNTL_MASK(15));
                rr(REG_A5XX_RB_CLEAR_COLOR_DW0,pass.colors[i].clear);blit();
            }
            r(REG_A5XX_RB_CLEAR_CNTL,0);flush();timestamp(index,1);
            // Each child sees this tile's origin after its normal constants.
            // Reset only replay cells; watchdog progress remains monotonic.
            if(status==GPU::Result::Success)status=e.fill(scratch+GMEMReplayCells,24,0);
            const uint64_t origin=scratch+GMEMOrigin;
            uint32_t v[18]={uint32_t(origin),uint32_t(origin>>32),x,y};p(0x3d,v,18);p(0x12,nullptr,0);
            for(uint32_t i=0;i<count;++i) {
                const uint32_t call[]={uint32_t(draws[i].address),uint32_t(draws[i].address>>32),draws[i].words};p(0x3f,call,3);
            }
            idle();timestamp(index,2);
            // A child may have disabled writes to a read-only attachment, but
            // every MRT remains bound to its original GMEM plane for resolve.
            for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)if((pass.colorMask&(1u<<i)) && pass.colors[i].store==GPU::RenderPass::Store) {
                const auto &t=pass.colors[i].texture;
                const bool edge=width&31;
                const uint64_t base=edge?scratch+GPU::PageBytes:addresses[i];
                const uint32_t pitch=edge?tile.pitches[i]:t.pitch,rows=edge?tile.height:t.height;
                if(edge) {
                    const uint32_t local[]={0,(width-1)|((height-1)<<16)};
                    rr(REG_A5XX_RB_RESOLVE_CNTL_1,local);r(REG_A5XX_RB_WINDOW_OFFSET,0);
                }
                const uint32_t dst[]={4,uint32_t(base),uint32_t(base>>32),A5XX_RB_BLIT_DST_PITCH(pitch),A5XX_RB_BLIT_DST_ARRAY_PITCH(pitch*rows)};
                rr(REG_A5XX_RB_RESOLVE_CNTL_3,dst);r(REG_A5XX_RB_BLIT_CNTL,A5XX_RB_BLIT_CNTL_BUF(i));blit();
                if(edge) {
                    const uint32_t cpp=GPU::Shader::formatBytes(t.format);
                    storeEdge(addresses[i]+uint64_t(y)*t.pitch+x*cpp,t.pitch,width*cpp/4,height,pitch,rows);
                    r(REG_A5XX_RB_CNTL,bin);
                }
            }
            flush();timestamp(index,3);
        }
        mode(false);r(REG_A5XX_RB_CCU_CNTL,0x10000000);r(REG_A5XX_RB_CNTL,A5XX_RB_CNTL_BYPASS);
        return status;
    }
};
}
