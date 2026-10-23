// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "FunctionConstants.h"
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <algorithm>
#include <dlfcn.h>
#include <map>
#include <set>
#include <stdexcept>

namespace GPU { namespace Compiler { namespace AIRSpecialization {
[[noreturn]] inline void fail(const std::string &s){throw std::runtime_error("AIR specialization: "+s);}
inline LLVMValueRef switchCase(LLVMValueRef i,unsigned successor) {
    const unsigned count=LLVMGetNumSuccessors(i),operands=unsigned(LLVMGetNumOperands(i));
    if(!successor || successor>=count)fail("invalid switch successor");
    if(operands==2*count)return LLVMGetOperand(i,2*successor);
    // Runtime iOS LLVM has operand pairs. New host LLVM stores case values
    // separately; use its exported accessor without adding an iOS import.
    if(operands!=count+1)fail("unsupported LLVM switch representation");
    using Getter=LLVMValueRef (*)(LLVMValueRef,unsigned);
    static Getter getter=[] {
        auto symbol=dlsym(RTLD_DEFAULT,"LLVMGetSwitchCaseValue");
#ifdef __APPLE__
        if(!symbol) {
            void *module=dlopen("/usr/lib/libLLVM.dylib",RTLD_LAZY|RTLD_NOLOAD);
            if(module){symbol=dlsym(module,"LLVMGetSwitchCaseValue");dlclose(module);}
        }
#endif
        return reinterpret_cast<Getter>(symbol);
    }();
    if(!getter)fail("this LLVM needs the exported switch-case accessor");
    return getter(i,successor);
}
inline std::vector<LLVMValueRef> node(LLVMValueRef v) {
    if(!v || !LLVMIsAMDNode(v))fail("expected metadata node");
    std::vector<LLVMValueRef> a(LLVMGetMDNodeNumOperands(v));LLVMGetMDNodeOperands(v,a.data());return a;
}
inline std::string text(LLVMValueRef v){unsigned n=0;auto s=v?LLVMGetMDString(v,&n):nullptr;return s?std::string(s,n):std::string();}
inline std::string name(LLVMValueRef v){size_t n=0;auto s=LLVMGetValueName2(v,&n);return {s,n};}
inline uint64_t number(LLVMValueRef v) {
    if(!v || !LLVMIsAConstantInt(v) || LLVMGetIntTypeWidth(LLVMTypeOf(v))>64)fail("expected constant integer");
    return LLVMConstIntGetZExtValue(v);
}
inline LLVMValueRef field(LLVMValueRef v,const char *key) {
    auto a=node(v);for(size_t i=0;i+1<a.size();++i)if(text(a[i])==key)return a[i+1];return nullptr;
}
inline LLVMValueRef root(LLVMValueRef v) {
    for(unsigned i=0;i<8 && v && LLVMIsAConstantExpr(v);++i) {
        auto op=LLVMGetConstOpcode(v);if(op!=LLVMBitCast && op!=LLVMAddrSpaceCast)break;v=LLVMGetOperand(v,0);
    }
    return v;
}
inline uint64_t interfaceNumber(LLVMValueRef v) {
    auto g=root(v);
    if(g && LLVMIsAGlobalVariable(g)) {
        auto type=LLVMGlobalGetValueType(g);
        if(LLVMGetPointerAddressSpace(LLVMTypeOf(g))!=2 || !LLVMIsGlobalConstant(g) ||
            LLVMIsExternallyInitialized(g) || LLVMGetTypeKind(type)!=LLVMIntegerTypeKind ||
            LLVMGetIntTypeWidth(type)>64)
            fail("interface index is not a specialized constant integer");
        auto initial=LLVMGetInitializer(g);
        if(!initial || LLVMTypeOf(initial)!=type)fail("interface index lacks a constant initializer");
        return number(initial);
    }
    return number(v);
}
inline LLVMValueRef predicateRoot(LLVMValueRef v) {
    // Typed-pointer AIR passes a vector constant to the i8* predicate as
    // getelementptr (vector, global, 0, 0). Opaque-pointer LLVM folds this
    // alias away. Recognize only constant, all-zero GEPs; an interior address
    // must never acquire a function constant's identity or defined state.
    for(unsigned depth=0;depth<16;++depth) {
        v=root(v);
        if(!v || !LLVMIsAConstantExpr(v) || LLVMGetConstOpcode(v)!=LLVMGetElementPtr)return v;
        auto count=LLVMGetNumOperands(v);if(count<2)return v;
        for(int i=1;i<count;++i) {
            auto index=LLVMGetOperand(v,unsigned(i));
            if(!LLVMIsAConstantInt(index) || !LLVMIsNull(index))return v;
        }
        v=LLVMGetOperand(v,0);
    }
    fail("function constant predicate pointer graph is too deep");
}
inline std::string describe(LLVMValueRef v) {
    if(!v)return "null";
    auto type=LLVMPrintTypeToString(LLVMTypeOf(v));
    auto printed=LLVMPrintValueToString(v);
    std::string result="kind="+std::to_string(unsigned(LLVMGetValueKind(v)))+
        " symbol="+name(v).substr(0,128)+" type="+(type?std::string(type).substr(0,128):"null")+
        " value="+(printed?std::string(printed).substr(0,384):"null");
    LLVMDisposeMessage(type);LLVMDisposeMessage(printed);return result;
}
inline LLVMValueRef element(LLVMValueRef v,unsigned i) {
    if(LLVMIsUndef(v) || LLVMIsPoison(v)) {
        auto t=LLVMTypeOf(v);auto member=LLVMGetTypeKind(t)==LLVMStructTypeKind?LLVMStructGetTypeAtIndex(t,i):LLVMGetElementType(t);
        return LLVMIsPoison(v)?LLVMGetPoison(member):LLVMGetUndef(member);
    }
    if(LLVMIsAConstantDataSequential(v)) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        return LLVMGetElementAsConstant(v,i);
#pragma clang diagnostic pop
    }
    if(LLVMIsAConstantAggregateZero(v)) {
        auto t=LLVMTypeOf(v);return LLVMConstNull(LLVMGetTypeKind(t)==LLVMStructTypeKind?LLVMStructGetTypeAtIndex(t,i):LLVMGetElementType(t));
    }
    return LLVMGetOperand(v,i);
}
inline bool enabled(LLVMValueRef md) {
    auto pred=field(md,"air.function_constant");if(!pred)return true;
    auto a=node(pred);if(a.size()!=3 || text(a[1])!="bool")fail("invalid conditional interface predicate");
    auto g=root(a[0]);if(!LLVMIsAGlobalVariable(g))fail("conditional interface predicate is not a global");
    auto value=LLVMGetInitializer(g);
    // MSL optional interface attributes are absent if their constant is
    // undefined, or if its evaluated value is false.
    if(!value || LLVMIsUndef(value) || LLVMIsPoison(value))return false;
    return number(value)!=0;
}
struct Definition {FunctionConstant info;LLVMValueRef global=nullptr;};
class State {
    struct Builder {
        LLVMBuilderRef value;
        operator LLVMBuilderRef()const{return value;}
        ~Builder(){LLVMDisposeBuilder(value);}
    };
    struct Layout {LLVMTargetDataRef value;~Layout(){LLVMDisposeTargetData(value);}};
    LLVMModuleRef module;
    LLVMContextRef context;
    Builder builder;
    Layout layout;
    std::vector<Definition> definitions;
    std::vector<std::pair<uint64_t,LLVMValueRef>> constructors;
    std::vector<LLVMValueRef> entries,entryMetadata;
    std::map<LLVMValueRef,std::set<LLVMValueRef>> dependencies;
    std::set<LLVMValueRef> needed,provided;
    std::map<LLVMValueRef,LLVMValueRef> values;
    unsigned budget=0;
    LLVMValueRef storageRoot(LLVMValueRef v) {
        for(unsigned n=0;n<128;++n) {
            v=root(v);
            if(!v || LLVMIsAGlobalVariable(v))return v;
            if((LLVMIsAConstantExpr(v) && LLVMGetConstOpcode(v)==LLVMGetElementPtr) || LLVMIsAGetElementPtrInst(v))v=LLVMGetOperand(v,0);
            else return v;
        }
        fail("static initializer pointer graph is too deep");
    }
    struct Address {LLVMValueRef global;uint64_t offset;};
    Address address(LLVMValueRef v,unsigned depth=0) {
        if(depth>128)fail("static initializer pointer graph is too deep");
        v=root(v);
        if(LLVMIsAGlobalVariable(v)) {
            auto type=LLVMGlobalGetValueType(v);
            if(LLVMGetPointerAddressSpace(LLVMTypeOf(v))!=2 || !LLVMTypeIsSized(type) || LLVMABISizeOfType(layout.value,type)>4096)
                fail("static initializer requires a bounded constant-address-space object");
            return {v,0};
        }
        if(!((LLVMIsAConstantExpr(v) && LLVMGetConstOpcode(v)==LLVMGetElementPtr) || LLVMIsAGetElementPtrInst(v)))
            fail("static initializer pointer escaped constant storage");
        auto result=address(LLVMGetOperand(v,0),depth+1);auto type=LLVMGetGEPSourceElementType(v);
        for(int i=1;i<LLVMGetNumOperands(v);++i) {
            auto index=number(value(LLVMGetOperand(v,unsigned(i))));
            if(i==1){if(index)fail("static initializer GEP leaves its object");continue;}
            auto kind=LLVMGetTypeKind(type);uint64_t offset;
            if(kind==LLVMStructTypeKind) {
                if(index>=LLVMCountStructElementTypes(type))fail("static initializer struct index out of bounds");
                offset=LLVMOffsetOfElement(layout.value,type,unsigned(index));type=LLVMStructGetTypeAtIndex(type,unsigned(index));
            } else if(kind==LLVMArrayTypeKind || kind==LLVMVectorTypeKind) {
                auto count=kind==LLVMArrayTypeKind?LLVMGetArrayLength(type):LLVMGetVectorSize(type);
                if(index>=count)fail("static initializer array index out of bounds");
                type=LLVMGetElementType(type);offset=index*LLVMABISizeOfType(layout.value,type);
            } else fail("static initializer GEP indexes a scalar");
            if(offset>4096 || result.offset>4096-offset)fail("static initializer GEP offset out of bounds");
            result.offset+=offset;
        }
        return result;
    }
    // All reads/writes rebuild LLVM constants. No host address or target
    // pointer is dereferenced, and padding/cross-member accesses are rejected.
    LLVMValueRef access(LLVMValueRef initial,uint64_t offset,LLVMTypeRef type,LLVMValueRef replacement=nullptr,unsigned depth=0) {
        if(depth>16)fail("static initializer aggregate nesting limit exceeded");
        auto source=LLVMTypeOf(initial);uint64_t size=LLVMABISizeOfType(layout.value,source),bytes=LLVMABISizeOfType(layout.value,type);
        if(offset>size || bytes>size-offset)fail("static initializer memory access out of bounds");
        if(!offset && source==type)return replacement?replacement:initial;
        auto kind=LLVMGetTypeKind(source);unsigned count=kind==LLVMStructTypeKind?LLVMCountStructElementTypes(source):
            kind==LLVMArrayTypeKind?LLVMGetArrayLength(source):kind==LLVMVectorTypeKind?LLVMGetVectorSize(source):0;
        if(!count || count>4096)fail("static initializer memory type mismatch");
        for(unsigned n=0;n<count;++n) {
            auto member=kind==LLVMStructTypeKind?LLVMStructGetTypeAtIndex(source,n):LLVMGetElementType(source);
            uint64_t memberBytes=LLVMABISizeOfType(layout.value,member),at=kind==LLVMStructTypeKind?LLVMOffsetOfElement(layout.value,source,n):n*memberBytes;
            if(offset<at || offset-at>=memberBytes)continue;
            if(bytes>memberBytes-(offset-at))fail("static initializer crosses an aggregate member");
            auto child=access(element(initial,n),offset-at,type,replacement,depth+1);
            if(!replacement)return child;
            std::vector<LLVMValueRef> members;for(unsigned j=0;j<count;++j)members.push_back(j==n?child:element(initial,j));
            if(kind==LLVMStructTypeKind)return LLVMConstNamedStruct(source,members.data(),count);
            if(kind==LLVMVectorTypeKind)return LLVMConstVector(members.data(),count);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            return LLVMConstArray(LLVMGetElementType(source),members.data(),count);
#pragma clang diagnostic pop
        }
        fail("static initializer accesses aggregate padding");
    }
    void globals(LLVMValueRef v,std::set<LLVMValueRef> &out,std::set<LLVMValueRef> &seen) {
        // Real QuartzCore fragment shaders have long SSA chains and cycles
        // through loop PHIs. Bound visited values, not the C++ call stack or
        // a valid shader's dependency depth.
        std::vector<LLVMValueRef> pending{v};
        while(!pending.empty()) {
            auto item=pending.back();pending.pop_back();
            if(!item || !seen.insert(item).second)continue;
            if(seen.size()>65536)fail("constant dependency graph is too large");
            if(LLVMIsAGlobalVariable(item)){out.insert(item);continue;}
            if(LLVMIsAFunction(item)) {
                for(auto b=LLVMGetFirstBasicBlock(item);b;b=LLVMGetNextBasicBlock(b))
                    for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))pending.push_back(i);
                continue;
            }
            if(LLVMIsAInstruction(item) || LLVMIsAConstantExpr(item) || LLVMIsAConstantArray(item) || LLVMIsAConstantStruct(item) || LLVMIsAConstantVector(item))
                for(int i=0;i<LLVMGetNumOperands(item);++i)pending.push_back(LLVMGetOperand(item,unsigned(i)));
        }
    }
    LLVMValueRef value(LLVMValueRef v) {
        auto found=values.find(v);if(found!=values.end())return found->second;
        if(LLVMIsAConstant(v) && !LLVMIsAGlobalValue(v))return v;
        fail("nonconstant static initializer operand: "+describe(v));
    }
    LLVMValueRef operation(LLVMValueRef i) {
        auto op=LLVMGetInstructionOpcode(i);
        if(LLVMGetNumOperands(i)<1)fail("unsupported static initializer instruction");
        auto a=value(LLVMGetOperand(i,0));
        if(op==LLVMTrunc || op==LLVMZExt || op==LLVMSExt || op==LLVMFPTrunc || op==LLVMFPExt ||
            op==LLVMFPToUI || op==LLVMFPToSI || op==LLVMUIToFP || op==LLVMSIToFP || op==LLVMBitCast)
            return LLVMBuildCast(builder,op,a,LLVMTypeOf(i),"");
        if(op==LLVMExtractElement)return LLVMBuildExtractElement(builder,a,value(LLVMGetOperand(i,1)),"");
        if(op==LLVMInsertElement)return LLVMBuildInsertElement(builder,a,value(LLVMGetOperand(i,1)),value(LLVMGetOperand(i,2)),"");
        if(op==LLVMSelect)return LLVMBuildSelect(builder,a,value(LLVMGetOperand(i,1)),value(LLVMGetOperand(i,2)),"");
        if(LLVMGetNumOperands(i)<2)fail("unsupported unary static initializer instruction");
        auto b=value(LLVMGetOperand(i,1));
        switch(op) {
            case LLVMAdd:return LLVMBuildAdd(builder,a,b,"");case LLVMSub:return LLVMBuildSub(builder,a,b,"");
            case LLVMMul:return LLVMBuildMul(builder,a,b,"");case LLVMUDiv:return LLVMBuildUDiv(builder,a,b,"");
            case LLVMSDiv:return LLVMBuildSDiv(builder,a,b,"");case LLVMURem:return LLVMBuildURem(builder,a,b,"");
            case LLVMSRem:return LLVMBuildSRem(builder,a,b,"");case LLVMAnd:return LLVMBuildAnd(builder,a,b,"");
            case LLVMOr:return LLVMBuildOr(builder,a,b,"");case LLVMXor:return LLVMBuildXor(builder,a,b,"");
            case LLVMShl:return LLVMBuildShl(builder,a,b,"");case LLVMLShr:return LLVMBuildLShr(builder,a,b,"");
            case LLVMAShr:return LLVMBuildAShr(builder,a,b,"");case LLVMFAdd:return LLVMBuildFAdd(builder,a,b,"");
            case LLVMFSub:return LLVMBuildFSub(builder,a,b,"");case LLVMFMul:return LLVMBuildFMul(builder,a,b,"");
            case LLVMFDiv:return LLVMBuildFDiv(builder,a,b,"");case LLVMFRem:return LLVMBuildFRem(builder,a,b,"");
            case LLVMICmp:return LLVMBuildICmp(builder,LLVMGetICmpPredicate(i),a,b,"");
            case LLVMFCmp:return LLVMBuildFCmp(builder,LLVMGetFCmpPredicate(i),a,b,"");
            default:fail("unsupported static initializer instruction");
        }
    }
    LLVMValueRef intrinsic(LLVMValueRef i) {
        auto fn=LLVMGetCalledValue(i);if(!LLVMIsAFunction(fn))fail("indirect static initializer call");auto n=name(fn);
        if(n=="air.is_function_constant_defined") {
            if(LLVMGetNumArgOperands(i)!=1 || LLVMGetTypeKind(LLVMTypeOf(i))!=LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(i))!=1)
                fail("invalid is_function_constant_defined signature");
            auto g=predicateRoot(LLVMGetOperand(i,0));bool known=false;
            for(const auto &d:definitions)known|=d.global==g;
            if(!known) {
                std::string detail="is_function_constant_defined does not name a function constant; operand "+describe(g)+
                    "; call "+describe(i)+"; definitions="+std::to_string(definitions.size())+" [";
                for(const auto &d:definitions)detail+=std::to_string(d.info.index)+":"+d.info.name.substr(0,80)+"@"+
                    name(d.global).substr(0,128)+(provided.count(d.global)?"=provided;":"=absent;");
                fail(detail+"]");
            }
            return LLVMConstInt(LLVMTypeOf(i),provided.count(g),false);
        }
        if(n=="air.normalize_function_constant_predicate.i8") {
            if(LLVMGetNumArgOperands(i)!=1 || LLVMGetTypeKind(LLVMTypeOf(i))!=LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(i))!=8 || LLVMTypeOf(LLVMGetOperand(i,0))!=LLVMTypeOf(i))
                fail("invalid function constant predicate signature");
            auto a=value(LLVMGetOperand(i,0));
            bool result=!LLVMIsUndef(a) && !LLVMIsPoison(a) && number(a)!=0;
            return LLVMConstInt(LLVMTypeOf(i),result,false);
        }
        if(n=="air.convert.f.f32.u.i1") {
            if(LLVMGetNumArgOperands(i)!=1 || LLVMGetTypeKind(LLVMTypeOf(i))!=LLVMFloatTypeKind ||
                LLVMGetTypeKind(LLVMTypeOf(LLVMGetOperand(i,0)))!=LLVMIntegerTypeKind ||
                LLVMGetIntTypeWidth(LLVMTypeOf(LLVMGetOperand(i,0)))!=1)
                fail("invalid boolean-to-float conversion signature");
            return LLVMBuildUIToFP(builder,value(LLVMGetOperand(i,0)),LLVMTypeOf(i),"");
        }
        fail("unsupported static initializer call: "+n);
    }
    void run(LLVMValueRef fn) {
        values.clear();LLVMBasicBlockRef previous=nullptr,block=LLVMGetFirstBasicBlock(fn);
        if(!block || LLVMCountParams(fn) || LLVMGetTypeKind(LLVMGetReturnType(LLVMGlobalGetValueType(fn)))!=LLVMVoidTypeKind)
            fail("invalid static initializer");
        for(;;) {
            const auto incomingValues=values;
            LLVMBasicBlockRef next=nullptr;
            for(auto i=LLVMGetFirstInstruction(block);i;i=LLVMGetNextInstruction(i)) {
                if(++budget>8192)fail("static initializer instruction budget exceeded");
                auto op=LLVMGetInstructionOpcode(i);
                if(op==LLVMRet)return;
                // LLVM18 has one Br opcode2; LLVM23 split it into70/71.
                if(unsigned(op)==2 || op==LLVMUncondBr || op==LLVMCondBr) {
                    unsigned count=LLVMGetNumSuccessors(i);
                    if(count!=1 && count!=2)fail("invalid static initializer branch");
                    next=LLVMGetSuccessor(i,count==1?0:number(value(LLVMGetOperand(i,0)))?0:1);break;
                }
                if(op==LLVMSwitch) {
                    const unsigned count=LLVMGetNumSuccessors(i);
                    if(!count || count>4097)
                        fail("invalid static initializer switch");
                    auto condition=value(LLVMGetOperand(i,0));
                    const auto selected=number(condition);
                    next=LLVMGetSwitchDefaultDest(i);
                    for(unsigned j=1;j<count;++j) {
                        if(++budget>8192)fail("static initializer instruction budget exceeded");
                        auto match=switchCase(i,j);
                        if(LLVMTypeOf(match)!=LLVMTypeOf(condition))fail("static initializer switch type mismatch");
                        if(number(match)==selected)next=LLVMGetSuccessor(i,j);
                    }
                    break;
                }
                if(op==LLVMPHI) {
                    bool found=false;
                    for(unsigned j=0;j<LLVMCountIncoming(i);++j)if(LLVMGetIncomingBlock(i,j)==previous) {
                        auto source=LLVMGetIncomingValue(i,j);auto old=incomingValues.find(source);
                        if(old!=incomingValues.end())values[i]=old->second;
                        else if(LLVMIsAConstant(source) && !LLVMIsAGlobalValue(source))values[i]=source;
                        else fail("unresolved static initializer incoming value");found=true;break;
                    }
                    if(!found)fail("unresolved static initializer phi");continue;
                }
                if(op==LLVMLoad || op==LLVMStore) {
                    auto a=address(LLVMGetOperand(i,op==LLVMLoad?0:1));auto g=a.global;auto initial=LLVMGetInitializer(g);
                    if(!initial)fail("external static initializer memory access");
                    if(op==LLVMLoad)values[i]=access(initial,a.offset,LLVMTypeOf(i));
                    else {
                        auto v=value(LLVMGetOperand(i,0));
                        if(LLVMIsGlobalConstant(g))fail("invalid static initializer store");
                        LLVMSetInitializer(g,access(initial,a.offset,LLVMTypeOf(v),v));dependencies[g];
                    }
                    continue;
                }
                auto result=op==LLVMCall?intrinsic(i):operation(i);
                if(!LLVMIsAConstant(result))fail("static initialization did not produce a constant");values[i]=result;
            }
            if(!next)fail("static initializer lacks a return/branch");previous=block;block=next;
        }
    }
    LLVMValueRef literal(const FunctionConstantValue &v,LLVMTypeRef type) {
        auto shape=constantShape(v.type);std::vector<LLVMValueRef> components;
        auto scalar=shape.lanes==1?type:LLVMGetElementType(type);
        for(unsigned i=0;i<shape.lanes;++i) {
            uint64_t bits=0;for(unsigned j=0;j<shape.bits/8;++j)bits|=uint64_t(v.bytes[i*(shape.bits/8)+j])<<(8*j);
            if(shape.boolean)bits=bits!=0;
            auto integer=LLVMConstInt(LLVMIntTypeInContext(context,shape.boolean?LLVMGetIntTypeWidth(scalar):shape.bits),bits,false);
            components.push_back(shape.floating?LLVMConstBitCast(integer,scalar):integer);
        }
        return shape.lanes==1?components[0]:LLVMConstVector(components.data(),unsigned(components.size()));
    }
    std::vector<LLVMValueRef> reachableFunctions()const {
        std::vector<LLVMValueRef> functions=entries;std::set<LLVMValueRef> seen(entries.begin(),entries.end());
        unsigned instructions=0;
        for(size_t n=0;n<functions.size();++n) {
            if(functions.size()>256)fail("helper function graph is too large");
            for(auto b=LLVMGetFirstBasicBlock(functions[n]);b;b=LLVMGetNextBasicBlock(b))
                for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i)) {
                    if(++instructions>65536)fail("helper instruction graph is too large");
                    if(LLVMGetInstructionOpcode(i)!=LLVMCall)continue;
                    auto fn=root(LLVMGetCalledValue(i));
                    if(LLVMIsAFunction(fn) && LLVMCountBasicBlocks(fn) && seen.insert(fn).second)functions.push_back(fn);
                }
        }
        return functions;
    }
    void discardUnusedHelperBodies() {
        // LLVM's module-to-function adaptor visits every definition, including
        // helpers whose only callers disappeared during specialization. Keep
        // the selected entries and every IR-referenced function. A declaration
        // preserves AIR metadata references without optimizing an unused body.
        // Iterate because dropping one body can release its callees. Cycles or
        // address-taken functions remain conservatively intact.
        bool changed;
        do {
            changed=false;
            for(auto fn=LLVMGetFirstFunction(module);fn;fn=LLVMGetNextFunction(fn)) {
                if(!LLVMCountBasicBlocks(fn) || LLVMGetFirstUse(fn) ||
                   std::find(entries.begin(),entries.end(),fn)!=entries.end())continue;
                bool ordinary=true;
                for(auto b=LLVMGetFirstBasicBlock(fn);b;b=LLVMGetNextBasicBlock(b))
                    for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))
                        if(LLVMGetTypeKind(LLVMTypeOf(i))==LLVMTokenTypeKind)ordinary=false;
                if(!ordinary)continue;
                // Drop SSA uses before erasing instructions, including PHIs
                // and backedges. Erase all terminators before deleting blocks.
                for(auto b=LLVMGetFirstBasicBlock(fn);b;b=LLVMGetNextBasicBlock(b))
                    for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))
                        if(LLVMGetTypeKind(LLVMTypeOf(i))!=LLVMVoidTypeKind)
                            LLVMReplaceAllUsesWith(i,LLVMGetUndef(LLVMTypeOf(i)));
                for(auto b=LLVMGetFirstBasicBlock(fn);b;b=LLVMGetNextBasicBlock(b))
                    while(auto i=LLVMGetLastInstruction(b))LLVMInstructionEraseFromParent(i);
                while(auto b=LLVMGetFirstBasicBlock(fn))LLVMDeleteBasicBlock(b);
                LLVMSetLinkage(fn,LLVMExternalLinkage);
                changed=true;
            }
        } while(changed);
    }
    void passes(const char *pipeline) {
        discardUnusedHelperBodies();
        auto options=LLVMCreatePassBuilderOptions();auto error=LLVMRunPasses(module,pipeline,nullptr,options);LLVMDisposePassBuilderOptions(options);
        if(error){auto message=LLVMGetErrorMessage(error);std::string diagnostic=message;LLVMDisposeErrorMessage(message);fail(diagnostic);}
    }
public:
    State(LLVMModuleRef m,const std::string &selected):State(m,selected.empty()?std::set<std::string>{}:std::set<std::string>{selected}) {}
    State(LLVMModuleRef m,const std::set<std::string> &selected):module(m),context(LLVMGetModuleContext(m)),builder{LLVMCreateBuilderInContext(context)},layout{LLVMCreateTargetData(LLVMGetDataLayoutStr(m))} {
        unsigned count=LLVMGetNamedMetadataNumOperands(module,"air.function_constants");
        if(count>MaxFunctionConstants)fail("too many function constants");
        std::vector<LLVMValueRef> metadata(count);LLVMGetNamedMetadataOperands(module,"air.function_constants",metadata.data());
        std::set<std::string> names;std::set<uint32_t> indices;
        for(auto md:metadata) {
            auto a=node(md);if(a.size()!=5)fail("invalid function constant descriptor");
            auto index=number(a[3]),required=number(a[4]);if(index>65535 || required>1)fail("invalid function constant index/required flag");
            Definition d;d.global=root(a[0]);d.info={text(a[2]),uint32_t(index),constantType(text(a[1])),required!=0};
            auto shape=constantShape(d.info.type);
            if(!LLVMIsAGlobalVariable(d.global) || LLVMGetPointerAddressSpace(LLVMTypeOf(d.global))!=2 ||
                !shape.bits || d.info.name.empty() || d.info.index>65535 || !names.insert(d.info.name).second || !indices.insert(d.info.index).second)
                fail("invalid, duplicate or unsupported function constant");
            auto type=LLVMGlobalGetValueType(d.global);unsigned lanes=LLVMGetTypeKind(type)==LLVMVectorTypeKind?LLVMGetVectorSize(type):1;
            auto scalar=lanes==1?type:LLVMGetElementType(type);auto kind=LLVMGetTypeKind(scalar);
            if(lanes!=shape.lanes || (shape.floating?(kind!=(shape.bits==16?LLVMHalfTypeKind:LLVMFloatTypeKind)):
                (kind!=LLVMIntegerTypeKind || (LLVMGetIntTypeWidth(scalar)!=shape.bits && !(shape.boolean && LLVMGetIntTypeWidth(scalar)==1)))))
                fail("function constant type/metadata mismatch");
            definitions.push_back(std::move(d));
        }
        for(unsigned stage=0;stage<3;++stage) {
            auto key=stage==0?"air.kernel":stage==1?"air.vertex":"air.fragment";unsigned n=LLVMGetNamedMetadataNumOperands(module,key);
            if(n>64)fail("too many AIR stage entries");std::vector<LLVMValueRef> list(n);LLVMGetNamedMetadataOperands(module,key,list.data());
            for(auto md:list) {auto a=node(md);if(a.size()!=3 || !LLVMIsAFunction(a[0]))fail("invalid AIR stage metadata");
                if(selected.empty() || selected.count(name(a[0]))){entries.push_back(a[0]);entryMetadata.push_back(md);}}
        }
        if(entries.empty())fail("selected AIR entry is absent");
        if(definitions.empty())return;
        auto ctors=LLVMGetNamedGlobal(module,"llvm.global_ctors");
        if(ctors) {
            auto initial=LLVMGetInitializer(ctors);auto n=initial?LLVMGetArrayLength(LLVMTypeOf(initial)):0;
            if(n>32)fail("too many static initializers");
            for(unsigned i=0;i<n;++i){auto item=element(initial,i);auto fn=root(element(item,1));
                if(!LLVMIsAFunction(fn))fail("invalid static initializer function");constructors.push_back({number(element(item,0)),fn});}
            std::stable_sort(constructors.begin(),constructors.end(),[](const auto &a,const auto &b){return a.first<b.first;});
        }
        for(const auto &ctor:constructors) {
            std::set<LLVMValueRef> control;
            for(auto b=LLVMGetFirstBasicBlock(ctor.second);b;b=LLVMGetNextBasicBlock(b))for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i)) {
                auto op=LLVMGetInstructionOpcode(i);
                if(((unsigned(op)==2 || op==LLVMUncondBr || op==LLVMCondBr) && LLVMGetNumSuccessors(i)==2) || op==LLVMSwitch) {
                    std::set<LLVMValueRef> seen;globals(LLVMGetOperand(i,0),control,seen);
                }
            }
            for(auto b=LLVMGetFirstBasicBlock(ctor.second);b;b=LLVMGetNextBasicBlock(b))for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))if(LLVMGetInstructionOpcode(i)==LLVMStore) {
                auto g=storageRoot(LLVMGetOperand(i,1));
                if(!LLVMIsAGlobalVariable(g))fail("static initializer store is not rooted in a global");
                std::set<LLVMValueRef> seen;globals(LLVMGetOperand(i,0),dependencies[g],seen);
                dependencies[g].insert(control.begin(),control.end());
            }
        }
        for(auto fn:entries) {
            std::set<LLVMValueRef> seen;
            for(auto b=LLVMGetFirstBasicBlock(fn);b;b=LLVMGetNextBasicBlock(b))
                for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))globals(i,needed,seen);
        }
        for(auto md:entryMetadata){auto a=node(md);for(unsigned side:{1u,2u})for(auto item:node(a[side]))if(auto pred=field(item,"air.function_constant")) {
            auto p=node(pred);if(p.size()!=3)fail("invalid conditional interface predicate");needed.insert(root(p[0]));}}
        // Native AIR can select a binding through a constant-expression
        // global. Its initializer dependencies are part of public reflection,
        // even if the selected kernel never loads that global itself.
        for(auto md:entryMetadata){auto a=node(md);for(unsigned side:{1u,2u})for(auto item:node(a[side]))
            if(auto location=field(item,"air.location_index")) {
                auto g=root(location);if(LLVMIsAGlobalVariable(g))needed.insert(g);
            }}
        for(size_t before=0;before!=needed.size();) {before=needed.size();auto copy=needed;for(auto g:copy){auto it=dependencies.find(g);if(it!=dependencies.end())needed.insert(it->second.begin(),it->second.end());}}
    }
    void inspect(std::vector<FunctionConstant> &out)const {
        out.clear();for(const auto &d:definitions)if(needed.count(d.global))out.push_back(d.info);
        std::sort(out.begin(),out.end(),[](const auto &a,const auto &b){return a.index<b.index;});
    }
    bool present()const{return !definitions.empty();}
    void apply(const std::vector<FunctionConstantValue> &input) {
        std::vector<FunctionConstant> declaration;inspect(declaration);
        std::vector<FunctionConstantValue> effective;std::string diagnostic;
        if(!resolveFunctionConstants(declaration,input,effective,diagnostic))fail(diagnostic);
        std::map<uint32_t,const FunctionConstantValue *> indexed;
        for(const auto &v:effective)indexed[v.index]=&v;
        for(const auto &d:definitions) {
            auto byIndex=indexed.find(d.info.index);auto v=byIndex!=indexed.end()?byIndex->second:nullptr;bool valid=v!=nullptr;
            LLVMSetInitializer(d.global,valid?literal(*v,LLVMGlobalGetValueType(d.global)):LLVMGetUndef(LLVMGlobalGetValueType(d.global)));
            LLVMSetExternallyInitialized(d.global,false);LLVMSetGlobalConstant(d.global,true);if(valid)provided.insert(d.global);
        }
        for(const auto &ctor:constructors)run(ctor.second);
        for(const auto &item:dependencies)if(LLVMIsAGlobalVariable(item.first))LLVMSetGlobalConstant(item.first,true);
        if(auto ctors=LLVMGetNamedGlobal(module,"llvm.global_ctors"))LLVMDeleteGlobal(ctors);
        for(const auto &ctor:constructors)if(!LLVMGetFirstUse(ctor.second))LLVMDeleteFunction(ctor.second);
        // Expose AIR predicate queries and boolean reductions as ordinary LLVM
        // values before propagation. Inactive color-program paths otherwise
        // remain opaque calls even when every predicate lane is constant.
        for(auto fn:reachableFunctions())for(auto b=LLVMGetFirstBasicBlock(fn);b;b=LLVMGetNextBasicBlock(b))
            for(auto i=LLVMGetFirstInstruction(b);i;) {auto next=LLVMGetNextInstruction(i);
                if(LLVMGetInstructionOpcode(i)==LLVMCall && LLVMIsAFunction(LLVMGetCalledValue(i))) {
                    auto n=name(LLVMGetCalledValue(i));LLVMValueRef replacement=nullptr;
                    if(n=="air.is_function_constant_defined")replacement=intrinsic(i);
                    for(unsigned lanes=2;lanes<=4;++lanes)for(bool all:{false,true})if(n==std::string(all?"air.all.v":"air.any.v")+std::to_string(lanes)+"i1") {
                        auto result=LLVMTypeOf(i);
                        if(LLVMGetNumArgOperands(i)!=1 || LLVMGetTypeKind(result)!=LLVMIntegerTypeKind || LLVMGetIntTypeWidth(result)!=1)
                            fail("invalid boolean reduction signature");
                        auto argument=LLVMGetOperand(i,0);auto type=LLVMTypeOf(argument);
                        if(LLVMGetTypeKind(type)!=LLVMVectorTypeKind || LLVMGetVectorSize(type)!=lanes || LLVMGetElementType(type)!=result)
                            fail("invalid boolean reduction argument");
                        LLVMPositionBuilderBefore(builder,i);replacement=LLVMConstInt(result,all,false);
                        for(unsigned lane=0;lane<lanes;++lane) {
                            auto value=LLVMBuildExtractElement(builder,argument,LLVMConstInt(LLVMInt32TypeInContext(context),lane,false),"");
                            replacement=all?LLVMBuildAnd(builder,replacement,value,""):LLVMBuildOr(builder,replacement,value,"");
                        }
                    }
                    if(replacement){LLVMReplaceAllUsesWith(i,replacement);LLVMInstructionEraseFromParent(i);}
                }i=next;}
        // An absent output must not keep its calculations or resource reads
        // alive merely because the unspecialized LLVM return aggregate has it.
        for(size_t j=0;j<entries.size();++j) {
            auto outputs=node(node(entryMetadata[j])[1]);auto type=LLVMGetReturnType(LLVMGlobalGetValueType(entries[j]));
            if(LLVMGetTypeKind(type)!=LLVMStructTypeKind)continue;
            if(outputs.size()!=LLVMCountStructElementTypes(type))fail("conditional output/return shape mismatch");
            for(auto b=LLVMGetFirstBasicBlock(entries[j]);b;b=LLVMGetNextBasicBlock(b))for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))if(LLVMGetInstructionOpcode(i)==LLVMRet && LLVMGetNumOperands(i)) {
                auto result=LLVMGetOperand(i,0);LLVMPositionBuilderBefore(builder,i);
                for(unsigned k=0;k<outputs.size();++k)if(!enabled(outputs[k]))result=LLVMBuildInsertValue(builder,result,LLVMGetUndef(LLVMStructGetTypeAtIndex(type,k)),k,"");
                LLVMSetOperand(i,0,result);
            }
        }
        // Native AIR leaves undef/NaN sentinels and noundef helper calls in
        // inactive specialization paths. Propagate the predicates and erase
        // dead computations before InstCombine can turn those calls into
        // poison that contaminates the otherwise live return/control flow.
        // Prune newly unreferenced helpers after predicate propagation, before
        // spending InstCombine/SimplifyCFG work on every dead dispatcher arm.
        passes("function(sccp,adce)");
        passes("function(instcombine,simplifycfg,adce)");
    }
    void lowerHelpers() {
        auto functions=reachableFunctions();if(functions.size()==entries.size())return;
        const auto always=LLVMGetEnumAttributeKindForName("alwaysinline",12);
        const auto never=LLVMGetEnumAttributeKindForName("noinline",8);
        const auto optnone=LLVMGetEnumAttributeKindForName("optnone",7);
        // A540 import has no device-call ABI. Treat source inlining attributes
        // as hints, then inline only the selected entries' live call sites.
        // Do not eagerly expand every branch of QuartzCore's color dispatcher.
        for(auto fn=LLVMGetFirstFunction(module);fn;fn=LLVMGetNextFunction(fn)) {
            LLVMRemoveEnumAttributeAtIndex(fn,LLVMAttributeFunctionIndex,always);
            for(auto b=LLVMGetFirstBasicBlock(fn);b;b=LLVMGetNextBasicBlock(b))
                for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))if(LLVMGetInstructionOpcode(i)==LLVMCall)
                    LLVMRemoveCallSiteEnumAttribute(i,LLVMAttributeFunctionIndex,always);
        }
        for(unsigned round=0;round<=16;++round) {
            std::vector<LLVMValueRef> calls;unsigned expansion=0;
            auto count=[&](LLVMValueRef fn) {
                for(auto b=LLVMGetFirstBasicBlock(fn);b;b=LLVMGetNextBasicBlock(b))
                    for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))
                        if(++expansion>16384)fail("helper inlining instruction budget exceeded");
            };
            for(auto fn:entries) {
                count(fn);
                for(auto b=LLVMGetFirstBasicBlock(fn);b;b=LLVMGetNextBasicBlock(b))
                    for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))if(LLVMGetInstructionOpcode(i)==LLVMCall) {
                        auto callee=root(LLVMGetCalledValue(i));
                        if(!LLVMIsAFunction(callee))fail("indirect helper calls are unsupported");
                        if(!LLVMCountBasicBlocks(callee))continue;
                        if(std::find(entries.begin(),entries.end(),callee)!=entries.end())fail("recursive/stage helper calls are unsupported");
                        count(callee);calls.push_back(i);
                    }
            }
            if(calls.empty())return;
            if(round==16)fail("helper inlining depth exceeded or call cannot be inlined");
            for(auto call:calls) {
                auto callee=root(LLVMGetCalledValue(call));
                LLVMRemoveEnumAttributeAtIndex(callee,LLVMAttributeFunctionIndex,never);
                LLVMRemoveEnumAttributeAtIndex(callee,LLVMAttributeFunctionIndex,optnone);
                LLVMRemoveCallSiteEnumAttribute(call,LLVMAttributeFunctionIndex,never);
                LLVMAddCallSiteAttribute(call,LLVMAttributeFunctionIndex,LLVMCreateEnumAttribute(context,always,0));
            }
            // Constant propagation between rounds eliminates inactive helper
            // calls and promotes local parameter cursors before the next copy.
            passes("always-inline,function(sroa,sccp,adce)");
            passes("function(instcombine,simplifycfg,adce)");
        }
    }
};
} } }
