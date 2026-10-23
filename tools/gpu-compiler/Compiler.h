// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUJITProtocol.h>
#ifdef __cplusplus
#include <string>
#include <vector>
#include "FunctionConstants.h"
namespace GPU { namespace Compiler {
struct Entry {
    std::string name, glsl;
    QGPUProgramDescription reflection{};
};
bool lowerMSL(const std::string &, std::vector<Entry> &, std::string &);
bool inspectAIREntries(const void *,size_t,std::vector<std::string> &,std::string &);
bool inspectAIRConstants(const void *,size_t,const std::string &,std::vector<FunctionConstant> &,unsigned &,std::string &);
bool lowerAIR(const void *, size_t, std::vector<Entry> &, std::string &, const std::string &selected = {},
    const std::vector<FunctionConstantValue> &constants = {});
// Serializes Mesa's standalone front end and IR3 globals. No subprocesses,
// executable CPU mappings or temporary shader files.
bool compile(const std::vector<Entry> &, uint64_t, std::vector<QGPUProgramImage> &, std::string &);
enum class AIRRoute { GLSL, NIR, GLSLFallback };
struct AIRCompileInfo {
    AIRRoute route=AIRRoute::GLSL;
    bool lowerFailed=false,nirFailureCacheHit=false;
    uint64_t lowerNS=0,backendNS=0;
    std::string nirDiagnostic;
};
// Selected functions and whole AIR libraries try the direct frontend first. Every unsuccessful
// attempt is discarded before the existing GLSL compiler is invoked.
// Both routes use the same compiler mutex as compile().
bool compileAIR(const void *,size_t,uint64_t,std::vector<QGPUProgramImage> &,std::string &,
    const std::string &selected={},const std::vector<FunctionConstantValue> &constants={},
    AIRCompileInfo *info=nullptr,bool preferNIR=true);
} }
extern "C" {
#endif
__attribute__((visibility("default"))) int qgpu_compile_glsl(const char *, unsigned, QGPUProgramImage *, char *, unsigned);
#ifndef __APPLE__
int qgpu_check_buffer_guards(void);
#endif
#ifdef __cplusplus
}
#endif
