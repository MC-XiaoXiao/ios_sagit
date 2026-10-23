// SPDX-License-Identifier: BSD-2-Clause
#include "Compiler.h"
#include "AIRNIR.h"
#include <chrono>
#include <cstring>
#include <mutex>
namespace GPU { namespace Compiler {
namespace {
std::mutex &compilerMutex(){static std::mutex gate;return gate;}
struct DirectFailure {
    std::string bytes,entry,constants,diagnostic;
    uint64_t registry=0;
};
std::vector<DirectFailure> &directFailures(){static std::vector<DirectFailure> entries;return entries;}
std::string directConstantKey(const std::vector<QGPUAIRConstant> &values) {
    std::string key;
    for(const auto &v:values){key.append(reinterpret_cast<const char *>(&v.index),sizeof(v.index));
        key.append(reinterpret_cast<const char *>(&v.type),sizeof(v.type));
        key.append(reinterpret_cast<const char *>(v.bytes),constantBytes(v.type));}
    return key;
}
bool cachedDirectFailure(const void *bytes,size_t size,const std::string &entry,const std::string &constants,uint64_t registry,std::string &diagnostic) {
    auto &cache=directFailures();
    for(size_t i=0;i<cache.size();++i)if(cache[i].registry==registry && cache[i].entry==entry && cache[i].constants==constants &&
        cache[i].bytes.size()==size && !memcmp(cache[i].bytes.data(),bytes,size)) {
        auto hit=std::move(cache[i]);cache.erase(cache.begin()+i);diagnostic=hit.diagnostic;cache.push_back(std::move(hit));return true;
    }
    return false;
}
void rememberDirectFailure(const void *bytes,size_t size,const std::string &entry,const std::string &constants,uint64_t registry,const std::string &diagnostic) {
    if(!bytes || !size || size>16u*1024*1024 || diagnostic.find("NIR")==std::string::npos || diagnostic.find("backend")!=std::string::npos)
        return;
    auto &cache=directFailures();size_t total=size;for(const auto &v:cache)total+=v.bytes.size();
    while(!cache.empty() && (cache.size()>=8 || total>32u*1024*1024)){total-=cache.front().bytes.size();cache.erase(cache.begin());}
    cache.push_back({std::string(static_cast<const char *>(bytes),size),entry,constants,diagnostic,registry});
}
bool compileEntries(const std::vector<Entry> &entries,uint64_t registry,std::vector<QGPUProgramImage> &out,std::string &error) {
    out.clear();error.clear();
    if (entries.empty() || entries.size()>JIT::MaxPrograms) { error="Invalid shader entry count";return false; }
    for (const auto &entry:entries) {
        QGPUProgramImage image{};image.registry=registry;image.version=JIT::Version;image.gpu=540;
        if (entry.name.empty() || entry.name.size()>=sizeof(image.name) || entry.name.find('\0')!=std::string::npos) {
            error="Invalid shader entry name";out.clear();return false;
        }
        std::memcpy(image.name,entry.name.c_str(),entry.name.size());
        char diagnostic[4096]{};
        if (!qgpu_compile_glsl(entry.glsl.c_str(),entry.reflection.stage,&image,diagnostic,sizeof(diagnostic))) {
            error=entry.name+": "+diagnostic;out.clear();return false;
        }
        for (unsigned variant=0;variant<(image.info.stage==Shader::Fragment?4u:1u);++variant) {
            auto &info=variant==0?image.info:variant==1?image.msaa.info:variant==2?image.constantWindow.info:image.msaaConstantWindow.info;
            if(variant>=2 && !info.codeWords)continue;
            info.bufferMask=entry.reflection.bufferMask;info.writeMask=entry.reflection.writeMask;
            info.instanceMask=entry.reflection.instanceMask;
            info.textureBindings=entry.reflection.textureBindings;
            if(info.framebufferMask&~entry.reflection.framebufferMask) {
                error=entry.name+": undeclared framebuffer input";out.clear();return false;
            }
            if(info.depthTextureMask&~entry.reflection.depthTextureMask) {
                error=entry.name+": comparison sampler requires a declared depth texture";out.clear();return false;
            }
            info.imageUintMask=entry.reflection.imageUintMask&info.imageMask;
            info.depthTextureMask=entry.reflection.depthTextureMask&(info.imageMask|info.sampleMask|info.fetchMask|info.queryMask);
            if (info.varyingCount>entry.reflection.varyingCount) {
                error=entry.name+": undeclared compiler stage input/output";out.clear();return false;
            }
            info.varyingCount=entry.reflection.varyingCount;
            for (unsigned i=0;i<QGPU_MAX_VARYINGS;++i) {
                auto &v=info.varyings[i];const auto &decl=entry.reflection.varyings[i];
                if (i>=info.varyingCount) { v={};continue; }
                if (v.components && (v.components!=decl.components ||
                    (info.stage==Shader::Fragment && v.mode!=decl.mode))) {
                    error=entry.name+": compiler stage interface disagrees with source";out.clear();return false;
                }
                std::memcpy(v.semantic,decl.semantic,sizeof(v.semantic));v.components=decl.components;v.mode=decl.mode;
            }
            std::memcpy(info.strides,entry.reflection.strides,sizeof(info.strides));
        }
        if(image.constantWindow.info.codeWords &&
            (!JIT::compatibleConstantWindow(image.info,image.constantWindow.info) ||
             !JIT::compatibleConstantWindow(image.msaa.info,image.msaaConstantWindow.info))) {
            image.constantWindow={};image.msaaConstantWindow={};
        }
        if (!JIT::valid(image,registry)) { error=entry.name+": compiler output exceeds the kernel JIT profile";out.clear();return false; }
        out.push_back(image);
    }
    return true;
}
}
bool compile(const std::vector<Entry> &entries,uint64_t registry,std::vector<QGPUProgramImage> &out,std::string &error) {
    std::lock_guard<std::mutex> lock(compilerMutex());return compileEntries(entries,registry,out,error);
}
bool compileAIR(const void *bytes,size_t size,uint64_t registry,std::vector<QGPUProgramImage> &out,std::string &error,
    const std::string &selected,const std::vector<FunctionConstantValue> &constants,AIRCompileInfo *details,bool preferNIR) {
    std::lock_guard<std::mutex> lock(compilerMutex());out.clear();error.clear();AIRCompileInfo info;
    using Clock=std::chrono::steady_clock;auto begin=Clock::now();
    auto ns=[](Clock::time_point a,Clock::time_point b){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count());};
    if(preferNIR && bytes && size && size<=16u*1024*1024 && selected.size()<96 && selected.find('\0')==std::string::npos && constants.size()<=MaxConstantValues) {
        std::vector<std::string> names{selected};
        bool ready=!selected.empty() || inspectAIREntries(bytes,size,names,info.nirDiagnostic);
        bool directOK=ready;
        uint64_t backendNS=0;
        for(const auto &entry:names) {
            if(!directOK)break;
            // Resolve names against each entry's reachable constants. A whole
            // library is committed only if every entry produces a valid image.
            auto effective=constants;
            if(std::any_of(constants.begin(),constants.end(),[](const FunctionConstantValue &v){return v.index==UINT32_MAX;})) {
                std::vector<FunctionConstant> declarations;unsigned stage;
                if(!inspectAIRConstants(bytes,size,entry,declarations,stage,info.nirDiagnostic) ||
                   !resolveFunctionConstants(declarations,constants,effective,info.nirDiagnostic)) {directOK=false;break;}
            }
            std::vector<QGPUAIRConstant> values;values.reserve(effective.size());
            for(const auto &v:effective) {
                // Check before hashing: an invalid shape must never overrun
                // the fixed bytes field in the capability-cache key.
                if(v.index>65535 || !constantBytes(v.type)){directOK=false;info.nirDiagnostic="Invalid resolved AIR constant";break;}
                QGPUAIRConstant value{};value.index=v.index;value.type=v.type;
                std::memcpy(value.bytes,v.bytes.data(),sizeof(value.bytes));values.push_back(value);
            }
            if(!directOK)break;
            QGPUProgramImage image{};QGPUAIRNIRTiming timing{};char diagnostic[4096]{};
            const auto constantKey=directConstantKey(values);
            bool known=cachedDirectFailure(bytes,size,entry,constantKey,registry,info.nirDiagnostic);
            if(!known && qgpu_compile_air_nir(bytes,size,entry.c_str(),values.data(),values.size(),registry,&image,&timing,diagnostic,sizeof(diagnostic)) &&
               JIT::valid(image,registry)) {
                out.push_back(image);backendNS+=timing.backendNS;continue;
            }
            directOK=false;info.nirFailureCacheHit=known;
            if(!known) {
                info.nirDiagnostic=diagnostic[0]?diagnostic:"Direct NIR returned an invalid JIT image";
                rememberDirectFailure(bytes,size,entry,constantKey,registry,info.nirDiagnostic);
            }
        }
        if(directOK && !out.empty()) {
            info.route=AIRRoute::NIR;info.backendNS=backendNS;info.lowerNS=ns(begin,Clock::now())-backendNS;
            if(details)*details=info;return true;
        }
        out.clear();info.route=AIRRoute::GLSLFallback;
    }
    std::vector<Entry> entries;
    bool ok=lowerAIR(bytes,size,entries,error,selected,constants);info.lowerFailed=!ok;auto lowerEnd=Clock::now();
    if(ok)ok=compileEntries(entries,registry,out,error);auto end=Clock::now();
    // Include a failed direct attempt in the total cost, while keeping actual
    // fallback lowering and backend spans useful to runtime diagnostics.
    info.lowerNS=ns(begin,lowerEnd);info.backendNS=ns(lowerEnd,end);
    if(!ok)out.clear();if(details)*details=info;return ok;
}
} }
