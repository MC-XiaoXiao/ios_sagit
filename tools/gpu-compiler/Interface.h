// SPDX-License-Identifier: BSD-2-Clause
// Shared by the Mesa command-line exporter and in-process backend. Callers
// include Mesa's NIR/IR3 headers first. No Metal or kernel ABI dependencies.
#pragma once
#include "GPUJITProtocol.h"
#include <string.h>
static void qgpu_sort_variables(nir_shader *nir, nir_variable_mode mode) {
    struct exec_list sorted; exec_list_make_empty(&sorted);
    nir_foreach_variable_with_modes_safe(var, nir, mode) {
        exec_node_remove(&var->node); bool inserted = false;
        nir_foreach_variable_in_list(other, &sorted) {
            if (other->data.location > var->data.location) {
                exec_node_insert_node_before(&other->node, &var->node); inserted = true; break;
            }
        }
        if (!inserted) exec_list_push_tail(&sorted, &var->node);
    }
    exec_list_append(&nir->variables, &sorted);
}
// Both compiler entry points must assign driver locations by semantic
// location, including sparse fragment colors declared in arbitrary order.
static void qgpu_assign_graphics_io(nir_shader *nir) {
    if(nir->info.stage==MESA_SHADER_COMPUTE)return;
    qgpu_sort_variables(nir,nir_var_shader_out);qgpu_sort_variables(nir,nir_var_shader_in);
    nir_assign_var_locations(nir,nir_var_shader_out,&nir->num_outputs,ir3_glsl_type_size);
    if(nir->info.stage==MESA_SHADER_FRAGMENT)nir_assign_var_locations(nir,nir_var_shader_in,&nir->num_inputs,ir3_glsl_type_size);
}
// The original AIR matrix is a set of contiguous half values. Combine only
// constant-offset, readonly fragment half loads before guards scalarize them.
// Mesa retains its alias/barrier checks; holes, volatile loads, writable and
// dynamic-offset accesses keep their existing lowering.
static bool qgpu_vectorize_half_read(unsigned align_mul,unsigned align_offset,
    unsigned bits,unsigned components,unsigned hole,nir_intrinsic_instr *low,
    nir_intrinsic_instr *high,void *data) {
    (void)data;
    if(!(!hole && bits==16 && components<=4 && align_mul>=2 && !(align_offset&1) &&
        low->intrinsic==nir_intrinsic_load_ssbo && high->intrinsic==nir_intrinsic_load_ssbo &&
        low->def.bit_size==16 && high->def.bit_size==16 &&
        (nir_intrinsic_access(low)&ACCESS_NON_WRITEABLE) &&
        (nir_intrinsic_access(high)&ACCESS_NON_WRITEABLE) &&
        !((nir_intrinsic_access(low)|nir_intrinsic_access(high))&ACCESS_VOLATILE) &&
        nir_src_is_const(low->src[1]) && nir_src_is_const(high->src[1])))return false;
    uint64_t lo=nir_src_as_uint(low->src[1]),hi=nir_src_as_uint(high->src[1]);
    // Independent expressions on opposite sides of uint32 wrap must not
    // acquire the combined vector's different per-component overflow rule.
    return hi>=lo && hi-lo<=6 && lo+2*components<=0x100000000ull;
}
static void qgpu_vectorize_half_reads(nir_shader *nir) {
    if(nir->info.stage!=MESA_SHADER_FRAGMENT)return;
    bool candidate=false;
    nir_foreach_function_impl(impl,nir)nir_foreach_block(block,impl)nir_foreach_instr(instr,block) {
        if(instr->type!=nir_instr_type_intrinsic)continue;
        nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
        candidate|=in->intrinsic==nir_intrinsic_load_ssbo && in->def.bit_size==16 &&
            (nir_intrinsic_access(in)&ACCESS_NON_WRITEABLE) && !(nir_intrinsic_access(in)&ACCESS_VOLATILE);
    }
    if(!candidate)return;
    NIR_PASS_V(nir,nir_lower_vars_to_ssa);
    NIR_PASS_V(nir,nir_copy_prop);NIR_PASS_V(nir,nir_opt_constant_folding);
    NIR_PASS_V(nir,nir_opt_cse);
    NIR_PASS_V(nir,nir_opt_dce);
    const nir_load_store_vectorize_options options={
        .callback=qgpu_vectorize_half_read,.modes=nir_var_mem_ssbo,
    };
    NIR_PASS_V(nir,nir_opt_load_store_vectorize,&options);
}
// Guard final byte offsets, after GL block indices have been remapped. Every
// bound range has at least four bytes (checked by native admission). Scalar
// fallback loads at zero are therefore safe even when a vector spans the end;
// each invalid component returns zero, and invalid stores do not execute.
typedef struct qgpu_memory_guard { bool supported;nir_variable *discarded;QGPUProgramDescription *description; } qgpu_memory_guard;
// Promote static readonly 16/32-bit fragment ranges without changing their
// integer bits. Several nonoverlapping 64B windows may name one buffer. Native
// submission validates every actual bound source independently before loading.
static bool qgpu_window_candidate(const nir_intrinsic_instr *in) {
    return in->intrinsic==nir_intrinsic_load_ssbo && (in->def.bit_size==16 || in->def.bit_size==32) &&
        in->num_components && in->num_components<=4 &&
        (nir_intrinsic_access(in)&ACCESS_NON_WRITEABLE) &&
        !(nir_intrinsic_access(in)&(ACCESS_VOLATILE|ACCESS_COHERENT)) &&
        nir_src_is_const(in->src[0]) && nir_src_is_const(in->src[1]) && nir_src_bit_size(in->src[1])==32;
}
static void qgpu_collect_constant_windows(nir_shader *nir,QGPUProgramDescription *p) {
    if(!p)return;
    p->constantWindowCount=0;memset(p->constantWindows,0,sizeof(p->constantWindows));
    if(nir->info.stage!=MESA_SHADER_FRAGMENT)return;
    nir_foreach_function_impl(impl,nir)nir_foreach_block(block,impl)nir_foreach_instr(instr,block) {
        if(instr->type!=nir_instr_type_intrinsic)continue;
        nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
        if(in->intrinsic==nir_intrinsic_store_ssbo || in->intrinsic==nir_intrinsic_ssbo_atomic || in->intrinsic==nir_intrinsic_ssbo_atomic_swap)return;
    }
    // Retain the existing high-density windows first. A small uniform load
    // still executes once per fragment through SSBO if it is left behind;
    // use otherwise unused constant-bank capacity for those compact ranges.
    // Runtime range/alignment checks and the guarded SSBO fallback still
    // apply to the entire 64B source, including short Metal setBytes ranges.
    for(unsigned pass=0;pass<2;++pass)
    for(unsigned slot=0;slot<8 && p->constantWindowCount<QGPU_MAX_CONSTANT_WINDOWS;++slot) {
        uint64_t first=UINT64_MAX,last=0;unsigned total=0;
        nir_foreach_function_impl(impl,nir)nir_foreach_block(block,impl)nir_foreach_instr(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(!qgpu_window_candidate(in) || nir_src_as_uint(in->src[0])!=QGPU_FRAGMENT_BUFFER_BASE+slot)continue;
            uint64_t at=nir_src_as_uint(in->src[1]);unsigned bytes=in->def.bit_size/8;
            if((at&(bytes-1)) || at+bytes*in->num_components>UINT32_MAX)continue;
            if(at<first)first=at;
            if(at+bytes*in->num_components>last)last=at+bytes*in->num_components;
            total+=bytes*in->num_components;
        }
        if(first==UINT64_MAX || (pass==0 ? total<32 : total>=32))continue;
        if(pass && last-first>QGPU_CONSTANT_WINDOW_BYTES)continue;
        // Preserve the previous compact half range's ending alignment, so
        // its complete 64B source stays valid for the same real bindings.
        if(last-first<=QGPU_CONSTANT_WINDOW_BYTES) {
            uint64_t end=(last+15)&~15ull;
            if(end<QGPU_CONSTANT_WINDOW_BYTES)end=QGPU_CONSTANT_WINDOW_BYTES;
            uint64_t start=end-QGPU_CONSTANT_WINDOW_BYTES;
            if(start<=first && end<=UINT32_MAX)
                p->constantWindows[p->constantWindowCount++]=(QGPUConstantWindow){slot,(uint32_t)start};
            continue;
        }
        uint64_t floor=0;
        while(p->constantWindowCount<QGPU_MAX_CONSTANT_WINDOWS) {
            uint64_t next=UINT64_MAX;unsigned score=0;
            nir_foreach_function_impl(impl,nir)nir_foreach_block(block,impl)nir_foreach_instr(instr,block) {
                if(instr->type!=nir_instr_type_intrinsic)continue;
                nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
                if(!qgpu_window_candidate(in) || nir_src_as_uint(in->src[0])!=QGPU_FRAGMENT_BUFFER_BASE+slot)continue;
                uint64_t at=nir_src_as_uint(in->src[1]);unsigned bytes=in->def.bit_size/8;
                if((at&(bytes-1)) || at<floor || at+bytes*in->num_components>UINT32_MAX)continue;
                if(at<next)next=at;
            }
            if(next==UINT64_MAX)break;
            uint64_t start=next&~(uint64_t)(QGPU_CONSTANT_WINDOW_BYTES-1),end=start+QGPU_CONSTANT_WINDOW_BYTES;
            if(end>UINT32_MAX)break;
            nir_foreach_function_impl(impl,nir)nir_foreach_block(block,impl)nir_foreach_instr(instr,block) {
                if(instr->type!=nir_instr_type_intrinsic)continue;
                nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
                if(!qgpu_window_candidate(in) || nir_src_as_uint(in->src[0])!=QGPU_FRAGMENT_BUFFER_BASE+slot)continue;
                uint64_t at=nir_src_as_uint(in->src[1]);unsigned bytes=in->def.bit_size/8;
                if((at&(bytes-1)) || at<start || at+bytes*in->num_components>end)continue;
                score+=in->num_components*bytes;
            }
            if(score>=32)p->constantWindows[p->constantWindowCount++]=(QGPUConstantWindow){slot,(uint32_t)start};
            floor=end;
        }
    }
    // Restore the wire format's buffer/offset order before generating the
    // guard indices and their matching runtime constant-bank metadata.
    for(unsigned i=1;i<p->constantWindowCount;++i) {
        QGPUConstantWindow value=p->constantWindows[i];unsigned j=i;
        while(j && (p->constantWindows[j-1].buffer>value.buffer ||
            (p->constantWindows[j-1].buffer==value.buffer &&
             p->constantWindows[j-1].byteOffset>value.byteOffset))) {
            p->constantWindows[j]=p->constantWindows[j-1];--j;
        }
        p->constantWindows[j]=value;
    }
}
// A5xx cannot demote a lane in hardware. Keep it executing for derivatives,
// predicate externally visible writes, and terminate only at the shared exit.
// Private variables, reads and arithmetic still execute in discarded lanes.
static nir_variable *qgpu_lower_fragment_demote(nir_shader *nir) {
    if(nir->info.stage!=MESA_SHADER_FRAGMENT)return NULL;
    bool found=false;
    nir_foreach_function_impl(impl,nir) nir_foreach_block(block,impl) nir_foreach_instr(instr,block) {
        if(instr->type!=nir_instr_type_intrinsic)continue;
        nir_intrinsic_op op=nir_instr_as_intrinsic(instr)->intrinsic;
        found|=op==nir_intrinsic_demote || op==nir_intrinsic_demote_if;
    }
    if(!found)return NULL;
    NIR_PASS_V(nir,nir_lower_returns);
    nir_function_impl *impl=nir_shader_get_entrypoint(nir);
    nir_variable *discarded=nir_local_variable_create(impl,glsl_bool_type(),"_qdiscarded");
    nir_builder b=nir_builder_at(nir_before_impl(impl));
    nir_store_var(&b,discarded,nir_imm_false(&b),1);
    nir_foreach_block(block,impl) nir_foreach_instr_safe(instr,block) {
        if(instr->type!=nir_instr_type_intrinsic)continue;
        nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
        if(in->intrinsic!=nir_intrinsic_demote && in->intrinsic!=nir_intrinsic_demote_if)continue;
        b.cursor=nir_before_instr(instr);
        nir_def *value=in->intrinsic==nir_intrinsic_demote?nir_imm_true(&b):
            nir_ior(&b,nir_load_var(&b,discarded),in->src[0].ssa);
        nir_store_var(&b,discarded,value,1);nir_instr_remove(instr);
    }
    b.cursor=nir_after_block_before_jump(nir_impl_last_block(impl));
    nir_terminate_if(&b,nir_load_var(&b,discarded));
    nir_metadata_preserve(impl,nir_metadata_none);
    return discarded;
}
// Static readonly fragment offsets admit an overflow-free end comparison.
// Keep all dynamic/shared/store/volatile/coherent paths on the original guard.
static nir_def *qgpu_static_read_bound(nir_builder *b,nir_intrinsic_instr *in,nir_def *size,
    unsigned offset_src,unsigned relative,unsigned span) {
    if(b->shader->info.stage!=MESA_SHADER_FRAGMENT || in->intrinsic!=nir_intrinsic_load_ssbo ||
       !(nir_intrinsic_access(in)&ACCESS_NON_WRITEABLE) ||
       (nir_intrinsic_access(in)&(ACCESS_VOLATILE|ACCESS_COHERENT)) ||
       !nir_src_is_const(in->src[offset_src]) || nir_src_bit_size(in->src[offset_src])!=32)return NULL;
    uint64_t at=(uint64_t)nir_src_as_uint(in->src[offset_src])+relative,end=at+span;
    if(end>UINT32_MAX || (in->def.bit_size==16 && (at&1)))return nir_imm_false(b);
    return nir_uge_imm(b,size,(uint32_t)end);
}
static bool qgpu_guard_access_fallback(nir_builder *b,nir_intrinsic_instr *in,void *opaque) {
    qgpu_memory_guard *guard=(qgpu_memory_guard *)opaque;
    const bool shared_atomic=in->intrinsic==nir_intrinsic_shared_atomic || in->intrinsic==nir_intrinsic_shared_atomic_swap;
    const bool atomic=shared_atomic || in->intrinsic==nir_intrinsic_ssbo_atomic || in->intrinsic==nir_intrinsic_ssbo_atomic_swap;
    if (atomic) {
        nir_atomic_op op=nir_intrinsic_atomic_op(in);
        if (in->def.bit_size!=32 || in->def.num_components!=1 ||
            (op!=nir_atomic_op_iadd && op!=nir_atomic_op_imin && op!=nir_atomic_op_umin &&
             op!=nir_atomic_op_imax && op!=nir_atomic_op_umax && op!=nir_atomic_op_iand &&
             op!=nir_atomic_op_ior && op!=nir_atomic_op_ixor && op!=nir_atomic_op_xchg && op!=nir_atomic_op_cmpxchg)) {
            guard->supported=false;return false;
        }
        b->cursor=nir_before_instr(&in->instr);unsigned offset_src=shared_atomic?0:1;
        nir_def *offset=in->src[offset_src].ssa,*base_valid=nir_imm_true(b),*size;
        if (offset->bit_size!=32) { guard->supported=false;return false; }
        if (shared_atomic) {
            nir_def *relative=offset;offset=nir_iadd_imm(b,relative,nir_intrinsic_base(in));
            base_valid=nir_uge(b,offset,relative);size=nir_imm_int(b,b->shader->info.shared_size);
        } else {
            b->shader->info.writes_memory=true;size=nir_get_ssbo_size(b,in->src[0].ssa);
        }
        nir_def *valid=nir_iand(b,base_valid,nir_iand(b,nir_ieq_imm(b,nir_iand_imm(b,offset,3),0),
            nir_iand(b,nir_uge_imm(b,size,4),nir_uge(b,nir_iadd_imm(b,size,-4),offset))));
        if(guard->discarded)valid=nir_iand(b,valid,nir_inot(b,nir_load_var(b,guard->discarded)));
        nir_intrinsic_instr *guarded=nir_instr_as_intrinsic(nir_instr_clone(b->shader,&in->instr));
        guarded->src[offset_src]=nir_src_for_ssa(offset);if (shared_atomic) nir_intrinsic_set_base(guarded,0);
        nir_push_if(b,valid);nir_builder_instr_insert(b,&guarded->instr);nir_push_else(b,NULL);
        nir_def *zero=nir_imm_int(b,0);nir_pop_if(b,NULL);
        nir_def_rewrite_uses(&in->def,nir_if_phi(b,&guarded->def,zero));nir_instr_remove(&in->instr);return true;
    }
    const bool shared=in->intrinsic==nir_intrinsic_load_shared || in->intrinsic==nir_intrinsic_store_shared;
    const bool load=in->intrinsic==nir_intrinsic_load_ssbo || in->intrinsic==nir_intrinsic_load_shared;
    const bool store=in->intrinsic==nir_intrinsic_store_ssbo || in->intrinsic==nir_intrinsic_store_shared;
    if (!load && !store) return false;
    // Linked GLSL can miss writes_memory on a store behind early returns.
    // IR3 uses this bit to defer fragment depth testing around side effects.
    if (store && !shared) b->shader->info.writes_memory=true;
    const unsigned components=in->num_components;
    const unsigned bits=load?in->def.bit_size:nir_src_bit_size(in->src[0]),bytes=bits/8;
    if (!components || components>4 || (bits!=32 && bits!=16)) {
        guard->supported=false;return false;
    }
    b->cursor=nir_before_instr(&in->instr);
    const unsigned index_src=load?0:1,offset_src=shared?(load?0:1):(load?1:2);
    nir_def *offset=in->src[offset_src].ssa;
    if (offset->bit_size!=32) { guard->supported=false;return false; }
    nir_def *base_valid=nir_imm_true(b),*size,*values[4];
    if (shared) {
        nir_def *relative=offset;offset=nir_iadd_imm(b,relative,nir_intrinsic_base(in));
        base_valid=nir_uge(b,offset,relative);size=nir_imm_int(b,b->shader->info.shared_size);
    } else size=nir_get_ssbo_size(b,in->src[index_src].ssa);
    // A fully bounded readonly half vector uses one native LDGB. The partial
    // range path below still reads each valid component separately. Never
    // speculatively read a vector from zero: a short binding may not fit it.
    nir_intrinsic_instr *wide=NULL;
    if(load && !shared && bits==16 && components>1 && b->shader->info.stage==MESA_SHADER_FRAGMENT &&
       nir_src_is_const(in->src[offset_src]) &&
       (nir_intrinsic_access(in)&ACCESS_NON_WRITEABLE) &&
       !(nir_intrinsic_access(in)&ACCESS_VOLATILE)) {
        unsigned span=components*bytes;
        nir_def *valid=qgpu_static_read_bound(b,in,size,offset_src,0,span);
        if(!valid)valid=nir_iand(b,base_valid,nir_iand(b,nir_ieq_imm(b,nir_iand_imm(b,offset,1),0),
            nir_iand(b,nir_uge_imm(b,size,span),nir_uge(b,nir_iadd_imm(b,size,-(int)span),offset))));
        wide=nir_instr_as_intrinsic(nir_instr_clone(b->shader,&in->instr));
        wide->src[offset_src]=nir_src_for_ssa(offset);
        // Bounds are proved only inside this branch. Do not let later NIR
        // peephole-select/GCM speculate this load across its range check.
        nir_intrinsic_set_access(wide,nir_intrinsic_access(in)&~ACCESS_CAN_REORDER);
        nir_push_if(b,valid);nir_builder_instr_insert(b,&wide->instr);nir_push_else(b,NULL);
    }
    // Keep a fully bounded compute uint4 store intact for A5xx STGB. Clear
    // kernels otherwise become four independently guarded scalar stores.
    // Partial bindings retain the exact per-component behavior below.
    if(store && !shared && bits==32 && components==4 &&
       b->shader->info.stage==MESA_SHADER_COMPUTE && nir_intrinsic_write_mask(in)==15 &&
       nir_intrinsic_align_mul(in)>=4 && !(nir_intrinsic_align_offset(in)&3) &&
       !(nir_intrinsic_access(in)&(ACCESS_VOLATILE|ACCESS_COHERENT))) {
        nir_def *valid=nir_iand(b,nir_uge_imm(b,size,16),nir_uge(b,nir_iadd_imm(b,size,-16),offset));
        wide=nir_instr_as_intrinsic(nir_instr_clone(b->shader,&in->instr));
        nir_push_if(b,valid);nir_builder_instr_insert(b,&wide->instr);nir_push_else(b,NULL);
    }
    for (unsigned i=0;i<components;++i) {
        if (store && !(nir_intrinsic_write_mask(in)&(1u<<i))) continue;
        nir_def *at=nir_iadd_imm(b,offset,i*bytes);
        nir_def *valid=qgpu_static_read_bound(b,in,size,offset_src,i*bytes,bytes);
        if(!valid) {
            valid=nir_iand(b,nir_uge(b,at,offset),
                nir_iand(b,nir_uge_imm(b,size,bytes),nir_uge(b,nir_iadd_imm(b,size,-(int)bytes),at)));
            if(bits==16)valid=nir_iand(b,valid,nir_ieq_imm(b,nir_iand_imm(b,at,1),0));
            valid=nir_iand(b,base_valid,valid);
        }
        if(store && guard->discarded)valid=nir_iand(b,valid,nir_inot(b,nir_load_var(b,guard->discarded)));
        nir_intrinsic_instr *scalar=nir_instr_as_intrinsic(nir_instr_clone(b->shader,&in->instr));
        scalar->num_components=1;
        if (shared) nir_intrinsic_set_base(scalar,0);
        // Native bases are four-byte aligned; typed half accesses may address
        // either two-byte lane. Never widen their bounds or stores to a word.
        nir_intrinsic_set_align_mul(scalar,bytes);nir_intrinsic_set_align_offset(scalar,0);
        if (load) {
            scalar->def.num_components=1;
            scalar->src[offset_src]=nir_src_for_ssa(nir_bcsel(b,valid,at,nir_imm_int(b,0)));
            nir_builder_instr_insert(b,&scalar->instr);
            values[i]=nir_bcsel(b,valid,&scalar->def,nir_imm_intN_t(b,0,bits));
        } else {
            scalar->src[0]=nir_src_for_ssa(nir_channel(b,in->src[0].ssa,i));
            scalar->src[offset_src]=nir_src_for_ssa(at);nir_intrinsic_set_write_mask(scalar,1);
            nir_push_if(b,valid);nir_builder_instr_insert(b,&scalar->instr);nir_pop_if(b,NULL);
        }
    }
    if (load) {
        nir_def *value=nir_vec(b,values,components);
        if(wide) {nir_pop_if(b,NULL);value=nir_if_phi(b,&wide->def,value);}
        nir_def_rewrite_uses(&in->def,value);
    } else if(wide) nir_pop_if(b,NULL);
    nir_instr_remove(&in->instr);return true;
}
// A kernel-authored per-draw flag proves the complete 64B source window was
// eligible and loaded. Preserve 16/32-bit integer payloads, including NaN/sign.
static bool qgpu_guard_access(nir_builder *b,nir_intrinsic_instr *in,void *opaque) {
    qgpu_memory_guard *guard=(qgpu_memory_guard *)opaque;
    QGPUProgramDescription *p=guard->description;
    if(!p || !p->constantWindowCount || !qgpu_window_candidate(in) ||
        !in->num_components || in->num_components>4 ||
        !(nir_intrinsic_access(in)&ACCESS_NON_WRITEABLE) ||
        (nir_intrinsic_access(in)&(ACCESS_VOLATILE|ACCESS_COHERENT)) ||
        !nir_src_is_const(in->src[0]) || !nir_src_is_const(in->src[1]) || nir_src_bit_size(in->src[1])!=32)
        return qgpu_guard_access_fallback(b,in,opaque);
    unsigned slot=nir_src_as_uint(in->src[0]),at=nir_src_as_uint(in->src[1]),n=in->num_components,bytes=in->def.bit_size/8;
    if((at&(bytes-1)) || slot<QGPU_FRAGMENT_BUFFER_BASE)return qgpu_guard_access_fallback(b,in,opaque);
    for(unsigned index=0;index<p->constantWindowCount;++index) {
        const QGPUConstantWindow *window=&p->constantWindows[index];
        if(slot-QGPU_FRAGMENT_BUFFER_BASE!=window->buffer || at<window->byteOffset ||
            (uint64_t)at+bytes*n>(uint64_t)window->byteOffset+QGPU_CONSTANT_WINDOW_BYTES)continue;
        b->cursor=nir_before_instr(&in->instr);
        nir_def *enabled=nir_load_const_ir3(b,1,32,nir_imm_int(b,qgpu_constant_window_flag(p->constantWindowCount,index)),.base=0);
        nir_push_if(b,nir_ine_imm(b,enabled,0));nir_def *lanes[4];
        for(unsigned i=0;i<n;++i) {
            unsigned relative=at-window->byteOffset+bytes*i;
            nir_def *word=nir_load_const_ir3(b,1,32,nir_imm_int(b,QGPU_CONSTANT_WINDOW_BASE+index*QGPU_CONSTANT_WINDOW_WORDS+relative/4),.base=0);
            lanes[i]=bytes==4?word:(relative&2)?nir_unpack_32_2x16_split_y(b,word):nir_unpack_32_2x16_split_x(b,word);
        }
        nir_def *constant=nir_vec(b,lanes,n);nir_push_else(b,NULL);
        nir_intrinsic_instr *fallback=nir_instr_as_intrinsic(nir_instr_clone(b->shader,&in->instr));
        nir_builder_instr_insert(b,&fallback->instr);nir_pop_if(b,NULL);
        nir_def *result=nir_if_phi(b,constant,&fallback->def);
        nir_def_rewrite_uses(&in->def,result);nir_instr_remove(&in->instr);
        return qgpu_guard_access_fallback(b,fallback,opaque);
    }
    return qgpu_guard_access_fallback(b,in,opaque);
}
// Called after GL has resolved active-resource indices to explicit bindings.
// Keep public reflection logical, but compile FS loads, stores and RESINFO
// against the second graphics bank. CS linear images use a separate bank.
static bool qgpu_ssbo_binding(nir_shader *nir, unsigned logical, unsigned *physical) {
    if (logical>=(nir->info.stage==MESA_SHADER_COMPUTE?QGPU_COMPUTE_BUFFER_COUNT:QGPU_STAGE_BUFFER_COUNT)) return false;
    *physical=logical+(nir->info.stage==MESA_SHADER_FRAGMENT?QGPU_FRAGMENT_BUFFER_BASE:0);
    if (nir->info.num_ssbos<=*physical) nir->info.num_ssbos=*physical+1;
    return true;
}
static void qgpu_record_image_access(nir_shader *nir,unsigned logical,nir_intrinsic_instr *in,QGPUProgramDescription *p) {
    if (nir->info.stage!=MESA_SHADER_COMPUTE || logical<QGPU_COMPUTE_IMAGE_BASE || !p) return;
    uint32_t bit=1u<<(logical-QGPU_COMPUTE_IMAGE_BASE);p->imageMask|=bit;
    if (in->intrinsic==nir_intrinsic_load_ssbo) p->imageReadMask|=bit;
    if (in->intrinsic==nir_intrinsic_store_ssbo || in->intrinsic==nir_intrinsic_ssbo_atomic || in->intrinsic==nir_intrinsic_ssbo_atomic_swap) p->imageWriteMask|=bit;
}
static unsigned qgpu_memory_constants(const QGPUProgramDescription *p) {
    if(p->constantWindowCount)return qgpu_constant_memory_words(p->constantWindowCount);
    if(p->flags&QGPU_BUFFER_SIZE_CONSTANTS)return QGPU_MEMORY_CONSTANT_WORDS;
    if(p->flags&QGPU_FRAMEBUFFER_TILE_ORIGIN)return QGPU_FRAMEBUFFER_TILE_MEMORY_CONSTANT_WORDS;
    if(p->stage!=0)return p->framebufferMask?QGPU_FRAMEBUFFER_MEMORY_CONSTANT_WORDS:p->queryMask?QGPU_GRAPHICS_MEMORY_CONSTANT_WORDS:p->samplerMask?QGPU_SAMPLER_CONSTANT_WORDS:0;
    return p->flags&(QGPU_ARRAY_TEXTURE_MASK|QGPU_CUBE_TEXTURE_MASK)?QGPU_ARRAY_MEMORY_CONSTANT_WORDS:p->flags&QGPU_DISPATCH_GEOMETRY?QGPU_DISPATCH_MEMORY_CONSTANT_WORDS:p->samplerMask?QGPU_SAMPLER_MEMORY_CONSTANT_WORDS:p->imageMask?QGPU_IMAGE_MEMORY_CONSTANT_WORDS:p->threadgroupMask?QGPU_THREADGROUP_CONSTANT_WORDS:0;
}
static void qgpu_pad_memory_constants(struct ir3_shader_variant *v,const QGPUProgramDescription *p) {
    unsigned reserved=((qgpu_memory_constants(p)+15)/16)*4;
    if(v->constlen<reserved)v->constlen=reserved;
}
// GLSL's combined handles encode (texture * 8 + sampler). Split them after
// GL resolves bindings, before IR3 assigns independent stage-local indices.
static bool qgpu_bind_samples(nir_shader *nir,QGPUProgramDescription *p) {
    if(!p)return false;
    nir_foreach_variable_with_modes(var,nir,nir_var_uniform) {
        if (!glsl_type_is_sampler(var->type)) continue;
        unsigned texture=0,sampler=0;bool level=false;
        if(nir->info.stage!=MESA_SHADER_COMPUTE && nir->info.stage!=MESA_SHADER_FRAGMENT && nir->info.stage!=MESA_SHADER_VERTEX)return false;
        int consumed=0;
        if(nir->info.stage==MESA_SHADER_FRAGMENT) {
            const char *names[]={"_qframebuffer%u%n","_qframebuffer_ms%u%n"};
            bool framebuffer=false;
            for(unsigned ms=0;ms<2;++ms)if(var->name && sscanf(var->name,names[ms],&texture,&consumed)==1 && !var->name[consumed]) {
                if(texture>=QGPU_COLOR_TARGET_COUNT)return false;
                var->data.binding=176+ms*8+texture;framebuffer=true;break;
            }
            if(framebuffer)continue;
        }
        if(nir->info.stage!=MESA_SHADER_COMPUTE) {
            unsigned shape=0;
            const char *fetch=nir->info.stage==MESA_SHADER_VERTEX?"_qvertex_fetch%u_%u%n":"_qfetch%u_%u%n";
            if(var->name && sscanf(var->name,fetch,&shape,&texture,&consumed)==2) {
                if(var->name[consumed] || shape>5 || texture>=QGPU_STAGE_TEXTURE_COUNT)return false;
                var->data.binding=128+shape*8+texture;continue;
            }
        }
        const char *pattern=nir->info.stage==MESA_SHADER_VERTEX?"_qvertex_sample%u_%u%n":"_qsample%u_%u%n";
        if(!var->name || sscanf(var->name,pattern,&texture,&sampler,&consumed)!=2 || texture>=QGPU_STAGE_TEXTURE_COUNT)return false;
        const char *tail=var->name+consumed;
        char shadowTail[192];size_t tailBytes=strlen(tail);
        if(tailBytes>=7 && !strcmp(tail+tailBytes-7,"_shadow")) {
            if(tailBytes>=sizeof(shadowTail))return false;
            memcpy(shadowTail,tail,tailBytes-7);shadowTail[tailBytes-7]=0;tail=shadowTail;
        }
        if(sampler>=QGPU_STAGE_SAMPLER_COUNT) {
            unsigned index=sampler-QGPU_STAGE_SAMPLER_COUNT;QGPUConstantSampler state={0};int count=0;
            if(index>=QGPU_MAX_CONSTANT_SAMPLERS || sscanf(tail,"_c_%u_%u_%u_%u_%u_%u_%u%n",
                &state.filters,&state.minLOD,&state.maxLOD,&state.addressModes,&state.maxAnisotropy,&state.borderColor,&state.normalized,&count)!=7)return false;
            // A shared logical literal must have exactly one semantic state.
            if((p->constantSamplerMask&(1u<<index)) && memcmp(&state,&p->constantSamplers[index],sizeof(state)))return false;
            p->constantSamplerMask|=1u<<index;p->constantSamplers[index]=state;tail+=count;
            level=!strcmp(tail,"_level");if(*tail && !level)return false;
            // These virtual indices are assigned AFTER GL links uniform units.
            // nir_lower_samplers only copies them; our next pass resolves them
            // before IR3 or any textures_used/samplers_used bitset sees them.
            var->data.binding=256+texture*16+index+(level?128:0);
        } else {
            level=!strcmp(tail,"_level");if(*tail && !level)return false;
            var->data.binding=texture*8+sampler+(level?64:0);
        }
    }
    return true;
}
static bool qgpu_remap_samples(nir_shader *nir,QGPUProgramDescription *p) {
    if (nir->info.stage!=MESA_SHADER_COMPUTE && nir->info.stage!=MESA_SHADER_FRAGMENT && nir->info.stage!=MESA_SHADER_VERTEX) return true;
    const uint32_t declared=p->constantSamplerMask;p->constantSamplerMask=0;
    BITSET_ZERO(nir->info.textures_used);BITSET_ZERO(nir->info.samplers_used);
    // Gather usage before assigning literals. External slots retain the old
    // ISA indices: i for implicit/gradient and i+8 for explicit LOD. Only the
    // states actually used occupy hardware slots, leaving the rest for literals.
    for(unsigned pass=0;pass<2;++pass) {
        if(pass) {
            uint32_t occupied=p->implicitSamplerMask|(p->explicitSamplerMask<<8);
            for(unsigned i=0;i<QGPU_MAX_CONSTANT_SAMPLERS;++i) {
                if(!(p->constantSamplerMask&(1u<<i)))memset(&p->constantSamplers[i],0,sizeof(p->constantSamplers[i]));
                uint32_t masks[]={p->constantImplicitSamplerMask,p->constantExplicitSamplerMask};
                uint32_t *slots[]={p->constantSamplerSlots,p->constantSamplerLevelSlots};
                for(unsigned n=0;n<2;++n)if(masks[n]&(1u<<i)) {
                    unsigned slot=0;while(slot<QGPU_HARDWARE_SAMPLER_COUNT && (occupied&(1u<<slot)))++slot;
                    if(slot==QGPU_HARDWARE_SAMPLER_COUNT)return false;
                    slots[n][i]=slot;occupied|=1u<<slot;
                }
            }
        }
        nir_foreach_function_impl(impl,nir) nir_foreach_block(block,impl) nir_foreach_instr(instr,block) {
            if (instr->type!=nir_instr_type_tex) continue;
            nir_tex_instr *tex=nir_instr_as_tex(instr);
            if(nir_tex_instr_src_index(tex,nir_tex_src_texture_offset)>=0 || nir_tex_instr_src_index(tex,nir_tex_src_sampler_offset)>=0)return false;
            if(tex->op==nir_texop_txf || tex->op==nir_texop_txf_ms) {
                if(nir->info.stage==MESA_SHADER_FRAGMENT && tex->texture_index>=176 && tex->texture_index<192) {
                    unsigned ms=(tex->texture_index-176)/8,slot=tex->texture_index%8;
                    if(tex->is_array || (ms?(tex->op!=nir_texop_txf_ms || tex->sampler_dim!=GLSL_SAMPLER_DIM_MS):
                        (tex->op!=nir_texop_txf || tex->sampler_dim!=GLSL_SAMPLER_DIM_2D)))return false;
                    if(!pass) {
                        // Both AIR-NIR and MSL/AIR-GLSL reach this common
                        // virtual-texture pass. Shift the fetch operand only;
                        // their bounds predicates still inspect global pixels
                        // and original attachment dimensions. BYPASS uses zero.
                        int coord=nir_tex_instr_src_index(tex,nir_tex_src_coord);
                        if(coord<0 || tex->src[coord].src.ssa->num_components!=2 ||
                           tex->src[coord].src.ssa->bit_size!=32)return false;
                        nir_builder b=nir_builder_at(nir_before_instr(instr));
                        nir_def *origin=nir_load_const_ir3(&b,2,32,
                            nir_imm_int(&b,QGPU_FRAMEBUFFER_TILE_ORIGIN_BASE),.base=0);
                        nir_src_rewrite(&tex->src[coord].src,nir_isub(&b,tex->src[coord].src.ssa,origin));
                        p->framebufferMask|=1u<<slot;p->flags|=QGPU_FRAMEBUFFER_TILE_ORIGIN;
                    }
                    else {tex->texture_index=qgpu_framebuffer_slot(p->sampleMask,p->fetchMask,p->framebufferMask,slot);tex->sampler_index=0;
                        if(tex->texture_index==UINT32_MAX)return false;
                        BITSET_SET(nir->info.textures_used,tex->texture_index);}
                    continue;
                }
                if(nir->info.stage==MESA_SHADER_COMPUTE || tex->texture_index<128 || tex->texture_index>=176)return false;
                unsigned shape=(tex->texture_index-128)/8,image=tex->texture_index%8;
                if(tex->is_array!=((shape>0 && shape<4) || shape==5) || (shape>=4?
                    (tex->op!=nir_texop_txf_ms || tex->sampler_dim!=GLSL_SAMPLER_DIM_MS):
                    (tex->op!=nir_texop_txf || tex->sampler_dim!=GLSL_SAMPLER_DIM_2D)))return false;
                if(!pass) {
                    if(shape&1)p->flags|=1u<<(QGPU_ARRAY_TEXTURE_SHIFT+image);
                    if(shape&2)p->flags|=1u<<(QGPU_CUBE_TEXTURE_SHIFT+image);
                    if(shape>=4)p->multisampleMask|=1u<<image;
                    p->fetchMask|=1u<<image;
                } else {
                    tex->texture_index=QGPU_FETCH_TEXTURE_BASE+image;tex->sampler_index=0;
                    BITSET_SET(nir->info.textures_used,tex->texture_index);
                }
                continue;
            }
            bool literal=tex->texture_index>=256,fragment=nir->info.stage==MESA_SHADER_FRAGMENT;
            bool level=literal?tex->texture_index>=384:tex->texture_index>=64;
            if ((tex->op!=nir_texop_txl && tex->op!=nir_texop_txd && (!fragment || (tex->op!=nir_texop_tex && tex->op!=nir_texop_txb))) ||
                (literal?tex->texture_index>=512:tex->texture_index>=128) || tex->texture_index!=tex->sampler_index || (level && tex->op!=nir_texop_txl))return false;
            unsigned pair=literal?(tex->texture_index-256)%128:tex->texture_index%64;
            unsigned image=pair/(literal?16:8),sampler=pair%(literal?16:8);
            bool cube=tex->sampler_dim==GLSL_SAMPLER_DIM_CUBE;
            if(!cube && tex->sampler_dim!=GLSL_SAMPLER_DIM_2D)return false;
            if(literal && !(declared&(1u<<sampler)))return false;
            if(!pass) {
                p->sampleMask|=1u<<image;
                if(tex->is_shadow)p->depthTextureMask|=1u<<image;
                if(cube)p->flags|=1u<<(QGPU_CUBE_TEXTURE_SHIFT+image);
                if(tex->is_array)p->flags|=1u<<(QGPU_ARRAY_TEXTURE_SHIFT+image);
                if(literal) {
                    p->constantSamplerMask|=1u<<sampler;
                    if(level)p->constantExplicitSamplerMask|=1u<<sampler;else p->constantImplicitSamplerMask|=1u<<sampler;
                    if(cube)p->constantCubeSamplerMask|=1u<<sampler;
                } else {
                    p->samplerMask|=1u<<sampler;
                    if(level)p->explicitSamplerMask|=1u<<sampler;else p->implicitSamplerMask|=1u<<sampler;
                    if(cube)p->cubeSamplerMask|=1u<<sampler;
                }
            } else {
                tex->texture_index=image;
                tex->sampler_index=literal?(level?p->constantSamplerLevelSlots[sampler]:p->constantSamplerSlots[sampler]):sampler+(level?8:0);
                BITSET_SET(nir->info.textures_used,image);BITSET_SET(nir->info.samplers_used,tex->sampler_index);
            }
        }
    }
    nir_foreach_function_impl(impl,nir)nir_metadata_preserve(impl,nir_metadata_control_flow);
    return true;
}
// Keep independent readonly loads of one constant window adjacent so Mesa's
// existing identical-condition if merge can share their uniform guard. Move
// only within one basic block and across pure SSA instructions. Do not cross
// any other memory access, texture operation, barrier, output or control flow.
static unsigned qgpu_window_load(const nir_intrinsic_instr *in,const QGPUProgramDescription *p) {
    if(!qgpu_window_candidate(in) ||
       !in->num_components || in->num_components>4 ||
       !(nir_intrinsic_access(in)&ACCESS_NON_WRITEABLE) ||
       (nir_intrinsic_access(in)&(ACCESS_VOLATILE|ACCESS_COHERENT)) ||
       !nir_src_is_const(in->src[0]) || !nir_src_is_const(in->src[1]) ||
       nir_src_bit_size(in->src[1])!=32)return UINT32_MAX;
    unsigned slot=nir_src_as_uint(in->src[0]),at=nir_src_as_uint(in->src[1]),bytes=in->def.bit_size/8;
    if((at&(bytes-1)) || slot<QGPU_FRAGMENT_BUFFER_BASE)return UINT32_MAX;
    for(unsigned i=0;i<p->constantWindowCount;++i) {
        const QGPUConstantWindow *window=&p->constantWindows[i];
        if(slot-QGPU_FRAGMENT_BUFFER_BASE==window->buffer && at>=window->byteOffset &&
           (uint64_t)at+bytes*in->num_components<=(uint64_t)window->byteOffset+QGPU_CONSTANT_WINDOW_BYTES)return i;
    }
    return UINT32_MAX;
}
static void qgpu_group_window_reads(nir_shader *nir,const QGPUProgramDescription *p) {
    if(!p || !p->constantWindowCount)return;
    nir_foreach_function_impl(impl,nir) {
        bool progress=false;
        nir_foreach_block(block,impl) {
            nir_instr *cursor=nir_block_first_instr(block);
            while(cursor) {
                unsigned window=cursor->type==nir_instr_type_intrinsic?
                    qgpu_window_load(nir_instr_as_intrinsic(cursor),p):UINT32_MAX;
                if(window==UINT32_MAX) {cursor=nir_instr_next(cursor);continue;}
                nir_intrinsic_instr *loads[32]={nir_instr_as_intrinsic(cursor)};
                unsigned count=1,lanes=loads[0]->num_components;
                nir_instr *next=nir_instr_next(cursor);
                for(;next;next=nir_instr_next(next)) {
                    if(next->type==nir_instr_type_alu || next->type==nir_instr_type_load_const ||
                       next->type==nir_instr_type_deref)continue;
                    if(next->type!=nir_instr_type_intrinsic)break;
                    nir_intrinsic_instr *load=nir_instr_as_intrinsic(next);
                    if(qgpu_window_load(load,p)!=window || count==32 || lanes+load->num_components>32)break;
                    loads[count++]=load;lanes+=load->num_components;
                }
                if(count>1) {
                    nir_builder b=nir_builder_at(nir_before_instr(cursor));
                    // A later load's literal source may originally be defined
                    // after the first load. Rebuild both constants before the
                    // moved instructions so every SSA source still dominates.
                    for(unsigned i=0;i<count;++i) {
                        unsigned slot=nir_src_as_uint(loads[i]->src[0]),at=nir_src_as_uint(loads[i]->src[1]);
                        nir_src_rewrite(&loads[i]->src[0],nir_imm_int(&b,slot));
                        nir_src_rewrite(&loads[i]->src[1],nir_imm_int(&b,at));
                    }
                    for(unsigned i=0;i<count;++i) {
                        nir_instr_remove(&loads[i]->instr);
                        nir_builder_instr_insert(&b,&loads[i]->instr);
                    }
                    progress=true;
                }
                cursor=next;
            }
        }
        if(progress)nir_metadata_preserve(impl,nir_metadata_none);
    }
}

static bool qgpu_guard_memory_profile(nir_shader *nir,QGPUProgramDescription *description,bool constantWindows) {
    qgpu_vectorize_half_reads(nir);
    if(constantWindows)qgpu_collect_constant_windows(nir,description);
    else if(description) {description->constantWindowCount=0;memset(description->constantWindows,0,sizeof(description->constantWindows));}
    qgpu_memory_guard guard={true,qgpu_lower_fragment_demote(nir),description};
    nir_variable *dynamic=NULL;uint32_t mask=0;
    nir_foreach_variable_with_modes(var,nir,nir_var_mem_shared)
        if (var->name && !strcmp(var->name,"_qdynamic")) dynamic=var;
    nir_foreach_function_impl(impl,nir) {
        nir_builder b=nir_builder_create(impl);
        nir_foreach_block(block,impl) nir_foreach_instr_safe(instr,block) {
            if (instr->type!=nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if (in->intrinsic!=nir_intrinsic_load_deref) continue;
            nir_deref_instr *deref=nir_src_as_deref(in->src[0]);nir_variable *var=nir_deref_instr_get_variable(deref);
            if (!var || !var->name) continue;
            if(!strcmp(var->name,"_qframebuffer_info")) {
                if(!description || nir->info.stage!=MESA_SHADER_FRAGMENT || in->def.bit_size!=32 || in->def.num_components!=4 ||
                    deref->deref_type!=nir_deref_type_array || !nir_src_is_const(deref->arr.index) ||
                    nir_src_as_uint(deref->arr.index)>=QGPU_COLOR_TARGET_COUNT)return false;
                unsigned slot=nir_src_as_uint(deref->arr.index);description->framebufferMask|=1u<<slot;b.cursor=nir_before_instr(instr);
                nir_def *value=nir_load_const_ir3(&b,4,32,nir_imm_int(&b,QGPU_FRAMEBUFFER_CONSTANT_BASE+slot*4),.base=0);
                nir_def_rewrite_uses(&in->def,value);nir_instr_remove(instr);continue;
            }
            // Graphics queries load only native-owned metadata. They do not
            // imply a pixel read, an SSBO image bank or a sampler dependency.
            static const char *graphics_names[2][6]={
                {"_qtexture2d_info","_qtexture2d_array_info","_qtexturecube_info","_qtexturecube_array_info","_qtexture2d_ms_info","_qtexture2d_ms_array_info"},
                {"_qvertex_texture2d_info","_qvertex_texture2d_array_info","_qvertex_texturecube_info","_qvertex_texturecube_array_info","_qvertex_texture2d_ms_info","_qvertex_texture2d_ms_array_info"}};
            int graphics_shape=-1;unsigned graphics_stage=0;
            for(unsigned stage=0;stage<2;++stage)for(unsigned shape=0;shape<6;++shape)
                if(!strcmp(var->name,graphics_names[stage][shape])) {graphics_shape=shape;graphics_stage=stage;}
            if(graphics_shape>=0) {
                if(!description || (nir->info.stage!=MESA_SHADER_VERTEX && nir->info.stage!=MESA_SHADER_FRAGMENT) ||
                    (graphics_stage!=0)!=(nir->info.stage==MESA_SHADER_VERTEX) || in->def.bit_size!=32 || in->def.num_components!=4 ||
                    deref->deref_type!=nir_deref_type_array || !nir_src_is_const(deref->arr.index) ||
                    nir_src_as_uint(deref->arr.index)>=QGPU_STAGE_TEXTURE_COUNT)return false;
                unsigned slot=nir_src_as_uint(deref->arr.index);description->queryMask|=1u<<slot;
                if(graphics_shape&1)description->flags|=1u<<(QGPU_ARRAY_TEXTURE_SHIFT+slot);
                if(graphics_shape&2)description->flags|=1u<<(QGPU_CUBE_TEXTURE_SHIFT+slot);
                if(graphics_shape>=4)description->multisampleMask|=1u<<slot;
                b.cursor=nir_before_instr(instr);
                nir_def *value=nir_load_const_ir3(&b,4,32,nir_imm_int(&b,QGPU_GRAPHICS_TEXTURE_CONSTANT_BASE+slot*4),.base=0);
                nir_def_rewrite_uses(&in->def,value);nir_instr_remove(instr);continue;
            }
            bool vertex_sampler=!strcmp(var->name,"_qvertex_sampler_info");
            bool cube=!strcmp(var->name,"_qcube_info") || !strcmp(var->name,"_qcube_face_info");
            bool cube_array=!strcmp(var->name,"_qcubearray_info") || !strcmp(var->name,"_qcubearray_face_info");
            bool ms_layout=!strcmp(var->name,"_qimage_ms_array_layout");
            bool ms_array=ms_layout || !strcmp(var->name,"_qimage_ms_array_info");
            bool array=ms_layout || !strcmp(var->name,"_qimage_array_info") || !strcmp(var->name,"_qcube_face_info") || !strcmp(var->name,"_qcubearray_face_info");
            bool ms=ms_array || !strcmp(var->name,"_qimage_ms_info");
            bool image=(ms && !ms_layout) || !strcmp(var->name,"_qimage_info") || !strcmp(var->name,"_qcube_info") || !strcmp(var->name,"_qcubearray_info");
            bool sampler=vertex_sampler || !strcmp(var->name,"_qsampler_info"),dispatch=!strcmp(var->name,"_qdispatch_info");
            if (!image && !sampler && !dispatch && !array && strcmp(var->name,"_qthreadgroup_info")) continue;
            if ((!sampler && nir->info.stage!=MESA_SHADER_COMPUTE) ||
                (sampler && (vertex_sampler!=(nir->info.stage==MESA_SHADER_VERTEX))) ||
                in->def.bit_size!=32 || in->def.num_components!=(sampler?1:(image || dispatch)?4:2) ||
                deref->deref_type!=nir_deref_type_array || !nir_src_is_const(deref->arr.index) ||
                nir_src_as_uint(deref->arr.index)>=(dispatch?4u:QGPU_STAGE_BUFFER_COUNT)) return false;
            unsigned slot=nir_src_as_uint(deref->arr.index);b.cursor=nir_before_instr(instr);
            if(sampler && !description)return false;
            if (image || array) {
                if (!description) return false;
                description->imageMask|=1u<<slot;
                if(ms)description->multisampleMask|=1u<<slot;
                if((array && !cube) || cube_array || ms_array)description->flags|=1u<<(QGPU_ARRAY_TEXTURE_SHIFT+slot);
                if(cube || cube_array)description->flags|=1u<<(QGPU_CUBE_TEXTURE_SHIFT+slot);
            }
            else if (dispatch) {if (!description) return false;description->flags|=QGPU_DISPATCH_GEOMETRY;}
            else if(!sampler)mask|=1u<<slot;
            unsigned base=array?QGPU_ARRAY_CONSTANT_BASE+slot*2:dispatch?QGPU_DISPATCH_CONSTANT_BASE+slot*4:sampler?(nir->info.stage==MESA_SHADER_COMPUTE?QGPU_SAMPLER_CONSTANT_BASE:0)+slot:image?QGPU_IMAGE_CONSTANT_BASE+slot*4:slot*2;
            nir_def *value=nir_load_const_ir3(&b,sampler?1:(image || dispatch)?4:2,32,nir_imm_int(&b,base),.base=0);
            nir_def_rewrite_uses(&in->def,value);nir_instr_remove(instr);
        }
    }
    if (mask && (!description || !dynamic)) return false;
    if (description) {description->threadgroupMask=mask;description->flags|=mask?QGPU_DYNAMIC_THREADGROUP:0;}
    if (mask) {
        // Put the dynamic pool after every static object. The uniform words
        // describe offsets relative to this 16-byte-aligned base, never GPU VA.
        dynamic->data.alignment=16;exec_node_remove(&dynamic->node);exec_list_push_tail(&nir->variables,&dynamic->node);
    }
    // Shared variables survive the standalone GL linker as derefs. Give IR3
    // explicit byte offsets and retain its actual static memory footprint.
    NIR_PASS_V(nir,nir_lower_vars_to_explicit_types,nir_var_mem_shared,glsl_get_natural_size_align_bytes);
    if (mask) {
        if (dynamic->data.driver_location>QGPU_MAX_THREADGROUP_BYTES) return false;
        description->sharedBytes=dynamic->data.driver_location;
        nir->info.shared_size=QGPU_MAX_THREADGROUP_BYTES;
    }
    NIR_PASS_V(nir,nir_opt_dce);NIR_PASS_V(nir,nir_remove_dead_variables,nir_var_uniform,NULL);
    NIR_PASS_V(nir,nir_lower_explicit_io,nir_var_mem_shared,nir_address_format_32bit_offset);
    if (nir->info.shared_size>QGPU_MAX_THREADGROUP_BYTES ||
        (nir->info.shared_size && nir->info.stage!=MESA_SHADER_COMPUTE)) return false;
    qgpu_group_window_reads(nir,description);
    nir_shader_intrinsics_pass(nir,qgpu_guard_access,nir_metadata_none,&guard);
    return guard.supported;
}
static bool qgpu_guard_memory(nir_shader *nir,QGPUProgramDescription *description) {
    return qgpu_guard_memory_profile(nir,description,true);
}
static bool qgpu_guard_ssbos(nir_shader *nir) {return qgpu_guard_memory(nir,NULL);}
// Hardware probes on A540 distinguish 65532/65540 (complete stores) from
// 65536 (no stores). Do not derive safety bounds from RESINFO's zero-width
// descriptor case: the kernel already validated the exact binding length.
static bool qgpu_lower_buffer_sizes(nir_shader *nir,QGPUProgramDescription *p) {
    if(!p)return false;
    nir_foreach_function_impl(impl,nir) {
        nir_builder b=nir_builder_create(impl);
        nir_foreach_block(block,impl) nir_foreach_instr_safe(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_get_ssbo_size)continue;
            if(!nir_src_is_const(in->src[0]) || in->def.bit_size!=32 || in->def.num_components!=1)return false;
            unsigned slot=nir_src_as_uint(in->src[0]);
            if(slot>=QGPU_BUFFER_SIZE_CONSTANT_WORDS)return false;
            b.cursor=nir_before_instr(instr);
            nir_def *size=nir_load_const_ir3(&b,1,32,nir_imm_int(&b,QGPU_BUFFER_SIZE_CONSTANT_BASE+slot),.base=0);
            nir_def_rewrite_uses(&in->def,size);nir_instr_remove(instr);
            p->flags|=QGPU_BUFFER_SIZE_CONSTANTS;
        }
        nir_metadata_preserve(impl,nir_metadata_none);
    }
    return true;
}
static void qgpu_center_fragment_position(nir_shader *nir,bool msaa) {
    if (!msaa || nir->info.stage!=MESA_SHADER_FRAGMENT || !nir->info.fs.uses_sample_shading) return;
    nir_function_impl *impl=nir_shader_get_entrypoint(nir);bool used=false;
    nir_foreach_block(block,impl) nir_foreach_instr(instr,block)
        if (instr->type==nir_instr_type_intrinsic && nir_instr_as_intrinsic(instr)->intrinsic==nir_intrinsic_load_frag_coord) used=true;
    if (!used) return;
    // Metal position is always evaluated at pixel center. A5xx SAMPLEMODE=3
    // supplies sample coordinates. Window depth and reciprocal W are affine
    // planes, so shift their values back by the fine quad gradients. Evaluate
    // once at entry, before branches/discard can make derivatives undefined.
    nir_builder b=nir_builder_create(impl);b.cursor=nir_before_impl(impl);
    nir_def *raw=nir_load_frag_coord(&b),*xy=nir_channels(&b,raw,3),*zw=nir_channels(&b,raw,12);
    nir_def *center=nir_fadd_imm(&b,nir_ffloor(&b,xy),0.5),*delta=nir_fsub(&b,center,xy);
    nir_def *dx=nir_ddx_fine(&b,zw),*dy=nir_ddy_fine(&b,zw);
    nir_def *zcenter=nir_fadd(&b,zw,nir_fadd(&b,nir_fmul(&b,dx,nir_channel(&b,delta,0)),nir_fmul(&b,dy,nir_channel(&b,delta,1))));
    nir_def *position=nir_vec4(&b,nir_channel(&b,center,0),nir_channel(&b,center,1),nir_channel(&b,zcenter,0),nir_channel(&b,zcenter,1));
    nir_foreach_block(block,impl) nir_foreach_instr_safe(instr,block) {
        if (instr->type!=nir_instr_type_intrinsic) continue;
        nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
        if (in->intrinsic!=nir_intrinsic_load_frag_coord || &in->def==raw) continue;
        nir_def_rewrite_uses(&in->def,position);nir_instr_remove(instr);
    }
    nir_metadata_preserve(impl,nir_metadata_none);
}
static bool qgpu_capture_interface(nir_shader *nir,QGPUProgramDescription *p) {
    p->stage=nir->info.stage==MESA_SHADER_COMPUTE?0:nir->info.stage==MESA_SHADER_VERTEX?1:2;
    memset(p->varyings,0,sizeof(p->varyings));p->varyingCount=0;
    memset(p->attributes,0,sizeof(p->attributes));p->attributeMask=0;
    if (nir->info.stage==MESA_SHADER_VERTEX) nir_foreach_variable_with_modes(var,nir,nir_var_shader_in) {
        unsigned slot=var->data.location-VERT_ATTRIB_GENERIC0;
        enum glsl_base_type type=glsl_get_base_type(var->type);
        if (slot>=QGPU_MAX_ATTRIBUTES || var->data.location_frac || (!glsl_type_is_scalar(var->type) && !glsl_type_is_vector(var->type)) ||
            (type!=GLSL_TYPE_FLOAT && type!=GLSL_TYPE_INT && type!=GLSL_TYPE_UINT)) return false;
        p->attributes[slot]=(QGPUAttribute){glsl_get_vector_elements(var->type),type==GLSL_TYPE_FLOAT?0u:type==GLSL_TYPE_INT?1u:2u,252,0};
    }
    for (unsigned i=0;i<QGPU_MAX_VARYINGS;++i) p->varyings[i].regid=252;
    if (nir->info.stage!=MESA_SHADER_VERTEX && nir->info.stage!=MESA_SHADER_FRAGMENT) return true;
    nir_variable_mode mode=nir->info.stage==MESA_SHADER_VERTEX ? nir_var_shader_out : nir_var_shader_in;
    nir_foreach_variable_with_modes(var,nir,mode) {
        if (var->data.location<VARYING_SLOT_VAR0) continue;
        unsigned slot=var->data.location-VARYING_SLOT_VAR0;
        if (slot>=QGPU_MAX_VARYINGS || (!glsl_type_is_scalar(var->type) && !glsl_type_is_vector(var->type)) ||
            glsl_get_base_type(var->type)!=GLSL_TYPE_FLOAT || var->data.location_frac || (var->data.centroid && var->data.sample) ||
            (var->data.interpolation==INTERP_MODE_FLAT && (var->data.centroid || var->data.sample))) return false;
        QGPUVarying *v=&p->varyings[slot];
        if (v->components) return false;
        v->components=glsl_get_vector_elements(var->type);
        v->mode=var->data.interpolation==INTERP_MODE_FLAT ? 2 : var->data.interpolation==INTERP_MODE_NOPERSPECTIVE ? 1 : 0;
        if (v->mode!=2) v->mode+=var->data.sample?5:var->data.centroid?3:0;
        if (!v->components || v->components>4) return false;
        if (slot>=p->varyingCount) p->varyingCount=slot+1;
    }
    return true;
}
static bool qgpu_export_interface(const struct ir3_shader_variant *v,QGPUProgramDescription *p) {
    p->colorMask=0;p->depthReg=v->type==MESA_SHADER_FRAGMENT && v->writes_pos?ir3_find_output_regid(v,FRAG_RESULT_DEPTH):0;memset(p->colorRegisters,0,sizeof(p->colorRegisters));
    if(v->type==MESA_SHADER_FRAGMENT)for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT;++i) {
        unsigned reg=ir3_find_output_regid(v,v->color0_mrt && !i?FRAG_RESULT_COLOR:FRAG_RESULT_DATA0+i);
        p->colorRegisters[i]=reg;if(reg!=252)p->colorMask|=1u<<i;
    }
    p->totalIn=v->type==MESA_SHADER_FRAGMENT ? v->total_in : 0;
    p->perspective=ir3_find_sysval_regid(v,SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL);
    p->linear=ir3_find_sysval_regid(v,SYSTEM_VALUE_BARYCENTRIC_LINEAR_PIXEL);
    p->centerRHW=ir3_find_sysval_regid(v,SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTER_RHW);
    p->sampleFlags=v->type==MESA_SHADER_FRAGMENT?((v->per_samp?QGPU_SAMPLE_EXECUTION:0)|(v->key.msaa?QGPU_SAMPLE_MSAA:0)):0;
    uint32_t *registers[]={&p->sampleID,&p->sampleMaskIn,&p->sampleMaskOut,&p->perspectiveCentroid,
        &p->linearCentroid,&p->perspectiveSample,&p->linearSample};
    const unsigned values[]={ir3_find_sysval_regid(v,SYSTEM_VALUE_SAMPLE_ID),ir3_find_sysval_regid(v,SYSTEM_VALUE_SAMPLE_MASK_IN),
        v->type==MESA_SHADER_FRAGMENT && v->writes_smask?ir3_find_output_regid(v,FRAG_RESULT_SAMPLE_MASK):252,
        ir3_find_sysval_regid(v,SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTROID),ir3_find_sysval_regid(v,SYSTEM_VALUE_BARYCENTRIC_LINEAR_CENTROID),
        ir3_find_sysval_regid(v,SYSTEM_VALUE_BARYCENTRIC_PERSP_SAMPLE),ir3_find_sysval_regid(v,SYSTEM_VALUE_BARYCENTRIC_LINEAR_SAMPLE)};
    for (unsigned i=0;i<7;++i) {
        *registers[i]=values[i]==252?0:values[i];
        if (values[i]!=252) p->sampleFlags|=2u<<i;
    }
    if (v->type==MESA_SHADER_VERTEX) {
        for (unsigned i=0;i<v->inputs_count;++i) if (!v->inputs[i].sysval && v->inputs[i].compmask) {
            unsigned slot=v->inputs[i].slot-VERT_ATTRIB_GENERIC0;
            if (slot>=QGPU_MAX_ATTRIBUTES || !p->attributes[slot].components || v->inputs[i].half) return false;
            p->attributes[slot].regid=v->inputs[i].regid;p->attributes[slot].mask=v->inputs[i].compmask;p->attributeMask|=1u<<slot;
        }
        for (unsigned i=0;i<QGPU_MAX_ATTRIBUTES;++i) if (!(p->attributeMask&(1u<<i))) memset(&p->attributes[i],0,sizeof(p->attributes[i]));
        for (unsigned i=0;i<v->outputs_count;++i) {
            if (v->outputs[i].slot<VARYING_SLOT_VAR0) continue;
            unsigned slot=v->outputs[i].slot-VARYING_SLOT_VAR0;
            if (slot>=p->varyingCount || !p->varyings[slot].components || v->outputs[i].half) return false;
            p->varyings[slot].regid=v->outputs[i].regid;
            p->varyings[slot].mask=(1u<<p->varyings[slot].components)-1;
        }
    } else if (v->type==MESA_SHADER_FRAGMENT) {
        for (unsigned i=0;i<v->inputs_count;++i) if (v->inputs[i].bary && v->inputs[i].compmask) {
            if (v->inputs[i].slot<VARYING_SLOT_VAR0) return false;
            unsigned slot=v->inputs[i].slot-VARYING_SLOT_VAR0;
            if (slot>=p->varyingCount || !p->varyings[slot].components || v->inputs[i].half ||
                v->inputs[i].flat!=(p->varyings[slot].mode==2)) return false;
            p->varyings[slot].mask=v->inputs[i].compmask;
            p->varyings[slot].inloc=v->inputs[i].inloc;
        }
    }
    return p->totalIn<=124;
}
