// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <A5xxRegs.h>

namespace Adreno {
// Ordinary raster state only: fd5_emit.c emits these groups for dirty
// framebuffer/blend/viewport/scissor state. Never shadow HLSQ allocation,
// shader/descriptor loads, cache control, timestamps, LRZ or draw commands.
// This is an encoding lookup, not an admission limit. All other registers
// always emit normally. The owner invalidates it at IB and pipeline changes.
class RasterStateShadow {
    static constexpr unsigned Count=121;
    uint32_t values_[Count]{};
    uint32_t valid_[(Count+31)/32]{};
    static int slot(uint32_t reg) {
        if(reg>=REG_A5XX_RB_MRT_CONTROL(0) && reg<REG_A5XX_RB_MRT_CONTROL(0)+56)
            return reg-REG_A5XX_RB_MRT_CONTROL(0);
        if(reg>=REG_A5XX_RB_MRT_FLAG_BUFFER(0) && reg<REG_A5XX_RB_MRT_FLAG_BUFFER(0)+32)
            return 56+reg-REG_A5XX_RB_MRT_FLAG_BUFFER(0);
        if(reg>=REG_A5XX_GRAS_CL_VPORT_XOFFSET_0 && reg<REG_A5XX_GRAS_CL_VPORT_XOFFSET_0+6)
            return 88+reg-REG_A5XX_GRAS_CL_VPORT_XOFFSET_0;
        if(reg>=REG_A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0 && reg<REG_A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0+2)
            return 94+reg-REG_A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0;
        if(reg>=REG_A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0 && reg<REG_A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0+2)
            return 96+reg-REG_A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0;
        if(reg>=REG_A5XX_GRAS_SC_WINDOW_SCISSOR_TL && reg<REG_A5XX_GRAS_SC_WINDOW_SCISSOR_TL+2)
            return 98+reg-REG_A5XX_GRAS_SC_WINDOW_SCISSOR_TL;
        if(reg>=REG_A5XX_RB_BLEND_RED && reg<REG_A5XX_RB_BLEND_RED+8)
            return 100+reg-REG_A5XX_RB_BLEND_RED;
        if(reg>=REG_A5XX_GRAS_SC_RAS_MSAA_CNTL && reg<REG_A5XX_GRAS_SC_RAS_MSAA_CNTL+2)
            return 108+reg-REG_A5XX_GRAS_SC_RAS_MSAA_CNTL;
        if(reg>=REG_A5XX_RB_RAS_MSAA_CNTL && reg<REG_A5XX_RB_RAS_MSAA_CNTL+2)
            return 110+reg-REG_A5XX_RB_RAS_MSAA_CNTL;
        if(reg>=REG_A5XX_TPL1_TP_RAS_MSAA_CNTL && reg<REG_A5XX_TPL1_TP_RAS_MSAA_CNTL+2)
            return 112+reg-REG_A5XX_TPL1_TP_RAS_MSAA_CNTL;
        switch(reg) {
            case REG_A5XX_GRAS_SU_CNTL:return 114;
            case REG_A5XX_RB_RENDER_COMPONENTS:return 115;
            case REG_A5XX_RB_BLEND_CNTL:return 116;
            case REG_A5XX_SP_BLEND_CNTL:return 117;
            case REG_A5XX_RB_WINDOW_OFFSET:return 118;
            case REG_A5XX_RB_RESOLVE_CNTL_1:return 119;
            case REG_A5XX_RB_RESOLVE_CNTL_2:return 120;
            default:return -1;
        }
    }
public:
    void invalidate() {__builtin_memset(valid_,0,sizeof(valid_));}
    bool matches(uint32_t reg,uint32_t value) const {
        const int n=slot(reg);
        return n>=0 && (valid_[unsigned(n)/32]&(1u<<(unsigned(n)%32))) && values_[n]==value;
    }
    void forget(uint32_t reg) {
        const int n=slot(reg);if(n>=0)valid_[unsigned(n)/32]&=~(1u<<(unsigned(n)%32));
    }
    // Remember only after the complete original Type4 packet was emitted.
    // A failed packet must not suppress the later retry's first real write.
    void remember(uint32_t reg,uint32_t value) {
        const int n=slot(reg);if(n<0)return;
        values_[n]=value;valid_[unsigned(n)/32]|=1u<<(unsigned(n)%32);
    }
    template<unsigned N> bool matches(uint32_t reg,const uint32_t (&values)[N]) const {
        for(unsigned i=0;i<N;++i)if(!matches(reg+i,values[i]))return false;
        return true;
    }
};
}
