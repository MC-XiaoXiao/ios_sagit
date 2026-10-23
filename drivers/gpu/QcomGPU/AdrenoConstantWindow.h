// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "AdrenoSubmit.h"
#include <GPUShaderProtocol.h>
namespace Adreno {
// Progress-page space after timestamp/counter records and before GMEM origin.
constexpr uint32_t ConstantWindowScratch=1536;
struct ConstantWindowPlan {
    uint32_t packet[3]{};
    uint64_t source=0,destination=0;
    uint32_t bytes=0;
};
// CPU only plans a read of an already-resolved GPU binding. No source data is
// dereferenced or snapshotted. Physical write-alias admission remains native;
// programs with any writable SSBO conservatively keep the original path.
template<class Bindings> inline bool constantWindowBinding(const GPU::Shader::Program &program,
    unsigned index,const Bindings &bindings,uint64_t &source,uint64_t &bytes) {
    source=bytes=0;
    if(program.stage!=GPU::Shader::Fragment || program.writeMask ||
        !program.constantWindowCount || program.constantWindowCount>QGPU_MAX_CONSTANT_WINDOWS ||
        index>=program.constantWindowCount || program.constlen>128 ||
        program.constlen*4<qgpu_constant_memory_words(program.constantWindowCount))return false;
    const auto &window=program.constantWindows[index];
    if(window.buffer>=QGPU_STAGE_BUFFER_COUNT || !(program.bufferMask&(1u<<window.buffer)) ||
        (window.byteOffset&15) || window.byteOffset>UINT32_MAX-QGPU_CONSTANT_WINDOW_BYTES)return false;
    const unsigned slot=QGPU_FRAGMENT_BUFFER_BASE+window.buffer;
    const uint64_t base=bindings.addresses[slot],offset=window.byteOffset;
    // An entirely out-of-range window has the bounded shader's all-zero
    // value. It needs no source address, even when the binding is absent.
    if(offset>=bindings.bytes[slot])return true;
    if(base>UINT64_MAX-offset)return false;
    source=base+offset;bytes=bindings.bytes[slot]-offset;
    return true;
}
inline void encodeConstantWindowPacket(unsigned index,uint64_t source,uint32_t (&packet)[3]) {
    const uint32_t destination=QGPU_CONSTANT_WINDOW_BASE+index*QGPU_CONSTANT_WINDOW_WORDS;
    // fd5_emit_const_bo: dst in vec4s, 64B destination/count granularity,
    // source at least vec4 aligned, FS shader block, indirect constants.
    packet[0]=destination/4|(2u<<16)|(12u<<18)|((QGPU_CONSTANT_WINDOW_BYTES/16)<<22);
    packet[1]=uint32_t(source)|1u;packet[2]=uint32_t(source>>32);
}
template<class Bindings> inline bool constantWindowPacket(const GPU::Shader::Program &program,
    unsigned index,const Bindings &bindings,uint32_t (&packet)[3]) {
    packet[0]=packet[1]=packet[2]=0;uint64_t source=0,bytes=0;
    if(!constantWindowBinding(program,index,bindings,source,bytes) ||
        bytes<QGPU_CONSTANT_WINDOW_BYTES || (source&15) || !deviceRange(source,QGPU_CONSTANT_WINDOW_BYTES))return false;
    encodeConstantWindowPacket(index,source,packet);return true;
}
template<class Bindings> inline bool planConstantWindow(const GPU::Shader::Program &program,
    unsigned index,const Bindings &bindings,uint64_t scratch,uint32_t scratchBytes,ConstantWindowPlan &out) {
    out={};
    if(constantWindowPacket(program,index,bindings,out.packet))return true;
    uint64_t source=0,bytes=0;
    if(!constantWindowBinding(program,index,bindings,source,bytes))return false;
    if(bytes>QGPU_CONSTANT_WINDOW_BYTES)bytes=QGPU_CONSTANT_WINDOW_BYTES;
    if(((source|bytes)&3) || (bytes && !deviceRange(source,bytes)) ||
        scratchBytes<GPU::PageBytes || (scratch&127) || !deviceRange(scratch,GPU::PageBytes))return false;
    if(bytes && source<scratch+GPU::PageBytes && scratch<source+bytes)return false;
    // A5xx indirect constants require 64B. Copy only complete, in-range
    // words into a zero-filled GPU-owned window. This also aligns complete
    // windows whose Metal binding is word-aligned but not vec4-aligned.
    // An absent range only clears
    // this private window and never reads the missing binding. Partial half-word tails
    // retain guarded SSBO reads; no source range is enlarged.
    out.source=source;out.bytes=uint32_t(bytes);
    out.destination=scratch+ConstantWindowScratch+index*QGPU_CONSTANT_WINDOW_BYTES;
    encodeConstantWindowPacket(index,out.destination,out.packet);return true;
}
template<class Bindings> inline bool constantWindowRequirements(const GPU::Shader::Program &program,const Bindings &bindings,
    uint64_t scratch=0,uint32_t scratchBytes=0) {
    if(!(program.flags&QGPU_CONSTANT_WINDOWS_REQUIRED))return true;
    if(!program.constantWindowCount)return false;
    for(unsigned i=0;i<program.constantWindowCount;++i) {
        ConstantWindowPlan plan;if(!planConstantWindow(program,i,bindings,scratch,scratchBytes,plan))return false;
    }
    return true;
}
template<class Bindings> inline const GPU::Shader::Program &selectConstantWindowProgram(
    const GPU::Shader::Program &program,const Bindings &bindings,uint64_t scratch=0,uint32_t scratchBytes=0) {
    return program.constantWindow && (program.constantWindow->flags&QGPU_CONSTANT_WINDOWS_REQUIRED) &&
        constantWindowRequirements(*program.constantWindow,bindings,scratch,scratchBytes)?*program.constantWindow:program;
}
}
