// SPDX-License-Identifier: BSD-2-Clause
// Included by the pinned Mesa ir3_compiler, after its shader headers.
// Export the compiler's real register/constant allocation alongside its code.
#include "compiler/nir/nir_builder.h"
#include "QcomInterface.h"
static QGPUProgramDescription qcom_interface;
// GL links storage blocks in active-resource order. Metal bindings are explicit;
// remap the constant block indices before IR3 lowers SSBO instructions.
static void qcom_remap_ssbos(nir_shader *nir, const struct gl_program *program)
{
    NIR_PASS_V(nir, nir_opt_copy_prop_vars);
    NIR_PASS_V(nir, nir_opt_constant_folding);
    NIR_PASS_V(nir, nir_copy_prop);
    NIR_PASS_V(nir, nir_opt_constant_folding);
    nir_foreach_function_impl(impl, nir) {
        nir_builder b = nir_builder_create(impl);
        nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
                if (instr->type != nir_instr_type_intrinsic) continue;
                nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
                unsigned src;
                if (in->intrinsic == nir_intrinsic_load_ssbo ||
                    in->intrinsic == nir_intrinsic_get_ssbo_size || in->intrinsic == nir_intrinsic_ssbo_atomic ||
                    in->intrinsic == nir_intrinsic_ssbo_atomic_swap) src = 0;
                else if (in->intrinsic == nir_intrinsic_store_ssbo) src = 1;
                else continue;
                if (!nir_src_is_const(in->src[src])) errx(1, "dynamic SSBO binding");
                unsigned index = nir_src_as_uint(in->src[src]);
                if (index >= program->info.num_ssbos) errx(1, "invalid SSBO binding");
                unsigned binding;
                if (!qgpu_ssbo_binding(nir,program->sh.ShaderStorageBlocks[index]->Binding,&binding)) errx(1,"unsupported SSBO binding");
                qgpu_record_image_access(nir,program->sh.ShaderStorageBlocks[index]->Binding,in,&qcom_interface);
                b.cursor = nir_before_instr(instr);
                nir_src_rewrite(&in->src[src], nir_imm_int(&b,binding));
            }
        }
    }
    if (!qgpu_guard_memory(nir,&qcom_interface) || !qgpu_lower_buffer_sizes(nir,&qcom_interface)) errx(1,"unsupported shader memory layout/access");
}
static void qcom_export_program(struct ir3_shader_variant *v, const uint32_t *code)
{
    qgpu_pad_memory_constants(v,&qcom_interface);
    const char *path = getenv("QCOM_IR3_OUTPUT");
    if (!path) return;
    const struct ir3_const_state *c = ir3_const_state(v);
    if (v->pvtmem_size || v->shared_size>QGPU_MAX_THREADGROUP_BYTES || v->constant_data_size || c->num_ubos ||
        v->info.size > QGPU_MAX_CODE_WORDS*4 || v->instrlen > QGPU_MAX_INSTRUCTION_UNITS || v->constlen > 128 ||
        v->num_samp > (v->type==MESA_SHADER_COMPUTE?QGPU_STAGE_TEXTURE_COUNT:QGPU_GRAPHICS_TEXTURE_COUNT) || !qgpu_export_interface(v,&qcom_interface))
        errx(1, "shader exceeds A540 profile (code=%u bytes, instrlen=%u, constants=%u, private=%u, shared=%u, data=%u, UBOs=%u)",
             v->info.size,v->instrlen,v->constlen,v->pvtmem_size,v->shared_size,v->constant_data_size,c->num_ubos);
    FILE *out = fopen(path, "w");
    if (!out) err(1, "open compiler metadata");
    fprintf(out, "{\"metadata\":{");
#define FIELD(name, value) fprintf(out, "\"" name "\":%u,", (unsigned)(value))
    FIELD("stage", v->type == MESA_SHADER_COMPUTE ? 0 : v->type == MESA_SHADER_VERTEX ? 1 : 2);
    FIELD("codeWords", v->info.size / 4);
    FIELD("instrlen", v->instrlen);
    FIELD("constlen", v->constlen);
    FIELD("fullRegs", v->info.max_reg < 0 ? 1 : v->info.max_reg + 1);
    FIELD("halfRegs", v->info.max_half_reg + 1);
    FIELD("branchStack", ir3_shader_branchstack_hw(v));
    FIELD("threadSize", v->info.double_threadsize);
    FIELD("subgroupSize", v->info.subgroup_size);
    FIELD("immediateBase", c->immediates_count ? c->offsets.immediate * 4 : 0);
    FIELD("immediateCount", c->immediates_count);
    FIELD("driverBase", v->need_driver_params ? c->offsets.driver_param * 4 : 0);
    FIELD("driverCount", v->need_driver_params ? c->num_driver_params : 0);
    FIELD("localID", ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID));
    FIELD("groupID", ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID));
    FIELD("vertexID", ir3_find_sysval_regid(v, SYSTEM_VALUE_VERTEX_ID_ZERO_BASE));
    FIELD("instanceID", ir3_find_sysval_regid(v, SYSTEM_VALUE_INSTANCE_ID));
    FIELD("varyingCount", qcom_interface.varyingCount);
    FIELD("totalIn", qcom_interface.totalIn);
    FIELD("perspective", qcom_interface.perspective);
    FIELD("linear", qcom_interface.linear);
    FIELD("centerRHW", qcom_interface.centerRHW);
    FIELD("sharedBytes",qcom_interface.threadgroupMask?qcom_interface.sharedBytes:v->shared_size);
    FIELD("threadgroupMask",qcom_interface.threadgroupMask);
    FIELD("imageMask",qcom_interface.imageMask);FIELD("imageReadMask",qcom_interface.imageReadMask);FIELD("imageWriteMask",qcom_interface.imageWriteMask);FIELD("imageUintMask",qcom_interface.imageUintMask);
    FIELD("sampleMask",qcom_interface.sampleMask);FIELD("samplerMask",qcom_interface.samplerMask);FIELD("explicitSamplerMask",qcom_interface.explicitSamplerMask);FIELD("cubeSamplerMask",qcom_interface.cubeSamplerMask);
    FIELD("fetchMask",qcom_interface.fetchMask);FIELD("queryMask",qcom_interface.queryMask);
    FIELD("position", ir3_find_output_regid(v, VARYING_SLOT_POS));
    FIELD("color", ir3_find_output_regid(v, v->color0_mrt ? FRAG_RESULT_COLOR : FRAG_RESULT_DATA0));
    FIELD("fragCoord", ir3_find_sysval_regid(v, SYSTEM_VALUE_FRAG_COORD));
    FIELD("fragMask", v->fragcoord_compmask);
    FIELD("flags", (qcom_interface.flags&(QGPU_DISPATCH_GEOMETRY|QGPU_ARRAY_TEXTURE_MASK|QGPU_CUBE_TEXTURE_MASK|QGPU_BUFFER_SIZE_CONSTANTS|QGPU_FRAMEBUFFER_TILE_ORIGIN)) | (v->need_pixlod ? 1 : 0) | (v->no_earlyz || v->writes_smask ? 2 : 0) | (v->has_kill ? QGPU_FRAGMENT_DISCARD : 0) | (v->type == MESA_SHADER_FRAGMENT && v->writes_pos ? QGPU_FRAGMENT_DEPTH : 0) |
        (v->type == MESA_SHADER_COMPUTE && v->local_size_variable ? QGPU_VARIABLE_LOCAL_SIZE : 0) |
        QGPU_BOUNDED_BUFFERS | (v->type == MESA_SHADER_FRAGMENT ? QGPU_FRAGMENT_BUFFER_BANK : 0) |
        (v->has_barrier ? QGPU_THREADGROUP_BARRIER : 0) | (qcom_interface.threadgroupMask ? QGPU_DYNAMIC_THREADGROUP : 0));
    FIELD("attributeMask",qcom_interface.attributeMask);
    FIELD("colorMask",qcom_interface.colorMask);FIELD("depthReg",qcom_interface.depthReg);FIELD("multisampleMask",qcom_interface.multisampleMask);FIELD("depthTextureMask",qcom_interface.depthTextureMask);
    FIELD("sampleFlags",qcom_interface.sampleFlags|(getenv("QCOM_IR3_MSAA")?QGPU_SAMPLE_MSAA:0));
    FIELD("sampleID",qcom_interface.sampleID);FIELD("sampleMaskIn",qcom_interface.sampleMaskIn);FIELD("sampleMaskOut",qcom_interface.sampleMaskOut);
    FIELD("perspectiveCentroid",qcom_interface.perspectiveCentroid);FIELD("linearCentroid",qcom_interface.linearCentroid);
    FIELD("perspectiveSample",qcom_interface.perspectiveSample);FIELD("linearSample",qcom_interface.linearSample);
    FIELD("constantWindowCount",qcom_interface.constantWindowCount);
    fprintf(out,"\"constantWindows\":[");
    for(unsigned i=0;i<QGPU_MAX_CONSTANT_WINDOWS;++i)
        fprintf(out,"%s{\"buffer\":%u,\"byteOffset\":%u}",i?",":"",qcom_interface.constantWindows[i].buffer,qcom_interface.constantWindows[i].byteOffset);
    fprintf(out,"],");
    fprintf(out,"\"colorRegisters\":[");
    for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i)fprintf(out,"%s%u",i?",":"",qcom_interface.colorRegisters[i]);
    fprintf(out,"],");
    FIELD("implicitSamplerMask",qcom_interface.implicitSamplerMask);
    FIELD("constantSamplerMask",qcom_interface.constantSamplerMask);
    FIELD("constantImplicitSamplerMask",qcom_interface.constantImplicitSamplerMask);
    FIELD("constantExplicitSamplerMask",qcom_interface.constantExplicitSamplerMask);
    FIELD("constantCubeSamplerMask",qcom_interface.constantCubeSamplerMask);
    FIELD("framebufferMask",qcom_interface.framebufferMask);
    fprintf(out,"\"constantSamplerSlots\":[");
    for(unsigned i=0;i<QGPU_MAX_CONSTANT_SAMPLERS;++i)fprintf(out,"%s%u",i?",":"",qcom_interface.constantSamplerSlots[i]);
    fprintf(out,"],\"constantSamplerLevelSlots\":[");
    for(unsigned i=0;i<QGPU_MAX_CONSTANT_SAMPLERS;++i)fprintf(out,"%s%u",i?",":"",qcom_interface.constantSamplerLevelSlots[i]);
    fprintf(out,"],\"constantSamplers\":[");
    for(unsigned i=0;i<QGPU_MAX_CONSTANT_SAMPLERS;++i) {
        const QGPUConstantSampler *s=&qcom_interface.constantSamplers[i];
        fprintf(out,"%s{\"filters\":%u,\"minLOD\":%u,\"maxLOD\":%u,\"addressModes\":%u,\"maxAnisotropy\":%u,\"borderColor\":%u,\"normalized\":%u}",
            i?",":"",s->filters,s->minLOD,s->maxLOD,s->addressModes,s->maxAnisotropy,s->borderColor,s->normalized);
    }
    fprintf(out,"],");
    fprintf(out,"\"attributes\":[");
    for (unsigned i=0;i<QGPU_MAX_ATTRIBUTES;++i) {
        const QGPUAttribute *a=&qcom_interface.attributes[i];
        fprintf(out,"%s{\"components\":%u,\"type\":%u,\"regid\":%u,\"mask\":%u}",i?",":"",a->components,a->type,a->regid,a->mask);
    }
    fprintf(out,"],");
    fprintf(out, "\"textureCount\":%u,\"varyings\":[", (unsigned)v->num_samp);
    for (unsigned i=0;i<QGPU_MAX_VARYINGS;++i) {
        const QGPUVarying *io=&qcom_interface.varyings[i];
        fprintf(out,"%s{\"components\":%u,\"mode\":%u,\"regid\":%u,\"mask\":%u,\"inloc\":%u}",
            i?",":"",io->components,io->mode,io->regid,io->mask,io->inloc);
    }
    fprintf(out, "]},\"code\":[");
#undef FIELD
    for (unsigned i = 0; i < v->info.size / 4; ++i) fprintf(out, "%s%u", i ? "," : "", code[i]);
    fprintf(out, "],\"immediates\":[");
    for (unsigned i = 0; i < c->immediates_count; ++i)
        fprintf(out, "%s%u", i ? "," : "", c->immediates[i]);
    fprintf(out, "]}\n");
    if (fclose(out)) err(1, "write compiler metadata");
}
