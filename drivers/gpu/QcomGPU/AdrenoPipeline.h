// SPDX-License-Identifier: MIT
// A5xx state setup adapted from Mesa Freedreno, Copyright (c) 2016 Rob Clark.
// See tools/gpu-compiler/NOTICE.txt for the permission notice and source pin.
#pragma once
#include "AdrenoSubmit.h"
#include "AdrenoConstantWindow.h"
#include "AdrenoGMEM.h"
#include <GPUShaderProtocol.h>
#include <GPUShaderCounters.h>
#include <A5xxRegs.h>

namespace Adreno {
// All addresses here were resolved and retained by the native device namespace.
struct ShaderBuffers { uint64_t addresses[QGPU_GRAPHICS_BUFFER_COUNT]; uint32_t bytes[QGPU_GRAPHICS_BUFFER_COUNT]; };
struct GraphicsTextures { uint64_t fragment[QGPU_STAGE_TEXTURE_COUNT]={},vertex[QGPU_STAGE_TEXTURE_COUNT]={}; };
// Native admission has already reconstructed and frozen the builtin fill
// parameters. Use integer render formats so every pattern bit is preserved.
inline bool hardwareClearEligible(uint64_t address,uint32_t bytes,const uint32_t (&values)[8],bool pairs) {
    const uint32_t row=values[0],height=values[1],pitch=values[2],cpp=pairs?8:4;
    const uint64_t span=height?uint64_t(height-1)*pitch+row:0;
    // CP_BLIT solid R32 writes complete pixels, including a short final
    // vector. The compute uint4 width restriction does not apply here.
    return row && height && !(address&63) && !(row&(pairs?15u:3u)) && !(pitch&63) && pitch>=row &&
        (pairs || !values[3]) && row/cpp<=GPU::MaxTextureDimension && height<=GPU::MaxTextureDimension &&
        uint64_t(row)*height>=16384 && uint64_t(pitch)*height<=GPU::MaxBufferBytes &&
        span<=bytes && deviceRange(address,span);
}
struct FramebufferCopyRegion { uint32_t offset=0,bytes=0; };
struct Framebuffer2DRegion {uint32_t offset=0,pitch=0,rows=0,x=0,width=0;};
// Experimental texture-barrier feedback, not coherent GMEM framebuffer fetch.
// One triangle per barrier prevents self-overdraw. Keep MSAA, MRT, lines and
// non-UNORM targets on the original private snapshot path.
inline bool framebufferInplaceEligible(const GPU::Shader::Draw &d,const GPU::Shader::Program &fs) {
    using namespace GPU::Shader;
    return fs.framebufferMask==1 && fs.colorMask==1 && colorTargetMask(d)==1 &&
        (d.primitive==3 || d.primitive==4) && d.rasterSamples<=1 && d.target.samples<=1 &&
        !(fs.sampleFlags&QGPU_SAMPLE_MSAA) &&
        (d.target.format==RGBA8 || d.target.format==BGRA8 || d.target.format==RGBA8SRGB || d.target.format==BGRA8SRGB);
}
// A framebuffer argument need not be an output of the fragment function.
// Fluidity, for example, reads attachment1 while writing only attachment0.
// The native admission path rejects physical aliases between attachments,
// textures and writable buffers. These read-only planes cannot change during
// this draw, including overlapping primitives/instances; bind the original
// view after publishing preceding writes instead of copying it per triangle.
inline bool framebufferReadOnlyEligible(const GPU::Shader::Draw &d,const GPU::Shader::Program &fs) {
    using namespace GPU::Shader;
    if(fs.stage!=Fragment || !fs.framebufferMask || fs.framebufferMask>255 ||
        (fs.framebufferMask&fs.colorMask) || (fs.framebufferMask&~colorTargetMask(d)) ||
        (d.primitive!=3 && d.primitive!=4) || d.rasterSamples>1 || (fs.sampleFlags&QGPU_SAMPLE_MSAA))return false;
    for(unsigned slot=0;slot<QGPU_COLOR_TARGET_COUNT;++slot)
        if((fs.framebufferMask&(1u<<slot)) && colorTarget(d,slot).samples>1)return false;
    return true;
}
// Use the same validated plan for pre-Family budgeting, allocation and PM4
// encoding. In-place feedback needs only the private flush/progress page;
// snapshot fallback retains its original color-plane offsets and bounds.
struct FramebufferStorage {
    GPU::Shader::FramebufferLayout snapshots;
    uint32_t bytes=0;
    bool inplace=false,readOnly=false;
};
inline bool framebufferStoragePlan(const GPU::Shader::Draw &d,const GPU::Shader::Program &fs,
    bool enableInplace,bool primitiveSnapshot,FramebufferStorage &out,bool compact=true) {
    out={};
    if(!GPU::Shader::framebufferLayout(d,fs,out.snapshots))return false;
    out.inplace=enableInplace && !primitiveSnapshot && framebufferInplaceEligible(d,fs);
    out.readOnly=enableInplace && !primitiveSnapshot && framebufferReadOnlyEligible(d,fs);
    out.bytes=(out.inplace || out.readOnly) && compact?GPU::PageBytes:out.snapshots.bytes;
    return true;
}
enum : unsigned {Framebuffer2DRegionCount=5};
inline bool framebuffer2DRegions(uint64_t source,uint64_t destination,uint32_t bytes,Framebuffer2DRegion (&out)[Framebuffer2DRegionCount]) {
    for(auto &region:out)region={};
    if(!bytes || bytes>GPU::MaxBufferBytes || ((source|destination|bytes)&63) ||
        !deviceRange(source,bytes) || !deviceRange(destination,bytes) ||
        (source<destination+bytes && destination<source+bytes))return false;
    // The hardware experiment reached a source guard page for an arbitrary
    // multirow tail. Keep short copies in Mesa's1D buffer form. Large copies
    // use only16-row-aligned interiors, with at least64KiB and the final
    // partial tile handled by the original bounded compute shader. Actual
    // private mappings/guards remain authoritative; this is not permission
    // to read or map outside the validated original resource.
    if(bytes<=16320){out[0]={0,bytes,1};return true;}
    if(bytes<=65536)return false;
    const uint32_t rows=((bytes-65536)/4096)&~15u;
    if(!rows)return false;
    // Generic multirow blits need their real slice stride. ARRAY_PITCH has
    //16bits in64-byte units; use at most63*64KiB per rectangle, never truncate
    // an8/16MiB plane into that field. Preserve16-row alignment at each split.
    uint32_t remaining=rows,offset=0;unsigned count=0;
    while(remaining) {
        const uint32_t part=remaining>1008?1008:remaining;
        if(count==Framebuffer2DRegionCount)return false;
        out[count++]={offset,4096,part};offset+=4096*part;remaining-=part;
    }
    return true;
}
inline bool framebufferCopyRegion(const GPU::Shader::Draw &draw,const GPU::Shader::Texture &texture,FramebufferCopyRegion &out) {
    out={};uint64_t size=0;
    if(!GPU::Shader::textureBytes(texture,size) || !draw.scissor[3] || draw.scissor[1]>=texture.height ||
        draw.scissor[3]>texture.height-draw.scissor[1])return false;
    // Framebuffer inputs use the current fragment's pixel. Include complete
    // 2x2 fragment quads for helper lanes/derivatives at the scissor boundary;
    // retain the full native pitch and descriptor coordinates in the snapshot.
    uint32_t first=draw.scissor[1]&~1u;
    uint64_t last=(uint64_t(draw.scissor[1])+draw.scissor[3]+1)&~uint64_t(1);
    if(last>texture.height)last=texture.height;
    uint64_t offset=uint64_t(first)*texture.pitch,bytes=(last-first)*texture.pitch;
    if(!bytes || (bytes&3) || offset>size || bytes>size-offset || offset>UINT32_MAX || bytes>UINT32_MAX)return false;
    out={uint32_t(offset),uint32_t(bytes)};return true;
}
inline bool framebufferScissor2DRegions(uint64_t source,uint64_t destination,const GPU::Shader::Draw &draw,
    const GPU::Shader::Texture &texture,const FramebufferCopyRegion &span,
    Framebuffer2DRegion (&out)[Framebuffer2DRegionCount],uint32_t &covered,uint32_t &payload) {
    for(auto &region:out)region={};
    covered=payload=0;
    const uint32_t cpp=GPU::Shader::formatBytes(texture.format),pitch=texture.pitch;
    if(draw.rasterSamples>1 || texture.samples>1 || !cpp || !pitch || pitch>16320 || (pitch&63) ||
        !draw.scissor[2] || draw.scissor[0]>=texture.width || draw.scissor[2]>texture.width-draw.scissor[0] ||
        !span.bytes || span.bytes%pitch || ((source|destination)&63) ||
        !deviceRange(source,span.bytes) || !deviceRange(destination,span.bytes) ||
        (source<destination+span.bytes && destination<source+span.bytes))return false;
    // The compiler's framebuffer input is texelFetch(ivec2(gl_FragCoord.xy)).
    // Keep native coordinates/pitch, whole helper quads, and whole64-byte
    // blitter columns. No sampling descriptor or scissor is changed.
    const uint64_t first=(uint64_t(draw.scissor[0]&~1u)*cpp)&~uint64_t(63);
    uint64_t last=(((uint64_t(draw.scissor[0])+draw.scissor[2]+1)&~uint64_t(1))*cpp+63)&~uint64_t(63);
    if(last>pitch)last=pitch;
    if(first>=last || last-first==pitch)return false;
    // Retain the proven multirow guard margin and compute-copy the final
    // >=64KiB of full rows. ARRAY_PITCH always describes the real row stride.
    const uint32_t rows=span.bytes/pitch,tailRows=(65536+pitch-1)/pitch;
    if(rows<=tailRows)return false;
    uint32_t remaining=(rows-tailRows)&~15u;
    const uint32_t sliceRows=(0x3fffc0u/pitch)&~15u,maxRows=sliceRows>1008?1008:sliceRows;
    if(!remaining || !maxRows)return false;
    unsigned count=0;
    while(remaining) {
        if(count==Framebuffer2DRegionCount){for(auto &region:out)region={};covered=payload=0;return false;}
        const uint32_t part=remaining>maxRows?maxRows:remaining;
        out[count++]={covered,pitch,part,uint32_t(first),uint32_t(last-first)};
        covered+=pitch*part;payload+=uint32_t(last-first)*part;remaining-=part;
    }
    return true;
}
struct IndirectBufferAllocator {
    virtual GPU::Result next(Encoder &,uint64_t &address)=0;
protected:
    ~IndirectBufferAllocator()=default;
};
class PipelineEncoder {
    struct RasterContext {
        GPU::Shader::Linkage linkage;
        GPU::Shader::FramebufferLayout snapshots;
        uint64_t targets[QGPU_COLOR_TARGET_COUNT]={};
        uint32_t width=0,height=0,targetMask=0,targetCount=0,components=0,blendMask=0;
        bool inplace=false,readOnlyFeedback=false;
    };
    // One CPU-private workspace per encoder, outside the kernel call stack.
    // Primitive expansion invokes raster/dispatch without another draw frame.
    // Keep their live data separate so fallback dispatch cannot corrupt part.
    struct Workspace {
        RasterContext context;
        GPU::Shader::Draw part;
        GPU::Shader::Dispatch copy;
        ShaderBuffers copyBindings;
        Framebuffer2DRegion rectangles[Framebuffer2DRegionCount];
        uint32_t vertexParams[40],fragmentParams[4];
        uint32_t vertexMemory[QGPU_MEMORY_CONSTANT_WORDS],fragmentMemory[QGPU_MAX_MEMORY_CONSTANT_WORDS];
        ConstantWindowPlan constantWindowPlans[QGPU_MAX_CONSTANT_WINDOWS];
        uint32_t constantWindowMask;
    } workspace;
    Encoder &e;
    uint64_t ib,completionScratch,borderAddress=0,snapshotBytes=0,snapshotPrimitives=0,snapshot2DBytes=0,snapshotUncroppedBytes=0,inplacePrimitives=0;
    uint32_t scratchBytes,flushSequence=0;
    const GMEMLayout *gmem=nullptr;
    bool directShaders=false,framebuffer2D=false,framebufferXCrop=false,framebufferWord32=false,framebufferInplace=false;
    bool framebufferCompact=true;
    bool framebufferStateReuse=true;
    uint64_t rasterFullSetups=0,rasterReusedPrimitives=0,rasterDriverUpdates=0;
    // Immutable ISA is copied once per backing IB, independently of GPU state
    // reloads. The native gate keeps each source image alive and immutable
    // throughout encoding; only these owned copies are ever GPU-readable.
    struct ShaderObject {const uint32_t *code=nullptr;uint64_t address=0;uint32_t words=0;};
    ShaderObject shaderObjects[16]{};
    unsigned nextShaderObject=0;
    uint64_t shaderObjectHits=0,shaderObjectSavedBytes=0;
    void forgetShaderObjects() {for(auto &object:shaderObjects)object={};nextShaderObject=0;}
    // Failed diagnostic trials restore the original immutable-ISA lookup as
    // well as every cursor/counter. This workspace is outside the kernel stack.
    ShaderObject profileShaderObjects[16]{};
    unsigned profileNextShaderObject=0;
    uint64_t operationTimestamp=0;
    bool operationTimed=false,operationCounters=false;
    void shaderCounters(bool begin) {
        using namespace GPU::ShaderCounters;
        if(begin) {idle();rr(0x0ed0,SP);rr(0x0f10,TP);rr(0x0ea0,UCHE);}
        const uint64_t address=completionScratch+ScratchOffset+(begin?0:Count*8);
        const uint32_t registers[]={0x428,0x418,0x408},counts[]={8,4,4};
        unsigned offset=0;
        for(unsigned i=0;i<3;++i)for(unsigned n=0;n<counts[i];++n) {
            const auto destination=address+(offset++)*8;
            const uint32_t data[]={(registers[i]+n*2)|(1u<<30),uint32_t(destination),uint32_t(destination>>32)};
            p(0x3e,data,3); // Mesa fd5_query.c: one 64-bit counter per packet.
        }
    }
    uint32_t operationWindowState=0;
    GPU::Result status = GPU::Result::Success;
    void timestamp(uint64_t address) {
        const uint32_t data[]={0x4d2u|(1u<<30)|(2u<<18),uint32_t(address),uint32_t(address>>32)};
        p(0x3e,data,3); // CP_REG_TO_MEM: always-on 64-bit counter, no new idle.
    }
    void r(uint32_t reg, uint32_t value) { if (status == GPU::Result::Success) status = e.reg(reg,value); }
    template<unsigned N> void rr(uint32_t reg, const uint32_t (&values)[N]) {
        if (status == GPU::Result::Success) status = e.regs(reg,values,N);
    }
    void p(uint32_t op, const uint32_t *data, uint32_t n) { if (status == GPU::Result::Success) status = e.packet(op,data,n); }
    void idle() { p(0x26,nullptr,0); }
    uint64_t upload(const uint32_t *data, uint32_t words,uint32_t alignment=32) {
        uint32_t offset = 0;
        if (status == GPU::Result::Success) status = e.data(data,words,offset,alignment);
        return ib+offset;
    }
    void event(uint32_t code) { p(0x46,&code,1); }
    void flush(uint32_t code,unsigned cell) {
        idle();uint64_t address=completionScratch+(gmem?GMEMReplayCells:0)+cell*8;uint32_t sequence=++flushSequence;
        uint32_t payload[]={code,uint32_t(address),uint32_t(address>>32),sequence};p(0x46,payload,4);
        uint32_t wait[]={0x13,uint32_t(address),uint32_t(address>>32),sequence,0xffffffff,16};p(0x3c,wait,6);idle();
    }
    void invalidateUCHE() {
        r(REG_A5XX_UCHE_CACHE_INVALIDATE_MIN_LO,0); r(REG_A5XX_UCHE_CACHE_INVALIDATE_MIN_HI,0);
        r(REG_A5XX_UCHE_CACHE_INVALIDATE_MAX_LO,0); r(REG_A5XX_UCHE_CACHE_INVALIDATE_MAX_HI,0);
        r(REG_A5XX_UCHE_CACHE_INVALIDATE,0x12); idle();
    }
    void restore() {
        idle(); uint32_t mode[] = {gmem?3u:1u,0,0,gmem?16u:0u,0}; p(0x6c,mode,5);
        // SP/RBCCU can lose non-context state in runtime power collapse.
        // Reassert the A5xx 64-bit addressing contract after domain resume,
        // through the same trusted CP stream as the following object state.
        r(REG_A5XX_SP_ADDR_MODE_CNTL,1);
        r(REG_A5XX_RB_ADDR_MODE_CNTL,1);
        r(REG_A5XX_TPL1_ADDR_MODE_CNTL,1);
        uint32_t skip = 0; p(0x1d,&skip,1); // no inherited IB2 skip state
        invalidateUCHE();
        r(REG_A5XX_HLSQ_UPDATE_CNTL,0xfffff);
        r(REG_A5XX_PC_RESTART_INDEX,0xffffffff); r(REG_A5XX_PC_RASTER_CNTL,0x12);
        r(REG_A5XX_UNKNOWN_E292,0); r(REG_A5XX_UNKNOWN_E292+1,0);
        r(REG_A5XX_TPL1_TP_FS_ROTATION_CNTL,0); r(REG_A5XX_UNKNOWN_E004,0);
        r(REG_A5XX_GRAS_SU_LAYERED,0); r(REG_A5XX_PC_GS_LAYERED,0);
        r(REG_A5XX_UNKNOWN_E5AB,0); r(REG_A5XX_UNKNOWN_E5C2,0); r(REG_A5XX_UNKNOWN_E5DB,0);
        r(REG_A5XX_SP_HS_CTRL_REG0,0); r(REG_A5XX_SP_DS_CTRL_REG0,0); r(REG_A5XX_SP_GS_CTRL_REG0,0);
        r(REG_A5XX_RB_CLEAR_CNTL,0);
        const uint32_t resetRanges[] = {REG_A5XX_UNKNOWN_E7C0,REG_A5XX_UNKNOWN_E7C5,
            REG_A5XX_UNKNOWN_E7CA,REG_A5XX_UNKNOWN_E7CF,REG_A5XX_UNKNOWN_E7D4,REG_A5XX_UNKNOWN_E7D9};
        const uint32_t resetValues[3]={};
        for (auto reg : resetRanges) rr(reg,resetValues);
        r(REG_A5XX_GRAS_SU_CONSERVATIVE_RAS_CNTL,0); r(REG_A5XX_GRAS_SC_SCREEN_SCISSOR_CNTL,0);
        r(REG_A5XX_SP_VS_CONFIG_MAX_CONST,0); r(REG_A5XX_SP_FS_CONFIG_MAX_CONST,0);
        r(REG_A5XX_RB_MODE_CNTL,0x44); r(REG_A5XX_RB_DBG_ECO_CNTL,0x100000);
        r(REG_A5XX_RB_RENDER_CNTL,8); r(REG_A5XX_GRAS_SC_CNTL,8);
        r(REG_A5XX_VFD_MODE_CNTL,0); r(REG_A5XX_PC_MODE_CNTL,0x1f); r(REG_A5XX_SP_MODE_CNTL,0x1e);
        r(REG_A5XX_SP_DBG_ECO_CNTL,0x800); r(REG_A5XX_HLSQ_DBG_ECO_CNTL,0);
        r(REG_A5XX_TPL1_MODE_CNTL,0x544); r(REG_A5XX_HLSQ_MODE_CNTL,1);
        r(REG_A5XX_HLSQ_TIMEOUT_THRESHOLD_0,0x80); r(REG_A5XX_HLSQ_TIMEOUT_THRESHOLD_1,0);
        r(REG_A5XX_VPC_DBG_ECO_CNTL,0x400); r(REG_A5XX_VPC_MODE_CNTL,0);
        uint32_t drawStates[] = {1u<<18,0,0}; p(0x43,drawStates,3); // disable all draw-state groups
        r(REG_A5XX_GRAS_SC_BIN_CNTL,0); r(REG_A5XX_VPC_FS_PRIMITIVEID_CNTL,0xff);
        r(REG_A5XX_VPC_SO_OVERRIDE,A5XX_VPC_SO_OVERRIDE_SO_DISABLE);
        r(REG_A5XX_VPC_SO_CNTL,0); r(REG_A5XX_VPC_SO_BUF_CNTL,0);
        r(REG_A5XX_PC_GS_PARAM,0); r(REG_A5XX_PC_HS_PARAM,0);
        r(REG_A5XX_SP_HS_CONFIG,0); r(REG_A5XX_SP_DS_CONFIG,0); r(REG_A5XX_SP_GS_CONFIG,0);
        r(REG_A5XX_HLSQ_HS_CONFIG,0); r(REG_A5XX_HLSQ_DS_CONFIG,0); r(REG_A5XX_HLSQ_GS_CONFIG,0);
        r(REG_A5XX_HLSQ_HS_CNTL,0); r(REG_A5XX_HLSQ_DS_CNTL,0); r(REG_A5XX_HLSQ_GS_CNTL,0);
        r(REG_A5XX_HLSQ_HS_CONSTLEN,0); r(REG_A5XX_HLSQ_DS_CONSTLEN,0); r(REG_A5XX_HLSQ_GS_CONSTLEN,0);
        r(REG_A5XX_TPL1_VS_TEX_COUNT,0); r(REG_A5XX_TPL1_FS_TEX_COUNT,0);
        r(REG_A5XX_TPL1_HS_TEX_COUNT,0); r(REG_A5XX_TPL1_DS_TEX_COUNT,0); r(REG_A5XX_TPL1_GS_TEX_COUNT,0);
        r(REG_A5XX_TPL1_CS_TEX_COUNT,0);
        r(REG_A5XX_PC_POWER_CNTL,3); r(REG_A5XX_VFD_POWER_CNTL,3); // A540: four SP cores
        r(REG_A5XX_RB_CCU_CNTL,gmem?0x7c13c080:0x10000000);
        r(REG_A5XX_RB_CNTL,gmem?A5XX_RB_CNTL_WIDTH(gmem->width)|A5XX_RB_CNTL_HEIGHT(gmem->height):A5XX_RB_CNTL_BYPASS);
    }
    void snapshot2D(uint64_t source,uint64_t destination,const Framebuffer2DRegion (&regions)[Framebuffer2DRegionCount]) {
        // Mesa24.3.4 fd5_blitter.c emit_setup/emit_blit, linear R8_UNORM.
        // Only trusted resolved addresses enter this stream; private page
        // tables and original per-primitive ordering remain unchanged.
        restore();event(25);event(24);idle(); // fd5 sysmem prep: invalidate CCU color/depth after prior flushes
        // fd5's ordinary same-format blitter supports R32_UINT as well as R8.
        // Reinterpret only these trusted aligned raw copy extents, preserving
        // every bit without float/UNORM conversion. Pitch/slice stay in bytes;
        // only CP_BLIT horizontal coordinates change to four-byte texels.
        r(REG_A5XX_RB_2D_BLIT_CNTL,0x86000000);r(REG_A5XX_GRAS_2D_BLIT_CNTL,0x86000000);
        r(REG_A5XX_UNKNOWN_2184,9);r(REG_A5XX_RB_MODE_CNTL,4);r(REG_A5XX_SP_MODE_CNTL,12);
        r(REG_A5XX_TPL1_MODE_CNTL,0x344);r(REG_A5XX_HLSQ_MODE_CNTL,2);r(REG_A5XX_GRAS_CL_CNTL,0x181);
        for(const auto &region:regions)if(region.rows) {
            // Keep the original small single-row buffer form unchanged.
            const uint32_t cpp=framebufferWord32 && region.rows>1?4:1,format=cpp==4?75:3;
            uint32_t mode=5;p(0x6c,&mode,1); // BLIT2D
            const uint64_t src=source+region.offset,dst=destination+region.offset;
            r(REG_A5XX_RB_2D_SRC_INFO,format); // R8_UNORM / R32_UINT, LINEAR, WZYX
            r(REG_A5XX_RB_2D_SRC_LO,uint32_t(src));r(REG_A5XX_RB_2D_SRC_HI,uint32_t(src>>32));
            const uint32_t slice=region.rows==1?128:region.pitch*region.rows;
            r(REG_A5XX_RB_2D_SRC_SIZE,A5XX_RB_2D_SRC_SIZE_PITCH(region.pitch)|A5XX_RB_2D_SRC_SIZE_ARRAY_PITCH(slice));
            for(unsigned i=4;i<9;++i)r(REG_A5XX_RB_2D_SRC_INFO+i,0);
            r(REG_A5XX_GRAS_2D_SRC_INFO,format);
            r(REG_A5XX_RB_2D_DST_INFO,format);
            r(REG_A5XX_RB_2D_DST_LO,uint32_t(dst));r(REG_A5XX_RB_2D_DST_HI,uint32_t(dst>>32));
            r(REG_A5XX_RB_2D_DST_SIZE,A5XX_RB_2D_DST_SIZE_PITCH(region.pitch)|A5XX_RB_2D_DST_SIZE_ARRAY_PITCH(slice));
            for(unsigned i=4;i<9;++i)r(REG_A5XX_RB_2D_DST_INFO+i,0);
            r(REG_A5XX_GRAS_2D_DST_INFO,format);
            const uint32_t width=(region.width?region.width:region.pitch)/cpp,x=region.x/cpp;
            const uint32_t end=(x+width-1)|((region.rows-1)<<16),copy[]={1,x,end,x,end};p(0x2c,copy,5);
            mode=8;p(0x6c,&mode,1);idle(); // END2D, complete before another mode
        }
        // 2D writes use the color path, unlike the compute fallback. Prove
        // those writes drained before the existing UCHE invalidate and fetch.
        flush(29,0);
    }
    uint32_t block(const GPU::Shader::Program &program) const {
        return program.stage == GPU::Shader::Compute ? 13 : program.stage == GPU::Shader::Vertex ? 8 : 12;
    }
    void constants(const GPU::Shader::Program &program, const uint32_t *driver, uint32_t driverWords,const uint32_t *threadgroup=nullptr) {
        uint32_t values[515] = {}; uint32_t n = program.constlen*4;
        if (n > 512 || (program.immediateCount && program.immediateBase+program.immediateCount > n) ||
            (program.driverCount && (program.driverBase+program.driverCount > n || program.driverCount > driverWords))) {
            status = GPU::Result::Invalid; return;
        }
        uint32_t prefix=GPU::Shader::memoryConstantWords(program);
        if (prefix && (!threadgroup || n<prefix)) {status=GPU::Result::Invalid;return;}
        if (!n) return;
        values[0] = (block(program)<<18) | (program.constlen<<22); values[1] = 1; // direct constants
        for (uint32_t i=0;i<prefix;++i) values[3+i]=threadgroup[i];
        for (uint32_t i=0;i<program.immediateCount;++i) values[3+program.immediateBase+i] = program.immediates[i];
        for (uint32_t i=0;i<program.driverCount;++i) values[3+program.driverBase+i] = driver[i];
        p(0x30,values,n+3);
    }
    void vertexDriverConstants(const GPU::Shader::Program &program,const uint32_t *driver,const uint32_t *memory) {
        // fd5 direct constant loads address vec4 units. Preserve the adjacent
        // immutable words when driverBase/count are not vec4 aligned; this is
        // the identical composition order used by constants(), not zero fill
        // over an immediate or memory-size constant sharing that vec4.
        const uint32_t n=program.constlen*4,prefix=GPU::Shader::memoryConstantWords(program);
        if(n>512 || program.driverCount>40 || program.driverBase>n || program.driverCount>n-program.driverBase ||
            program.immediateBase>n || program.immediateCount>n-program.immediateBase || prefix>n) {
            status=GPU::Result::Invalid;return;
        }
        if(!program.driverCount)return;
        const uint32_t first=program.driverBase&~3u,last=(program.driverBase+program.driverCount+3)&~3u;
        uint32_t values[47]={};values[0]=(first/4)|(8u<<18)|(((last-first)/4)<<22);values[1]=1;
        for(uint32_t i=first;i<last;++i) {
            uint32_t word=i<prefix?memory[i]:0;
            if(i>=program.immediateBase && i-program.immediateBase<program.immediateCount)word=program.immediates[i-program.immediateBase];
            if(i>=program.driverBase && i-program.driverBase<program.driverCount)word=driver[i-program.driverBase];
            values[3+i-first]=word;
        }
        p(0x30,values,3+last-first);
        if(status==GPU::Result::Success)++rasterDriverUpdates;
    }
    void buffers(bool compute, uint32_t mask, const ShaderBuffers &buffers,uint32_t exactBoundsMask=0) {
        if (!mask) return;
        const uint32_t count=compute?QGPU_COMPUTE_BUFFER_COUNT:QGPU_GRAPHICS_BUFFER_COUNT;
        uint32_t values[3+QGPU_GRAPHICS_BUFFER_COUNT*2] = {}; uint32_t sb = compute ? 15 : 14;
        values[0] = (sb<<18) | (count<<22); values[1] = 1;
        for (uint32_t i=0;i<count;++i) {
            uint32_t bytes=buffers.bytes[i];
            // A zero low dimension can make A540 treat the descriptor as
            // empty. Only shaders with exact native bounds may use this
            // nonzero hardware extent; no access gains the extra byte.
            if(bytes && !(bytes&0xffff) && (exactBoundsMask&(1u<<i)))++bytes;
            values[3+i*2] = A5XX_SSBO_1_0_WIDTH(bytes&0xffff);
            values[4+i*2] = A5XX_SSBO_1_1_HEIGHT(bytes>>16);
        }
        p(0x30,values,3+count*2); values[1] = 2;
        for (uint32_t i=0;i<count;++i) { values[3+i*2] = uint32_t(buffers.addresses[i]); values[4+i*2] = uint32_t(buffers.addresses[i]>>32); }
        p(0x30,values,3+count*2);
    }
    uint64_t shaderObject(const GPU::Shader::Program &program, uint32_t objectReg,uint32_t cachedLength) {
        if(!program.code || !program.instrlen || program.instrlen>QGPU_MAX_INSTRUCTION_UNITS || !program.codeWords ||
            program.codeWords>QGPU_MAX_CODE_WORDS || program.codeWords<program.instrlen*32 ||
            program.codeWords>(program.instrlen+1)*32 || (program.codeWords&31) || cachedLength>program.instrlen) {status=GPU::Result::Invalid;return 0;}
        uint64_t address=0;
        for(const auto &object:shaderObjects)
            if(object.code==program.code && object.words==program.codeWords){address=object.address;break;}
        if(address) {++shaderObjectHits;shaderObjectSavedBytes+=uint64_t(program.codeWords)*4;}
        else {
            address=upload(program.code,program.codeWords);
            if(status==GPU::Result::Success) {
                shaderObjects[nextShaderObject]={program.code,address,program.codeWords};
                nextShaderObject=(nextShaderObject+1)%16;
            }
        }
        // No private stack/scratch shader memory is admitted by the compiler.
        const uint32_t object[]={uint32_t(address),uint32_t(address>>32),0,0,0,0};
        rr(objectReg,object);
        return address;
    }
    void loadShader(const GPU::Shader::Program &program,uint64_t address,uint32_t cachedLength) {
        if(status!=GPU::Result::Success)return;
        uint32_t values[] = {(2u<<16)|(block(program)<<18)|(cachedLength<<22),uint32_t(address),uint32_t(address>>32)};
        if(cachedLength) {
            if(directShaders) {
                // Freedreno's SS4_DIRECT path: CP carries the identical ISA
                // in the state packet instead of HLSQ fetching it by IOVA.
                // The object backing remains mapped and read-only.
                const uint32_t prefix[]={(block(program)<<18)|(cachedLength<<22),0,0};
                if(status==GPU::Result::Success)status=e.packetData(0x30,prefix,3,program.code,program.codeWords);
            } else p(0x30,values,3);
        }
    }
    void shader(const GPU::Shader::Program &program,uint32_t objectReg,uint32_t cachedLength) {
        const auto address=shaderObject(program,objectReg,cachedLength);
        loadShader(program,address,cachedLength);
    }
    void borderColors() {
        if(!borderAddress) {
            // fd5_emit.c's 128-byte entries. Predefined Metal border colors
            // use only integer 0/1 encodings and are independent of RGBA/BGRA.
            uint32_t table[3][32]={};
            for(unsigned color=1;color<3;++color) {
                auto *entry=table[color];bool white=color==2;
                for(unsigned c=0;c<4;++c)if(white || c==3) {
                    entry[c]=0x3f800000;
                    entry[4+c/2]|=0xffffu<<((c&1)*16);entry[6+c/2]|=0x7fffu<<((c&1)*16);
                    entry[8+c/2]|=0x3c00u<<((c&1)*16);entry[16+c/2]|=0x3c00u<<((c&1)*16);
                    entry[12]|=0xffu<<(c*8);entry[13]|=0x7fu<<(c*8);
                }
                entry[10]=white?0xffffffff:0x80000000;entry[11]=white?0xffff:0xf000;
                entry[14]=white?0xffffffff:0xc0000000;entry[15]=white?0xffffff:0;
            }
            borderAddress=upload(&table[0][0],96,128); // next-power-of-two table alignment, 512 bytes
        }
        r(REG_A5XX_TPL1_TP_BORDER_COLOR_BASE_ADDR_LO,uint32_t(borderAddress));
        r(REG_A5XX_TPL1_TP_BORDER_COLOR_BASE_ADDR_HI,uint32_t(borderAddress>>32));
    }
    template<class S> void samplerState(const S &sampler,uint32_t stateBlock,unsigned slot,bool explicitLevel=false,bool cube=false) {
        if (slot>=QGPU_HARDWARE_SAMPLER_COUNT || !GPU::Shader::validSampler(sampler)) {status=GPU::Result::Invalid;return;}
        uint32_t mip=(sampler.filters>>2)&3,min=explicitLevel?0:sampler.minLOD,max=explicitLevel?4095:sampler.maxLOD;
        if (!mip) {if(min>32)min=32;if(max>32)max=32;}
        uint32_t a=sampler.maxAnisotropy>>1,aniso=a?32-__builtin_clz(a):0;
        // Mirror-clamp uses abs() in the production shader for NPOT as well
        // as POT textures; hardware only needs ordinary edge clamping.
        static const uint32_t wrap[]={1,1,0,2,3,3};
        uint32_t modes[]={GPU::Shader::addressMode(sampler,0),GPU::Shader::addressMode(sampler,1),GPU::Shader::addressMode(sampler,2)};
        uint32_t sample[7] = {slot|(stateBlock<<18)|(1u<<22),0,0,0,0,0,0};
        sample[3] = A5XX_TEX_SAMP_0_XY_MIN((sampler.filters&1)?(aniso?2:1):0) |
            A5XX_TEX_SAMP_0_XY_MAG((sampler.filters&2)?(aniso?2:1):0) | A5XX_TEX_SAMP_0_ANISO(aniso) |
            (mip==2?A5XX_TEX_SAMP_0_MIPFILTER_LINEAR_NEAR:0) |
            A5XX_TEX_SAMP_0_WRAP_S(wrap[modes[0]]) | A5XX_TEX_SAMP_0_WRAP_T(wrap[modes[1]]) | A5XX_TEX_SAMP_0_WRAP_R(wrap[modes[2]]);
        sample[4] = (cube?0:A5XX_TEX_SAMP_1_CUBEMAPSEAMLESSFILTOFF) | (sampler.normalized?0:A5XX_TEX_SAMP_1_UNNORM_COORDS) |
            A5XX_TEX_SAMP_1_COMPARE_FUNC((sampler.filters>>8)&7u) |
            ((max<<A5XX_TEX_SAMP_1_MAX_LOD__SHIFT)&A5XX_TEX_SAMP_1_MAX_LOD__MASK) |
            ((min<<A5XX_TEX_SAMP_1_MIN_LOD__SHIFT)&A5XX_TEX_SAMP_1_MIN_LOD__MASK);
        if(modes[0]>=GPU::Shader::ClampToZero || modes[1]>=GPU::Shader::ClampToZero) {
            borderColors();uint32_t color=modes[0]==GPU::Shader::ClampToZero || modes[1]==GPU::Shader::ClampToZero?0:sampler.borderColor;
            sample[5]=color<<A5XX_TEX_SAMP_2_BCOLOR_OFFSET__SHIFT;
        }
        p(0x30,sample,7);
    }
    void textureView(const GPU::Shader::Texture &texture,uint64_t address,uint32_t stateBlock,unsigned slot,bool fetch=false,bool tile=false) {
        GPU::Shader::TextureLayout layout;
        if (!address || !GPU::Shader::textureStorageLayout(texture,layout,false)) {status=GPU::Result::Invalid;return;}
        uint32_t values[15] = {slot|(stateBlock<<18)|(1u<<22),1,0};
        // Color space affects RGB sampling; layout and alpha are unchanged.
        auto channel=[&](unsigned i) {
            uint32_t c=GPU::Shader::swizzleChannel(texture.swizzle,i);
            if (c<GPU::Shader::ChannelRed) return c==GPU::Shader::ChannelOne?5u:4u;
            c-=GPU::Shader::ChannelRed;
            // A8 contains alpha only. Apply the logical view swizzle before
            // selecting its sole stored channel; RGB must sample as zero.
            if (texture.format==GPU::Shader::A8) return c==3?0u:4u;
            if (GPU::Shader::bgraFormat(texture.format) && (c==0 || c==2)) c^=2;
            if ((GPU::Shader::formatBytes(texture.format)<4 && c>=GPU::Shader::formatBytes(texture.format)) ||
                (texture.format==GPU::Shader::RG32Float && c>=2))return c==3?5u:4u;
            return c;
        };
        // Pinned Mesa fd5_format.c / a5xx.xml: TFMT5_8_UNORM and
        // TFMT5_16_16_16_16_FLOAT. No CPU conversion or normalized repacking.
        uint32_t format=GPU::Shader::compressedFormat(texture.format)?0xc1:texture.format==GPU::Shader::Depth32?0x4a:texture.format==GPU::Shader::RGBA16Float?0x62:texture.format==GPU::Shader::RGBA16Unorm?0x60:texture.format==GPU::Shader::RG32Float?0x67:
            texture.format==GPU::Shader::R8 || texture.format==GPU::Shader::A8?0x03:texture.format==GPU::Shader::RG8?0x0f:0x30;
        values[3] = A5XX_TEX_CONST_0_FMT(format) | A5XX_TEX_CONST_0_TILE_MODE(tile?2:0) | A5XX_TEX_CONST_0_MIPLVLS(layout.levels-1) | A5XX_TEX_CONST_0_SWIZ_X(channel(0)) |
            A5XX_TEX_CONST_0_SWIZ_Y(channel(1)) | A5XX_TEX_CONST_0_SWIZ_Z(channel(2)) | A5XX_TEX_CONST_0_SWIZ_W(channel(3)) |
            (GPU::Shader::srgbFormat(texture.format) ? A5XX_TEX_CONST_0_SRGB : 0)|
            A5XX_TEX_CONST_0_SAMPLES(texture.samples==4?2:texture.samples==2?1:0);
        values[4] = A5XX_TEX_CONST_1_WIDTH(texture.width) | A5XX_TEX_CONST_1_HEIGHT(texture.height);
        uint32_t pitchAlign=tile || texture.samples || layout.levels>1 || GPU::Shader::compressedFormat(texture.format)?uint32_t(__builtin_ctz(GPU::Shader::formatBytes(texture.format)*GPU::Shader::textureSamples(texture))):0;
        values[5] = A5XX_TEX_CONST_2_PITCH(texture.pitch) | A5XX_TEX_CONST_2_PITCHALIGN(pitchAlign) | A5XX_TEX_CONST_2_TYPE(texture.cube && !fetch?2:1); // Cube or 2D
        // Views preserve the owner's layer stride even when mip/slice ranges
        // are rebased. Native range checks cover only their actual access span.
        values[6] = A5XX_TEX_CONST_3_ARRAY_PITCH(layout.layerStride);
        values[7] = A5XX_TEX_CONST_4_BASE_LO(uint32_t(address));
        values[8] = A5XX_TEX_CONST_5_BASE_HI(uint32_t(address>>32)) | A5XX_TEX_CONST_5_DEPTH(fetch?GPU::Shader::textureSlices(texture):(texture.layers?texture.layers:1));
        p(0x30,values,15);
    }
    void constantSamplers(const GPU::Shader::Program &program,uint32_t block) {
        for(unsigned i=0;i<QGPU_MAX_CONSTANT_SAMPLERS;++i) {
            bool cube=program.constantCubeSamplerMask&(1u<<i);
            if(program.constantImplicitSamplerMask&(1u<<i))samplerState(program.constantSamplers[i],block,program.constantSamplerSlots[i],false,cube);
            if(program.constantExplicitSamplerMask&(1u<<i))samplerState(program.constantSamplers[i],block,program.constantSamplerLevelSlots[i],true,cube);
        }
    }
    void textures(const GPU::Shader::Program &program,const GPU::Shader::Texture *textures,const uint64_t *addresses,const GPU::Shader::Sampler *samplers,bool vertex=false) {
        uint32_t block=vertex?0:4;
        for (unsigned i=0;i<QGPU_STAGE_SAMPLER_COUNT;++i) {
            if(program.implicitSamplerMask&(1u<<i))samplerState(samplers[i],block,i,false,program.cubeSamplerMask&(1u<<i));
            if(program.explicitSamplerMask&(1u<<i))samplerState(samplers[i],block,i+QGPU_STAGE_SAMPLER_COUNT,true,program.cubeSamplerMask&(1u<<i));
        }
        constantSamplers(program,block);
        for (unsigned i=0;i<QGPU_STAGE_TEXTURE_COUNT;++i) {
            if(program.sampleMask&(1u<<i))textureView(textures[i],addresses[i],block,i);
            if(program.fetchMask&(1u<<i))textureView(textures[i],addresses[i],block,QGPU_FETCH_TEXTURE_BASE+i,true);
        }
        r(vertex?REG_A5XX_TPL1_VS_TEX_COUNT:REG_A5XX_TPL1_FS_TEX_COUNT,program.textureCount);
    }
    void vertexFetch(const GPU::Shader::Draw &d,const GPU::Shader::Program &vs,const ShaderBuffers &bindings) {
        unsigned n=0;
        for (unsigned i=0;i<QGPU_MAX_ATTRIBUTES;++i) if (vs.attributeMask&(1u<<i)) {
            const auto &a=d.attributes[i];const auto &l=d.layouts[a.buffer];auto f=GPU::Shader::vertexFormat(a.format);
            uint64_t address=bindings.addresses[a.buffer]+a.offset;
            r(REG_A5XX_VFD_FETCH_BASE_LO(n),uint32_t(address));r(REG_A5XX_VFD_FETCH_BASE_HI(n),uint32_t(address>>32));
            r(REG_A5XX_VFD_FETCH_SIZE(n),bindings.bytes[a.buffer]-a.offset);
            r(REG_A5XX_VFD_FETCH_STRIDE(n),l.step==0?0:l.stride);
            r(REG_A5XX_VFD_DECODE_INSTR(n),A5XX_VFD_DECODE_INSTR_IDX(n)|A5XX_VFD_DECODE_INSTR_FORMAT(f.hardware)|
                (l.step==2?A5XX_VFD_DECODE_INSTR_INSTANCED:0)|A5XX_VFD_DECODE_INSTR_SWAP(f.swap)|
                A5XX_VFD_DECODE_INSTR_UNK30|(f.type==0?A5XX_VFD_DECODE_INSTR_FLOAT:0));
            r(REG_A5XX_VFD_DECODE_STEP_RATE(n),l.step==2?l.rate:1);
            r(REG_A5XX_VFD_DEST_CNTL_INSTR(n),A5XX_VFD_DEST_CNTL_INSTR_WRITEMASK(vs.attributes[i].mask)|
                A5XX_VFD_DEST_CNTL_INSTR_REGID(vs.attributes[i].regid));++n;
        }
        // VTXCNT closes stale slots after a draw with more attributes.
        r(REG_A5XX_VFD_CONTROL_0,A5XX_VFD_CONTROL_0_VTXCNT(n));
    }
    static uint32_t blendFactor(uint32_t value, bool alpha) {
        // adreno_rb_blend_factor from the pinned register descriptions.
        static const uint8_t factors[] = {0,1,4,5,6,7,8,9,10,11,16,12,13,14,15};
        return alpha && value == GPU::Shader::SourceAlphaSaturated ? 1 : factors[value];
    }
    template<class S> static uint32_t blendControl(const S &s) {
        bool rgbMinMax = s.rgbOperation >= GPU::Shader::Min, alphaMinMax = s.alphaOperation >= GPU::Shader::Min;
        return A5XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR(rgbMinMax ? 1 : blendFactor(s.sourceRGB,false)) |
            A5XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR(rgbMinMax ? 1 : blendFactor(s.destinationRGB,false)) |
            A5XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE(s.rgbOperation) |
            A5XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR(alphaMinMax ? 1 : blendFactor(s.sourceAlpha,true)) |
            A5XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR(alphaMinMax ? 1 : blendFactor(s.destinationAlpha,true)) |
            A5XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE(s.alphaOperation);
    }
    static uint32_t roundShift(uint32_t value, uint32_t shift) {
        uint32_t result = value >> shift, remainder = value & ((1u << shift)-1), halfway = 1u << (shift-1);
        return result + (remainder > halfway || (remainder == halfway && (result & 1)));
    }
    static uint32_t unitHalf(uint32_t bits) {
        // IEEE float32 [0,1] -> binary16, round-to-nearest/ties-to-even.
        // No floating-point instructions or helpers may enter the Kext.
        int exponent = int((bits >> 23) & 255) - 112;
        uint32_t mantissa = bits & 0x7fffff;
        if (exponent < -10) return 0;
        if (exponent <= 0) return roundShift(mantissa | 0x800000,14-exponent);
        return (uint32_t(exponent) << 10) + roundShift(mantissa,13);
    }
    static uint32_t unitInteger(uint32_t bits, uint32_t scale) {
        uint32_t exponent = (bits >> 23) & 255;
        if (exponent < 119) return 0; // less than 1/256, including zero/denormals
        uint32_t product = ((bits & 0x7fffff) | 0x800000) * scale;
        uint32_t top = 31 - __builtin_clz(product);
        uint64_t rounded = top > 23 ? uint64_t(roundShift(product,top-23)) << (top-23) : product;
        return uint32_t(rounded >> (150-exponent));
    }
    void blendColor(const GPU::Shader::RenderState &s) {
        for (unsigned i=0;i<4;++i) {
            uint32_t bits = s.blendColor[i];
            r(REG_A5XX_RB_BLEND_RED+i*2,(unitHalf(bits) << 16) | (unitInteger(bits,127) << 8) | unitInteger(bits,255));
            r(REG_A5XX_RB_BLEND_RED_F32+i*2,bits);
        }
    }
    void depthStencil(const GPU::Shader::Draw &d,uint64_t depth,uint64_t stencil,const GPU::Shader::Program &fs) {
        const auto &state=d.depthStencil;
        uint32_t control=0;
        if (depth) {
            control=A5XX_RB_DEPTH_CNTL_ZFUNC(state.compare)|A5XX_RB_DEPTH_CNTL_Z_TEST_ENABLE|A5XX_RB_DEPTH_CNTL_Z_READ_ENABLE;
            if (state.write) control|=A5XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE;
        }
        r(REG_A5XX_RB_DEPTH_CNTL,control);r(REG_A5XX_GRAS_LRZ_CNTL,0);r(REG_A5XX_RB_ALPHA_CONTROL,0);
        r(REG_A5XX_RB_DEPTH_BUFFER_INFO,depth?A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(4):0);
        r(REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO,depth?A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(4):0);
        r(REG_A5XX_RB_DEPTH_BUFFER_BASE_LO,uint32_t(depth));r(REG_A5XX_RB_DEPTH_BUFFER_BASE_HI,uint32_t(depth>>32));
        r(REG_A5XX_RB_DEPTH_BUFFER_PITCH,depth?A5XX_RB_DEPTH_BUFFER_PITCH(d.depth.pitch):0);
        r(REG_A5XX_RB_DEPTH_BUFFER_ARRAY_PITCH,depth?A5XX_RB_DEPTH_BUFFER_ARRAY_PITCH(d.depth.pitch*d.depth.height):0);
        r(REG_A5XX_RB_STENCIL_INFO,stencil?A5XX_RB_STENCIL_INFO_SEPARATE_STENCIL:0);
        r(REG_A5XX_RB_STENCIL_BASE_LO,uint32_t(stencil));r(REG_A5XX_RB_STENCIL_BASE_HI,uint32_t(stencil>>32));
        r(REG_A5XX_RB_STENCIL_PITCH,stencil?A5XX_RB_STENCIL_PITCH(d.stencil.pitch):0);
        r(REG_A5XX_RB_STENCIL_ARRAY_PITCH,stencil?A5XX_RB_STENCIL_ARRAY_PITCH(d.stencil.pitch*d.stencil.height):0);
        const auto &f=state.front,&b=state.back;
        control=0;
        if (stencil && (f.enabled || b.enabled)) {
            // Both faces are enabled in hardware; an absent face is the
            // explicit ALWAYS/KEEP no-op descriptor from the Metal adapter.
            control=A5XX_RB_STENCIL_CONTROL_STENCIL_READ|A5XX_RB_STENCIL_CONTROL_STENCIL_ENABLE|A5XX_RB_STENCIL_CONTROL_STENCIL_ENABLE_BF|
                A5XX_RB_STENCIL_CONTROL_FUNC(f.enabled?f.compare:GPU::Shader::Always)|A5XX_RB_STENCIL_CONTROL_FAIL(f.enabled?f.fail:0)|
                A5XX_RB_STENCIL_CONTROL_ZFAIL(f.enabled?f.depthFail:0)|A5XX_RB_STENCIL_CONTROL_ZPASS(f.enabled?f.pass:0)|
                A5XX_RB_STENCIL_CONTROL_FUNC_BF(b.enabled?b.compare:GPU::Shader::Always)|A5XX_RB_STENCIL_CONTROL_FAIL_BF(b.enabled?b.fail:0)|
                A5XX_RB_STENCIL_CONTROL_ZFAIL_BF(b.enabled?b.depthFail:0)|A5XX_RB_STENCIL_CONTROL_ZPASS_BF(b.enabled?b.pass:0);
        }
        r(REG_A5XX_RB_STENCIL_CONTROL,control);
        r(REG_A5XX_RB_STENCILREFMASK,stencil?A5XX_RB_STENCILREFMASK_STENCILREF(state.frontReference)|
            A5XX_RB_STENCILREFMASK_STENCILMASK(f.readMask)|A5XX_RB_STENCILREFMASK_STENCILWRITEMASK(f.enabled?f.writeMask:0):0);
        r(REG_A5XX_RB_STENCILREFMASK_BF,stencil?A5XX_RB_STENCILREFMASK_BF_STENCILREF(state.backReference)|
            A5XX_RB_STENCILREFMASK_BF_STENCILMASK(b.readMask)|A5XX_RB_STENCILREFMASK_BF_STENCILWRITEMASK(b.enabled?b.writeMask:0):0);
        const uint32_t depthFlags[3]={},lrz[5]={};
        rr(REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE_LO,depthFlags);
        rr(REG_A5XX_GRAS_LRZ_BUFFER_BASE_LO,lrz);
        // Late depth testing preserves pre-discard stores and shader depth.
        uint32_t plane=(fs.flags&(2u|QGPU_FRAGMENT_FLAGS))?A5XX_RB_DEPTH_PLANE_CNTL_FRAG_WRITES_Z|
            (fs.fragMask?A5XX_RB_DEPTH_PLANE_CNTL_UNK1:0):0;
        r(REG_A5XX_RB_DEPTH_PLANE_CNTL,plane);r(REG_A5XX_GRAS_SU_DEPTH_PLANE_CNTL,plane);
    }
public:
    static void *operator new(__SIZE_TYPE__,void *storage) noexcept {return storage;}
    uint32_t sequence() const {return flushSequence;}
    uint64_t copiedFramebufferBytes() const {return snapshotBytes;}
    uint64_t uncroppedFramebufferBytes() const {return snapshotUncroppedBytes;}
    uint64_t copiedFramebuffer2DBytes() const {return snapshot2DBytes;}
    uint64_t framebufferPrimitives() const {return snapshotPrimitives;}
    uint64_t inplaceFramebufferPrimitives() const {return inplacePrimitives;}
    uint64_t reusedShaderObjects() const {return shaderObjectHits;}
    uint64_t avoidedShaderObjectBytes() const {return shaderObjectSavedBytes;}
    uint64_t fullRasterSetups() const {return rasterFullSetups;}
    uint64_t reusedRasterPrimitives() const {return rasterReusedPrimitives;}
    uint64_t updatedVertexDriverConstants() const {return rasterDriverUpdates;}
    void enableFramebuffer2D(bool enabled=true) {framebuffer2D=enabled;}
    void enableFramebufferXCrop(bool enabled=true) {framebufferXCrop=enabled;}
    void enableFramebufferWord32(bool enabled=true) {framebufferWord32=enabled;}
    void enableFramebufferInplace(bool enabled=true) {framebufferInplace=enabled;}
    void enableFramebufferCompact(bool enabled=true) {framebufferCompact=enabled;}
    void enableFramebufferStateReuse(bool enabled=true) {framebufferStateReuse=enabled;}
    GPU::Result flushShaderWrites() {
        // Invalidation in restore() does not drain preceding SSBO/image writes.
        if(scratchBytes<GPU::PageBytes || !deviceRange(completionScratch,GPU::PageBytes))return GPU::Result::Invalid;
        flush(4,2);return status;
    }
    uint64_t address() const {return ib;}
    void beginOperation(uint64_t address) {ib=address;borderAddress=0;forgetShaderObjects();}
    // The callback only encodes already-resolved, retained resources. Try one
    // existing IB without letting a draw seal it; a failed trial can therefore
    // restore BOTH command/data cursors and all private pipeline accounting.
    // Only the retry may split a large draw across IBs. No work is posted here.
    // A diagnostic operation is sampled only if its complete encoding fits
    // the current IB. Otherwise retry the ordinary path with exact pretrial
    // state; large/multi-IB draws are explicitly skipped, never truncated.
    void profileNextOperation(unsigned index,bool counters=false) {
        operationCounters=counters && !gmem;
        operationTimestamp=!gmem && index<GPU::MaxCommands && scratchBytes>=GPU::PageBytes &&
            deviceRange(completionScratch,GPU::PageBytes)?completionScratch+128+uint64_t(index)*16:0;
    }
    bool lastOperationTimed() const {return operationTimed;}
    bool lastOperationCounters() const {return operationTimed && operationCounters;}
    uint32_t lastConstantWindowState() const {return operationWindowState;}
    template<class Encode> GPU::Result operation(IndirectBufferAllocator *allocator,bool compact,Encode encode) {
        operationTimed=false;
        operationWindowState=0;
        const auto address=operationTimestamp;operationTimestamp=0;
        // Run diagnostic trials INSIDE ordinary compact admission. If the
        // uninstrumented operation itself requires the next IB, the ordinary
        // retry may sample it there. Never switch IB solely to get a sample.
        return ordinaryOperation(allocator,compact,[&](IndirectBufferAllocator *chunks) {
            return address?profiledOperation(address,chunks,encode):encode(chunks);
        });
    }
    template<class Encode> GPU::Result profiledOperation(uint64_t address,IndirectBufferAllocator *allocator,Encode encode) {
        if(status!=GPU::Result::Success)return status;
        const auto point=e.checkpoint();
        const auto oldIB=ib,oldBorder=borderAddress,oldBytes=snapshotBytes,oldPrimitives=snapshotPrimitives;
        const auto old2D=snapshot2DBytes,oldUncropped=snapshotUncroppedBytes,oldInplace=inplacePrimitives;
        const auto oldSequence=flushSequence,oldNext=nextShaderObject;
        const auto oldHits=shaderObjectHits,oldSaved=shaderObjectSavedBytes;
        const auto oldFull=rasterFullSetups,oldReused=rasterReusedPrimitives,oldDriver=rasterDriverUpdates;
        for(unsigned i=0;i<16;++i)profileShaderObjects[i]=shaderObjects[i];
        profileNextShaderObject=oldNext;
        if(operationCounters)shaderCounters(true);
        timestamp(address);auto result=status==GPU::Result::Success?encode(nullptr):status;
        if(result==GPU::Result::Success){timestamp(address+8);if(operationCounters)shaderCounters(false);result=status;}
        if(result!=GPU::Result::NoResources){operationTimed=result==GPU::Result::Success;return result;}
        e.rewind(point);ib=oldIB;borderAddress=oldBorder;status=GPU::Result::Success;
        snapshotBytes=oldBytes;snapshotPrimitives=oldPrimitives;snapshot2DBytes=old2D;
        snapshotUncroppedBytes=oldUncropped;inplacePrimitives=oldInplace;flushSequence=oldSequence;
        shaderObjectHits=oldHits;shaderObjectSavedBytes=oldSaved;
        rasterFullSetups=oldFull;rasterReusedPrimitives=oldReused;rasterDriverUpdates=oldDriver;
        for(unsigned i=0;i<16;++i)shaderObjects[i]=profileShaderObjects[i];
        nextShaderObject=profileNextShaderObject;
        return encode(allocator);
    }
    template<class Encode> GPU::Result ordinaryOperation(IndirectBufferAllocator *allocator,bool compact,Encode encode) {
        if(!compact || !allocator)return encode(allocator);
        if(status!=GPU::Result::Success)return status;
        const auto point=e.checkpoint();
        const auto oldIB=ib,oldBorder=borderAddress,oldBytes=snapshotBytes,oldPrimitives=snapshotPrimitives;
        const auto old2D=snapshot2DBytes,oldUncropped=snapshotUncroppedBytes,oldInplace=inplacePrimitives;
        const auto oldSequence=flushSequence;
        const auto oldShaderHits=shaderObjectHits,oldShaderBytes=shaderObjectSavedBytes;
        const auto oldRasterFull=rasterFullSetups,oldRasterReused=rasterReusedPrimitives,oldRasterDriver=rasterDriverUpdates;
        auto result=encode(nullptr);
        if(result!=GPU::Result::NoResources)return result;
        // No allocator was available during the trial, so the backing cannot
        // have changed. Upload bytes beyond the restored tail are unreachable.
        e.rewind(point);ib=oldIB;borderAddress=oldBorder;status=GPU::Result::Success;
        // Some cached objects may have been uploaded into the rolled-back
        // tail. Drop all lookup entries rather than retain an invalid address.
        forgetShaderObjects();shaderObjectHits=oldShaderHits;shaderObjectSavedBytes=oldShaderBytes;
        rasterFullSetups=oldRasterFull;rasterReusedPrimitives=oldRasterReused;rasterDriverUpdates=oldRasterDriver;
        snapshotBytes=oldBytes;snapshotPrimitives=oldPrimitives;snapshot2DBytes=old2D;
        snapshotUncroppedBytes=oldUncropped;inplacePrimitives=oldInplace;flushSequence=oldSequence;
        if(point.count) {
            result=allocator->next(e,ib);
            if(result!=GPU::Result::Success)return result;
            borderAddress=0;
        }
        return encode(allocator);
    }
    PipelineEncoder(Encoder &encoder,uint64_t address,uint64_t scratch=0,uint32_t bytes=GPU::PageBytes,bool direct=false) :
        e(encoder),ib(address),completionScratch(scratch),scratchBytes(bytes),directShaders(direct) {}
    void renderInGMEM(const GMEMLayout &layout){gmem=&layout;}
    GPU::Result clearRows(uint64_t address,uint32_t bytes,const uint32_t (&values)[8],bool pairs) {
        if(!hardwareClearEligible(address,bytes,values,pairs) || scratchBytes<GPU::PageBytes ||
            !deviceRange(completionScratch,GPU::PageBytes))return GPU::Result::Invalid;
        // A5xx CP_BLIT solid fill uses the same linear 2D destination path
        // as snapshot2D, with explicit pixel bounds instead of GMEM resolve.
        const uint32_t width=values[0]/(pairs?8:4),height=values[1],pitch=values[2];
        const uint32_t format=pairs?104:75; // RB5_R32G32_UINT / RB5_R32_UINT
        restore();event(25);event(24);idle();
        r(REG_A5XX_RB_2D_BLIT_CNTL,0);r(REG_A5XX_GRAS_2D_BLIT_CNTL,0);
        r(REG_A5XX_UNKNOWN_2184,1);r(REG_A5XX_RB_MODE_CNTL,4);r(REG_A5XX_SP_MODE_CNTL,12);
        r(REG_A5XX_TPL1_MODE_CNTL,0x344);r(REG_A5XX_HLSQ_MODE_CNTL,2);r(REG_A5XX_GRAS_CL_CNTL,0x181);
        uint32_t mode=5;p(0x6c,&mode,1); // BLIT2D
        const uint32_t color[]={values[pairs?3:4],pairs?values[4]:0,0,0};
        rr(REG_A5XX_RB_2D_SRC_SOLID_DW0,color);
        r(REG_A5XX_RB_2D_SRC_INFO,format);r(REG_A5XX_GRAS_2D_SRC_INFO,format);
        r(REG_A5XX_RB_2D_DST_INFO,format);r(REG_A5XX_GRAS_2D_DST_INFO,format);
        r(REG_A5XX_RB_2D_DST_LO,uint32_t(address));r(REG_A5XX_RB_2D_DST_HI,uint32_t(address>>32));
        // The packed 2D pitch fields require an even 64-byte layer stride.
        // An odd stride sets bit 16 and adds 4 MiB to each row address.
        const uint32_t layerStride=(pitch*height+127)&~127u;
        r(REG_A5XX_RB_2D_DST_SIZE,A5XX_RB_2D_DST_SIZE_PITCH(pitch)|A5XX_RB_2D_DST_SIZE_ARRAY_PITCH(layerStride));
        for(unsigned i=4;i<9;++i)r(REG_A5XX_RB_2D_DST_INFO+i,0);
        const uint32_t end=(width-1)|((height-1)<<16),fill[]={0,0,end,0,end};p(0x2c,fill,5);
        mode=8;p(0x6c,&mode,1);idle(); // END2D
        flush(29,0); // Publish color writes before compute or texture reads.
        return status;
    }
    GPU::Result dispatch(const GPU::Shader::Dispatch &d, const GPU::Shader::Program &program, const ShaderBuffers &bindings) {
        uint64_t elements=0;unsigned dimensions=0;
        if (!GPU::Shader::validInterface(program) || !GPU::Shader::dispatchFootprint(d,program,elements,dimensions)) return GPU::Result::Invalid;
        GPU::Shader::DispatchTile tiles[8];uint32_t tileCount=0;
        if (!GPU::Shader::dispatchTiles(d,tiles,tileCount)) return GPU::Result::Invalid;
        uint32_t memory[QGPU_MEMORY_CONSTANT_WORDS]={},group[QGPU_THREADGROUP_CONSTANT_WORDS],images[QGPU_IMAGE_CONSTANT_WORDS],total;
        for(unsigned i=0;i<QGPU_BUFFER_SIZE_CONSTANT_WORDS;++i)memory[QGPU_BUFFER_SIZE_CONSTANT_BASE+i]=bindings.bytes[i];
        if (!GPU::Shader::threadgroupFootprint(d,program,group,total) || !GPU::Shader::imageFootprint(d,program,images)) return GPU::Result::Invalid;
        for(unsigned i=0;i<QGPU_THREADGROUP_CONSTANT_WORDS;++i)memory[i]=group[i];
        for(unsigned i=0;i<QGPU_IMAGE_CONSTANT_WORDS;++i)memory[QGPU_IMAGE_CONSTANT_BASE+i]=images[i];
        for(unsigned i=0;i<QGPU_COMPUTE_IMAGE_COUNT;++i)if((program.imageMask|program.sampleMask)&(1u<<i)) {
            GPU::Shader::TextureLayout layout;if(!GPU::Shader::textureStorageLayout(d.images[i],layout,false))return GPU::Result::Invalid;
            memory[QGPU_ARRAY_CONSTANT_BASE+i*2]=d.images[i].cube?GPU::Shader::textureSlices(d.images[i]):d.images[i].layers;
            memory[QGPU_ARRAY_CONSTANT_BASE+i*2+1]=layout.layerStride;
        }
        for(unsigned i=0;i<QGPU_COMPUTE_IMAGE_COUNT;++i)if(program.samplerMask&(1u<<i))memory[QGPU_SAMPLER_CONSTANT_BASE+i]=GPU::Shader::samplerInfoFlags(d.samplers[i]);
        restore(); r(REG_A5XX_SP_SP_CNTL,0);
        r(REG_A5XX_HLSQ_CONTROL_0_REG,A5XX_HLSQ_CONTROL_0_REG_CSTHREADSIZE(program.threadSize)|0x880);
        r(REG_A5XX_SP_VS_CONFIG,0); r(REG_A5XX_SP_FS_CONFIG,0);
        r(REG_A5XX_HLSQ_VS_CONFIG,0); r(REG_A5XX_HLSQ_FS_CONFIG,0);
        r(REG_A5XX_HLSQ_VS_CONSTLEN,0); r(REG_A5XX_HLSQ_FS_CONSTLEN,0);
        r(REG_A5XX_HLSQ_VS_INSTRLEN,0); r(REG_A5XX_HLSQ_FS_INSTRLEN,0);
        r(REG_A5XX_HLSQ_CS_CONFIG,A5XX_HLSQ_CS_CONFIG_ENABLED);
        r(REG_A5XX_SP_CS_CONFIG,A5XX_SP_CS_CONFIG_ENABLED);
        uint32_t cached=program.instrlen<=32?program.instrlen:0;
        r(REG_A5XX_SP_CS_CTRL_REG0,0x2|(cached?A5XX_SP_CS_CTRL_REG0_BUFFER:0)|
            A5XX_SP_CS_CTRL_REG0_THREADSIZE(program.threadSize)|A5XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(program.fullRegs)|
            A5XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(program.halfRegs)|A5XX_SP_CS_CTRL_REG0_BRANCHSTACK(program.branchStack));
        r(REG_A5XX_HLSQ_CS_CNTL,A5XX_HLSQ_CS_CNTL_INSTRLEN(cached)|
            ((program.bufferMask || program.imageMask) ? A5XX_HLSQ_CS_CNTL_SSBO_ENABLE : 0));
        r(REG_A5XX_HLSQ_CS_CONSTLEN,program.constlen/4); r(REG_A5XX_HLSQ_CS_INSTRLEN,cached);
        const auto shaderAddress=shaderObject(program,REG_A5XX_SP_CS_OBJ_START_LO,cached);
        r(REG_A5XX_HLSQ_UPDATE_CNTL,0x1f00000);
        r(REG_A5XX_HLSQ_CS_CNTL_0,A5XX_HLSQ_CS_CNTL_0_WGIDCONSTID(program.groupID)|
            A5XX_HLSQ_CS_CNTL_0_UNK0(252)|A5XX_HLSQ_CS_CNTL_0_UNK1(252)|A5XX_HLSQ_CS_CNTL_0_LOCALIDREGID(program.localID));
        // A5xx uses the fixed workgroup configuration from fd5_compute.c;
        // its 32 KiB local store has no A6xx-style SHARED_SIZE register.
        // Admission bounds the static prefix plus every dynamic allocation.
        r(REG_A5XX_HLSQ_CS_CNTL_1,1);
        // fd5_compute.c updates the CS allocation before loading code and
        // descriptors. Do not issue another update after those state loads.
        loadShader(program,shaderAddress,cached);
        const uint32_t bufferMask=program.bufferMask|(program.imageMask<<QGPU_COMPUTE_IMAGE_BASE);
        buffers(true,bufferMask,bindings,(program.flags&QGPU_BUFFER_SIZE_CONSTANTS)?bufferMask:0);
        for(unsigned i=0;i<QGPU_COMPUTE_IMAGE_COUNT;++i) {
            if(program.implicitSamplerMask&(1u<<i))samplerState(d.samplers[i],5,i,false,program.cubeSamplerMask&(1u<<i));
            if(program.explicitSamplerMask&(1u<<i))samplerState(d.samplers[i],5,i+8,true,program.cubeSamplerMask&(1u<<i));
            if(program.sampleMask&(1u<<i))textureView(d.images[i],bindings.addresses[QGPU_COMPUTE_IMAGE_BASE+i],5,i);
        }
        constantSamplers(program,5);
        r(REG_A5XX_TPL1_CS_TEX_COUNT,program.textureCount);
        // Code, descriptors and resources are shared by all regions. Each
        // region launches only real threads, so barriers need no masked lanes.
        for (uint32_t region=0;region<tileCount;++region) {
        const auto &tile=tiles[region];uint32_t geometry[QGPU_DISPATCH_CONSTANT_WORDS];
        GPU::Shader::dispatchConstants(d,tile,geometry);
        for (unsigned i=0;i<QGPU_DISPATCH_CONSTANT_WORDS;++i) memory[QGPU_DISPATCH_CONSTANT_BASE+i]=geometry[i];
        uint32_t driver[16] = {tile.groups[0],tile.groups[1],tile.groups[2],dimensions,0,0,0,program.subgroupSize,
            tile.local[0],tile.local[1],tile.local[2],0,0,0,0,0};
        driver[11] = program.subgroupSize ? 31-__builtin_clz(program.subgroupSize) : 0;
        constants(program,driver,16,memory);
        r(REG_A5XX_HLSQ_CS_NDRANGE_0,A5XX_HLSQ_CS_NDRANGE_0_KERNELDIM(dimensions)|
            A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX(tile.local[0]-1)|A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY(tile.local[1]-1)|
            A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ(tile.local[2]-1));
        r(REG_A5XX_HLSQ_CS_NDRANGE_1,tile.groups[0]*tile.local[0]); r(REG_A5XX_HLSQ_CS_NDRANGE_2,0);
        r(REG_A5XX_HLSQ_CS_NDRANGE_3,tile.groups[1]*tile.local[1]); r(REG_A5XX_HLSQ_CS_NDRANGE_4,0);
        r(REG_A5XX_HLSQ_CS_NDRANGE_5,tile.groups[2]*tile.local[2]); r(REG_A5XX_HLSQ_CS_NDRANGE_6,0);
        r(REG_A5XX_HLSQ_CS_KERNEL_GROUP_X,1); r(REG_A5XX_HLSQ_CS_KERNEL_GROUP_Y,1); r(REG_A5XX_HLSQ_CS_KERNEL_GROUP_Z,1);
        uint32_t exec[] = {0,tile.groups[0],tile.groups[1],tile.groups[2]}; p(0x33,exec,4); idle();
        }
        return status;
    }
    GPU::Result draw(const GPU::Shader::Draw &d, const GPU::Shader::Program &vs,
        const GPU::Shader::Program &fragment, const ShaderBuffers &bindings, uint64_t target, const GraphicsTextures &sampled,
        uint64_t indices=0,uint64_t depth=0,uint64_t stencil=0,const uint64_t *additionalTargets=nullptr,
        const GPU::Shader::Program *snapshotProgram=nullptr,const void *indexSnapshot=nullptr,bool primitiveSnapshot=false,
        IndirectBufferAllocator *allocator=nullptr) {
        const auto &fs=GPU::Shader::rasterProgram(fragment,d.rasterSamples);
        if(!constantWindowRequirements(fs,bindings,completionScratch,scratchBytes))return GPU::Result::Invalid;
        auto &context=workspace.context;context={};auto &linkage=context.linkage;
        auto &width=context.width;auto &height=context.height;uint32_t vertexBindings=0;
        if (!GPU::Shader::validDraw(d) || !GPU::Shader::link(vs,fs,linkage) ||
            (fs.bufferMask && (fs.flags&(QGPU_BOUNDED_BUFFERS|QGPU_FRAGMENT_BUFFER_BANK))!=(QGPU_BOUNDED_BUFFERS|QGPU_FRAGMENT_BUFFER_BANK)) ||
            !GPU::Shader::validAttachments(d,width,height) || !GPU::Shader::vertexLayout(vs,d,vertexBindings) || bool(d.target.resource)!=bool(target) ||
            !d.scissor[2] || !d.scissor[3] || !GPU::range(width,d.scissor[0],d.scissor[2]) || !GPU::range(height,d.scissor[1],d.scissor[3]) ||
            bool(d.depth.resource)!=bool(depth) || bool(d.stencil.resource)!=bool(stencil) ||
            (d.indexSize ? !deviceRange(indices,d.indices.bytes) : indices != 0) ||
            !GPU::Shader::graphicsTextures(vs,d.vertexTextures,d.vertexSamplers) ||
            !GPU::Shader::graphicsTextures(fs,d.textures,d.samplers)) return GPU::Result::Invalid;
        auto *targets=context.targets;targets[0]=target;
        auto &targetMask=context.targetMask;auto &targetCount=context.targetCount;
        auto &components=context.components;auto &blendMask=context.blendMask;
        targetMask=GPU::Shader::colorTargetMask(d);targetCount=targetMask?32u-__builtin_clz(targetMask):0;
        for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i) {
            if(i)targets[i]=additionalTargets?additionalTargets[i-1]:0;
            const auto &t=GPU::Shader::colorTarget(d,i);uint64_t bytes=0;
            if(t.resource ? (!targets[i] || !GPU::Shader::textureBytes(t,bytes) || !deviceRange(targets[i],bytes)) : targets[i]!=0)return GPU::Result::Invalid;
            if((targetMask&fs.colorMask)&(1u<<i)) {
                auto color=GPU::Shader::colorState(d,i);components|=15u<<(4*i);
                if(color.blend)blendMask|=1u<<i;
            }
        }
        const GPU::Shader::Texture *stageTextures[]={d.vertexTextures,d.textures};
        const uint64_t *stageAddresses[]={sampled.vertex,sampled.fragment};
        for (unsigned stage=0;stage<2;++stage)for(unsigned i=0;i<QGPU_STAGE_TEXTURE_COUNT;++i) {
            const auto &t=stageTextures[stage][i];uint64_t bytes=0,address=stageAddresses[stage][i];
            if(t.resource ? (!address || !GPU::Shader::textureBytes(t,bytes) || !deviceRange(address,bytes)) : address!=0)return GPU::Result::Invalid;
        }
        if ((completionScratch&127) || !deviceRange(completionScratch,128)) return GPU::Result::Invalid;
        if(gmem) {
            if(!gmemPrograms(vs,fs))return GPU::Result::Invalid;
            context.readOnlyFeedback=fs.framebufferMask!=0;
            // This whole pass has no shader writes. Root clear/restore and
            // each preceding draw already completed their color flush; the
            // normal raster invalidation makes GMEM attachment reads fresh.
            return raster(d,vs,fs,bindings,sampled,indices,depth,stencil,context,false,true);
        }
        auto &snapshots=context.snapshots;
        FramebufferStorage storage;
        if(!framebufferStoragePlan(d,fs,framebufferInplace,primitiveSnapshot,storage,framebufferCompact) || storage.bytes>scratchBytes ||
            !deviceRange(completionScratch,storage.bytes))return GPU::Result::Invalid;
        snapshots=storage.snapshots;context.inplace=storage.inplace;context.readOnlyFeedback=storage.readOnly;
        if(context.readOnlyFeedback) {
            // Publish preceding color/shader writes, including memoryless
            // attachment producers in this command buffer. raster() keeps
            // its normal texture invalidation and completion flushes.
            flush(29,0);if(depth || stencil)flush(28,1);flush(4,2);
            auto result=raster(d,vs,fs,bindings,sampled,indices,depth,stencil,context,false,true);
            if(result==GPU::Result::Success)++inplacePrimitives;
            return result;
        }
        if(fs.framebufferMask && !primitiveSnapshot) {
            if(!snapshotProgram || snapshotProgram->stage!=GPU::Shader::Compute || snapshotProgram->bufferMask!=3 ||
                snapshotProgram->writeMask!=2 || snapshotProgram->strides[0]!=4 || snapshotProgram->strides[1]!=4 ||
                (d.indexSize && !indexSnapshot))return GPU::Result::Invalid;
            // Snapshot fallback copies before each primitive. The opt-in
            // in-place path still splits EVERY primitive and instance, and
            // retains the color/UCHE flush timestamps plus matching waits.
            // raster() invalidates UCHE before binding the same-pixel view.
            // It is not permission to issue overlapping primitives together.
            flush(29,0);if(depth || stencil)flush(28,1);flush(4,2);
            bool reusable=false;
            for(uint32_t instance=0;instance<d.instances && status==GPU::Result::Success;++instance) {
                GPU::Shader::PrimitiveIterator iterator(d,indexSnapshot);GPU::Shader::Primitive primitive;
                while(iterator.next(primitive) && status==GPU::Result::Success) {
                  bool retried=false;
                  for(;;) {
                    const auto checkpoint=e.checkpoint();const auto previousBorder=borderAddress;
                    const auto previousShaderHits=shaderObjectHits,previousShaderBytes=shaderObjectSavedBytes;
                    const auto previousRasterFull=rasterFullSetups,previousRasterReused=rasterReusedPrimitives,previousRasterDriver=rasterDriverUpdates;
                    const auto previousBytes=snapshotBytes,previousPrimitives=snapshotPrimitives,previous2DBytes=snapshot2DBytes,previousUncropped=snapshotUncroppedBytes,previousInplace=inplacePrimitives;
                    for(unsigned slot=0;slot<QGPU_COLOR_TARGET_COUNT && status==GPU::Result::Success;++slot)if(!context.inplace && (fs.framebufferMask&(1u<<slot))) {
                        const auto &texture=GPU::Shader::colorTarget(d,slot);FramebufferCopyRegion region;
                        if(!framebufferCopyRegion(d,texture,region)){status=GPU::Result::Invalid;break;}
                        const uint32_t bytes=region.bytes;uint32_t covered=0,copied2D=0;
                        auto &rectangles=workspace.rectangles;
                        const uint64_t src=targets[slot]+region.offset,dst=completionScratch+snapshots.offsets[slot]+region.offset;
                        bool use2D=framebuffer2D && framebufferXCrop &&
                            framebufferScissor2DRegions(src,dst,d,texture,region,rectangles,covered,copied2D);
                        if(!use2D && framebuffer2D && d.rasterSamples<=1 && framebuffer2DRegions(src,dst,bytes,rectangles)) {
                            for(const auto &rectangle:rectangles)covered+=rectangle.pitch*rectangle.rows;
                            copied2D=covered;use2D=true;
                        }
                        if(use2D) {
                            snapshot2D(targets[slot]+region.offset,completionScratch+snapshots.offsets[slot]+region.offset,rectangles);
                            if(status!=GPU::Result::Success)continue;
                            snapshot2DBytes+=copied2D;
                            if(covered==bytes){snapshotBytes+=copied2D;snapshotUncroppedBytes+=bytes;continue;}
                        }
                        const uint32_t tail=bytes-covered;
                        auto &copy=workspace.copy;copy={};copy.registry=d.registry;copy.version=d.version;copy.program=snapshotProgram->id;
                        copy.groups[0]=8;copy.groups[1]=uint32_t((tail/4+255)/256);copy.groups[2]=1;
                        copy.local[0]=32;copy.local[1]=copy.local[2]=1;
                        copy.buffers[0]={1,0,tail,0};copy.buffers[1]={2,0,tail,0};
                        auto &memory=workspace.copyBindings;__builtin_memset(&memory,0,sizeof(memory));memory.addresses[0]=src+covered;memory.addresses[1]=dst+covered;
                        memory.bytes[0]=memory.bytes[1]=tail;
                        status=dispatch(copy,*snapshotProgram,memory);
                        if(status==GPU::Result::Success){snapshotBytes+=copied2D+tail;snapshotUncroppedBytes+=bytes;}
                    }
                    if(status==GPU::Result::Success) {
                    // In-place feedback has no copy/dispatch between the
                    // initial (or prior primitive's) completed UCHE flush and
                    // this draw. Snapshot fallback still requires its post-
                    // copy flush. Keep raster's full invalidation and its
                    // color/UCHE timestamp waits after EVERY primitive.
                    if(!context.inplace)flush(4,2);
                    auto &part=workspace.part;part=d;part.count=primitive.count;part.instances=1;part.baseInstance+=instance;
                    part.state.winding^=primitive.windingXor;
                    uint64_t firstIndex=indices;
                    if(d.indexSize) {part.indices.offset+=primitive.first*d.indexSize;part.indices.bytes=primitive.count*d.indexSize;firstIndex+=uint64_t(primitive.first)*d.indexSize;}
                    else part.start+=primitive.first;
                    // The initial/prior-primitive flush (in-place), or the
                    // post-copy flush above, has published every preceding
                    // shader write. Only state setup occurs before constants.
                    // This proof is local to this invocation, never cached
                    // across draws, operations or a rolled-back trial.
                    status=raster(part,vs,fs,bindings,sampled,firstIndex,depth,stencil,context,reusable,true);
                    if(status==GPU::Result::Success)reusable=context.inplace && framebufferStateReuse;
                    if(status==GPU::Result::Success){if(context.inplace)++inplacePrimitives;else ++snapshotPrimitives;}
                    }
                    if(status!=GPU::Result::NoResources || !allocator)break;
                    // A primitive and its copies are indivisible. Roll back
                    // command head and uploaded data before sealing the IB.
                    // All buffers remain unposted until complete admission.
                    e.rewind(checkpoint);borderAddress=previousBorder;status=GPU::Result::Success;
                    forgetShaderObjects();shaderObjectHits=previousShaderHits;shaderObjectSavedBytes=previousShaderBytes;
                    rasterFullSetups=previousRasterFull;rasterReusedPrimitives=previousRasterReused;rasterDriverUpdates=previousRasterDriver;
                    reusable=false;
                    snapshotBytes=previousBytes;snapshotPrimitives=previousPrimitives;snapshot2DBytes=previous2DBytes;snapshotUncroppedBytes=previousUncropped;
                    inplacePrimitives=previousInplace;
                    if(retried || !checkpoint.count){status=GPU::Result::NoResources;break;}
                    status=allocator->next(e,ib);if(status!=GPU::Result::Success)break;
                    borderAddress=0;forgetShaderObjects();retried=true; // immutable uploads belong to the old IB
                    // Keep flushSequence: repeated values could satisfy a
                    // wait using an earlier buffer's flush timestamp.
                  }
                }
            }
            return status;
        }
        return raster(d,vs,fs,bindings,sampled,indices,depth,stencil,context);
    }
private:
    // Common attachment/link validation is performed once for the whole draw.
    // Keep raster state emission separate from primitive expansion: recursive
    // draw() calls unnecessarily duplicated its multi-kilobyte stack frame.
    GPU::Result raster(const GPU::Shader::Draw &d,const GPU::Shader::Program &vs,const GPU::Shader::Program &fs,
        const ShaderBuffers &bindings,const GraphicsTextures &sampled,uint64_t indices,uint64_t depth,uint64_t stencil,
        const RasterContext &context,bool reuse=false,bool shaderWritesPublished=false) {
        const auto &linkage=context.linkage;const auto &snapshots=context.snapshots;const auto *targets=context.targets;
        uint32_t width=context.width,height=context.height,targetMask=context.targetMask,targetCount=context.targetCount;
        uint32_t components=context.components,blendMask=context.blendMask;
        auto &vertexParams=workspace.vertexParams;
        __builtin_memset(vertexParams,0,sizeof(vertexParams));
        vertexParams[1] = d.indexSize ? uint32_t(d.baseVertex) : d.start;
        vertexParams[2] = d.baseInstance; vertexParams[4] = d.indexSize ? UINT32_MAX : 0;
        if(!reuse) {
        restore(); event(25);event(24); // PC_CCU_INVALIDATE_COLOR / DEPTH
        r(REG_A5XX_SP_CS_CONFIG,0); r(REG_A5XX_HLSQ_CS_CONFIG,0);
        r(REG_A5XX_HLSQ_CS_CNTL,0); r(REG_A5XX_HLSQ_CS_CONSTLEN,0); r(REG_A5XX_HLSQ_CS_INSTRLEN,0);
        // Match fd5_program.c:64 cache units shared by VS/FS; prefer FS.
        uint32_t vertexCached=vs.instrlen,fragmentCached=fs.instrlen;
        if(vertexCached+fragmentCached>64) {
            if(fragmentCached<64)vertexCached=0;
            else if(vertexCached<64)fragmentCached=0;
            else vertexCached=fragmentCached=0;
        }
        r(REG_A5XX_HLSQ_VS_CONFIG,A5XX_HLSQ_VS_CONFIG_ENABLED);
        r(REG_A5XX_SP_VS_CONFIG,A5XX_SP_VS_CONFIG_ENABLED);
        r(REG_A5XX_HLSQ_FS_CONFIG,A5XX_HLSQ_FS_CONFIG_ENABLED|A5XX_HLSQ_FS_CONFIG_CONSTOBJECTOFFSET(vs.constlen/4)|
            A5XX_HLSQ_FS_CONFIG_SHADEROBJOFFSET(64-fragmentCached));
        r(REG_A5XX_SP_FS_CONFIG,A5XX_SP_FS_CONFIG_ENABLED|A5XX_SP_FS_CONFIG_CONSTOBJECTOFFSET(vs.constlen/4)|
            A5XX_SP_FS_CONFIG_SHADEROBJOFFSET(64-fragmentCached));
        r(REG_A5XX_HLSQ_VS_CNTL,A5XX_HLSQ_VS_CNTL_INSTRLEN(vertexCached)|(vs.bufferMask ? A5XX_HLSQ_VS_CNTL_SSBO_ENABLE : 0));
        r(REG_A5XX_HLSQ_FS_CNTL,A5XX_HLSQ_FS_CNTL_INSTRLEN(fragmentCached)|
            (fs.bufferMask?A5XX_HLSQ_FS_CNTL_SSBO_ENABLE:0));
        r(REG_A5XX_HLSQ_VS_CONSTLEN,vs.constlen/4); r(REG_A5XX_HLSQ_VS_INSTRLEN,vertexCached);
        r(REG_A5XX_HLSQ_FS_CONSTLEN,fs.constlen/4); r(REG_A5XX_HLSQ_FS_INSTRLEN,fragmentCached);
        r(REG_A5XX_SP_VS_CTRL_REG0,0x2|(vertexCached?A5XX_SP_VS_CTRL_REG0_BUFFER:0)|
            A5XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT(vs.halfRegs)|A5XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(vs.fullRegs)|
            A5XX_SP_VS_CTRL_REG0_BRANCHSTACK(vs.branchStack));
        r(REG_A5XX_SP_FS_CTRL_REG0,0x40002|(fragmentCached?A5XX_SP_FS_CTRL_REG0_BUFFER:0)|
            A5XX_SP_FS_CTRL_REG0_THREADSIZE(fs.threadSize)|A5XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT(fs.halfRegs)|
            A5XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT(fs.fullRegs)|A5XX_SP_FS_CTRL_REG0_BRANCHSTACK(fs.branchStack)|
            ((fs.flags&1) ? A5XX_SP_FS_CTRL_REG0_PIXLODENABLE : 0) |
            (fs.totalIn ? A5XX_SP_FS_CTRL_REG0_VARYING : 0));
        shader(vs,REG_A5XX_SP_VS_OBJ_START_LO,vertexCached);
        const auto fragmentAddress=shaderObject(fs,REG_A5XX_SP_FS_OBJ_START_LO,fragmentCached);
        auto &fragmentParams=workspace.fragmentParams;__builtin_memset(fragmentParams,0,sizeof(fragmentParams));
        auto &fragmentMemory=workspace.fragmentMemory;auto &vertexMemory=workspace.vertexMemory;
        __builtin_memset(fragmentMemory,0,sizeof(fragmentMemory));__builtin_memset(vertexMemory,0,sizeof(vertexMemory));
        GPU::Shader::graphicsConstants(vs,d.vertexTextures,d.vertexSamplers,vertexMemory);
        GPU::Shader::graphicsConstants(fs,d.textures,d.samplers,fragmentMemory,&d);
        for(unsigned i=0;i<QGPU_BUFFER_SIZE_CONSTANT_WORDS;++i)
            vertexMemory[QGPU_BUFFER_SIZE_CONSTANT_BASE+i]=fragmentMemory[QGPU_BUFFER_SIZE_CONSTANT_BASE+i]=bindings.bytes[i];
        workspace.constantWindowMask=0;
        for(unsigned i=0;i<fs.constantWindowCount;++i) {
            const bool valid=planConstantWindow(fs,i,bindings,completionScratch,scratchBytes,workspace.constantWindowPlans[i]);
            fragmentMemory[qgpu_constant_window_flag(fs.constantWindowCount,i)]=valid?1:0;
            if(valid)workspace.constantWindowMask|=1u<<i;
        }
        operationWindowState=(fs.constantWindowCount<<8)|workspace.constantWindowMask;
        uint32_t vertexOutputs[16],varyingDestinations[8],varyingDisable[4];
        for (unsigned i=0;i<16;++i) {
            uint32_t value=0;
            if (i*2<linkage.count) value|=A5XX_SP_VS_OUT_REG_A_REGID(linkage.outputs[i*2].regid)|A5XX_SP_VS_OUT_REG_A_COMPMASK(linkage.outputs[i*2].mask);
            if (i*2+1<linkage.count) value|=A5XX_SP_VS_OUT_REG_B_REGID(linkage.outputs[i*2+1].regid)|A5XX_SP_VS_OUT_REG_B_COMPMASK(linkage.outputs[i*2+1].mask);
            vertexOutputs[i]=value;
        }
        rr(REG_A5XX_SP_VS_OUT_REG(0),vertexOutputs);
        for (unsigned i=0;i<8;++i) {
            uint32_t value=0;
            for (unsigned j=0;j<4;++j) if (i*4+j<linkage.count) value|=linkage.outputs[i*4+j].loc<<(8*j);
            varyingDestinations[i]=value;
        }
        rr(REG_A5XX_SP_VS_VPC_DST_REG(0),varyingDestinations);
        r(REG_A5XX_SP_PRIMITIVE_CNTL,A5XX_SP_PRIMITIVE_CNTL_VSOUT(linkage.count));
        for (unsigned i=0;i<4;++i) varyingDisable[i]=~linkage.mask[i];
        rr(REG_A5XX_VPC_VAR_DISABLE(0),varyingDisable);
        r(REG_A5XX_VPC_CNTL_0,0x10000|A5XX_VPC_CNTL_0_STRIDE_IN_VPC(linkage.stride)|(fs.totalIn ? A5XX_VPC_CNTL_0_VARYING : 0));
        r(REG_A5XX_VPC_PACK,A5XX_VPC_PACK_PSIZELOC(255)|A5XX_VPC_PACK_NUMNONPOSVAR(fs.totalIn));
        r(REG_A5XX_VPC_CLIP_CNTL,A5XX_VPC_CLIP_CNTL_CLIP_DIST_03_LOC(255)|A5XX_VPC_CLIP_CNTL_CLIP_DIST_47_LOC(255));
        r(REG_A5XX_PC_CLIP_CNTL,0); r(REG_A5XX_GRAS_VS_CL_CNTL,0); r(REG_A5XX_PC_PRIM_VTX_CNTL,0);
        r(REG_A5XX_VPC_GS_SIV_CNTL,0xffff);
        for (unsigned i=0;i<8;++i) { r(REG_A5XX_VPC_VARYING_INTERP_MODE(i),linkage.interpolation[i]); r(REG_A5XX_VPC_VARYING_PS_REPL_MODE(i),0); }
        r(REG_A5XX_HLSQ_CONTROL_0_REG,0x880|A5XX_HLSQ_CONTROL_0_REG_FSTHREADSIZE(fs.threadSize));
        r(REG_A5XX_HLSQ_CONTROL_1_REG,A5XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD(63));
        const uint32_t sampleID=(fs.sampleFlags&QGPU_SAMPLE_ID)?fs.sampleID:252,
            sampleMaskIn=(fs.sampleFlags&QGPU_SAMPLE_MASK_IN)?fs.sampleMaskIn:252,
            sampleMaskOut=(d.rasterSamples && (fs.sampleFlags&QGPU_SAMPLE_MASK_OUT))?fs.sampleMaskOut:252,
            perspectiveCentroid=(fs.sampleFlags&QGPU_CENTROID_PERSPECTIVE)?fs.perspectiveCentroid:252,
            linearCentroid=(fs.sampleFlags&QGPU_CENTROID_LINEAR)?fs.linearCentroid:252,
            perspectiveSample=(fs.sampleFlags&QGPU_SAMPLE_PERSPECTIVE)?fs.perspectiveSample:252,
            linearSample=(fs.sampleFlags&QGPU_SAMPLE_LINEAR)?fs.linearSample:252;
        r(REG_A5XX_HLSQ_CONTROL_2_REG,A5XX_HLSQ_CONTROL_2_REG_FACEREGID(252)|
            A5XX_HLSQ_CONTROL_2_REG_SAMPLEID(sampleID)|A5XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(sampleMaskIn)|A5XX_HLSQ_CONTROL_2_REG_CENTERRHW(fs.centerRHW));
        r(REG_A5XX_HLSQ_CONTROL_3_REG,A5XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL(fs.perspective)|
            A5XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL(fs.linear)|A5XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID(perspectiveCentroid)|
            A5XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID(linearCentroid));
        r(REG_A5XX_HLSQ_CONTROL_4_REG,A5XX_HLSQ_CONTROL_4_REG_XYCOORDREGID(fs.fragCoord)|
            A5XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID(fs.fragCoord == 252 ? 252 : fs.fragCoord+2)|
            A5XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE(perspectiveSample)|A5XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE(linearSample));
        // fd5_program.c loads VS before the graphics allocation update, but
        // FS afterwards. Do not invalidate freshly loaded FS/constants or
        // descriptors with another update immediately before the draw.
        if(workspace.constantWindowMask) {
            // Publish prior GPU shader writes before the CP reads a buffer
            // through LOAD_STATE. This is a real CACHE_FLUSH_TS + observed
            // timestamp wait, not an early software completion. Also retire
            // preceding CP writes. Do this before the new HLSQ allocation.
            if(!shaderWritesPublished)flush(4,2);
            p(0x12,nullptr,0);idle();
            bool copied=false;
            for(unsigned i=0;i<fs.constantWindowCount && status==GPU::Result::Success;++i) {
                const auto &plan=workspace.constantWindowPlans[i];
                if(!(workspace.constantWindowMask&(1u<<i)) || !plan.destination)continue;
                status=e.fill(plan.destination,QGPU_CONSTANT_WINDOW_BYTES,0);
                if(plan.bytes && status==GPU::Result::Success)status=e.copy(plan.source,plan.destination,plan.bytes);
                copied=true;
            }
            // Retire CP stores before LOAD_STATE reads the staged bank.
            if(copied && status==GPU::Result::Success)status=e.barrier();
        }
        r(REG_A5XX_HLSQ_UPDATE_CNTL,0x020fffff);
        loadShader(fs,fragmentAddress,fragmentCached);
        constants(vs,vertexParams,40,vertexMemory); constants(fs,fragmentParams,4,fragmentMemory);
        if(gmem && (fs.flags&QGPU_FRAMEBUFFER_TILE_ORIGIN)) {
            const uint64_t origin=completionScratch+GMEMOrigin;
            const uint32_t load[]={QGPU_FRAMEBUFFER_TILE_ORIGIN_BASE/4|(2u<<16)|(12u<<18)|
                ((QGPU_FRAMEBUFFER_TILE_ORIGIN_WORDS/4)<<22),uint32_t(origin)|1u,uint32_t(origin>>32)};
            p(0x30,load,3);
        }
        for(unsigned i=0;i<fs.constantWindowCount;++i)if(workspace.constantWindowMask&(1u<<i))
            p(0x30,workspace.constantWindowPlans[i].packet,3);
        buffers(false,vs.bufferMask|(fs.bufferMask<<QGPU_FRAGMENT_BUFFER_BASE),bindings,
            ((vs.flags&QGPU_BUFFER_SIZE_CONSTANTS)?vs.bufferMask:0)|
            ((fs.flags&QGPU_BUFFER_SIZE_CONSTANTS)?fs.bufferMask<<QGPU_FRAGMENT_BUFFER_BASE:0));
        r(REG_A5XX_SP_SP_CNTL,0x10);
        r(REG_A5XX_GRAS_CNTL,(fs.fragMask ? A5XX_GRAS_CNTL_COORD_MASK(fs.fragMask)|A5XX_GRAS_CNTL_IJ_LINEAR_PIXEL : 0)|
            (fs.perspective!=252 ? A5XX_GRAS_CNTL_IJ_PERSP_PIXEL : 0)|((fs.linear!=252 || fs.centerRHW!=252) ? A5XX_GRAS_CNTL_IJ_LINEAR_PIXEL : 0)|
            (perspectiveCentroid!=252?A5XX_GRAS_CNTL_IJ_PERSP_CENTROID:0)|(linearCentroid!=252?A5XX_GRAS_CNTL_IJ_LINEAR_CENTROID:0)|
            (perspectiveSample!=252?A5XX_GRAS_CNTL_IJ_PERSP_SAMPLE:0)|(linearSample!=252?A5XX_GRAS_CNTL_IJ_LINEAR_SAMPLE:0));
        r(REG_A5XX_RB_RENDER_CONTROL0,(fs.fragMask ? A5XX_RB_RENDER_CONTROL0_COORD_MASK(fs.fragMask)|A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL : 0)|
            (fs.perspective!=252 ? A5XX_RB_RENDER_CONTROL0_IJ_PERSP_PIXEL : 0)|((fs.linear!=252 || fs.centerRHW!=252) ? A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL : 0)|
            (perspectiveCentroid!=252?A5XX_RB_RENDER_CONTROL0_IJ_PERSP_CENTROID:0)|(linearCentroid!=252?A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_CENTROID:0)|
            (perspectiveSample!=252?A5XX_RB_RENDER_CONTROL0_IJ_PERSP_SAMPLE:0)|(linearSample!=252?A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_SAMPLE:0));
        r(REG_A5XX_RB_RENDER_CONTROL1,(sampleID!=252?A5XX_RB_RENDER_CONTROL1_SAMPLEID:0)|
            (sampleMaskIn!=252?A5XX_RB_RENDER_CONTROL1_SAMPLEMASK:0)|(fs.centerRHW!=252?A5XX_RB_RENDER_CONTROL1_CENTERRHW:0)|
            ((d.rasterSamples && (fs.sampleFlags&QGPU_SAMPLE_EXECUTION))?A5XX_RB_RENDER_CONTROL1_SAMPLEMODE(3):0));
        r(REG_A5XX_RB_FS_OUTPUT_CNTL,A5XX_RB_FS_OUTPUT_CNTL_MRT(targetCount)|
            ((fs.flags&QGPU_FRAGMENT_DEPTH)?A5XX_RB_FS_OUTPUT_CNTL_FRAG_WRITES_Z:0)|
            (sampleMaskOut!=252?A5XX_RB_FS_OUTPUT_CNTL_FRAG_WRITES_SAMPMASK:0));
        r(REG_A5XX_SP_FS_OUTPUT_CNTL,A5XX_SP_FS_OUTPUT_CNTL_MRT(targetCount)|A5XX_SP_FS_OUTPUT_CNTL_DEPTH_REGID((fs.flags&QGPU_FRAGMENT_DEPTH)?fs.depthReg:252)|A5XX_SP_FS_OUTPUT_CNTL_SAMPLEMASK_REGID(sampleMaskOut));
        uint32_t fragmentOutputs[QGPU_COLOR_TARGET_COUNT];
        for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)fragmentOutputs[i]=A5XX_SP_FS_OUTPUT_REG_REGID(fs.colorRegisters[i]);
        rr(REG_A5XX_SP_FS_OUTPUT_REG(0),fragmentOutputs);
        vertexFetch(d,vs,bindings); r(REG_A5XX_VFD_CONTROL_1,0xfc0000|A5XX_VFD_CONTROL_1_REGID4VTX(vs.vertexID)|A5XX_VFD_CONTROL_1_REGID4INST(vs.instanceID));
        const uint32_t vertexControl[]={0xfcfc,0xfcfc,252,0};
        rr(REG_A5XX_VFD_CONTROL_2,vertexControl);
        r(REG_A5XX_PC_RESTART_INDEX,d.indexSize == 2 ? 0xffff : UINT32_MAX);
        r(REG_A5XX_PC_PRIMITIVE_CNTL,A5XX_PC_PRIMITIVE_CNTL_STRIDE_IN_VPC(linkage.stride)|
            ((d.indexSize && d.primitive == 4) ? A5XX_PC_PRIMITIVE_CNTL_PRIMITIVE_RESTART : 0));
        const auto &state = d.state;
        // FRONT_CW describes window winding; the negative Y viewport scale
        // already implements Metal's top-left framebuffer convention.
        // LINEHALFWIDTH is U6.2 at bits3..10: 0.5px gives Metal's 1px lines.
        r(REG_A5XX_GRAS_CL_CNTL,A5XX_GRAS_CL_CNTL_ZERO_GB_SCALE_Z);
        const uint32_t viewport[]={state.viewport[0],state.viewport[1],state.viewport[2],state.viewport[3],state.depthRange[0],state.depthRange[1]};
        rr(REG_A5XX_GRAS_CL_VPORT_XOFFSET_0,viewport);
        const uint32_t window[]={0,(width-1)|((height-1)<<16)};
        if(!gmem)rr(REG_A5XX_GRAS_SC_WINDOW_SCISSOR_TL,window);
        uint32_t tl = d.scissor[0]|(d.scissor[1]<<16), br = (d.scissor[0]+d.scissor[2]-1)|((d.scissor[1]+d.scissor[3]-1)<<16);
        const uint32_t scissor[]={tl,br};
        rr(REG_A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0,scissor);
        rr(REG_A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0,scissor);
        if(!gmem) {
            r(REG_A5XX_RB_WINDOW_OFFSET,0); r(REG_A5XX_RB_RESOLVE_CNTL_1,0);
            r(REG_A5XX_RB_RESOLVE_CNTL_2,(width-1)|((height-1)<<16));
        }
        depthStencil(d,depth,stencil,fs);
        r(REG_A5XX_RB_RENDER_COMPONENTS,components);
        r(REG_A5XX_SP_BLEND_CNTL,A5XX_SP_BLEND_CNTL_UNK8|A5XX_SP_BLEND_CNTL_ENABLE_BLEND(blendMask)|
            (d.alphaToCoverage?A5XX_SP_BLEND_CNTL_ALPHA_TO_COVERAGE:0));
        r(REG_A5XX_RB_BLEND_CNTL,A5XX_RB_BLEND_CNTL_SAMPLE_MASK(0xffff)|A5XX_RB_BLEND_CNTL_ENABLE_BLEND(blendMask)|
            ((targetMask&~1u)?A5XX_RB_BLEND_CNTL_INDEPENDENT_BLEND:0)|
            (d.alphaToCoverage?A5XX_RB_BLEND_CNTL_ALPHA_TO_COVERAGE:0));
        blendColor(state);
        for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i) {
            const auto &t=GPU::Shader::colorTarget(d,i);auto color=GPU::Shader::colorState(d,i);
            bool bound=targetMask&(1u<<i),writes=bound && (fs.colorMask&(1u<<i));
            uint32_t format=t.format==GPU::Shader::R8?0x03:t.format==GPU::Shader::RG8?0x0f:t.format==GPU::Shader::RGBA16Float?0x62:t.format==GPU::Shader::RG32Float?0x67:0x30;
            r(REG_A5XX_RB_MRT_CONTROL(i),A5XX_RB_MRT_CONTROL_ROP_CODE(12)|A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(writes?color.writeMask:0)|
                ((writes && color.blend)?A5XX_RB_MRT_CONTROL_BLEND|A5XX_RB_MRT_CONTROL_BLEND2:0));
            r(REG_A5XX_RB_MRT_BLEND_CONTROL(i),(writes && color.blend)?blendControl(color):0);
            r(REG_A5XX_RB_MRT_BUF_INFO(i),!bound?0:A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(format)|
                (gmem?A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(2)|0x800:0)|
                A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(GPU::Shader::bgraFormat(t.format)?1:0)|
                (GPU::Shader::srgbFormat(t.format)?A5XX_RB_MRT_BUF_INFO_COLOR_SRGB:0));
            const uint32_t pitch=gmem?gmem->pitches[i]:t.pitch;
            r(REG_A5XX_RB_MRT_PITCH(i),bound?A5XX_RB_MRT_PITCH(pitch):0);
            r(REG_A5XX_RB_MRT_ARRAY_PITCH(i),bound?A5XX_RB_MRT_ARRAY_PITCH(pitch*(gmem?gmem->height:t.height)):0);
            const uint64_t base=gmem?gmem->offsets[i]:targets[i];
            const uint32_t targetBase[]={uint32_t(base),uint32_t(base>>32)};
            rr(REG_A5XX_RB_MRT_BASE_LO(i),targetBase);
            r(REG_A5XX_SP_FS_MRT_REG(i),!bound?0:A5XX_SP_FS_MRT_REG_COLOR_FORMAT(format)|
                (GPU::Shader::srgbFormat(t.format)?A5XX_SP_FS_MRT_REG_COLOR_SRGB:0));
            const uint32_t flags[4]={};
            rr(REG_A5XX_RB_MRT_FLAG_BUFFER(i),flags);
        }
        uint32_t samples=d.rasterSamples==4?2:d.rasterSamples==2?1:0;
        r(REG_A5XX_RB_RAS_MSAA_CNTL,A5XX_RB_RAS_MSAA_CNTL_SAMPLES(samples));
        r(REG_A5XX_RB_DEST_MSAA_CNTL,A5XX_RB_DEST_MSAA_CNTL_SAMPLES(samples)|(samples?0:A5XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE));
        r(REG_A5XX_GRAS_SC_RAS_MSAA_CNTL,A5XX_GRAS_SC_RAS_MSAA_CNTL_SAMPLES(samples));
        r(REG_A5XX_GRAS_SC_DEST_MSAA_CNTL,A5XX_GRAS_SC_DEST_MSAA_CNTL_SAMPLES(samples)|(samples?0:A5XX_GRAS_SC_DEST_MSAA_CNTL_MSAA_DISABLE));
        r(REG_A5XX_TPL1_TP_RAS_MSAA_CNTL,A5XX_TPL1_TP_RAS_MSAA_CNTL_SAMPLES(samples));
        r(REG_A5XX_TPL1_TP_DEST_MSAA_CNTL,A5XX_TPL1_TP_DEST_MSAA_CNTL_SAMPLES(samples)|(samples?0:A5XX_TPL1_TP_DEST_MSAA_CNTL_MSAA_DISABLE));
        uint32_t visibility = 1; p(0x64,&visibility,1); // ignore binning visibility
        textures(vs,d.vertexTextures,sampled.vertex,d.vertexSamplers,true);
        textures(fs,d.textures,sampled.fragment,d.samplers);
        for(unsigned slot=0;slot<QGPU_COLOR_TARGET_COUNT;++slot)if(fs.framebufferMask&(1u<<slot)) {
            auto texture=GPU::Shader::colorTarget(d,slot);uint64_t address=(context.inplace || context.readOnlyFeedback)?targets[slot]:completionScratch+snapshots.offsets[slot];
            texture.offset=0;
            if(gmem) {
                texture.width=gmem->width;texture.height=gmem->height;texture.pitch=gmem->pitches[slot];
                address=GMEMTextureBase+gmem->offsets[slot];
            }
            textureView(texture,address,4,qgpu_framebuffer_slot(fs.sampleMask,fs.fetchMask,fs.framebufferMask,slot),true,gmem!=nullptr);
        }
        if(status==GPU::Result::Success)++rasterFullSetups;
        } else {
            // Only consecutive expanded primitives of this exact inplace
            // draw, in one IB, can enter here. There was no copy/compute or
            // HLSQ allocation/update between them. Retain immutable shader,
            // linkage, descriptors, constants and render target state as fd5
            // does for clean state. The previous primitive's real color/UCHE
            // flushes still precede this fresh framebuffer invalidation.
            idle();invalidateUCHE();event(25);event(24);
            vertexDriverConstants(vs,vertexParams,workspace.vertexMemory);
            if(status==GPU::Result::Success)++rasterReusedPrimitives;
        }
        r(REG_A5XX_VFD_INDEX_OFFSET,vertexParams[1]);r(REG_A5XX_VFD_INSTANCE_START_OFFSET,d.baseInstance);
        // Triangle-strip expansion changes winding; instance and vertex
        // starts also change. These registers are never part of the reuse.
        r(REG_A5XX_GRAS_SU_CNTL,(d.primitive==1 ? (2u<<3) : 0) |
            (d.state.cull == GPU::Shader::CullFront ? A5XX_GRAS_SU_CNTL_CULL_FRONT : 0) |
            (d.state.cull == GPU::Shader::CullBack ? A5XX_GRAS_SU_CNTL_CULL_BACK : 0) |
            (d.state.winding == GPU::Shader::Clockwise ? A5XX_GRAS_SU_CNTL_FRONT_CW : 0));
        uint32_t draw[] = {(d.primitive == 1 ? 2u : d.primitive == 3 ? 4u : 6u)|
            ((d.indexSize == 2 ? 1u : 2u)<<10)|(d.indexSize ? 0 : (2u<<6)),
            d.instances,d.count,0,uint32_t(indices),uint32_t(indices>>32),d.count};
        p(0x38,draw,d.indexSize ? 7 : 3);
        idle();
        // Flush the color cache, wait for its timestamp, then let the common
        // CACHE_FLUSH_TS fence flush UCHE. Timestamp cells use a separate
        // writable page; shader code, constants and PM4 remain read-only.
        flush(29,0);if(depth || stencil)flush(28,1);
        if(fs.framebufferMask && !gmem)flush(4,2);
        idle(); return status;
    }
};
}
