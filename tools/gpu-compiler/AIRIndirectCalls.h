// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <llvm-c/Core.h>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace GPU { namespace Compiler { namespace AIRIndirectCalls {
// A540 has no Metal device-function ABI. Preserve finite, shader-local
// function pointers as integer selectors, dispatch to direct calls, and let
// the existing bounded inliner lower the selected bodies. No host/device
// address is interpreted as executable code.
class Lower {
    LLVMModuleRef module;
    LLVMContextRef context;
    LLVMBuilderRef builder;
    LLVMTypeRef integer;
    LLVMValueRef entryAnchor=nullptr;
    std::map<LLVMValueRef,LLVMValueRef> ids,addresses;
    std::map<LLVMValueRef,unsigned> functions;
    std::set<LLVMValueRef> visited,tables,targets;
    [[noreturn]] static void fail(const char *s){throw std::runtime_error(std::string("AIR indirect calls: ")+s);}
    static LLVMValueRef strip(LLVMValueRef v) {
        for(unsigned n=0;n<32;++n) {
            if(LLVMIsABitCastInst(v) || (LLVMIsAConstantExpr(v) && LLVMGetConstOpcode(v)==LLVMBitCast))v=LLVMGetOperand(v,0);
            else return v;
        }
        fail("pointer cast nesting exceeded");
    }
    static bool gep(LLVMValueRef v){return LLVMIsAGetElementPtrInst(v) || (LLVMIsAConstantExpr(v) && LLVMGetConstOpcode(v)==LLVMGetElementPtr);}
    static LLVMValueRef base(LLVMValueRef v) {
        for(unsigned n=0;n<64;++n){v=strip(v);if(!gep(v))return v;v=LLVMGetOperand(v,0);}
        fail("table pointer nesting exceeded");
    }
    unsigned function(LLVMValueRef fn) {
        if(!LLVMIsAFunction(fn) || !LLVMCountBasicBlocks(fn))fail("function target has no local definition");
        if(!functions.count(fn)){if(functions.size()==64)fail("too many finite call targets");functions[fn]=unsigned(functions.size()+1);}
        targets.insert(fn);return functions.at(fn);
    }
    LLVMTypeRef storageType(LLVMTypeRef type,unsigned depth=0) {
        if(depth>8)fail("function table nesting exceeded");
        // AIR64 InstCombine may express table indexing as byte GEPs. Keep
        // each pointer slot eight bytes so those offsets remain exact.
        if(LLVMGetTypeKind(type)==LLVMPointerTypeKind)return LLVMArrayType(integer,2);
        if(LLVMGetTypeKind(type)==LLVMIntegerTypeKind)return type;
        if(LLVMGetTypeKind(type)!=LLVMArrayTypeKind || LLVMGetArrayLength(type)>256)fail("unsupported function table storage");
        return LLVMArrayType(storageType(LLVMGetElementType(type),depth+1),LLVMGetArrayLength(type));
    }
    void constantTargets(LLVMValueRef v,unsigned depth=0) {
        if(depth>8)fail("function table initializer nesting exceeded");
        v=strip(v);
        if(LLVMIsAFunction(v)){function(v);return;}
        if(LLVMIsNull(v) || LLVMIsUndef(v) || LLVMIsPoison(v))return;
        if(!LLVMIsAConstantArray(v) || LLVMGetNumOperands(v)>256)fail("function table stores an unknown pointer");
        for(int n=0;n<LLVMGetNumOperands(v);++n)constantTargets(LLVMGetOperand(v,unsigned(n)),depth+1);
    }
    void table(LLVMValueRef root) {
        if(!tables.insert(root).second)return;
        if(tables.size()>64)fail("too many function tables");
        if(LLVMIsAGlobalVariable(root)) {
            if(!LLVMIsGlobalConstant(root) || LLVMIsExternallyInitialized(root) || !LLVMGetInitializer(root))fail("mutable external function table");
            storageType(LLVMGlobalGetValueType(root));constantTargets(LLVMGetInitializer(root));return;
        }
        if(!LLVMIsAAllocaInst(root))fail("function pointer comes from external memory");
        storageType(LLVMGetAllocatedType(root));
        auto count=LLVMGetOperand(root,0);
        if(!LLVMIsAConstantInt(count) || LLVMConstIntGetZExtValue(count)!=1)fail("function table allocation must be fixed");
        std::vector<LLVMValueRef> pending{root};std::set<LLVMValueRef> seen;
        while(!pending.empty()) {
            auto pointer=pending.back();pending.pop_back();if(!seen.insert(pointer).second)continue;
            if(seen.size()>4096)fail("function table use graph exceeded");
            for(auto use=LLVMGetFirstUse(pointer);use;use=LLVMGetNextUse(use)) {
                auto user=LLVMGetUser(use);
                if(gep(user) || LLVMIsABitCastInst(user)){pending.push_back(user);continue;}
                if(LLVMIsALoadInst(user) && LLVMGetOperand(user,0)==pointer && !LLVMGetVolatile(user) &&
                    LLVMGetOrdering(user)==LLVMAtomicOrderingNotAtomic && LLVMGetTypeKind(LLVMTypeOf(user))==LLVMPointerTypeKind)continue;
                if(LLVMIsAStoreInst(user) && LLVMGetOperand(user,1)==pointer && !LLVMGetVolatile(user) && LLVMGetOrdering(user)==LLVMAtomicOrderingNotAtomic) {
                    constantTargets(LLVMGetOperand(user,0));continue;
                }
                if(LLVMIsACallInst(user)) {
                    auto fn=strip(LLVMGetCalledValue(user));size_t n=0;auto p=LLVMGetValueName2(fn,&n);std::string name(p,n);
                    if(name.find("llvm.lifetime.")==0)continue;
                }
                fail("function table address escapes its local load/store graph");
            }
        }
    }
    void discover(LLVMValueRef v) {
        v=strip(v);if(!visited.insert(v).second)return;
        if(visited.size()>8192)fail("function pointer graph exceeded");
        if(LLVMIsAFunction(v)){function(v);return;}
        if(LLVMIsNull(v) || LLVMIsUndef(v) || LLVMIsPoison(v))return;
        if(LLVMIsASelectInst(v)){discover(LLVMGetOperand(v,1));discover(LLVMGetOperand(v,2));return;}
        if(LLVMIsAPHINode(v)){for(unsigned i=0;i<LLVMCountIncoming(v);++i)discover(LLVMGetIncomingValue(v,i));return;}
        if(LLVMIsALoadInst(v) && !LLVMGetVolatile(v) && LLVMGetOrdering(v)==LLVMAtomicOrderingNotAtomic){table(base(LLVMGetOperand(v,0)));return;}
        fail("callee is not a finite local function pointer");
    }
    LLVMValueRef initial(LLVMValueRef v) {
        v=strip(v);auto type=storageType(LLVMTypeOf(v));
        if(LLVMIsAFunction(v)) {
            LLVMValueRef words[]={LLVMConstInt(integer,functions.at(v),false),LLVMConstNull(integer)};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            return LLVMConstArray(integer,words,2);
#pragma clang diagnostic pop
        }
        if(LLVMIsNull(v))return LLVMConstNull(type);
        if(LLVMIsUndef(v))return LLVMGetUndef(type);
        if(LLVMIsPoison(v))return LLVMGetPoison(type);
        std::vector<LLVMValueRef> values;for(int n=0;n<LLVMGetNumOperands(v);++n)values.push_back(initial(LLVMGetOperand(v,unsigned(n))));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        return LLVMConstArray(LLVMGetElementType(type),values.data(),unsigned(values.size()));
#pragma clang diagnostic pop
    }
    LLVMValueRef address(LLVMValueRef v) {
        v=strip(v);auto found=addresses.find(v);if(found!=addresses.end())return found->second;
        if(!gep(v))fail("unknown function table address");
        auto pointer=address(LLVMGetOperand(v,0));std::vector<LLVMValueRef> indices;
        for(int n=1;n<LLVMGetNumOperands(v);++n)indices.push_back(LLVMGetOperand(v,unsigned(n)));
        auto type=storageType(LLVMGetGEPSourceElementType(v));LLVMValueRef result;
        if(LLVMIsAConstantExpr(v)){LLVMPositionBuilderBefore(builder,entryAnchor);result=LLVMBuildGEP2(builder,type,pointer,indices.data(),unsigned(indices.size()),"call.table.index");}
        else {LLVMPositionBuilderBefore(builder,v);result=LLVMBuildGEP2(builder,type,pointer,indices.data(),unsigned(indices.size()),"call.table.index");}
        return addresses[v]=result;
    }
    void materializeTables() {
        for(auto root:tables) {
            if(LLVMIsAGlobalVariable(root)) {
                LLVMPositionBuilderBefore(builder,entryAnchor);
                auto copy=LLVMBuildAlloca(builder,storageType(LLVMGlobalGetValueType(root)),"call.targets");
                LLVMBuildStore(builder,initial(LLVMGetInitializer(root)),copy);addresses[root]=copy;
            } else {
                LLVMPositionBuilderBefore(builder,root);addresses[root]=LLVMBuildAlloca(builder,storageType(LLVMGetAllocatedType(root)),"call.targets");
            }
        }
        for(auto root:tables)if(LLVMIsAAllocaInst(root)) {
            std::vector<LLVMValueRef> pending{root};std::set<LLVMValueRef> seen;
            while(!pending.empty()) {
                auto pointer=pending.back();pending.pop_back();if(!seen.insert(pointer).second)continue;
                for(auto use=LLVMGetFirstUse(pointer);use;use=LLVMGetNextUse(use)) {
                    auto user=LLVMGetUser(use);
                    if(gep(user) || LLVMIsABitCastInst(user))pending.push_back(user);
                    if(LLVMIsAStoreInst(user)) {
                        auto destination=address(LLVMGetOperand(user,1));LLVMPositionBuilderBefore(builder,user);
                        LLVMBuildStore(builder,initial(LLVMGetOperand(user,0)),destination);
                    }
                }
            }
        }
    }
    LLVMValueRef selector(LLVMValueRef v) {
        v=strip(v);auto found=ids.find(v);if(found!=ids.end())return found->second;
        if(LLVMIsAFunction(v))return LLVMConstInt(integer,functions.at(v),false);
        if(LLVMIsNull(v))return LLVMConstNull(integer);
        if(LLVMIsUndef(v) || LLVMIsPoison(v))return LLVMGetUndef(integer);
        if(LLVMIsAPHINode(v)) {
            LLVMPositionBuilderBefore(builder,v);auto phi=LLVMBuildPhi(builder,integer,"call.target");ids[v]=phi;
            for(unsigned n=0;n<LLVMCountIncoming(v);++n){auto value=selector(LLVMGetIncomingValue(v,n));auto block=LLVMGetIncomingBlock(v,n);LLVMAddIncoming(phi,&value,&block,1);}return phi;
        }
        if(LLVMIsASelectInst(v)) {
            auto a=selector(LLVMGetOperand(v,1)),b=selector(LLVMGetOperand(v,2));LLVMPositionBuilderBefore(builder,v);
            return ids[v]=LLVMBuildSelect(builder,LLVMGetOperand(v,0),a,b,"call.target");
        }
        if(LLVMIsALoadInst(v)) {
            auto pointer=address(LLVMGetOperand(v,0));LLVMPositionBuilderBefore(builder,v);
            return ids[v]=LLVMBuildLoad2(builder,integer,pointer,"call.target");
        }
        fail("unknown finite function pointer");
    }
    void dispatch(LLVMValueRef call) {
        auto type=LLVMGetCalledFunctionType(call);if(LLVMIsFunctionVarArg(type))fail("variadic indirect calls are unsupported");
        auto convention=LLVMGetInstructionCallConv(call);
        std::vector<LLVMValueRef> candidates;
        for(auto fn:targets)if(LLVMGlobalGetValueType(fn)==type && LLVMGetFunctionCallConv(fn)==convention)candidates.push_back(fn);
        if(candidates.empty())fail("no function target has the call signature");
        auto id=selector(LLVMGetCalledValue(call));unsigned count=LLVMGetNumArgOperands(call);
        std::vector<LLVMTypeRef> types(count+1);types[0]=integer;LLVMGetParamTypes(type,types.data()+1);
        auto wrapperType=LLVMFunctionType(LLVMGetReturnType(type),types.data(),count+1,false);
        auto wrapper=LLVMAddFunction(module,"call.dispatch",wrapperType);LLVMSetLinkage(wrapper,LLVMInternalLinkage);LLVMSetFunctionCallConv(wrapper,convention);
        auto entry=LLVMAppendBasicBlockInContext(context,wrapper,"dispatch"),invalid=LLVMAppendBasicBlockInContext(context,wrapper,"invalid");
        LLVMPositionBuilderAtEnd(builder,invalid);
        if(LLVMGetTypeKind(LLVMTypeOf(call))==LLVMVoidTypeKind)LLVMBuildRetVoid(builder);
        else LLVMBuildRet(builder,LLVMConstNull(LLVMTypeOf(call)));
        LLVMPositionBuilderAtEnd(builder,entry);
        auto branch=LLVMBuildSwitch(builder,LLVMGetParam(wrapper,0),invalid,unsigned(candidates.size()));
        std::vector<LLVMValueRef> arguments;for(unsigned n=0;n<count;++n)arguments.push_back(LLVMGetParam(wrapper,n+1));
        for(auto fn:candidates) {
            auto block=LLVMAppendBasicBlockInContext(context,wrapper,"target");LLVMAddCase(branch,LLVMConstInt(integer,functions.at(fn),false),block);
            LLVMPositionBuilderAtEnd(builder,block);auto result=LLVMBuildCall2(builder,type,fn,arguments.data(),count,"");LLVMSetInstructionCallConv(result,convention);
            // Retain byval/alignment, convergent and return attributes on each
            // real call. The wrapper's parameter indices are shifted below.
            for(unsigned n=0;n<=count+1;++n) {
                auto index=n==count+1?LLVMAttributeFunctionIndex:n;unsigned size=LLVMGetCallSiteAttributeCount(call,index);
                std::vector<LLVMAttributeRef> attributes(size);LLVMGetCallSiteAttributes(call,index,attributes.data());
                for(auto attribute:attributes)LLVMAddCallSiteAttribute(result,index,attribute);
            }
            if(LLVMGetTypeKind(LLVMTypeOf(call))==LLVMVoidTypeKind)LLVMBuildRetVoid(builder);else LLVMBuildRet(builder,result);
        }
        arguments.clear();arguments.push_back(id);for(unsigned n=0;n<count;++n)arguments.push_back(LLVMGetOperand(call,n));
        LLVMPositionBuilderBefore(builder,call);auto replacement=LLVMBuildCall2(builder,wrapperType,wrapper,arguments.data(),unsigned(arguments.size()),"");LLVMSetInstructionCallConv(replacement,convention);
        for(unsigned n=0;n<=count;++n) {
            unsigned size=LLVMGetCallSiteAttributeCount(call,n),index=n?n+1:0;
            std::vector<LLVMAttributeRef> attributes(size);LLVMGetCallSiteAttributes(call,n,attributes.data());
            for(auto attribute:attributes) {
                LLVMAddAttributeAtIndex(wrapper,index,attribute);
                LLVMAddCallSiteAttribute(replacement,index,attribute);
            }
        }
        const unsigned convergent=LLVMGetEnumAttributeKindForName("convergent",10);
        bool needsConvergence=LLVMGetCallSiteEnumAttribute(call,LLVMAttributeFunctionIndex,convergent)!=nullptr;
        for(auto fn:candidates)needsConvergence|=LLVMGetEnumAttributeAtIndex(fn,LLVMAttributeFunctionIndex,convergent)!=nullptr;
        if(needsConvergence) {
            auto attribute=LLVMCreateEnumAttribute(context,convergent,0);
            LLVMAddAttributeAtIndex(wrapper,LLVMAttributeFunctionIndex,attribute);
            LLVMAddCallSiteAttribute(replacement,LLVMAttributeFunctionIndex,attribute);
        }
        LLVMReplaceAllUsesWith(call,replacement);LLVMInstructionEraseFromParent(call);
    }
public:
    explicit Lower(LLVMModuleRef m):module(m),context(LLVMGetModuleContext(m)),builder(LLVMCreateBuilderInContext(context)),integer(LLVMInt32TypeInContext(context)){}
    ~Lower(){LLVMDisposeBuilder(builder);}
    void run(const std::vector<LLVMValueRef> &calls) {
        auto parent=LLVMGetBasicBlockParent(LLVMGetInstructionParent(calls.at(0)));
        entryAnchor=LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(parent));
        for(auto call:calls)if(LLVMGetBasicBlockParent(LLVMGetInstructionParent(call))!=parent)fail("mixed function dispatch lowering");
        for(auto call:calls)discover(LLVMGetCalledValue(call));
        materializeTables();for(auto call:calls)dispatch(call);
    }
};
} } }
