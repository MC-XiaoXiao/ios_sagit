// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "Compiler.h"
#include <array>
#include <cstring>

namespace GPU { namespace SystemShaders {
struct Declaration {const char *name;uint32_t index,type;bool required;};
struct Constant {uint32_t index,type;uint8_t bytes[32];};
struct Variant {const Constant *constants;size_t count;const uint8_t *packed;size_t bytes;};
struct Entry {
    std::array<uint8_t,32> airHash;
    const char *name;uint32_t stage;
    const Declaration *declarations;size_t declarationCount;
    const Variant *variants;size_t variantCount;
};
inline const Entry *find(const Entry *entries,size_t count,const std::array<uint8_t,32> &hash,const char *name) {
    if(!name)return nullptr;
    // Generated entries are sorted by the complete AIR digest and entry name.
    size_t first=0;
    while(first<count) {
        size_t mid=first+(count-first)/2;const auto &e=entries[mid];
        int order=std::memcmp(e.airHash.data(),hash.data(),hash.size());
        if(!order)order=std::strcmp(e.name,name);
        if(order<0)first=mid+1;else if(order>0)count=mid;else return &e;
    }
    return nullptr;
}
inline void reflect(const Entry &e,std::vector<Compiler::FunctionConstant> &out,unsigned &stage) {
    out.clear();stage=e.stage;out.reserve(e.declarationCount);
    for(size_t i=0;i<e.declarationCount;++i) {
        const auto &d=e.declarations[i];out.push_back({d.name,d.index,d.type,d.required});
    }
}
inline const Variant *match(const Entry &e,const std::vector<Compiler::FunctionConstantValue> &values) {
    for(size_t n=0;n<e.variantCount;++n) {
        const auto &v=e.variants[n];if(v.count!=values.size())continue;bool same=true;
        for(size_t i=0;i<v.count;++i) {
            const auto &a=v.constants[i];const auto &b=values[i];unsigned size=Compiler::constantBytes(a.type);
            if(!size || a.index!=b.index || a.type!=b.type || std::memcmp(a.bytes,b.bytes.data(),size)){same=false;break;}
        }
        if(same)return &v;
    }
    return nullptr;
}
// Build-generated, read-only data is part of the signed driver. Zero/literal
// runs avoid embedding the large canonical zero tails of each full JIT image.
// Never use this decoder for a partially matched function or constant profile.
inline bool unpack(const uint8_t *bytes,size_t size,QGPUProgramImage &out) {
    out={};if(!bytes || size>2*sizeof(out))return false;
    auto *dst=reinterpret_cast<uint8_t *>(&out);size_t at=0,pos=0;
    auto word=[](const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);};
    while(pos<size) {
        if(size-pos<8)return false;
        size_t zero=word(bytes+pos),literal=word(bytes+pos+4);pos+=8;
        if((!zero && !literal) || zero>sizeof(out)-at)return false;at+=zero;
        if(literal>sizeof(out)-at || literal>size-pos)return false;
        std::memcpy(dst+at,bytes+pos,literal);pos+=literal;at+=literal;
    }
    return at==sizeof(out);
}
inline bool restore(const Entry &e,const Variant &v,uint64_t registry,QGPUProgramImage &out) {
    if(unpack(v.packed,v.bytes,out) && JIT::valid(out,registry) && out.info.stage==e.stage &&
       std::strcmp(out.name,e.name)==0)return true;
    out={};return false;
}
} }
