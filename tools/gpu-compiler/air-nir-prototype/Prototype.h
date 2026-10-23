// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "Compiler.h"
struct nir_shader;
struct PrototypeStats { double parseMS=0,emitMS=0,backendMS=0;unsigned blocks=0,instructions=0; };
extern "C" int prototype_compile_nir(nir_shader *,const QGPUProgramDescription *,QGPUProgramImage *,char *,unsigned);
extern "C" void prototype_validate(nir_shader *);
extern "C" void prototype_set_validation(int);
extern "C" int prototype_disassemble(uint32_t *,unsigned,const char *);
extern "C" void prototype_capture_enable(int);
extern "C" void prototype_capture(nir_shader *,const QGPUProgramDescription *,unsigned);
int runDifferential(const std::string &);
int runRemainingDifferential(const std::string &);
int runFeatureDifferential(const std::string &);
namespace GPU { namespace Compiler {
bool compileAIRNIR(const void *,size_t,const std::string &,const std::vector<FunctionConstantValue> &,
    uint64_t,QGPUProgramImage &,std::string &,PrototypeStats &,const std::string &dump={});
} }
