// SPDX-License-Identifier: BSD-2-Clause
// Included inside Direct. Logical row-major element r*8+c is owned by lane
// r*8+c. Metal leaves this distribution unspecified; a full 64-lane subgroup
// is required by native admission. All matrix operations must be convergent.
    nir_def *matrixMemory(NV pointer,unsigned width,nir_def *value=nullptr) {
        auto p=pointerOffset(pointer);
        if(p.kind!=NV::Pointer || !buffers.count(p.root) || (width!=16 && width!=32))fail("NIR matrix memory provenance/width");
        auto binding=buffers.at(p.root);
        if(binding.constantElement || (value && binding.readonly))fail("NIR matrix memory access");
        auto valid=nir_iand(&b,nir_ieq_imm(&b,c(p.def,1),0),nir_ieq_imm(&b,nir_iand_imm(&b,c(p.def,0),width/8-1),0));
        if(!binding.storage) {
            unsigned slot=physicalBuffer(p,value!=nullptr);
            auto offset=nir_bcsel(&b,valid,c(p.def,0),u(width==16?0xfffffffeu:0xfffffffcu));
            if(value) {
                _nir_store_ssbo_indices attrs{};attrs.write_mask=1;attrs.align_mul=width/8;
                _nir_build_store_ssbo(&b,value,u(slot),offset,attrs);return nullptr;
            }
            return ssboLoad(slot,offset,1,width,binding.readonly?ACCESS_NON_WRITEABLE:ACCESS_COHERENT);
        }
        valid=nir_iand(&b,valid,nir_ult_imm(&b,c(p.def,0),binding.bytes));
        auto word=nir_ushr_imm(&b,c(p.def,0),2),shift=nir_ishl_imm(&b,nir_iand_imm(&b,c(p.def,0),2),3);
        auto result=nir_local_variable_create(b.impl,glsl_uint_type(),"matrix_memory");
        nir_store_var(&b,result,u(0),1);nir_push_if(&b,valid);
        auto deref=nir_build_deref_array(&b,nir_build_deref_var(&b,binding.storage),word);
        if(!value)nir_store_var(&b,result,nir_load_deref(&b,deref),1);
        else if(width==32)nir_store_deref(&b,deref,value,1);
        else {
            auto mask=nir_ishl(&b,u(65535),shift);
            auto aligned=p;aligned.def=nir_vec2(&b,nir_iand_imm(&b,c(p.def,0),~3u),c(p.def,1));
            if(binding.storage->data.mode==nir_var_mem_shared) {
                auto expected=nir_local_variable_create(b.impl,glsl_uint_type(),"matrix_expected");
                nir_store_var(&b,expected,u(0),1);nir_push_loop(&b);
                auto old=nir_load_var(&b,expected);
                auto desired=nir_ior(&b,nir_iand(&b,old,nir_inot(&b,mask)),nir_ishl(&b,nir_u2u32(&b,value),shift));
                auto observed=atomic(aligned,"cas",desired,old);
                nir_push_if(&b,nir_ieq(&b,old,observed));nir_jump(&b,nir_jump_break);nir_pop_if(&b,nullptr);
                nir_store_var(&b,expected,observed,1);nir_pop_loop(&b,nullptr);
            } else {
                auto old=nir_load_deref(&b,deref);
                nir_store_deref(&b,deref,nir_ior(&b,nir_iand(&b,old,nir_inot(&b,mask)),nir_ishl(&b,nir_u2u32(&b,value),shift)),1);
            }
        }
        nir_pop_if(&b,nullptr);
        return value?nullptr:width==32?nir_load_var(&b,result):nir_u2u16(&b,nir_ushr(&b,nir_load_var(&b,result),shift));
    }
    bool matrixCall(LLVMValueRef i,const std::string &n) {
        const std::string prefix="air.simdgroup_matrix_8x8_";
        if(n.rfind(prefix,0)!=0)return false;
        if(stage)fail("NIR SIMD matrix requires compute");
        unsigned count=LLVMGetNumArgOperands(i);auto t=LLVMTypeOf(i);
        auto spelling=[&](LLVMTypeRef type) {
            if(!matrixType(type))fail("NIR SIMD matrix shape");
            return std::string("v64")+(LLVMGetTypeKind(scalar(type))==LLVMBFloatTypeKind?"bf16":half(type)?"f16":"f32");
        };
        auto decode=[&](nir_def *v,LLVMTypeRef type) {
            return LLVMGetTypeKind(scalar(type))==LLVMBFloatTypeKind?nir_ishl_imm(&b,nir_u2u32(&b,v),16):nir_f2f32(&b,v);
        };
        auto encode=[&](nir_def *v,LLVMTypeRef type) {
            if(LLVMGetTypeKind(scalar(type))==LLVMBFloatTypeKind) {
                auto bias=nir_iadd_imm(&b,nir_iand_imm(&b,nir_ushr_imm(&b,v,16),1),0x7fff);
                auto rounded=nir_ushr_imm(&b,nir_iadd(&b,v,bias),16);
                return nir_u2u16(&b,nir_bcsel(&b,nir_fneu(&b,v,v),nir_ior_imm(&b,nir_ushr_imm(&b,v,16),64),rounded));
            }
            return nir_f2fN(&b,v,bits(type));
        };
        auto lane=nir_load_subgroup_invocation(&b),row=nir_ushr_imm(&b,lane,3),col=nir_iand_imm(&b,lane,7);
        auto shuffle=[&](nir_def *v,nir_def *index) {
            auto result=nir_shuffle(&b,v->bit_size==16?nir_u2u32(&b,v):v,index);
            return v->bit_size==16?nir_u2u16(&b,result):result;
        };
        auto operation=n.substr(prefix.size());
        if(operation.rfind("init_diag.",0)==0 || operation.rfind("init_filled.",0)==0) {
            bool diag=operation.rfind("init_diag.",0)==0;
            auto suffix=spelling(t);auto scalarSuffix=suffix.substr(3);
            if(count!=1 || LLVMTypeOf(LLVMGetOperand(i,0))!=scalar(t) || operation!=(diag?"init_diag.":"init_filled.")+suffix+"."+scalarSuffix)fail("NIR matrix initializer signature");
            set(i,diag?nir_bcsel(&b,nir_ieq(&b,row,col),arg(i,0),nir_imm_zero(&b,1,bits(t))):arg(i,0));
        } else if(operation.rfind("load.",0)==0 || operation.rfind("store.",0)==0) {
            bool store=operation.rfind("store.",0)==0;unsigned first=store?1:0;
            auto type=store?LLVMTypeOf(LLVMGetOperand(i,0)):t;auto suffix=spelling(type);
            if(count!=first+4 || (store && LLVMGetTypeKind(t)!=LLVMVoidTypeKind))fail("NIR matrix load/store arity");
            auto pointerType=LLVMTypeOf(LLVMGetOperand(i,first));
            auto space=LLVMGetTypeKind(pointerType)==LLVMPointerTypeKind?LLVMGetPointerAddressSpace(pointerType):0;
            if((space!=1 && space!=3) || !integer(LLVMTypeOf(LLVMGetOperand(i,first+1)),64) ||
                lanes(LLVMTypeOf(LLVMGetOperand(i,first+2)))!=2 || !integer(scalar(LLVMTypeOf(LLVMGetOperand(i,first+2))),64) ||
                !integer(LLVMTypeOf(LLVMGetOperand(i,first+3)),1) ||
                operation!=(store?"store.":"load.")+suffix+".p"+std::to_string(space)+suffix.substr(3))fail("NIR matrix memory signature");
            auto origin=arg(i,first+2),transpose=arg(i,first+3);
            auto x=nir_iadd(&b,c(origin,0),nir_u2u64(&b,nir_bcsel(&b,transpose,row,col)));
            auto y=nir_iadd(&b,c(origin,1),nir_u2u64(&b,nir_bcsel(&b,transpose,col,row)));
            auto index=nir_iadd(&b,nir_imul(&b,y,arg(i,first+1)),x);
            auto pointer=pointerOffset(get(LLVMGetOperand(i,first)));
            if(pointer.kind!=NV::Pointer)fail("NIR matrix memory pointer");
            pointer.def=addAddress(pointer.def,mulAddress(nir_unpack_64_2x32(&b,index),bits(type)/8));
            auto value=matrixMemory(pointer,bits(type),store?arg(i,0):nullptr);
            if(!store)set(i,value);
        } else if(operation.rfind("multiply_accumulate.",0)==0) {
            if(count!=3)fail("NIR matrix multiply arity");
            auto aType=LLVMTypeOf(LLVMGetOperand(i,0)),bType=LLVMTypeOf(LLVMGetOperand(i,1)),cType=LLVMTypeOf(LLVMGetOperand(i,2));
            if(operation!="multiply_accumulate."+spelling(t)+"."+spelling(aType)+"."+spelling(bType)+"."+spelling(cType))fail("NIR matrix multiply signature");
            auto a=arg(i,0),d=arg(i,1),result=decode(arg(i,2),cType);
            for(unsigned k=0;k<8;++k) {
                auto left=decode(shuffle(a,nir_iadd_imm(&b,nir_ishl_imm(&b,row,3),k)),aType);
                auto right=decode(shuffle(d,nir_ior_imm(&b,col,k*8)),bType);
                result=nir_ffma(&b,left,right,result);
            }
            set(i,encode(result,t));
        } else fail("NIR unsupported SIMD matrix operation");
        reflection.flags|=QGPU_SIMD_MATRIX;return true;
    }
