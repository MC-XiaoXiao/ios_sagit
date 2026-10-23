// SPDX-License-Identifier: BSD-2-Clause
// The original reader is frozen by the offline build. The emitter is shared
// with the runtime so differential checks exercise the code being shipped.
#include "AIRRuntime.cpp"
#include "Prototype.h"
#include "AIRNIRLowering.h"
namespace GPU { namespace Compiler {
bool compileAIRNIR(const void *bytes,size_t size,const std::string &selected,
    const std::vector<FunctionConstantValue> &constants,uint64_t registry,
    QGPUProgramImage &out,std::string &error,PrototypeStats &stats,const std::string &dump) {
    prototype_set_validation(!dump.empty());AIRNIRMetrics metrics;
    auto inspect=[&](nir_shader *nir){
        if(dump.empty())return;FILE *file=fopen(dump.c_str(),"w");if(!file)fail("NIR dump open");
        nir_print_shader(nir,file);fclose(file);prototype_validate(nir);
    };
    bool ok=compileAIRNIRModule(bytes,size,selected,constants,registry,out,error,metrics,prototype_compile_nir,inspect);
    stats={metrics.parseMS,metrics.emitMS,metrics.backendMS,metrics.blocks,metrics.instructions};return ok;
}
} }
