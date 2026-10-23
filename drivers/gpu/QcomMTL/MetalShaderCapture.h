// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUJITProtocol.h>
#include <string>

namespace GPU { namespace MetalShaderCapture {
constexpr size_t MaxJSONBytes=512*1024,MaxFileBytes=1536*1024,MaxDeviceBytes=8*1024*1024;
constexpr unsigned MaxFiles=32,ChunkBytes=128;
struct Budget {
    size_t bytes=0;unsigned files=0;
    bool admit(size_t count) {
        if(!count || count>MaxFileBytes || files>=MaxFiles || count>MaxDeviceBytes-bytes)return false;
        bytes+=count;++files;return true; // Failed writes remain charged too.
    }
};
inline std::string hex(const void *memory,size_t count) {
    constexpr char digits[]="0123456789abcdef";const auto *data=static_cast<const uint8_t *>(memory);
    std::string out;out.reserve(count*2);
    for(size_t i=0;i<count;++i){out+=digits[data[i]>>4];out+=digits[data[i]&15];}return out;
}
inline uint64_t programHash(const QGPUProgramImage &image,bool multisample=false) {
    const auto &info=multisample?image.msaa.info:image.info;
    const auto *code=multisample?image.msaa.code:image.code;
    const auto *immediates=multisample?image.msaa.immediates:image.immediates;
    uint64_t value=14695981039346656037ULL;
    auto add=[&](const void *memory,size_t count){const auto *p=static_cast<const uint8_t *>(memory);
        for(size_t i=0;i<count;++i){value^=p[i];value*=1099511628211ULL;}};
    if(!info.codeWords || info.codeWords>QGPU_MAX_CODE_WORDS || info.immediateCount>512 ||
        (multisample && info.stage!=Shader::Fragment))return 0;
    add(&info.stage,sizeof(info)-sizeof(info.id));
    add(code,info.codeWords*4);add(immediates,info.immediateCount*4);return value;
}
inline std::string frame(const void *json,size_t count,const std::string &sha256,uint64_t identity) {
    if(!json || !count || count>MaxJSONBytes || sha256.size()!=64)return {};
    for(char c:sha256)if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return {};
    const auto *data=static_cast<const uint8_t *>(json);
    std::string out="GPU_SHADER_CAPTURE_BEGIN version=1 bytes="+std::to_string(count)+" sha256="+sha256+
        " operation_hash="+std::to_string(identity)+"\n";
    for(size_t offset=0;offset<count;offset+=ChunkBytes) {
        const size_t bytes=count-offset<ChunkBytes?count-offset:ChunkBytes;
        out+="GPU_SHADER_CAPTURE_DATA offset="+std::to_string(offset)+" hex="+hex(data+offset,bytes)+"\n";
    }
    out+="GPU_SHADER_CAPTURE_END bytes="+std::to_string(count)+" sha256="+sha256+"\n";
    return out.size()<=MaxFileBytes?out:std::string();
}
} }
