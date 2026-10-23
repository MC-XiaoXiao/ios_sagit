// SPDX-License-Identifier: MIT
// A540 lowering follows Mesa's ir3_cmdline.c, Copyright (c) 2014 Rob Clark.
// See NOTICE.txt. This entry owns every per-compilation allocation and reports
// errors to the caller; it never exits the client process or opens a device.
#include "Compiler.h"
#include "AIRNIR.h"
#include <stdio.h>
#include <string.h>
#include "ir3/ir3.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "main/mtypes.h"
#include "compiler/glsl/gl_nir.h"
#include "compiler/glsl/standalone.h"
#include "compiler/nir/nir_builder.h"
#include "QcomInterface.h"
extern struct gl_shader_program *qgpu_standalone_source(const char *, unsigned,
    struct gl_context *, char *, unsigned);
static bool remap_buffers(nir_shader *nir, const struct gl_program *program,QGPUProgramDescription *description,bool constantWindows) {
    NIR_PASS_V(nir, nir_opt_copy_prop_vars); NIR_PASS_V(nir, nir_opt_constant_folding);
    NIR_PASS_V(nir, nir_copy_prop); NIR_PASS_V(nir, nir_opt_constant_folding);
    nir_foreach_function_impl(impl, nir) {
        nir_builder b = nir_builder_create(impl);
        nir_foreach_block(block, impl) nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr); unsigned src;
            if (in->intrinsic == nir_intrinsic_load_ssbo || in->intrinsic == nir_intrinsic_get_ssbo_size ||
                in->intrinsic == nir_intrinsic_ssbo_atomic || in->intrinsic == nir_intrinsic_ssbo_atomic_swap) src=0;
            else if (in->intrinsic == nir_intrinsic_store_ssbo) src=1; else continue;
            if (!nir_src_is_const(in->src[src])) return false;
            unsigned index=nir_src_as_uint(in->src[src]); if (index>=program->info.num_ssbos) return false;
            unsigned binding;
            if (!qgpu_ssbo_binding(nir,program->sh.ShaderStorageBlocks[index]->Binding,&binding)) return false;
            qgpu_record_image_access(nir,program->sh.ShaderStorageBlocks[index]->Binding,in,description);
            b.cursor=nir_before_instr(instr);
            nir_src_rewrite(&in->src[src], nir_imm_int(&b,binding));
        }
    }
    return qgpu_guard_memory_profile(nir,description,constantWindows) && qgpu_lower_buffer_sizes(nir,description);
}
#ifndef __APPLE__
static int qgpu_check_msaa_reuse(void);
// Compare the optimized literal path with the original symbolic-offset guard
// on identical inputs. Both must preserve exact values and never issue a load
// outside the binding, including end==2^32 and per-component offset wrap.
static bool qgpu_check_static_read_bounds(void) {
    static const nir_shader_compiler_options options={0};
    const uint32_t sizes[]={0,1,2,3,4,5,8,16,176,178,182,184,214,216,224,0xfffffffeu,0xffffffffu};
    const uint32_t offsets[]={0,1,2,3,4,6,176,208,0xfffffff0u,0xfffffff8u,0xfffffffcu,0xfffffffeu,0xffffffffu};
    unsigned cases=0;
    for(unsigned bits=16;bits<=32;bits+=16)for(unsigned n=1;n<=4;++n)
    for(unsigned s=0;s<ARRAY_SIZE(sizes);++s)for(unsigned o=0;o<ARRAY_SIZE(offsets);++o) {
        unsigned bytes=bits/8;uint32_t size=sizes[s],offset=offsets[o];
        nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,&options,"static and dynamic bounds");
        b.shader->info.num_ssbos=1;
        nir_def *runtime=nir_load_const_ir3(&b,1,32,nir_imm_int(&b,300),.base=0);
        nir_def *literal=nir_load_ssbo(&b,n,bits,nir_imm_int(&b,0),nir_imm_int(&b,offset),
            .access=ACCESS_NON_WRITEABLE,.align_mul=bytes,.align_offset=offset&(bytes-1));
        nir_def *dynamic=nir_load_ssbo(&b,n,bits,nir_imm_int(&b,0),runtime,
            .access=ACCESS_NON_WRITEABLE,.align_mul=1);
        nir_store_output(&b,nir_u2u32(&b,literal),nir_imm_int(&b,0),.base=0,.write_mask=(1u<<n)-1);
        nir_store_output(&b,nir_u2u32(&b,dynamic),nir_imm_int(&b,0),.base=1,.write_mask=(1u<<n)-1);
        QGPUProgramDescription p={0};
        bool ok=qgpu_guard_ssbos(b.shader) && qgpu_lower_buffer_sizes(b.shader,&p);
        nir_foreach_block(block,b.impl)nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_const_ir3)continue;
            unsigned at=nir_src_as_uint(in->src[0]);
            ok&=at==300 || at==QGPU_BUFFER_SIZE_CONSTANT_BASE;
            b.cursor=nir_before_instr(instr);nir_def_rewrite_uses(&in->def,nir_imm_int(&b,at==300?offset:size));nir_instr_remove(instr);
        }
        for(unsigned pass=0;pass<6;++pass) {
            NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_opt_algebraic);
            NIR_PASS_V(b.shader,nir_opt_dead_cf);NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);
        }
        nir_foreach_block(block,b.impl)nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_ssbo)continue;
            ok&=nir_src_is_const(in->src[1]) && in->def.bit_size==bits;
            uint32_t at=nir_src_as_uint(in->src[1]);unsigned count=in->num_components;
            ok&=(uint64_t)at+bytes*count<=size && (bits!=16 || !(at&1));
            nir_def *lanes[4];b.cursor=nir_before_instr(instr);
            for(unsigned i=0;i<count;++i)lanes[i]=nir_imm_intN_t(&b,(at+i*bytes)^0xdeadbeefu,bits);
            nir_def_rewrite_uses(&in->def,nir_vec(&b,lanes,count));nir_instr_remove(instr);
        }
        for(unsigned pass=0;pass<3;++pass) {
            NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);
        }
        unsigned outputs=0;
        nir_foreach_block(block,b.impl)nir_foreach_instr(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_store_output)continue;
            unsigned row=nir_intrinsic_base(in);ok&=row<2 && nir_src_is_const(in->src[0]);outputs|=1u<<row;
            const nir_const_value *v=nir_src_as_const_value(in->src[0]);
            for(unsigned i=0;i<n;++i) {
                uint64_t at=(uint64_t)offset+i*bytes;
                bool valid=at+bytes<=size && (bits!=16 || !(at&1));
                uint32_t want=valid?(((uint32_t)at^0xdeadbeefu)&(bits==16?0xffffu:UINT32_MAX)):0;
                ok&=v[i].u32==want;
            }
        }
        ok&=outputs==3;
        if(!ok)fprintf(stderr,"static/dynamic bounds bits=%u n=%u size=%u offset=%u failed\n",bits,n,size,offset);
        ralloc_free(b.shader);if(!ok)return false;++cases;
    }
    printf("NIR static read bounds: %u literal/symbolic differential cases passed, including uint32 wrap and partial vectors\n",cases);
    return cases==1768;
}
static bool qgpu_check_window_schedule(void) {
    static const nir_shader_compiler_options options={0};
    for(unsigned mode=0;mode<6;++mode) {
        nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,&options,"window load ordering");
        QGPUProgramDescription p={0};p.constantWindowCount=1;p.constantWindows[0]=(QGPUConstantWindow){1,160};
        nir_def *a=nir_load_ssbo(&b,4,16,nir_imm_int(&b,9),nir_imm_int(&b,176),
            .access=ACCESS_NON_WRITEABLE,.align_mul=2);
        nir_def *value=nir_u2u32(&b,a);
        nir_instr *separator=value->parent_instr;
        if(mode==1 || mode==2 || mode==3) {
            enum gl_access_qualifier access=ACCESS_NON_WRITEABLE;
            if(mode==1)access|=ACCESS_VOLATILE;
            if(mode==2)access|=ACCESS_COHERENT;
            nir_def *other=nir_load_ssbo(&b,1,16,nir_imm_int(&b,mode==3?10:9),nir_imm_int(&b,180),
                .access=access,.align_mul=2);
            separator=other->parent_instr;
        } else if(mode==4) {
            nir_barrier(&b,.execution_scope=SCOPE_NONE,.memory_scope=SCOPE_DEVICE,
                .memory_semantics=NIR_MEMORY_ACQ_REL,.memory_modes=nir_var_mem_ssbo);
            separator=nir_block_last_instr(nir_cursor_current_block(b.cursor));
        } else if(mode==5) {
            nir_store_output(&b,value,nir_imm_int(&b,0),.base=0,.write_mask=15);
            separator=nir_block_last_instr(nir_cursor_current_block(b.cursor));
        }
        // The second load's sources are intentionally defined after the
        // separator. Moving it must not leave a forward SSA reference.
        nir_def *z=nir_load_ssbo(&b,4,16,nir_imm_int(&b,9),nir_imm_int(&b,184),
            .access=ACCESS_NON_WRITEABLE,.align_mul=2);
        nir_store_output(&b,nir_ixor(&b,value,nir_u2u32(&b,z)),nir_imm_int(&b,0),.base=1,.write_mask=15);
        qgpu_group_window_reads(b.shader,&p);
        unsigned position=0,first=UINT32_MAX,second=UINT32_MAX,fence=UINT32_MAX;
        nir_foreach_block(block,b.impl)nir_foreach_instr(instr,block) {
            if(instr==a->parent_instr)first=position;
            if(instr==z->parent_instr)second=position;
            if(instr==separator)fence=position;
            ++position;
        }
        bool ok=first<second && (mode?first<fence && fence<second:second<fence);
        nir_validate_shader(b.shader,"after readonly window load scheduling");
        // Explicitly check the source dominance even in NDEBUG builds where
        // NIR's validation may be compiled out.
        nir_foreach_block(block,b.impl)nir_foreach_instr(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_ssbo)continue;
            for(unsigned source=0;source<2;++source) {
                bool before=false;
                nir_foreach_instr(candidate,block) {
                    if(candidate==in->src[source].ssa->parent_instr)before=true;
                    if(candidate==instr)break;
                }
                ok&=before;
            }
        }
        ralloc_free(b.shader);if(!ok)return false;
    }
    printf("NIR window scheduling: pure ALU hoist and volatile/coherent/other-buffer/barrier/output boundaries passed\n");
    return true;
}
static bool qgpu_check_constant_windows(void) {
    static const nir_shader_compiler_options options={0};
    const uint32_t sizes[]={0,2,176,178,182,184,190,192,214,216,222,224,226};
    const uint16_t seeds[]={0,0x8000,0x3c00,0x7c00,0x7e01,0xfc00,0xfe55,0xffff};
    unsigned cases=0,promoted=0;
    const unsigned rowCounts[]={1,2,5},componentCounts[]={1,2,4};
    for(unsigned shape=0;shape<ARRAY_SIZE(rowCounts);++shape)
    for(unsigned lanes=0;lanes<ARRAY_SIZE(componentCounts);++lanes)
    for(unsigned slot=0;slot<8;++slot)for(unsigned s=0;s<ARRAY_SIZE(sizes);++s)
    for(unsigned enabled=0;enabled<2;++enabled)for(unsigned seed=0;seed<ARRAY_SIZE(seeds);++seed) {
        const unsigned rows=rowCounts[shape],components=componentCounts[lanes];
        const unsigned windowEnd=(176+8*(rows-1)+2*components+15)&~15u;
        const unsigned windowStart=windowEnd-QGPU_CONSTANT_WINDOW_BYTES;
        if(enabled && sizes[s]<windowEnd)continue;
        nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,&options,"constant window semantics");
        b.shader->info.num_ssbos=16;
        nir_def *values[5];
        for(unsigned row=0;row<rows;++row) {
            nir_def *value=nir_load_ssbo(&b,components,16,nir_imm_int(&b,8+slot),nir_imm_int(&b,176+8*row),
                .access=ACCESS_NON_WRITEABLE,.align_mul=2);
            // Interleave real consumers so grouping moves later independent
            // loads across ALU uses, while preserving every half bit value.
            values[row]=nir_ixor(&b,nir_u2u32(&b,value),nir_imm_int(&b,0x9e3779b9u*(row+1)));
        }
        for(unsigned row=0;row<rows;++row)
            nir_store_output(&b,values[row],nir_imm_int(&b,0),.base=row,.write_mask=(1u<<components)-1);
        QGPUProgramDescription p={0};p.stage=2;
        bool ok=qgpu_guard_memory(b.shader,&p) && qgpu_lower_buffer_sizes(b.shader,&p);
        ok&=p.constantWindowCount==1 && p.constantWindows[0].buffer==slot && p.constantWindows[0].byteOffset==windowStart;
        bool initial_ok=ok;unsigned bad_index=UINT32_MAX;
        
        nir_foreach_block(block,b.impl)nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_const_ir3)continue;
            unsigned index=nir_src_as_uint(in->src[0]);uint32_t word=0;
            if(index==qgpu_constant_window_flag(p.constantWindowCount,0))word=enabled;
            else if(index>=QGPU_CONSTANT_WINDOW_BASE && index<QGPU_CONSTANT_WINDOW_BASE+QGPU_CONSTANT_WINDOW_WORDS) {
                unsigned byte=windowStart+4*(index-QGPU_CONSTANT_WINDOW_BASE);
                word=(uint16_t)(seeds[seed]^byte)|((uint32_t)(uint16_t)(seeds[seed]^(byte+2))<<16);
            } else {if(index!=QGPU_BUFFER_SIZE_CONSTANT_BASE+8+slot)bad_index=index;ok&=index==QGPU_BUFFER_SIZE_CONSTANT_BASE+8+slot;word=sizes[s];}
            b.cursor=nir_before_instr(instr);nir_def_rewrite_uses(&in->def,nir_imm_int(&b,word));nir_instr_remove(instr);
        }
        for(unsigned pass=0;pass<5;++pass) {
            NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_opt_algebraic);
            NIR_PASS_V(b.shader,nir_opt_dead_cf);NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);
        }
        unsigned memory=0;
        nir_foreach_block(block,b.impl)nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_ssbo)continue;
            ++memory;uint32_t byte=nir_src_as_uint(in->src[1]);unsigned count=in->num_components;
            ok&=nir_src_as_uint(in->src[0])==8+slot && in->def.bit_size==16 && (uint64_t)byte+2*count<=sizes[s];
            nir_def *v[4];b.cursor=nir_before_instr(instr);
            for(unsigned i=0;i<count;++i)v[i]=nir_imm_intN_t(&b,(uint16_t)(seeds[seed]^(byte+2*i)),16);
            nir_def_rewrite_uses(&in->def,nir_vec(&b,v,count));nir_instr_remove(instr);
        }
        if(enabled)ok&=memory==0;
        for(unsigned pass=0;pass<3;++pass){NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);}
        unsigned outputs=0;
        nir_foreach_block(block,b.impl)nir_foreach_instr(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_store_output)continue;
            unsigned row=nir_intrinsic_base(in);ok&=row<rows && in->num_components==components && nir_src_is_const(in->src[0]);outputs|=1u<<row;
            const nir_const_value *v=nir_src_as_const_value(in->src[0]);
            for(unsigned i=0;i<components;++i) {
                unsigned at=176+8*row+2*i;
                ok&=v[i].u32==((at+2<=sizes[s]?(uint16_t)(seeds[seed]^at):0u)^(0x9e3779b9u*(row+1)));
            }
        }
        ok&=outputs==((1u<<rows)-1);
        if(!ok){fprintf(stderr,"constant window slot=%u size=%u enabled=%u seed=%u memory=%u windows=%u buffer=%u offset=%u initial=%u badindex=%u outputs=%u\n",slot,sizes[s],enabled,seed,memory,p.constantWindowCount,p.constantWindows[0].buffer,p.constantWindows[0].byteOffset,initial_ok,bad_index,outputs);nir_print_shader(b.shader,stderr);}
        ralloc_free(b.shader);if(!ok)return false;++cases;promoted+=enabled;
    }
    {
        // Unsupported wider NIR vectors must reach the existing rejection,
        // never overrun the promotion helper's four-element lane workspace.
        nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,&options,"unsupported half vector");
        QGPUProgramDescription p={0};p.constantWindowCount=1;p.constantWindows[0]=(QGPUConstantWindow){1,0};
        qgpu_memory_guard guard={true,NULL,&p};
        nir_def *value=nir_load_ssbo(&b,8,16,nir_imm_int(&b,9),nir_imm_int(&b,0),.access=ACCESS_NON_WRITEABLE,.align_mul=2);
        bool changed=qgpu_guard_access(&b,nir_instr_as_intrinsic(value->parent_instr),&guard);
        bool ok=!changed && !guard.supported;ralloc_free(b.shader);if(!ok)return false;
    }
    printf("NIR constant windows: %u cases, %u promoted paths without SSBO reads; oversized vector rejection passed\n",cases,promoted);
    return cases && promoted;
}
// Exercise independently enabled windows of one logical buffer. The payload
// stays integer data so float NaNs, signed zero and packed half bits are all
// checked without a floating-point conversion masking a wrong source word.
static bool qgpu_check_word_windows(void) {
    static const nir_shader_compiler_options options={0};
    const uint32_t sizes[]={4,60,64,68,124,128,132,188,192,196,252,254,256,260};
    const uint32_t seeds[]={0x00000000,0x80000000,0x7fc12345,0xff812345};
    unsigned cases=0;
    for(unsigned slot=0;slot<8;++slot)for(unsigned s=0;s<ARRAY_SIZE(sizes);++s)
    for(unsigned mask=0;mask<16;++mask)for(unsigned seed=0;seed<ARRAY_SIZE(seeds);++seed) {
        bool possible=true;
        for(unsigned i=0;i<4;++i)if((mask&(1u<<i)) && sizes[s]<(i+1)*64)possible=false;
        if(!possible)continue;
        nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,&options,"32-bit multiple window semantics");
        b.shader->info.num_ssbos=16;nir_def *values[16];
        for(unsigned row=0;row<16;++row)
            values[row]=nir_load_ssbo(&b,4,32,nir_imm_int(&b,8+slot),nir_imm_int(&b,row*16),
                .access=ACCESS_NON_WRITEABLE,.align_mul=4);
        for(unsigned row=0;row<16;++row)
            nir_store_output(&b,values[row],nir_imm_int(&b,0),.base=row,.write_mask=15);
        QGPUProgramDescription p={0};p.stage=2;
        bool ok=qgpu_guard_memory(b.shader,&p) && qgpu_lower_buffer_sizes(b.shader,&p);
        ok&=p.constantWindowCount==4;
        for(unsigned i=0;i<4;++i)ok&=p.constantWindows[i].buffer==slot && p.constantWindows[i].byteOffset==64*i;
        nir_foreach_block(block,b.impl)nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_const_ir3)continue;
            unsigned index=nir_src_as_uint(in->src[0]);uint32_t word=0;
            if(index>=qgpu_constant_window_flag(4,0) && index<qgpu_constant_window_flag(4,0)+4)
                word=(mask>>(index-qgpu_constant_window_flag(4,0)))&1;
            else if(index>=QGPU_CONSTANT_WINDOW_BASE && index<QGPU_CONSTANT_WINDOW_BASE+64)
                word=seeds[seed]^((index-QGPU_CONSTANT_WINDOW_BASE)*4u*0x9e3779b9u);
            else {ok&=index==QGPU_BUFFER_SIZE_CONSTANT_BASE+8+slot;word=sizes[s];}
            b.cursor=nir_before_instr(instr);nir_def_rewrite_uses(&in->def,nir_imm_int(&b,word));nir_instr_remove(instr);
        }
        for(unsigned pass=0;pass<6;++pass) {
            NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_opt_algebraic);
            NIR_PASS_V(b.shader,nir_opt_dead_cf);NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);
        }
        nir_foreach_block(block,b.impl)nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_ssbo)continue;
            uint32_t at=nir_src_as_uint(in->src[1]);unsigned n=in->num_components;
            ok&=in->def.bit_size==32 && nir_src_as_uint(in->src[0])==8+slot &&
                !(at&3) && (uint64_t)at+4*n<=sizes[s] && !(mask&(1u<<(at/64)));
            nir_def *lanes[4];b.cursor=nir_before_instr(instr);
            for(unsigned i=0;i<n;++i)lanes[i]=nir_imm_int(&b,seeds[seed]^((at+4*i)*0x9e3779b9u));
            nir_def_rewrite_uses(&in->def,nir_vec(&b,lanes,n));nir_instr_remove(instr);
        }
        for(unsigned pass=0;pass<3;++pass){NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);}
        unsigned outputs=0;
        nir_foreach_block(block,b.impl)nir_foreach_instr(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_store_output)continue;
            unsigned row=nir_intrinsic_base(in);ok&=row<16 && in->num_components==4 && nir_src_is_const(in->src[0]);outputs|=1u<<row;
            if(!nir_src_is_const(in->src[0])){ok=false;continue;}
            const nir_const_value *v=nir_src_as_const_value(in->src[0]);
            for(unsigned i=0;i<4;++i) {
                unsigned at=16*row+4*i;
                ok&=v[i].u32==(at+4<=sizes[s]?(seeds[seed]^(at*0x9e3779b9u)):0u);
            }
        }
        ok&=outputs==65535;
        if(!ok)fprintf(stderr,"word window slot=%u bytes=%u enabled=%u seed=%u failed\n",slot,sizes[s],mask,seed);
        ralloc_free(b.shader);if(!ok)return false;++cases;
    }
    printf("NIR 32-bit windows: %u bit-exact partial-bound and independently-enabled source-window cases passed\n",cases);
    return cases!=0;
}
// Run the actual vectorizer, memory guard and NIR optimizer against concrete
// binding sizes. Substitute byte-derived memory values only after proving
// every surviving load is inside that binding, then check all output lanes.
static bool qgpu_check_half_read_vectors(void) {
    static const nir_shader_compiler_options options={0};
    const uint32_t sizes[]={0,1,2,3,4,5,6,7,8,9,10,16,20,216,0xffffffffu};
    const uint32_t offsets[]={0,1,2,4,6,12,176,0xfffffff8u,0xfffffffcu,0xfffffffeu};
    unsigned cases=0,wide_cases=0,partial_cases=0;
    for(unsigned s=0;s<ARRAY_SIZE(sizes);++s)for(unsigned o=0;o<ARRAY_SIZE(offsets);++o)
    for(unsigned n=2;n<=4;++n)for(unsigned mode=0;mode<3;++mode)for(unsigned split=0;split<2;++split) {
        uint32_t size=sizes[s],offset=offsets[o];
        // Split expressions have independent uint32 addresses; a vector's
        // original base additionally carries the no-wrap rule for its lanes.
        bool wrapped=split && (uint64_t)offset+2*n>0x100000000ull;
        enum gl_access_qualifier access=mode==0?ACCESS_NON_WRITEABLE:mode==1?0:ACCESS_NON_WRITEABLE|ACCESS_VOLATILE;
        nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,&options,"readonly half vector bounds");
        b.shader->info.num_ssbos=2;
        nir_def *index=nir_imm_int(&b,0),*value;
        if(split) {
            nir_def *lanes[4];
            for(unsigned i=0;i<n;++i)lanes[i]=nir_load_ssbo(&b,1,16,index,nir_imm_int(&b,offset+2*i),
                .access=access,.align_mul=2,.align_offset=offset&1);
            value=nir_vec(&b,lanes,n);
        } else value=nir_load_ssbo(&b,n,16,index,nir_imm_int(&b,offset),
            .access=access,.align_mul=2,.align_offset=offset&1);
        nir_store_ssbo(&b,nir_u2u32(&b,value),nir_imm_int(&b,1),nir_imm_int(&b,0),
            .write_mask=(1u<<n)-1,.align_mul=4);
        bool ok=qgpu_guard_ssbos(b.shader);
        // Run the same select speculation pass used by IR3 before resolving
        // descriptor sizes. A wide load must remain in its guarded then arm.
        NIR_PASS_V(b.shader,nir_opt_peephole_select,16,true,true);
        nir_foreach_block(block,b.impl)nir_foreach_instr(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_ssbo || in->num_components==1)continue;
            ok&=!nir_intrinsic_can_reorder(in) && block->cf_node.parent->type==nir_cf_node_if;
            if(block->cf_node.parent->type==nir_cf_node_if)
                ok&=block==nir_if_first_then_block(nir_cf_node_as_if(block->cf_node.parent));
        }
        QGPUProgramDescription description={0};
        ok&=qgpu_lower_buffer_sizes(b.shader,&description);
        nir_foreach_block(block,b.impl)nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_const_ir3)continue;
            unsigned slot=nir_src_as_uint(in->src[0])-QGPU_BUFFER_SIZE_CONSTANT_BASE;
            ok&=slot<2;b.cursor=nir_before_instr(instr);
            nir_def_rewrite_uses(&in->def,nir_imm_int(&b,slot?n*4:size));nir_instr_remove(instr);
        }
        for(unsigned pass=0;pass<4;++pass) {
            NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_opt_algebraic);
            NIR_PASS_V(b.shader,nir_opt_dead_cf);NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);
        }
        unsigned loads=0,components=0,wide=0;
        nir_foreach_block(block,b.impl)nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_ssbo)continue;
            ok&=nir_src_is_const(in->src[0]) && nir_src_as_uint(in->src[0])==0 &&
                nir_src_is_const(in->src[1]) && in->def.bit_size==16;
            uint32_t at=nir_src_as_uint(in->src[1]);unsigned count=in->num_components;
            ok&=!(at&1) && (uint64_t)at+2*count<=size;
            ++loads;components+=count;wide+=count>1;
            nir_def *lanes[4];b.cursor=nir_before_instr(instr);
            for(unsigned i=0;i<count;++i)lanes[i]=nir_imm_intN_t(&b,(at+2*i)^0xa55au,16);
            nir_def_rewrite_uses(&in->def,nir_vec(&b,lanes,count));nir_instr_remove(instr);
        }
        bool full=!(offset&1) && (uint64_t)offset+2*n<=size;
        unsigned expected=0;
        for(unsigned i=0;i<n;++i) {
            uint64_t at=split?(uint32_t)(offset+2*i):(uint64_t)offset+2*i;
            expected+=!(offset&1) && at+2<=size;
        }
        ok&=components==expected;
        if(full && mode==0) {ok&=loads==1 && wide==1;++wide_cases;}
        else {if(!wrapped)ok&=loads==expected && !wide;partial_cases+=expected>0 && expected<n;}
        for(unsigned pass=0;pass<4;++pass) {
            NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);
        }
        unsigned outputs=0;
        nir_foreach_block(block,b.impl)nir_foreach_instr(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_store_ssbo)continue;
            ok&=nir_src_is_const(in->src[0]) && nir_src_is_const(in->src[2]) &&
                nir_src_as_uint(in->src[1])==1 && in->num_components==1;
            unsigned lane=nir_src_as_uint(in->src[2])/4;ok&=lane<n;outputs|=1u<<lane;
            uint64_t at=split?(uint32_t)(offset+2*lane):(uint64_t)offset+2*lane;
            uint32_t want=!(offset&1) && at+2<=size?(uint16_t)(at^0xa55au):0;
            ok&=nir_src_as_uint(in->src[0])==want;
        }
        ok&=outputs==(1u<<n)-1;
        if(!ok) {fprintf(stderr,"half vector guard size=%u offset=%u n=%u mode=%u split=%u loads=%u components=%u wide=%u\n",size,offset,n,mode,split,loads,components,wide);nir_print_shader(b.shader,stderr);}
        ralloc_free(b.shader);if(!ok)return false;++cases;
    }
    fprintf(stdout,"NIR half vector guards: %u cases, %u full-vector paths, %u partial paths passed\n",cases,wide_cases,partial_cases);
    return cases && wide_cases && partial_cases;
}
// Resolve symbolic descriptor lengths/offsets only after applying the real
// guard, then check every surviving store's range and payload. This covers
// the uint4 clear path and its partial-range/ordering fallbacks.
static bool qgpu_check_word_store_vectors(void) {
    static const nir_shader_compiler_options options={0};
    const uint32_t sizes[]={0,4,8,12,16,20,32,0xffffffffu};
    const uint32_t offsets[]={0,4,12,16,0xfffffff0u,0xfffffff4u,0xfffffffcu};
    unsigned cases=0,wide_cases=0,partial_cases=0;
    for(unsigned mode=0;mode<6;++mode)for(unsigned s=0;s<ARRAY_SIZE(sizes);++s)
    for(unsigned o=0;o<ARRAY_SIZE(offsets);++o) {
        uint32_t size=sizes[s],offset=offsets[o];unsigned mask=mode==3?5:15;
        nir_builder b=nir_builder_init_simple_shader(mode==5?MESA_SHADER_FRAGMENT:MESA_SHADER_COMPUTE,&options,"uint4 store bounds");
        b.shader->info.num_ssbos=1;
        nir_def *runtime=nir_load_const_ir3(&b,1,32,nir_imm_int(&b,300),.base=0);
        nir_store_ssbo(&b,nir_imm_ivec4(&b,11,12,13,14),nir_imm_int(&b,0),runtime,
            .write_mask=mask,.align_mul=mode==4?1:4,
            .access=mode==1?ACCESS_VOLATILE:mode==2?ACCESS_COHERENT:0);
        QGPUProgramDescription p={0};
        bool ok=qgpu_guard_ssbos(b.shader) && qgpu_lower_buffer_sizes(b.shader,&p);
        nir_foreach_block(block,b.impl)nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_load_const_ir3)continue;
            unsigned at=nir_src_as_uint(in->src[0]);ok&=at==300 || at==QGPU_BUFFER_SIZE_CONSTANT_BASE;
            b.cursor=nir_before_instr(instr);nir_def_rewrite_uses(&in->def,nir_imm_int(&b,at==300?offset:size));nir_instr_remove(instr);
        }
        for(unsigned pass=0;pass<6;++pass) {
            NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_opt_algebraic);
            NIR_PASS_V(b.shader,nir_opt_dead_cf);NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);
        }
        unsigned written=0,stores=0,wide=0,expected=0;
        for(unsigned i=0;i<4;++i)if((mask&(1u<<i)) && (uint64_t)offset+4*i+4<=size)expected|=1u<<i;
        nir_foreach_block(block,b.impl)nir_foreach_instr(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_store_ssbo)continue;
            ok&=nir_src_is_const(in->src[0]) && nir_src_is_const(in->src[2]);
            uint32_t at=nir_src_as_uint(in->src[2]);unsigned n=in->num_components;
            ok&=at>=offset && (uint64_t)at+4*n<=size && !(at&3);
            ok&=nir_intrinsic_write_mask(in)==(1u<<n)-1;
            for(unsigned i=0;i<n;++i) {
                unsigned lane=(at-offset)/4+i;
                ok&=lane<4 && nir_src_comp_as_uint(in->src[0],i)==11+lane && !(written&(1u<<lane));
                written|=1u<<lane;
            }
            ++stores;wide+=n==4;
        }
        bool full=mode==0 && expected==15;
        ok&=written==expected && wide==(unsigned)full;
        ok&=stores==(full?1:(unsigned)__builtin_popcount(expected));
        wide_cases+=full;partial_cases+=expected && expected!=mask;
        if(!ok){fprintf(stderr,"uint4 store guard size=%u offset=%u mode=%u written=%x expected=%x\n",size,offset,mode,written,expected);nir_print_shader(b.shader,stderr);}
        ralloc_free(b.shader);if(!ok)return false;++cases;
    }
    fprintf(stdout,"NIR uint4 store guards: %u cases, %u vector paths, %u partial paths passed\n",cases,wide_cases,partial_cases);
    return wide_cases && partial_cases;
}
// Exercise the production NIR transform and optimizer with concrete descriptor
// lengths, including partial vectors and unsigned-offset wrap. No GPU model.
__attribute__((visibility("default"))) int qgpu_check_buffer_guards(void) {
    static const nir_shader_compiler_options options={0};
    const uint32_t sizes[]={4,16,4,4,16,4,32,2,6,4,2,0,65536,65536,131072},offsets[]={0,4,0xfffffffcu,0,4,0xfffffffcu,0xfffffff0u,0,2,0xfffffffeu,1,0,65532,65536,131068},masks[]={1,7,0,1,7,0,0,1,3,0,0,0,1,0,1};
    for (unsigned test=0;test<sizeof(sizes)/sizeof(sizes[0]);++test) {
        bool shared=test>=3 && test<7;unsigned base=test==6?16:0,bits=test>=7 && test<12?16:32,bytes=bits/8;
        nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,&options,"buffer bounds");
        b.shader->info.num_ssbos=2;b.shader->info.workgroup_size[0]=b.shader->info.workgroup_size[1]=b.shader->info.workgroup_size[2]=1;
        nir_def *index=nir_imm_int(&b,0),*offset=nir_imm_int(&b,offsets[test]);
        b.shader->info.shared_size=shared?sizes[test]:0;
        nir_def *value=shared?nir_load_shared(&b,4,bits,offset,.base=base,.align_mul=bytes):nir_load_ssbo(&b,4,bits,index,offset,.align_mul=bytes);
        nir_def *input=nir_imm_ivec4(&b,11,12,13,14);if(bits==16)input=nir_u2u16(&b,input);
        if (shared) nir_store_shared(&b,input,offset,.base=base,.write_mask=15,.align_mul=bytes);
        else nir_store_ssbo(&b,input,index,offset,.write_mask=15,.align_mul=bytes);
        // Keep read-result observation scalar; vector writes are checked separately.
        for(unsigned i=0;i<4;++i)
            nir_store_ssbo(&b,nir_channel(&b,value,i),nir_imm_int(&b,1),nir_imm_int(&b,i*bytes),.write_mask=1,.align_mul=bytes);
        bool ok=true;
        if (shared) {qgpu_memory_guard guard={true,NULL};nir_shader_intrinsics_pass(b.shader,qgpu_guard_access,nir_metadata_none,&guard);ok=guard.supported;}
        else ok=qgpu_guard_ssbos(b.shader);
        QGPUProgramDescription description={0};
        ok&=qgpu_lower_buffer_sizes(b.shader,&description) && (description.flags&QGPU_BUFFER_SIZE_CONSTANTS)!=0;
        nir_foreach_function_impl(impl,b.shader) nir_foreach_block(block,impl) nir_foreach_instr_safe(instr,block) {
            if (instr->type!=nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if (in->intrinsic!=nir_intrinsic_load_const_ir3) continue;
            unsigned slot=nir_src_as_uint(in->src[0])-QGPU_BUFFER_SIZE_CONSTANT_BASE;
            ok&=slot<2;
            b.cursor=nir_before_instr(instr);
            nir_def_rewrite_uses(&in->def,nir_imm_int(&b,slot?4*bytes:sizes[test]));nir_instr_remove(instr);
        }
        for (unsigned pass=0;pass<4;++pass) {
            NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_opt_algebraic);NIR_PASS_V(b.shader,nir_opt_dead_cf);
            NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);
        }
        unsigned loads=0,input_stores=0,outputs=0;
        nir_foreach_function_impl(impl,b.shader) nir_foreach_block(block,impl) nir_foreach_instr(instr,block) {
            if (instr->type!=nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if (in->intrinsic==nir_intrinsic_load_ssbo || in->intrinsic==nir_intrinsic_load_shared) {
                unsigned src=in->intrinsic==nir_intrinsic_load_shared?0:1;
                ok&=in->num_components==1 && nir_src_is_const(in->src[src]) && sizes[test]>=bytes && nir_src_as_uint(in->src[src])<=sizes[test]-bytes && in->def.bit_size==bits;++loads;
            } else if (in->intrinsic==nir_intrinsic_store_ssbo || in->intrinsic==nir_intrinsic_store_shared) {
                bool local=in->intrinsic==nir_intrinsic_store_shared;unsigned src=local?1:2;
                ok&=in->num_components==1 && nir_src_is_const(in->src[src]);
                uint32_t at=nir_src_as_uint(in->src[src]);
                if (local || nir_src_as_uint(in->src[1])==0) { ok&=sizes[test]>=bytes && at<=sizes[test]-bytes && nir_src_bit_size(in->src[0])==bits;++input_stores; }
                else {
                    ok&=at<4*bytes && !(at&(bytes-1));unsigned component=at/bytes;outputs|=1u<<component;
                    if (!(masks[test]&(1u<<component))) ok&=nir_src_is_const(in->src[0]) && nir_src_as_uint(in->src[0])==0;
                }
            }
        }
        ok&=loads==(unsigned)__builtin_popcount(masks[test]) && input_stores==loads && outputs==15;
        if (!ok) { fprintf(stderr,"NIR guard case %u: loads %u stores %u outputs %u\n",test,loads,input_stores,outputs);nir_print_shader(b.shader,stderr); }
        ralloc_free(b.shader);if (!ok) return 0;
    }
    // Actual RMW/CAS nodes must disappear entirely on invalid offsets. A
    // fallback atomic at zero would corrupt otherwise unrelated memory.
    const uint32_t atomic_offsets[]={0,0,4,1,0xfffffffcu,0xfffffff0u};
    for (unsigned test=0;test<12;++test) {
        bool local=test>=6,swap=test&1,expected=(test%6)<2;unsigned base=local && test%6==5?16:0;
        nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,&options,"atomic bounds");
        b.shader->info.num_ssbos=2;b.shader->info.shared_size=base?32:4;
        nir_def *offset=nir_imm_int(&b,atomic_offsets[test%6]),*index=nir_imm_int(&b,0),*value=nir_imm_int(&b,7),*result;
        if (local) result=swap?nir_shared_atomic_swap(&b,32,offset,value,value,.base=base,.atomic_op=nir_atomic_op_cmpxchg):
            nir_shared_atomic(&b,32,offset,value,.base=base,.atomic_op=nir_atomic_op_iadd);
        else result=swap?nir_ssbo_atomic_swap(&b,32,index,offset,value,value,.atomic_op=nir_atomic_op_cmpxchg):
            nir_ssbo_atomic(&b,32,index,offset,value,.atomic_op=nir_atomic_op_iadd);
        nir_store_ssbo(&b,result,nir_imm_int(&b,1),nir_imm_int(&b,0),.write_mask=1,.align_mul=4);
        qgpu_memory_guard guard={true,NULL};nir_shader_intrinsics_pass(b.shader,qgpu_guard_access,nir_metadata_none,&guard);bool ok=guard.supported;
        nir_foreach_function_impl(impl,b.shader) nir_foreach_block(block,impl) nir_foreach_instr_safe(instr,block) {
            if (instr->type!=nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if (in->intrinsic!=nir_intrinsic_get_ssbo_size) continue;
            b.cursor=nir_before_instr(instr);nir_def_rewrite_uses(&in->def,nir_imm_int(&b,4));nir_instr_remove(instr);
        }
        for (unsigned pass=0;pass<4;++pass) {
            NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_opt_algebraic);NIR_PASS_V(b.shader,nir_opt_dead_cf);
            NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);
        }
        unsigned atomics=0,outputs=0;
        nir_foreach_function_impl(impl,b.shader) nir_foreach_block(block,impl) nir_foreach_instr(instr,block) {
            if (instr->type!=nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if (in->intrinsic==nir_intrinsic_ssbo_atomic || in->intrinsic==nir_intrinsic_ssbo_atomic_swap ||
                in->intrinsic==nir_intrinsic_shared_atomic || in->intrinsic==nir_intrinsic_shared_atomic_swap) {
                ++atomics;unsigned src=local?0:1;ok&=nir_src_is_const(in->src[src]) && nir_src_as_uint(in->src[src])==0;
            } else if (in->intrinsic==nir_intrinsic_store_ssbo) {
                ++outputs;if (!expected) ok&=nir_src_is_const(in->src[0]) && nir_src_as_uint(in->src[0])==0;
            }
        }
        ok&=atomics==(unsigned)expected && outputs==1;
        if (!ok) {fprintf(stderr,"NIR atomic guard case %u: atomics %u outputs %u\n",test,atomics,outputs);nir_print_shader(b.shader,stderr);}
        ralloc_free(b.shader);if (!ok) return 0;
    }
    // Check observable side effects on each side of an actual NIR demote.
    // A discarded lane keeps its earlier store, but neither a later store
    // nor a later atomic may touch memory. This also exercises demote_if.
    for(unsigned discarded=0;discarded<2;++discarded) {
        nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,&options,"fragment demotion");
        b.shader->info.num_ssbos=1;
        nir_def *index=nir_imm_int(&b,0),*offset=nir_imm_int(&b,0);
        nir_store_ssbo(&b,nir_imm_int(&b,11),index,offset,.write_mask=1,.align_mul=4);
        nir_demote_if(&b,nir_imm_bool(&b,discarded));
        nir_store_ssbo(&b,nir_imm_int(&b,22),index,offset,.write_mask=1,.align_mul=4);
        nir_ssbo_atomic(&b,32,index,offset,nir_imm_int(&b,1),.atomic_op=nir_atomic_op_iadd);
        bool ok=qgpu_guard_ssbos(b.shader);
        nir_foreach_block(block,b.impl) nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_get_ssbo_size)continue;
            b.cursor=nir_before_instr(instr);nir_def_rewrite_uses(&in->def,nir_imm_int(&b,4));nir_instr_remove(instr);
        }
        NIR_PASS_V(b.shader,nir_lower_vars_to_ssa);
        for(unsigned pass=0;pass<4;++pass) {
            NIR_PASS_V(b.shader,nir_opt_constant_folding);NIR_PASS_V(b.shader,nir_opt_algebraic);NIR_PASS_V(b.shader,nir_opt_dead_cf);
            NIR_PASS_V(b.shader,nir_copy_prop);NIR_PASS_V(b.shader,nir_opt_dce);
        }
        unsigned before=0,after=0,atomics=0;
        nir_foreach_block(block,b.impl) nir_foreach_instr(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic==nir_intrinsic_store_ssbo) {
                ok&=nir_src_is_const(in->src[0]);before+=nir_src_as_uint(in->src[0])==11;after+=nir_src_as_uint(in->src[0])==22;
            }
            atomics+=in->intrinsic==nir_intrinsic_ssbo_atomic;
            ok&=in->intrinsic!=nir_intrinsic_demote && in->intrinsic!=nir_intrinsic_demote_if;
        }
        ok&=before==1 && after==1-discarded && atomics==1-discarded;
        if(!ok){fprintf(stderr,"NIR fragment demotion case %u failed\n",discarded);nir_print_shader(b.shader,stderr);}
        ralloc_free(b.shader);if(!ok)return 0;
    }
    return qgpu_check_word_store_vectors() && qgpu_check_half_read_vectors() && qgpu_check_constant_windows() && qgpu_check_word_windows() &&
        qgpu_check_window_schedule() && qgpu_check_static_read_bounds() && qgpu_check_msaa_reuse();
}
#endif
struct qgpu_prepared_shader {
    struct ir3_compiler *compiler;
    struct gl_context *ctx;
    struct gl_shader_program *prog;
    nir_shader *nir;
    QGPUProgramDescription description;
};
static bool prepare_shader(const char *source, unsigned kind, struct qgpu_prepared_shader *prepared, bool constantWindows, char *error, unsigned errorBytes) {
    if (!source || !prepared || !error || !errorBytes || kind>2) return false;
    QGPUProgramDescription *description=&prepared->description;
    error[0]=0;
    gl_shader_stage stage=kind==0?MESA_SHADER_COMPUTE:kind==1?MESA_SHADER_VERTEX:MESA_SHADER_FRAGMENT;
    struct fd_dev_id id={.gpu_id=540};
    struct ir3_compiler *compiler=ir3_compiler_create(NULL,&id,fd_dev_info_raw(&id),
        &(struct ir3_compiler_options){.disable_cache=true,.qcom_runtime=true});
    struct gl_context *ctx=calloc(1,sizeof(*ctx)); struct gl_shader_program *prog=NULL;
    nir_shader *nir=NULL; bool ok=false;
    prepared->compiler=compiler;prepared->ctx=ctx;
    if (!compiler || !ctx) goto done;
    prog=qgpu_standalone_source(source,kind,ctx,error,errorBytes);prepared->prog=prog;
    if (!prog || !prog->data->LinkStatus || !prog->_LinkedShaders[stage] || !prog->_LinkedShaders[stage]->Program) goto done;
    nir=prog->_LinkedShaders[stage]->Program->nir;
    // Keep the shared NIR alive until both variants finish. The linked program
    // still supplies buffer/sampler metadata during the common lowering.
    ralloc_steal(NULL,nir);prepared->nir=nir;
    const nir_shader_compiler_options *options=ir3_get_compiler_options(compiler); nir->options=options;
    if (!qgpu_capture_interface(nir,description)) {
        snprintf(error,errorBytes,"Unsupported float32 stage interface");goto done;
    }
    // Metal instance_id includes baseInstance. NIR's INSTANCE_INDEX lowers to
    // the hardware zero-based instance sysval plus the VS driver constant.
    if (stage==MESA_SHADER_VERTEX) {
        nir_foreach_variable_with_modes(var,nir,nir_var_system_value)
            if (var->data.location==SYSTEM_VALUE_INSTANCE_ID) var->data.location=SYSTEM_VALUE_INSTANCE_INDEX;
    }
    if (stage==MESA_SHADER_FRAGMENT) {
        nir_foreach_variable_with_modes(var,nir,nir_var_shader_in) if (var->data.location==VARYING_SLOT_POS) {
            var->data.mode=nir_var_system_value;var->data.location=SYSTEM_VALUE_FRAG_COORD;
        }
        nir_fixup_deref_modes(nir);
    }
    NIR_PASS_V(nir,nir_lower_io_to_temporaries,nir_shader_get_entrypoint(nir),true,stage==MESA_SHADER_VERTEX || options->lower_all_io_to_temps);
    NIR_PASS_V(nir,nir_lower_global_vars_to_local);NIR_PASS_V(nir,nir_split_var_copies);NIR_PASS_V(nir,nir_lower_var_copies);
    NIR_PASS_V(nir,gl_nir_lower_atomics,prog,true);NIR_PASS_V(nir,gl_nir_lower_buffers,prog);
    if (!remap_buffers(nir,prog->_LinkedShaders[stage]->Program,description,constantWindows)) goto done;
    NIR_PASS_V(nir,nir_lower_atomics_to_ssbo,0);
    if (stage==MESA_SHADER_VERTEX) {
        nir_assign_var_locations(nir,nir_var_shader_in,&nir->num_inputs,ir3_glsl_type_size);
        NIR_PASS_V(nir,nir_lower_global_vars_to_local);
    }
    qgpu_assign_graphics_io(nir);
    nir_assign_var_locations(nir,nir_var_uniform,&nir->num_uniforms,ir3_glsl_type_size);
    NIR_PASS_V(nir,nir_lower_system_values);
    const nir_lower_compute_system_values_options cs={};NIR_PASS_V(nir,nir_lower_compute_system_values,&cs);
    NIR_PASS_V(nir,nir_lower_frexp);
    NIR_PASS_V(nir,nir_lower_io,nir_var_shader_in|nir_var_shader_out|nir_var_uniform,ir3_glsl_type_size,(nir_lower_io_options)0);
    if (stage==MESA_SHADER_COMPUTE || stage==MESA_SHADER_FRAGMENT || stage==MESA_SHADER_VERTEX) {
        NIR_PASS_V(nir,gl_nir_lower_samplers_as_deref,prog);
        if (!qgpu_bind_samples(nir,description)) goto done;
        NIR_PASS_V(nir,nir_lower_samplers);
    } else NIR_PASS_V(nir,gl_nir_lower_samplers,prog);
    if (!qgpu_remap_samples(nir,description)) {
        snprintf(error,errorBytes,"A540 descriptor profile exceeded (16 texture descriptors / 16 hardware sampler states)");goto done;
    }
    ir3_nir_lower_io_to_temporaries(nir);
    ok=true;
done:
    if (!ok && !error[0]) snprintf(error,errorBytes,"A540 shader compilation failed");
    return ok;
}
// Fold only scalar truncations of two bytes reconstructed from the same word.
// Keep all source loads/guards intact; changing a word load to a half load would
// change partial-binding and unaligned-access semantics.
static bool qgpu_packed_op(nir_scalar s,nir_op op) {
    return s.def->bit_size==32 && nir_scalar_is_alu(s) && nir_scalar_alu_op(s)==op;
}
static bool qgpu_packed_constant(nir_scalar s,unsigned value) {
    return s.def->bit_size==32 && nir_scalar_is_const(s) && nir_scalar_as_uint(s)==value;
}
static bool qgpu_packed_shift(nir_scalar s,nir_op op,unsigned amount,nir_scalar *input) {
    if(!qgpu_packed_op(s,op) || !qgpu_packed_constant(nir_scalar_chase_alu_src(s,1),amount))return false;
    *input=nir_scalar_chase_alu_src(s,0);return input->def->bit_size==32;
}
static bool qgpu_packed_mask(nir_scalar s,nir_scalar *input) {
    if(!qgpu_packed_op(s,nir_op_iand))return false;
    for(unsigned n=0;n<2;++n)if(qgpu_packed_constant(nir_scalar_chase_alu_src(s,n),255)) {
        *input=nir_scalar_chase_alu_src(s,1-n);return input->def->bit_size==32;
    }
    return false;
}
static bool qgpu_fold_packed_integer(nir_builder *b,nir_alu_instr *alu,void *opaque) {
    (void)opaque;
    if(alu->op!=nir_op_u2u16 || alu->def.num_components!=1 || alu->def.bit_size!=16)return false;
    nir_scalar joined=nir_scalar_chase_alu_src(nir_get_scalar(&alu->def,0),0);
    if(!qgpu_packed_op(joined,nir_op_ior))return false;
    for(unsigned order=0;order<2;++order) {
        nir_scalar lo,hi,x,y,tmp;
        if(!qgpu_packed_mask(nir_scalar_chase_alu_src(joined,order),&lo) ||
           !qgpu_packed_shift(nir_scalar_chase_alu_src(joined,1-order),nir_op_ishl,8,&hi))continue;
        unsigned shift;
        if(qgpu_packed_mask(hi,&tmp) && qgpu_packed_shift(tmp,nir_op_ushr,8,&y) && nir_scalar_equal(lo,y)) {
            x=lo;shift=0;
        } else if(qgpu_packed_shift(lo,nir_op_ushr,16,&x) &&
                  qgpu_packed_shift(hi,nir_op_ushr,24,&y) && nir_scalar_equal(x,y)) {
            shift=16;
        } else continue;
        b->cursor=nir_before_instr(&alu->instr);
        nir_def *word=nir_channel(b,x.def,x.comp);
        if(shift)word=nir_ushr_imm(b,word,shift);
        nir_def_rewrite_uses(&alu->def,nir_u2u16(b,word));nir_instr_remove(&alu->instr);return true;
    }
    return false;
}
static bool qgpu_fold_packed_integers(nir_shader *nir) {
    return nir_shader_alu_pass(nir,qgpu_fold_packed_integer,nir_metadata_control_flow,NULL);
}

static bool compile_variant(struct qgpu_prepared_shader *prepared, unsigned kind,
    QGPUProgramDescription *description, uint32_t *code, uint32_t *immediates, bool msaa, bool fullWindow, char *error, unsigned errorBytes,
    bool *msaaInvariant, struct ir3_shader **reusable) {
    if (msaaInvariant) *msaaInvariant=false;
    struct ir3_compiler *compiler=prepared->compiler;
    struct ir3_shader *shader=reusable?*reusable:NULL;
    if(reusable)*reusable=NULL;
    nir_shader *nir=NULL;
    bool ok=false;*description=prepared->description;
    // A pair may share finalized NIR, but never the IR3 per-key compile.
    // Sample-shaded input needs a different pixel-center transform and keeps
    // independent NIR. Full-window and ordinary pairs have separate owners.
    if(shader)goto variant;
    nir=nir_shader_clone(NULL,prepared->nir);
    if (!nir) goto done;
    if(fullWindow) {
        // Specialize only kernel-authored eligibility flags, never user data.
        // Native draw admission selects this variant only after independently
        // validating every window for the actual resolved GPU bindings.
        const unsigned first=qgpu_constant_window_flag(description->constantWindowCount,0);
        nir_foreach_function_impl(impl,nir) {
            nir_builder b=nir_builder_create(impl);
            nir_foreach_block(block,impl)nir_foreach_instr_safe(instr,block) {
                if(instr->type!=nir_instr_type_intrinsic)continue;
                nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
                if(in->intrinsic!=nir_intrinsic_load_const_ir3 || !nir_src_is_const(in->src[0]) ||
                    in->num_components!=1 || in->def.bit_size!=32)continue;
                const unsigned at=nir_src_as_uint(in->src[0]);
                if(at<first || at-first>=description->constantWindowCount)continue;
                b.cursor=nir_before_instr(instr);nir_def_rewrite_uses(&in->def,nir_imm_int(&b,1));nir_instr_remove(instr);
            }
            nir_metadata_preserve(impl,nir_metadata_none);
        }
    }
    qgpu_center_fragment_position(nir,msaa);
    ir3_finalize_nir(compiler,&(struct ir3_shader_nir_options){},nir);
    if(kind==2 && qgpu_fold_packed_integers(nir))NIR_PASS_V(nir,nir_opt_dce);
    shader=ir3_shader_from_nir(compiler,nir,&(struct ir3_shader_options){.num_reserved_user_consts=qgpu_memory_constants(description)/4},NULL);
    if (!shader) goto done;
    nir=NULL; // shader owns it now
variant:
    // This memory API exports machine code and reflection only. Requesting
    // disassembly also formats the complete NIR and ISA, then discards both
    // when the per-compilation shader is destroyed. The offline command-line
    // compiler requests its own disassembly for exported diagnostics.
    bool created=false;
    // get_variant attaches allocations to shader; create_variant(NULL owner)
    // would leak its binary/constant state when the shader is destroyed.
    struct ir3_shader_variant *v=ir3_shader_get_variant(shader,&(struct ir3_shader_key){.msaa=msaa},false,false,&created);
    if (!v || !v->bin) goto done;
    qgpu_pad_memory_constants(v,description);
    const struct ir3_const_state *c=ir3_const_state(v);
    if (v->pvtmem_size || v->shared_size>QGPU_MAX_THREADGROUP_BYTES || v->constant_data_size || c->num_ubos ||
        !v->info.size || v->info.size>QGPU_MAX_CODE_WORDS*4 || v->instrlen>QGPU_MAX_INSTRUCTION_UNITS || v->constlen>128 ||
        c->immediates_count>512 || v->num_samp>(kind==0?QGPU_STAGE_TEXTURE_COUNT:QGPU_GRAPHICS_TEXTURE_COUNT) || !qgpu_export_interface(v,description)) {
        snprintf(error,errorBytes,"Shader exceeds A540 profile (code=%u bytes, instrlen=%u, constants=%u, private=%u, shared=%u)",
            v->info.size,v->instrlen,v->constlen,v->pvtmem_size,v->shared_size);goto done;
    }
    description->sampleFlags|=msaa?QGPU_SAMPLE_MSAA:0;
    QGPUProgramDescription *p=description;
    p->stage=kind;p->codeWords=v->info.size/4;p->instrlen=v->instrlen;p->constlen=v->constlen;
    p->fullRegs=v->info.max_reg<0?1:v->info.max_reg+1;p->halfRegs=v->info.max_half_reg+1;
    p->branchStack=ir3_shader_branchstack_hw(v);p->threadSize=v->info.double_threadsize;p->subgroupSize=v->info.subgroup_size;
    p->immediateBase=c->immediates_count?c->offsets.immediate*4:0;p->immediateCount=c->immediates_count;
    p->driverBase=v->need_driver_params?c->offsets.driver_param*4:0;p->driverCount=v->need_driver_params?c->num_driver_params:0;
    p->localID=ir3_find_sysval_regid(v,SYSTEM_VALUE_LOCAL_INVOCATION_ID);p->groupID=ir3_find_sysval_regid(v,SYSTEM_VALUE_WORKGROUP_ID);
    p->vertexID=ir3_find_sysval_regid(v,SYSTEM_VALUE_VERTEX_ID_ZERO_BASE);p->position=ir3_find_output_regid(v,VARYING_SLOT_POS);
    p->instanceID=ir3_find_sysval_regid(v,SYSTEM_VALUE_INSTANCE_ID);
    p->color=ir3_find_output_regid(v,v->color0_mrt?FRAG_RESULT_COLOR:FRAG_RESULT_DATA0);
    p->fragCoord=ir3_find_sysval_regid(v,SYSTEM_VALUE_FRAG_COORD);p->fragMask=v->fragcoord_compmask;
    p->flags=(p->flags&(QGPU_DISPATCH_GEOMETRY|QGPU_ARRAY_TEXTURE_MASK|QGPU_CUBE_TEXTURE_MASK|QGPU_BUFFER_SIZE_CONSTANTS|QGPU_FRAMEBUFFER_TILE_ORIGIN))|(v->need_pixlod?1:0)|(v->no_earlyz || v->writes_smask?2:0)|(v->has_kill?QGPU_FRAGMENT_DISCARD:0)|(v->type==MESA_SHADER_FRAGMENT && v->writes_pos?QGPU_FRAGMENT_DEPTH:0)|
        (v->type==MESA_SHADER_COMPUTE && v->local_size_variable?QGPU_VARIABLE_LOCAL_SIZE:0)|
        QGPU_BOUNDED_BUFFERS|(v->type==MESA_SHADER_FRAGMENT?QGPU_FRAGMENT_BUFFER_BANK:0)|
        (fullWindow?QGPU_CONSTANT_WINDOWS_REQUIRED:0)|
        (v->has_barrier?QGPU_THREADGROUP_BARRIER:0)|(p->threadgroupMask?QGPU_DYNAMIC_THREADGROUP:0);p->textureCount=v->num_samp;
    if (!p->threadgroupMask) p->sharedBytes=v->shared_size;
    memcpy(code,v->bin,v->info.size);memcpy(immediates,c->immediates,c->immediates_count*4);ok=true;
    // The pinned A5xx backend consumes key.msaa for sample/centroid
    // barycentrics. The standalone path does not always refresh shader_info
    // before creating the key mask, so also inspect actual finalized NIR and
    // declared interpolation modes. Our pixel-center transform precedes IR3;
    // conservatively exclude all sample-shaded inputs as well.
    if (msaaInvariant && kind==2 && !msaa && !prepared->nir->info.fs.uses_sample_shading && !shader->key_mask.msaa) {
        bool invariant=true;
        for(unsigned i=0;i<prepared->description.varyingCount;++i)
            if(prepared->description.varyings[i].mode>=3)invariant=false;
        nir_foreach_function_impl(impl,shader->nir)
            nir_foreach_block(block,impl)nir_foreach_instr(instr,block) {
                if(instr->type!=nir_instr_type_intrinsic)continue;
                nir_intrinsic_op op=nir_instr_as_intrinsic(instr)->intrinsic;
                if(op==nir_intrinsic_load_barycentric_sample || op==nir_intrinsic_load_barycentric_centroid ||
                   op==nir_intrinsic_load_barycentric_at_sample)invariant=false;
            }
        *msaaInvariant=invariant;
    }
done:
    if (!ok && !error[0]) snprintf(error,errorBytes,"A540 shader compilation failed");
    if (shader && ok && reusable && !prepared->nir->info.fs.uses_sample_shading)*reusable=shader;
    else if (shader) ir3_shader_destroy(shader);
    else if (nir) ralloc_free(nir);
    return ok;
}
static bool compile_variants(struct qgpu_prepared_shader *prepared, unsigned kind,
    QGPUProgramDescription *description, uint32_t *code, uint32_t *immediates, QGPUProgramVariant *msaa,
    bool fullWindow, char *error, unsigned errorBytes) {
    bool invariant=false;
    struct ir3_shader *shared=NULL;
    bool ok=compile_variant(prepared,kind,description,code,immediates,false,fullWindow,error,errorBytes,&invariant,kind==2?&shared:NULL);
    if (!ok || kind!=2)return ok;
    if (!invariant) {
        ok=compile_variant(prepared,kind,&msaa->info,msaa->code,msaa->immediates,true,fullWindow,error,errorBytes,NULL,&shared);
        if(shared)ir3_shader_destroy(shared);
        return ok;
    }
    if(shared)ir3_shader_destroy(shared);
    // Preserve the complete MSAA ABI record, including canonical zero tails.
    // GPU admission still sees its normal sample-mode marker and validations.
    msaa->info=*description;msaa->info.sampleFlags|=QGPU_SAMPLE_MSAA;
    memcpy(msaa->code,code,sizeof(msaa->code));memcpy(msaa->immediates,immediates,sizeof(msaa->immediates));
    return true;
}
static void destroy_prepared_shader(struct qgpu_prepared_shader *prepared, unsigned kind) {
    gl_shader_stage stage=kind==0?MESA_SHADER_COMPUTE:kind==1?MESA_SHADER_VERTEX:MESA_SHADER_FRAGMENT;
    if (prepared->nir) {
        if (prepared->prog && prepared->prog->_LinkedShaders[stage] && prepared->prog->_LinkedShaders[stage]->Program)
            prepared->prog->_LinkedShaders[stage]->Program->nir=NULL;
        ralloc_free(prepared->nir);
    }
    if (prepared->prog) standalone_compiler_cleanup(prepared->prog);
    free(prepared->ctx);
    if (prepared->compiler) ir3_compiler_destroy(prepared->compiler);
}

#ifndef __APPLE__
static int qgpu_check_msaa_reuse(void) {
    // Independently compile MSAA even when the optimized path would copy it.
    // Compare the complete records, including registers, constants and tails.
    const struct { const char *source; bool reusable,window; } cases[]={
        {"#version 430\nlayout(location=0) out vec4 c;void main(){c=vec4(0.25);}",true,false},
        {"#version 430\nlayout(location=0) out vec4 c;void main(){c=gl_FragCoord*0.25;}",true,false},
        {"#version 430\nlayout(location=0) out vec4 c;void main(){c=gl_FragCoord*float(gl_SampleID+1);}",false,false},
        {"#version 430\nlayout(location=0) centroid in vec4 v;layout(location=0) out vec4 c;void main(){c=v;}",false,false},
        {"#version 430\nlayout(location=0) sample in vec4 v;layout(location=0) out vec4 c;void main(){c=v;}",false,false},
        {"#version 430\nlayout(location=0) in vec4 v;layout(location=0) out vec4 c;void main(){c=interpolateAtCentroid(v);}",false,false},
        {"#version 430\nlayout(std430,binding=0) readonly buffer B{vec4 v[];} _qbuffer0;"
         "layout(location=0) out vec4 c;void main(){c=_qbuffer0.v[0]+_qbuffer0.v[1]+_qbuffer0.v[2];}",true,true},
    };
    for(unsigned i=0;i<ARRAY_SIZE(cases);++i) {
        struct qgpu_prepared_shader p={0};char error[4096]={0};
        bool ok=prepare_shader(cases[i].source,2,&p,true,error,sizeof(error));
        if(ok && cases[i].window)ok=p.description.constantWindowCount!=0;
        for(unsigned full=0;ok && full<(cases[i].window?2u:1u);++full) {
            QGPUProgramVariant base={0},independent={0},pairedBase={0},pairedMSAA={0};bool invariant=false;
            ok=compile_variant(&p,2,&base.info,base.code,base.immediates,false,full,error,sizeof(error),&invariant,NULL);
            if(ok && invariant!=cases[i].reusable)
                snprintf(error,sizeof(error),"reuse decision=%u expected=%u",invariant,cases[i].reusable);
            ok=ok && invariant==cases[i].reusable &&
                compile_variant(&p,2,&independent.info,independent.code,independent.immediates,true,full,error,sizeof(error),NULL,NULL) &&
                compile_variants(&p,2,&pairedBase.info,pairedBase.code,pairedBase.immediates,&pairedMSAA,full,error,sizeof(error)) &&
                !memcmp(&base,&pairedBase,sizeof(base)) && !memcmp(&independent,&pairedMSAA,sizeof(independent));
        }
        destroy_prepared_shader(&p,2);
        if(!ok){fprintf(stderr,"MSAA reuse case %u failed: %s\n",i,error);return 0;}
    }
    printf("MSAA reuse: exact independent output, pixel center, sample/centroid exclusions and full windows passed\n");
    return 1;
}
#endif

int qgpu_compile_glsl(const char *source, unsigned kind, QGPUProgramImage *out, char *error, unsigned errorBytes) {
    if (!out) return 0;
    for(unsigned attempt=0;attempt<2;++attempt) {
        // A larger cold fallback may exceed the ISA/register profile. Retry
        // both variants without promotion instead of losing shader support.
        // Clear unused code tails too so the resulting image is canonical.
        memset(&out->info,0,sizeof(out->info));memset(out->code,0,sizeof(out->code));
        memset(out->immediates,0,sizeof(out->immediates));memset(&out->msaa,0,sizeof(out->msaa));
        memset(&out->constantWindow,0,sizeof(out->constantWindow));memset(&out->msaaConstantWindow,0,sizeof(out->msaaConstantWindow));
        struct qgpu_prepared_shader prepared={};
        bool ok=prepare_shader(source,kind,&prepared,attempt==0,error,errorBytes);
        // Preserve promotion metadata on preparation failure so the existing
        // no-promotion fallback can still retry the same source.
        out->info=prepared.description;
        if(ok)ok=compile_variants(&prepared,kind,&out->info,out->code,out->immediates,&out->msaa,false,error,errorBytes);
        if(ok && kind==2 && out->info.constantWindowCount) {
            const bool specialized=compile_variants(&prepared,kind,&out->constantWindow.info,out->constantWindow.code,
                out->constantWindow.immediates,&out->msaaConstantWindow,true,error,errorBytes);
            if(!specialized) {
                // An optional optimization cannot remove ordinary support.
                memset(&out->constantWindow,0,sizeof(out->constantWindow));
                memset(&out->msaaConstantWindow,0,sizeof(out->msaaConstantWindow));
                if(errorBytes)error[0]=0;
            }
        }
        destroy_prepared_shader(&prepared,kind);
        if(ok)return 1;
        if(!out->info.constantWindowCount && !out->msaa.info.constantWindowCount)return 0;
    }
    return 0;
}

// AIR builder entry: reuse the exact guards, lowering and variant compiler.
int qgpu_compile_nir(nir_shader *input,const QGPUProgramDescription *reflection,QGPUProgramImage *out,char *error,unsigned errorBytes) {
    if(!input || !reflection || !out || !error || !errorBytes || reflection->stage>2)return 0;
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
            ir3_nir_lower_io_to_temporaries(nir);
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
