// SPDX-License-Identifier: BSD-2-Clause
// Included by AIRRuntime.cpp inside GPU::Compiler. Uses the public LLVM C ABI.
namespace {
void visibleBitcode(LLVMModuleRef module,std::vector<uint8_t> &out) {
    auto buffer=LLVMWriteBitcodeToMemoryBuffer(module);
    if(!buffer)fail("visible AIR serialization failed");
    auto data=LLVMGetBufferStart(buffer);auto size=LLVMGetBufferSize(buffer);
    if(size>16u*1024*1024){LLVMDisposeMemoryBuffer(buffer);fail("linked AIR exceeds 16 MiB");}
    out.assign(data,data+size);LLVMDisposeMemoryBuffer(buffer);
}
void visibleVerify(LLVMModuleRef module) {
    char *message=nullptr;
    if(LLVMVerifyModule(module,LLVMReturnStatusAction,&message)) {
        std::string error=message?message:"invalid linked AIR";LLVMDisposeMessage(message);fail(error);
    }
    if(message)LLVMDisposeMessage(message);
}
}
bool inspectAIRLinkage(const void *bytes,size_t size,const std::string &selected,unsigned &stage,bool &tables,std::string &error) {
    stage=UINT32_MAX;tables=false;error.clear();
    try {
        Module m;initializeModule(bytes,size,m);
        for(unsigned s=0;s<4;++s) {
            auto key=s==0?"air.kernel":s==1?"air.vertex":s==2?"air.fragment":"air.visible";
            unsigned count=LLVMGetNamedMetadataNumOperands(m.module,key);if(count>256)fail("AIR linkage entry limit");
            std::vector<LLVMValueRef> list(count);LLVMGetNamedMetadataOperands(m.module,key,list.data());
            for(auto md:list) {
                auto entry=node(md);
                if(entry.size()!=(s==3?4u:3u) || !LLVMIsAFunction(entry[0]))fail("AIR linkage metadata");
                if(name(entry[0])!=selected)continue;
                if(stage!=UINT32_MAX)fail("duplicate AIR linkage entry");stage=s;
                for(auto argument:node(entry[2]))tables|=has(argument,"air.visible_function_table");
            }
        }
        if(stage==UINT32_MAX)fail("AIR linkage entry absent");return true;
    }catch(const std::exception &e){error=e.what();return false;}
}
bool specializeVisibleAIR(const void *bytes,size_t size,const std::string &selected,const std::vector<FunctionConstantValue> &constants,
    std::vector<uint8_t> &out,std::string &error) {
    out.clear();error.clear();
    try {
        Module m;initializeModule(bytes,size,m);
        AIRSpecialization::State state(m.module,selected,true);if(state.present())state.apply(constants);
        // LLVM's public C ABI on iOS18 has no named-metadata eraser. Remove
        // this one canonical printer line after constants have been baked;
        // target constants must not be re-specialized in the caller namespace.
        auto printed=LLVMPrintModuleToString(m.module);std::string text(printed);LLVMDisposeMessage(printed);
        const std::string marker="!air.function_constants =";
        auto at=text.find("\n"+marker);
        if(at!=std::string::npos){auto end=text.find('\n',at+1);text.erase(at,end==std::string::npos?text.size()-at:end-at);}
        LLVMModuleRef specialized=nullptr;char *message=nullptr;
        auto buffer=LLVMCreateMemoryBufferWithMemoryRangeCopy(text.data(),text.size(),"visible-specialized.air");
        if(LLVMParseIRInContext(m.context,buffer,&specialized,&message)) {
            std::string diagnostic=message?message:"visible AIR parse failed";LLVMDisposeMessage(message);fail(diagnostic);
        }
        try {visibleVerify(specialized);visibleBitcode(specialized,out);}catch(...){LLVMDisposeModule(specialized);throw;}
        LLVMDisposeModule(specialized);return true;
    }catch(const std::exception &e){error=e.what();out.clear();return false;}
}
bool linkVisibleAIR(const void *bytes,size_t size,const std::string &selected,const std::vector<AIRVisibleFunction> &targets,
    const std::vector<FunctionConstantValue> &constants,std::vector<uint8_t> &out,std::string &error) {
    out.clear();error.clear();
    try {
        if(targets.size()>64)fail("visible function target limit is 64");
        Module m;initializeModule(bytes,size,m);
        AIRSpecialization::State state(m.module,selected);if(state.present())state.apply(constants);
        std::vector<std::string> linkedNames;size_t total=size;
        for(const auto &target:targets) {
            if(target.name.empty() || target.air.empty() || target.air.size()>16u*1024*1024 ||
                total>32u*1024*1024-target.air.size())fail("invalid or oversized visible function set");
            total+=target.air.size();LLVMModuleRef module=nullptr;char *message=nullptr;
            auto buffer=LLVMCreateMemoryBufferWithMemoryRangeCopy(reinterpret_cast<const char *>(target.air.data()),target.air.size(),"linked-visible.air");
            if(LLVMParseIRInContext(m.context,buffer,&module,&message)) {
                std::string diagnostic=message?message:"visible AIR parse failed";LLVMDisposeMessage(message);fail(diagnostic);
            }
            try{visibleVerify(module);}catch(...){LLVMDisposeModule(module);throw;}
            bool declared=false;unsigned count=LLVMGetNamedMetadataNumOperands(module,"air.visible");
            if(count>256){LLVMDisposeModule(module);fail("visible metadata limit");}
            std::vector<LLVMValueRef> list(count);LLVMGetNamedMetadataOperands(module,"air.visible",list.data());
            for(auto metadata:list){auto a=node(metadata);declared|=a.size()==4 && name(a[0])==target.name;}
            auto function=LLVMGetNamedFunction(module,target.name.c_str());
            bool valid=declared && function && LLVMCountBasicBlocks(function) &&
                std::string(LLVMGetTarget(module)).rfind("air64",0)==0 &&
                !LLVMGetNamedMetadataNumOperands(module,"air.function_constants") &&
                !strcmp(LLVMGetDataLayoutStr(module),LLVMGetDataLayoutStr(m.module));
            for(const char *key:{"air.kernel","air.vertex","air.fragment"})valid&=!LLVMGetNamedMetadataNumOperands(module,key);
            if(!valid){LLVMDisposeModule(module);fail("visible target must be a specialized visible-only module with the same data layout");}
            // Two specializations can have the same public Metal name. Give
            // each pipeline target its own IR symbol and keep helper bodies
            // private so the linker cannot merge different specializations.
            for(auto fn=LLVMGetFirstFunction(module);fn;fn=LLVMGetNextFunction(fn))
                if(LLVMCountBasicBlocks(fn))LLVMSetLinkage(fn,LLVMInternalLinkage);
            std::string symbol="__qvisible_target_"+std::to_string(linkedNames.size());
            while(LLVMGetNamedFunction(m.module,symbol.c_str()) || LLVMGetNamedGlobal(m.module,symbol.c_str()))symbol+="_";
            LLVMSetValueName2(function,symbol.data(),symbol.size());LLVMSetLinkage(function,LLVMExternalLinkage);symbol=name(function);linkedNames.push_back(symbol);
            if(LLVMLinkModules2(m.module,module))fail("LLVM visible function link failed");
        }
        auto context=m.context;auto builder=LLVMCreateBuilderInContext(context);
        auto integer=LLVMInt32TypeInContext(context);
        std::vector<LLVMValueRef> calls;
        for(auto fn=LLVMGetFirstFunction(m.module);fn;fn=LLVMGetNextFunction(fn))
            for(auto block=LLVMGetFirstBasicBlock(fn);block;block=LLVMGetNextBasicBlock(block))
                for(auto i=LLVMGetFirstInstruction(block);i;i=LLVMGetNextInstruction(i))
                    if(LLVMIsACallInst(i) && LLVMIsAFunction(LLVMGetCalledValue(i)) &&
                        name(LLVMGetCalledValue(i)).find("visible_function_table")!=std::string::npos)calls.push_back(i);
        try {for(auto call:calls) {
            auto op=name(LLVMGetCalledValue(call));unsigned count=LLVMGetNumArgOperands(call);
            LLVMPositionBuilderBefore(builder,call);LLVMValueRef result=nullptr;
            auto load=[&](LLVMValueRef index) {
                auto table=LLVMGetOperand(call,0);
                if(LLVMGetTypeKind(LLVMTypeOf(table))!=LLVMPointerTypeKind || LLVMGetPointerAddressSpace(LLVMTypeOf(table))!=1)fail("visible table pointer type");
                auto address=LLVMBuildGEP2(builder,integer,table,&index,1,"visible.slot");
                auto value=LLVMBuildLoad2(builder,integer,address,"visible.id");LLVMSetAlignment(value,4);return value;
            };
            if(op=="air.get_function_pointer_visible_function_table") {
                if(count!=2 || LLVMTypeOf(LLVMGetOperand(call,1))!=integer || LLVMGetTypeKind(LLVMTypeOf(call))!=LLVMPointerTypeKind)fail("visible function lookup signature");
                if(targets.empty())fail("visible function lookup requires linked functions");
                auto index=LLVMGetOperand(call,1),length=load(LLVMConstNull(integer));
                auto valid=LLVMBuildICmp(builder,LLVMIntULT,index,length,"");
                auto slot=LLVMBuildAdd(builder,index,LLVMConstInt(integer,1,false),"");
                slot=LLVMBuildSelect(builder,valid,slot,LLVMConstNull(integer),"");
                auto id=load(slot);id=LLVMBuildSelect(builder,valid,id,LLVMConstNull(integer),"");
                result=LLVMConstNull(LLVMTypeOf(call));
                for(size_t n=0;n<targets.size();++n) {
                    auto fn=LLVMGetNamedFunction(m.module,linkedNames[n].c_str());
                    if(!fn || !LLVMCountBasicBlocks(fn))fail("linked visible target definition was lost");
                    auto pointer=LLVMConstBitCast(fn,LLVMTypeOf(call));
                    auto equal=LLVMBuildICmp(builder,LLVMIntEQ,id,LLVMConstInt(integer,n+1,false),"");
                    result=LLVMBuildSelect(builder,equal,pointer,result,"visible.target");
                }
            } else if(op=="air.get_size_visible_function_table" || op=="air.is_null_visible_function_table") {
                if(count!=1)fail("visible table query arity");auto length=load(LLVMConstNull(integer));
                result=op=="air.get_size_visible_function_table"?length:LLVMBuildICmp(builder,LLVMIntEQ,length,LLVMConstNull(integer),"");
            } else if(op=="air.get_null_visible_function_table") {
                if(count || LLVMGetTypeKind(LLVMTypeOf(call))!=LLVMPointerTypeKind)fail("visible null table signature");result=LLVMConstNull(LLVMTypeOf(call));
            } else fail("unsupported visible table intrinsic: "+op);
            if(LLVMTypeOf(result)!=LLVMTypeOf(call))fail("visible table result type");
            LLVMReplaceAllUsesWith(call,result);LLVMInstructionEraseFromParent(call);
        }}catch(...){LLVMDisposeBuilder(builder);throw;}
        LLVMDisposeBuilder(builder);visibleVerify(m.module);visibleBitcode(m.module,out);return true;
    }catch(const std::exception &e){error=e.what();out.clear();return false;}
}
