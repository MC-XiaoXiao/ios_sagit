// SPDX-License-Identifier: BSD-2-Clause
// Host AOT entry point for the same checked MSL frontend used by device JIT.
#include "Compiler.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>
#include <iterator>
int main() {
    std::string source{std::istreambuf_iterator<char>(std::cin),{}},error;
    std::vector<GPU::Compiler::Entry> entries;
    if (!GPU::Compiler::lowerMSL(source,entries,error)) { llvm::errs()<<error<<'\n';return 1; }
    llvm::json::Array result;
    for (const auto &e:entries) {
        llvm::json::Array strides,varyings;
        for (auto stride:e.reflection.strides) strides.push_back(stride);
        for (unsigned i=0;i<e.reflection.varyingCount;++i) {
            const auto &v=e.reflection.varyings[i];llvm::json::Array hash;
            for (auto word:v.semantic) hash.push_back(word);
            varyings.push_back(llvm::json::Object{{"semantic",std::move(hash)},{"components",v.components},{"mode",v.mode}});
        }
        result.push_back(llvm::json::Object{{"name",e.name},{"stage",e.reflection.stage},
            {"extension",e.reflection.stage==0?"comp":e.reflection.stage==1?"vert":"frag"},
            {"bufferMask",e.reflection.bufferMask},{"writeMask",e.reflection.writeMask},
            {"instanceMask",e.reflection.instanceMask},{"framebufferMask",e.reflection.framebufferMask},{"depthTextureMask",e.reflection.depthTextureMask},{"imageUintMask",e.reflection.imageUintMask},{"strides",std::move(strides)},
            {"varyings",std::move(varyings)},{"glsl",e.glsl}});
    }
    llvm::outs()<<llvm::json::Value(std::move(result))<<'\n';return 0;
}
