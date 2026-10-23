// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUJITProtocol.h>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace GPU { namespace MetalArchive {
using Key=std::array<uint8_t,32>;
constexpr uint32_t Version=1,MaxFunctions=256,MaxPipelines=1024;
constexpr size_t HeaderBytes=112,DigestOffset=80,MaxBytes=16*1024*1024;
enum : uint32_t { AOT=1,Compiled=2,Compute=1,Render=2 };
struct Function {
    Key key{};
    uint32_t kind=0,aot=0,stage=0,maxThreads=0;
    std::string name;
    std::vector<uint8_t> image;
};
struct Pipeline {
    Key key{};
    uint32_t kind=0;
    Key functions[2]{};
};
struct Contents {std::vector<Function> functions;std::vector<Pipeline> pipelines;};
inline bool nonzero(const Key &key) {for(auto byte:key)if(byte)return true;return false;}
inline void put32(std::vector<uint8_t> &out,uint32_t value) {
    for(unsigned i=0;i<4;++i)out.push_back(uint8_t(value>>(8*i)));
}
inline uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
inline bool validFunction(const Function &f,uint64_t registry) {
    if(!nonzero(f.key) || f.name.empty() || f.name.size()>95 || f.name.find('\0')!=std::string::npos ||
        f.stage>Shader::Fragment || !f.maxThreads || f.maxThreads>Shader::MaxComputeThreads)return false;
    if(f.kind==AOT)return f.aot && f.aot<JIT::FirstID && f.image.empty();
    if(f.kind!=Compiled || f.aot || f.image.size()!=sizeof(QGPUProgramImage))return false;
    auto image=std::make_unique<QGPUProgramImage>();std::memcpy(image.get(),f.image.data(),sizeof(*image));
    return GPU::JIT::valid(*image,registry) && image->info.stage==f.stage && f.name==image->name;
}
inline bool valid(const Contents &contents,uint64_t registry) {
    if(contents.functions.size()>MaxFunctions || contents.pipelines.size()>MaxPipelines)return false;
    std::map<Key,const Function *> functions;std::map<Key,bool> pipelines;
    for(const auto &f:contents.functions)
        if(!validFunction(f,registry) || !functions.emplace(f.key,&f).second)return false;
    for(const auto &p:contents.pipelines) {
        if(!nonzero(p.key) || (p.kind!=Compute && p.kind!=Render) || !pipelines.emplace(p.key,true).second)return false;
        auto first=functions.find(p.functions[0]);if(first==functions.end())return false;
        if(p.kind==Compute) {
            if(first->second->stage!=Shader::Compute || nonzero(p.functions[1]))return false;
        } else {
            auto second=functions.find(p.functions[1]);
            if(first->second->stage!=Shader::Vertex || second==functions.end() || second->second->stage!=Shader::Fragment)return false;
        }
    }
    return true;
}
// The caller computes SHA-256 over these bytes with the digest field zero,
// then writes the digest at DigestOffset. No native IDs or pointers occur.
inline bool encode(const Contents &contents,const Key &context,uint64_t registry,std::vector<uint8_t> &out) {
    out.clear();if(!nonzero(context) || !valid(contents,registry))return false;
    const char magic[]="QMTLARC1";out.insert(out.end(),magic,magic+8);
    for(uint32_t value:{Version,540u,uint32_t(registry),uint32_t(registry>>32),GPU::JIT::Version,Shader::Version,
        uint32_t(contents.functions.size()),uint32_t(contents.pipelines.size()),0u,0u})put32(out,value);
    out.insert(out.end(),context.begin(),context.end());out.resize(HeaderBytes,0);
    for(const auto &f:contents.functions) {
        out.insert(out.end(),f.key.begin(),f.key.end());
        for(uint32_t value:{f.kind,f.aot,f.stage,f.maxThreads,uint32_t(f.name.size()),uint32_t(f.image.size()),0u,0u})put32(out,value);
        out.insert(out.end(),f.name.begin(),f.name.end());out.insert(out.end(),f.image.begin(),f.image.end());
    }
    for(const auto &p:contents.pipelines) {
        out.insert(out.end(),p.key.begin(),p.key.end());put32(out,p.kind);put32(out,p.kind==Compute?1:2);
        for(const auto &key:p.functions)out.insert(out.end(),key.begin(),key.end());
        put32(out,0);put32(out,0);
    }
    if(out.size()>MaxBytes){out.clear();return false;}
    uint64_t size=out.size();for(unsigned i=0;i<8;++i)out[40+i]=uint8_t(size>>(8*i));return true;
}
// computedDigest must be SHA-256 of the complete immutable file with bytes
// [DigestOffset,DigestOffset+32) zeroed. Structural checks remain independent.
inline bool decode(const void *bytes,size_t size,const Key &context,uint64_t registry,const Key &computedDigest,Contents &out) {
    out={};if(!bytes || size<HeaderBytes || size>MaxBytes || !nonzero(context))return false;
    auto data=static_cast<const uint8_t *>(bytes);
    if(std::memcmp(data,"QMTLARC1",8) || get32(data+8)!=Version || get32(data+12)!=540 ||
        get32(data+16)!=uint32_t(registry) || get32(data+20)!=uint32_t(registry>>32) ||
        get32(data+24)!=GPU::JIT::Version || get32(data+28)!=Shader::Version ||
        get32(data+40)!=size || get32(data+44) || std::memcmp(data+48,context.data(),32) ||
        std::memcmp(data+DigestOffset,computedDigest.data(),32))return false;
    uint32_t functions=get32(data+32),pipelines=get32(data+36);
    if(functions>MaxFunctions || pipelines>MaxPipelines)return false;
    size_t offset=HeaderBytes;Contents parsed;
    for(unsigned i=0;i<functions;++i) {
        if(size-offset<64)return false;
        const auto *p=data+offset;Function f;std::memcpy(f.key.data(),p,32);
        f.kind=get32(p+32);f.aot=get32(p+36);f.stage=get32(p+40);f.maxThreads=get32(p+44);
        uint32_t names=get32(p+48),image=get32(p+52);offset+=64;
        if(!names || names>95 || image>sizeof(QGPUProgramImage) || get32(p+56) || get32(p+60) ||
            names>size-offset || image>size-offset-names)return false;
        f.name.assign(reinterpret_cast<const char *>(data+offset),names);offset+=names;
        f.image.assign(data+offset,data+offset+image);offset+=image;parsed.functions.push_back(std::move(f));
    }
    for(unsigned i=0;i<pipelines;++i) {
        if(size-offset<112)return false;
        const auto *p=data+offset;Pipeline pipeline;std::memcpy(pipeline.key.data(),p,32);
        pipeline.kind=get32(p+32);if(get32(p+36)!=(pipeline.kind==Compute?1u:2u) || get32(p+104) || get32(p+108))return false;
        std::memcpy(pipeline.functions,p+40,64);parsed.pipelines.push_back(pipeline);offset+=112;
    }
    if(offset!=size || !valid(parsed,registry))return false;
    out=std::move(parsed);return true;
}
} }
