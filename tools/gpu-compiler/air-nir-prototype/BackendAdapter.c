// SPDX-License-Identifier: BSD-2-Clause
// Frozen production backend is included, leaving its GLSL entry untouched.
struct nir_shader;
void prototype_validate(struct nir_shader *);
void prototype_set_validation(int);
static int prototype_validating;
static int prototype_capturing;
struct QGPUProgramDescription;
void prototype_capture(struct nir_shader *,const struct QGPUProgramDescription *,unsigned);
void prototype_capture_variant(struct nir_shader *,const struct QGPUProgramDescription *,unsigned,unsigned);
void prototype_capture_enable(int);
#include "CheckedBackend.c"
#include "common/disasm.h"
int prototype_disassemble(uint32_t *,unsigned,const char *);
int prototype_disassemble(uint32_t *code,unsigned words,const char *path) {
    FILE *file=fopen(path,"w");if(!file)return 0;
    int result=disasm_a3xx(code,words,0,file,540);fclose(file);return result>=0;
}
void prototype_set_validation(int enabled){prototype_validating=enabled;}
void prototype_capture_enable(int enabled){prototype_capturing=enabled;}
int prototype_compile_nir(nir_shader *,const QGPUProgramDescription *,QGPUProgramImage *,char *,unsigned);
int prototype_compile_nir(nir_shader *input,const QGPUProgramDescription *reflection,QGPUProgramImage *out,char *error,unsigned errorBytes) {
    const unsigned kind=reflection->stage;
    for(unsigned attempt=0;attempt<2;++attempt) {
        memset(out,0,sizeof(*out));error[0]=0;
        struct fd_dev_id id={.gpu_id=540};
        struct qgpu_prepared_shader p={0};p.description=*reflection;
        p.compiler=ir3_compiler_create(NULL,&id,fd_dev_info_raw(&id),
            &(struct ir3_compiler_options){.disable_cache=true,.qcom_runtime=true});
        p.nir=nir_shader_clone(NULL,input);bool ok=p.compiler && p.nir;
        if(ok) {
            nir_shader *nir=p.nir;nir->options=ir3_get_compiler_options(p.compiler);
            ok=qgpu_capture_interface(nir,&p.description);
            NIR_PASS_V(nir,nir_lower_returns);
            NIR_PASS_V(nir,nir_lower_io_to_temporaries,nir_shader_get_entrypoint(nir),true,kind==1 || nir->options->lower_all_io_to_temps);
            NIR_PASS_V(nir,nir_lower_global_vars_to_local);
            NIR_PASS_V(nir,nir_split_var_copies);NIR_PASS_V(nir,nir_lower_var_copies);
            NIR_PASS_V(nir,nir_lower_vars_to_ssa);
            NIR_PASS_V(nir,nir_opt_copy_prop_vars);NIR_PASS_V(nir,nir_copy_prop);
            NIR_PASS_V(nir,nir_opt_constant_folding);NIR_PASS_V(nir,nir_opt_dce);
            ok=ok && qgpu_guard_memory_profile(nir,&p.description,attempt==0) && qgpu_lower_buffer_sizes(nir,&p.description);
            if(kind==1)nir_assign_var_locations(nir,nir_var_shader_in,&nir->num_inputs,ir3_glsl_type_size);
            qgpu_assign_graphics_io(nir);
            nir_assign_var_locations(nir,nir_var_uniform,&nir->num_uniforms,ir3_glsl_type_size);
            NIR_PASS_V(nir,nir_lower_system_values);
            const nir_lower_compute_system_values_options cs={};NIR_PASS_V(nir,nir_lower_compute_system_values,&cs);
            NIR_PASS_V(nir,nir_lower_frexp);
            NIR_PASS_V(nir,nir_lower_io,nir_var_shader_in|nir_var_shader_out|nir_var_uniform,ir3_glsl_type_size,(nir_lower_io_options)0);
            ok=ok && qgpu_remap_samples(nir,&p.description);
            if(ok && prototype_capturing)prototype_capture(nir,&p.description,1);
            ir3_nir_lower_io_to_temporaries(nir);
            if(ok && prototype_validating)prototype_validate(nir);
        }
        out->info=p.description;
        if(ok)ok=compile_variants(&p,kind,&out->info,out->code,out->immediates,&out->msaa,false,error,errorBytes);
        if(ok && kind==2 && out->info.constantWindowCount) {
            bool specialized=compile_variants(&p,kind,&out->constantWindow.info,out->constantWindow.code,
                out->constantWindow.immediates,&out->msaaConstantWindow,true,error,errorBytes);
            if(!specialized){memset(&out->constantWindow,0,sizeof(out->constantWindow));memset(&out->msaaConstantWindow,0,sizeof(out->msaaConstantWindow));error[0]=0;}
        }
        destroy_prepared_shader(&p,kind);
        if(ok)return 1;
        if(!out->info.constantWindowCount && !out->msaa.info.constantWindowCount)return 0;
    }
    return 0;
}
