// SPDX-License-Identifier: BSD-2-Clause
// Runtime AIR frontend uses LLVM's exported C ABI. It does not link against
// Apple's private C++ class layouts. The host AOT reader remains independent.
#include "Compiler.h"
#include "Semantic.h"
#include "AIRControlFlow.h"
#include "AIRSpecialization.h"
#include <memory>
#include <mutex>
#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <map>
#include <set>
#include <stdexcept>
namespace GPU { namespace Compiler {
namespace {
[[noreturn]] void fail(const std::string &s) { throw std::runtime_error("AIR JIT: "+s); }
std::string name(LLVMValueRef v) { size_t n=0;const char *s=LLVMGetValueName2(v,&n);return {s,n}; }
std::vector<LLVMValueRef> node(LLVMValueRef v) {
    if (!v || !LLVMIsAMDNode(v)) fail("expected metadata node");
    std::vector<LLVMValueRef> out(LLVMGetMDNodeNumOperands(v));LLVMGetMDNodeOperands(v,out.data());return out;
}
std::string text(LLVMValueRef v) { unsigned n=0;const char *s=v?LLVMGetMDString(v,&n):nullptr;return s?std::string(s,n):std::string(); }
uint64_t number(LLVMValueRef v) {
    if(v && LLVMIsAGlobalVariable(AIRSpecialization::root(v)))return AIRSpecialization::interfaceNumber(v);
    if (!v || !LLVMIsAConstantInt(v)) fail("expected metadata/constant integer");
    auto t=LLVMTypeOf(v);if (LLVMGetTypeKind(t)==LLVMVectorTypeKind) t=LLVMGetElementType(t);
    if (LLVMGetIntTypeWidth(t)>64) fail("integer wider than 64 bits");
    return LLVMConstIntGetZExtValue(v);
}
bool has(LLVMValueRef n,const std::string &key) { for (auto v:node(n)) if (text(v)==key) return true;return false; }
LLVMValueRef field(LLVMValueRef n,const std::string &key) {
    auto a=node(n);for (size_t i=0;i+1<a.size();++i) if (text(a[i])==key) return a[i+1];return nullptr;
}
unsigned lanes(LLVMTypeRef t) { return LLVMGetTypeKind(t)==LLVMVectorTypeKind?LLVMGetVectorSize(t):1; }
LLVMTypeRef scalar(LLVMTypeRef t) { return LLVMGetTypeKind(t)==LLVMVectorTypeKind?LLVMGetElementType(t):t; }
bool integer(LLVMTypeRef t,unsigned bits) { return LLVMGetTypeKind(t)==LLVMIntegerTypeKind && LLVMGetIntTypeWidth(t)==bits; }
bool half(LLVMTypeRef t) {return LLVMGetTypeKind(scalar(t))==LLVMHalfTypeKind;}
bool floating(LLVMTypeRef t) {return half(t) || LLVMGetTypeKind(scalar(t))==LLVMFloatTypeKind;}
bool containsHalf(LLVMTypeRef t) {
    if(half(t))return true;
    if(LLVMGetTypeKind(t)==LLVMStructTypeKind)for(unsigned n=0;n<LLVMCountStructElementTypes(t);++n)
        if(containsHalf(LLVMStructGetTypeAtIndex(t,n)))return true;
    return false;
}
bool branch(LLVMValueRef i) {
    // iOS LLVM 18 returns the former LLVMBr=2. LLVM 23 split this C enum
    // into LLVMUncondBr=70 / LLVMCondBr=71; the successor C APIs are shared.
    auto op=LLVMGetInstructionOpcode(i);
    return unsigned(op)==2 || op==LLVMUncondBr || op==LLVMCondBr;
}
LLVMValueRef switchCase(LLVMValueRef i,unsigned successor) {
    return AIRSpecialization::switchCase(i,successor);
}
std::string type(LLVMTypeRef t,bool sign=false) {
    auto s=scalar(t);unsigned n=lanes(t);if (n>4 || !n) fail("unsupported vector width");
    std::string a,b;
    if (floating(t)) return airFloatType(half(t),n);
    else if (integer(s,32) || integer(s,8) || integer(s,16)) { a=sign?"int":"uint";b=sign?"ivec":"uvec"; }
    else if (integer(s,1)) { a="bool";b="bvec"; }
    else fail("only float16/float32, int32 and bool arithmetic is supported");
    return n==1?a:b+std::to_string(n);
}
struct Value {
    enum Kind { Data,Pointer,PrivatePointer,PrivateArray,Texture,Sampler,ReadSampler,Aggregate,WideIndex,Unavailable } kind=Data;
    std::string text;LLVMValueRef root=nullptr;unsigned linear=0,index=0;
    std::vector<Value> members;
};
struct Binding { unsigned slot,stride;bool readonly;LLVMTypeRef type;unsigned sharedWords=0,dynamicSlot=UINT32_MAX; };
class Lower {
    LLVMValueRef f;LLVMTargetDataRef layout;Entry e;unsigned stage,serial=0;
    std::string header,body,framebufferInputs;bool usesDiscard=false;std::map<LLVMValueRef,Value> values;std::map<LLVMValueRef,Binding> bindings;
    std::map<LLVMValueRef,std::pair<std::string,LLVMTypeRef>> constantTables;
    std::map<LLVMValueRef,std::string> privateArrays;
    unsigned privateWords=0;
    std::map<LLVMValueRef,AIRTextureBinding> imageBindings;
    std::map<LLVMValueRef,unsigned> samplerBindings;std::set<unsigned> samplePairs;
    std::vector<QGPUConstantSampler> constantSamplers;
    bool imageIncluded=false,samplerIncluded=false;
    bool usesHalf=false;std::set<unsigned> halfBuffers;
    std::set<unsigned> graphicsHeaders,cubeHeaders,fetchSlots;
    bool controlFlow=false;
    unsigned sharedSerial=0,sharedBytes=0;
    bool dynamicIncluded=false;
    std::string globals;
    std::map<LLVMValueRef,Value> storage;
    std::set<LLVMValueRef> resolving;
    std::set<std::string> atomicHelpers;
    bool system=false,instance=false;
    std::vector<std::string> outputTargets;
    std::string varying(LLVMValueRef metadata,LLVMTypeRef t,bool output) {
        auto kind=text(field(metadata,"air.arg_type_name"));unsigned width=lanes(t);
        if (!floating(t) || width>4 || !width || kind!=airFloatName(half(t),width)) fail("stage varying must be float16/float32 scalar/vector");
        std::string identity;
        if (!airSemantic(text(field(metadata,output?"air.vertex_output":"air.fragment_input")),identity)) fail("unsupported AIR varying semantic");
        if(half(t))identity+=":half"; // logical precision participates in linking
        unsigned index=e.reflection.varyingCount;if (index>=QGPU_MAX_VARYINGS) fail("too many varyings");
        auto &v=e.reflection.varyings[index];semantic(identity,v.semantic);v.components=width;
        if (!airInterpolation(has(metadata,"air.flat"),has(metadata,"air.no_perspective"),has(metadata,"air.perspective"),
            has(metadata,"air.center"),has(metadata,"air.centroid"),has(metadata,"air.sample"),v.mode)) fail("conflicting interpolation qualifiers");
        for (unsigned i=0;i<index;++i) if (Shader::sameSemantic(v,e.reflection.varyings[i])) fail("duplicate varying semantic");
        std::string name="_qvarying"+std::to_string(index);++e.reflection.varyingCount;
        header+="layout(location="+std::to_string(index)+") "+interpolationQualifier(v.mode)+
            (output?"out ":"in ")+airFloatType(false,width)+" "+name+";\n";return output || !half(t)?name:type(t)+"("+name+")";
    }
    Value data(std::string s) { Value v;v.text=std::move(s);return v; }
    Value zero(LLVMTypeRef t) {
        Value v;if (LLVMGetTypeKind(t)==LLVMStructTypeKind) {
            v.kind=Value::Aggregate;for (unsigned i=0;i<LLVMCountStructElementTypes(t);++i) v.members.push_back(zero(LLVMStructGetTypeAtIndex(t,i)));
        } else v.text=type(t)+"(0)";return v;
    }
    Value get(LLVMValueRef v) {
        if (values.count(v)) return values.at(v);
        if (LLVMIsAAllocaInst(v)) {
            auto count=LLVMGetOperand(v,0);
            if (LLVMGetInstructionParent(v)!=LLVMGetFirstBasicBlock(f) || !LLVMIsAConstantInt(count) || number(count)!=1 || LLVMGetPointerAddressSpace(LLVMTypeOf(v))!=0)
                fail("thread-local allocations require a fixed entry-block object");
            auto allocated=LLVMGetAllocatedType(v);
            if(!integer(allocated,32) && LLVMGetTypeKind(allocated)!=LLVMFloatTypeKind) {
                auto leaf=allocated;uint64_t elements=1;unsigned depth=0;
                while(LLVMGetTypeKind(leaf)==LLVMArrayTypeKind) {
                    // The pinned iOS LLVM exports only the older 32-bit API.
                    // Check its exact textual count before trusting a possibly
                    // truncated length or asking DataLayout for its size.
                    auto count=LLVMGetArrayLength(leaf);auto spelling=LLVMPrintTypeToString(leaf);
                    auto prefix="["+std::to_string(count)+" x ";
                    bool exact=spelling && !std::strncmp(spelling,prefix.c_str(),prefix.size());
                    if(spelling)LLVMDisposeMessage(spelling);
                    if(++depth>8 || !exact || !count || count>256/elements)fail("thread-local array shape exceeds the bounded profile");
                    elements*=count;leaf=LLVMGetElementType(leaf);
                }
                auto kind=LLVMGetTypeKind(leaf);
                if((kind!=LLVMIntegerTypeKind && kind!=LLVMFloatTypeKind && kind!=LLVMVectorTypeKind) ||
                    (!integer(scalar(leaf),32) && LLVMGetTypeKind(scalar(leaf))!=LLVMFloatTypeKind) || lanes(leaf)>4)
                    fail("thread-local array elements require fixed int32/float32 scalar/vector values");
                auto bytes=LLVMABISizeOfType(layout,allocated);
                if((!integer(scalar(leaf),32) && LLVMGetTypeKind(scalar(leaf))!=LLVMFloatTypeKind) || lanes(leaf)>4 ||
                    !bytes || (bytes&3) || bytes>1024 || privateWords>256-bytes/4)fail("thread-local array exceeds the bounded int32/float32 profile");
                privateWords+=unsigned(bytes/4);auto array="_qprivate_array"+std::to_string(serial++);
                header+=privateArrayHeader(array,unsigned(bytes/4));privateArrays[v]=array;
                Value p;p.kind=Value::PrivateArray;p.root=v;p.text="uvec2(0u)";values[v]=p;return p;
            }
            Value p;p.kind=Value::PrivatePointer;p.root=v;p.text="_qprivate"+std::to_string(serial++);values[v]=p;
            (controlFlow?globals:body)+=type(LLVMGetAllocatedType(v))+" "+p.text+";\n";return p;
        }
        if(LLVMIsAConstantExpr(v)) {
            if(LLVMGetConstOpcode(v)!=LLVMBitCast || LLVMGetTypeKind(LLVMTypeOf(v))!=LLVMPointerTypeKind ||
                LLVMGetPointerAddressSpace(LLVMTypeOf(v))!=2)fail("unsupported constant resource expression");
            auto value=get(LLVMGetOperand(v,0));if(value.kind!=Value::Sampler)fail("constant resource cast requires a sampler");return value;
        }
        if (LLVMIsAGlobalVariable(v)) {
            if(LLVMGetPointerAddressSpace(LLVMTypeOf(v))==2 && LLVMIsGlobalConstant(v) && LLVMGetInitializer(v)) {
                auto initial=LLVMGetInitializer(v);auto t=LLVMTypeOf(initial);
                if(LLVMGetTypeKind(t)==LLVMArrayTypeKind && floating(LLVMGetElementType(t)) && lanes(LLVMGetElementType(t))<=4) {
                    auto count=LLVMGetArrayLength(t);if(!count || count>256)fail("constant table size exceeds AIR profile");
                    auto element=LLVMGetElementType(t);std::string table="_qconst"+std::to_string(serial++),init;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                    for(unsigned i=0;i<count;++i)init+=(i+1==count?"default:":"case "+std::to_string(i)+"u:")+"return "+(LLVMIsAConstantAggregateZero(initial)?zero(element).text:
                        get(LLVMIsAConstantDataArray(initial)?LLVMGetElementAsConstant(initial,i):LLVMGetOperand(initial,i)).text)+";";
#pragma clang diagnostic pop
                    header+=type(element)+" "+table+"(uint index){switch(index){"+init+"}}\n";
                    constantTables[v]={table,element};Value p;p.kind=Value::Pointer;p.root=v;p.text=addressConstant(0);values[v]=p;return p;
                }
                uint64_t words[2]={};
                if(integer(t,64) && LLVMIsAConstantInt(initial))words[0]=number(initial);
                else {
                    if(LLVMGetTypeKind(t)!=LLVMArrayTypeKind || LLVMGetArrayLength(t)!=2 || !integer(LLVMGetElementType(t),64))fail("invalid AIR sampler constant shape");
                    for(unsigned i=0;i<2;++i) {
                        if(LLVMIsAConstantAggregateZero(initial))continue;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                        auto word=LLVMIsAConstantDataArray(initial)?LLVMGetElementAsConstant(initial,i):LLVMGetOperand(initial,i);
#pragma clang diagnostic pop
                        if(!LLVMIsAConstantInt(word))fail("sampler initializer must contain integer words");words[i]=number(word);
                    }
                }
                QGPUConstantSampler state{};if(!airConstantSampler(words[0],words[1],state))fail("unsupported constexpr sampler encoding/state");
                unsigned index=internConstantSampler(constantSamplers,state);if(index==UINT32_MAX)fail("too many constant sampler states");
                Value value;value.kind=Value::Sampler;value.root=v;values[v]=value;samplerBindings[v]=QGPU_STAGE_SAMPLER_COUNT+index;return value;
            }
            auto initial=LLVMGetInitializer(v);
            if (stage || LLVMGetPointerAddressSpace(LLVMTypeOf(v))!=3 || !initial || (!LLVMIsUndef(initial) && !LLVMIsPoison(initial))) fail("only uninitialized compute threadgroup globals are supported");
            uint64_t bytes=LLVMABISizeOfType(layout,LLVMGlobalGetValueType(v));
            if (!bytes || (bytes&3) || bytes>QGPU_MAX_THREADGROUP_BYTES || sharedBytes>QGPU_MAX_THREADGROUP_BYTES-bytes) fail("invalid threadgroup global size");
            sharedBytes+=(unsigned)bytes;Binding b{};b.slot=sharedSerial++;b.sharedWords=(unsigned)bytes/4;bindings[v]=b;
            Value p;p.kind=Value::Pointer;p.root=v;p.text="uvec2(0u)";values[v]=p;
            header+="shared uint _qshared"+std::to_string(b.slot)+"["+std::to_string(b.sharedWords)+"];\n";return p;
        }
        if (controlFlow && LLVMIsAInstruction(v)) {
            auto op=LLVMGetInstructionOpcode(v);
            if ((op==LLVMGetElementPtr || ((op==LLVMZExt || op==LLVMSExt) && integer(LLVMTypeOf(v),64))) && resolving.insert(v).second) {
                // Resolve address aliases without moving data computations
                // or speculating memory accesses across basic blocks.
                instruction(v);resolving.erase(v);
                if (values.count(v)) return values.at(v);
            }
            fail("unresolved or cyclic resource/wide-index value in AIR CFG");
        }
        auto t=LLVMTypeOf(v);
        if (LLVMIsAConstantInt(v)) {
            auto n=number(v);if (n>UINT32_MAX) fail("integer constant outside 32-bit profile");
            std::string s=integer(scalar(t),1)?(n?"true":"false"):std::to_string(n)+"u";
            return data(lanes(t)==1?s:type(t)+"("+s+")");
        }
        if (LLVMIsAConstantFP(v)) {
            if(half(t)) {
                auto element=lanes(t)==1?v:LLVMConstExtractElement(v,LLVMConstInt(LLVMInt32TypeInContext(LLVMGetTypeContext(t)),0,false));
                auto value="float16_t(unpackHalf2x16("+std::to_string(number(LLVMConstBitCast(element,LLVMInt16TypeInContext(LLVMGetTypeContext(t)))))+"u).x)";
                return data(lanes(t)==1?value:type(t)+"("+value+")");
            }
            LLVMBool loses;double n=LLVMConstRealGetDouble(v,&loses);(void)loses;
            if (!std::isfinite(n)) fail("non-finite float constant");char s[64];std::snprintf(s,sizeof(s),"%.9g",n);
            std::string result=s;if (result.find_first_of(".eE")==std::string::npos) result+=".0";
            return data(lanes(t)==1?result:type(t)+"("+result+")");
        }
        if (LLVMIsUndef(v) || LLVMIsAConstantAggregateZero(v) || LLVMIsPoison(v)) return zero(t);
        if (LLVMIsAConstant(v) && LLVMGetTypeKind(t)==LLVMVectorTypeKind) {
            std::string s=type(t)+"(";for (unsigned i=0;i<lanes(t);++i) {
                // This older C entry remains exported by iOS 18 libLLVM.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                LLVMValueRef item=LLVMIsAConstantDataVector(v)?LLVMGetElementAsConstant(v,i):LLVMGetOperand(v,i);
#pragma clang diagnostic pop
                s+=(i?",":"")+get(item).text;
            }return data(s+")");
        }
        if (LLVMIsAConstantStruct(v)) {
            Value out;out.kind=Value::Aggregate;
            for (unsigned i=0;i<LLVMCountStructElementTypes(t);++i) out.members.push_back(get(LLVMGetOperand(v,i)));
            return out;
        }
        fail("unlowered value or unsupported constant");
    }
    std::string expr(LLVMValueRef v) { auto x=get(v);if (x.kind!=Value::Data) fail("resource or wide index escaped its operation");return x.text; }
    void assign(LLVMValueRef i,const std::string &expression,unsigned linear=0) {
        if (controlFlow && !linear) linear=values.at(i).linear;
        Value v=controlFlow?storage.at(i):data("v"+std::to_string(serial++));v.linear=linear;
        if (v.kind!=Value::Data) fail("arithmetic result is not a data value");
        auto t=LLVMTypeOf(i);auto text=expression;
        if(LLVMGetTypeKind(scalar(t))==LLVMIntegerTypeKind && !integer(scalar(t),1))
            text=airIntegerMask(text,LLVMGetIntTypeWidth(scalar(t)),lanes(t));
        body+=(controlFlow?"":type(t)+" ")+v.text+" = "+text+";\n";values[i]=v;
    }
    std::string component(const Value &v,LLVMTypeRef t,unsigned n) {
        if (v.kind!=Value::Data || n>=lanes(t)) fail("invalid vector component");
        return lanes(t)==1?v.text:"("+v.text+")["+std::to_string(n)+"]";
    }
    std::string bitcast(const std::string &s,LLVMTypeRef from,LLVMTypeRef to) {
        if (from==to) return s;
        if(half(from) && integer(scalar(to),32) && lanes(from)==2*lanes(to)) {
            auto result=airHalfBitcast(s,lanes(to),false);if(!result.empty())return result;
        }
        if(half(to) && integer(scalar(from),32) && lanes(to)==2*lanes(from)) {
            auto result=airHalfBitcast(s,lanes(from),true);if(!result.empty())return result;
        }
        if (lanes(from)!=lanes(to)) fail("bitcast changes vector shape");
        if (LLVMGetTypeKind(scalar(from))==LLVMFloatTypeKind && integer(scalar(to),32)) return "floatBitsToUint("+s+")";
        if (LLVMGetTypeKind(scalar(to))==LLVMFloatTypeKind && integer(scalar(from),32)) return "uintBitsToFloat("+s+")";
        fail("unsupported bitcast");
    }
    std::pair<std::string,LLVMTypeRef> memory(LLVMValueRef pointer,LLVMTypeRef access,bool write) {
        auto p=get(pointer);
        if (p.kind==Value::PrivatePointer) {
            auto stored=LLVMGetAllocatedType(p.root);
            if(access!=stored)fail("thread-local access type disagrees with allocation");return {p.text,stored};
        }
        if(p.kind==Value::PrivateArray) {
            if((!integer(scalar(access),32) && LLVMGetTypeKind(scalar(access))!=LLVMFloatTypeKind) || lanes(access)>4)
                fail("private array access requires int32/float32 scalar/vector values");
            auto raw=LLVMInt32TypeInContext(LLVMGetTypeContext(access));if(lanes(access)>1)raw=LLVMVectorType(raw,lanes(access));
            return {write?p.text:privateArrayLoad(privateArrays.at(p.root),p.text,lanes(access)),raw};
        }
        auto table=constantTables.find(p.root);
        if(p.kind==Value::Pointer && table!=constantTables.end()) {
            auto element=table->second.second;auto stride=LLVMABISizeOfType(layout,element);
            if(write || (access!=element && access!=scalar(element)))fail("invalid constant table access");
            auto out=table->second.first+"(("+p.text+").x/"+std::to_string(stride)+"u)";
            if(access!=element)out+="[(("+p.text+").x%"+std::to_string(stride)+"u)/"+std::to_string(LLVMABISizeOfType(layout,access))+"u]";
            return {out,access};
        }
        if (p.kind!=Value::Pointer || !bindings.count(p.root)) fail("memory is not a declared buffer");
        auto b=bindings.at(p.root);unsigned n=lanes(access);
        if ((!integer(scalar(access),32) && !integer(scalar(access),8) && !integer(scalar(access),16) && !floating(access)) || !n || n>4)
            fail("buffer accesses require float16/float32 or int32 scalar/vector values");
        if (write && (b.readonly || stage==1)) fail("store to readonly or vertex buffer");
        bool dynamic=b.dynamicSlot!=UINT32_MAX;
        if (dynamic && !dynamicIncluded) {header+=dynamicThreadgroupHeader();dynamicIncluded=true;}
        if (!b.sharedWords && !dynamic) {
            e.reflection.bufferMask|=1u<<b.slot;if (write) e.reflection.writeMask|=1u<<b.slot;
            if (p.index==2) e.reflection.instanceMask|=1u<<b.slot;
        }
        if(integer(scalar(access),8) || integer(scalar(access),16)) {
            if(write || b.sharedWords || dynamic)fail("packed integer loads require device or constant buffers");
            return {narrowBufferLoad("_qbuffer"+std::to_string(b.slot)+".data",p.text,LLVMGetIntTypeWidth(scalar(access)),n),access};
        }
        if(half(access)) {
            if(b.sharedWords || dynamic)fail("half threadgroup memory is not implemented");
            auto array="_qhalfbuffer"+std::to_string(b.slot)+".data";
            if(halfBuffers.insert(b.slot).second)header+="layout(std430,binding="+std::to_string(b.slot)+") "+
                (b.readonly?"readonly ":"")+"buffer QHalfBuffer"+std::to_string(b.slot)+" { float16_t data[]; } _qhalfbuffer"+std::to_string(b.slot)+";\n";
            return {write?p.text:halfBufferLoad(array,p.text,n),access};
        }
        auto raw=LLVMInt32TypeInContext(LLVMGetTypeContext(access));if (n>1) raw=LLVMVectorType(raw,n);
        return {write?p.text:wordLoad(dynamic?"_qdynamic":b.sharedWords?"_qshared"+std::to_string(b.slot):"_qbuffer"+std::to_string(b.slot)+".data",p.text,n,b.sharedWords!=0,b.dynamicSlot),raw};
    }
    std::string atomic(LLVMValueRef pointer,const std::string &op,const std::string &value="0u",const std::string &compare="0u",bool sign=false) {
        auto p=get(pointer);if (p.kind!=Value::Pointer) fail("atomic pointer is not device/threadgroup memory");
        auto wordType=LLVMInt32TypeInContext(LLVMGetTypeContext(LLVMTypeOf(pointer)));memory(pointer,wordType,true);
        const auto &b=bindings.at(p.root);bool dynamic=b.dynamicSlot!=UINT32_MAX;
        std::string array=dynamic?"_qdynamic":b.sharedWords?"_qshared"+std::to_string(b.slot):"_qbuffer"+std::to_string(b.slot)+".data";
        std::string word=dynamic?"_q_dynamic_word("+std::to_string(b.dynamicSlot)+"u,"+p.text+",0u)":(b.sharedWords?"_q_shared_word(":"_q_word(")+p.text+",0u)";
        if (sign && (op=="min" || op=="max")) {
            std::string function=(dynamic?std::string("_qdynamic"):(b.sharedWords?"_qshared":"_qbuffer")+std::to_string(b.slot))+"_s"+op;
            if (atomicHelpers.insert(function).second) header+=signedAtomicHelper(function,array,op);
            return function+"("+word+",uint("+value+"))";
        }
        if (dynamic) {
            auto function="_qdynamic_atomic_"+op;
            if (atomicHelpers.insert(function).second) header+=dynamicAtomicHelper(function,op);
            return function+"("+word+",uint("+value+"),uint("+compare+"))";
        }
        return atomicExpression(op,array+"["+word+"]",value,compare);
    }
    void gep(LLVMValueRef i) {
        auto p=get(LLVMGetOperand(i,0));if (p.kind!=Value::Pointer && p.kind!=Value::PrivateArray) fail("GEP base is not a buffer");
        auto t=LLVMGetGEPSourceElementType(i);uint64_t offset=0;std::string address=p.text;
        for (unsigned n=1;n<(unsigned)LLVMGetNumOperands(i);++n) {
            auto index=LLVMGetOperand(i,n);auto k=LLVMGetTypeKind(t);uint64_t bytes;
            if (n>1 && k==LLVMStructTypeKind) {
                auto member=number(index);if (member>=LLVMCountStructElementTypes(t)) fail("invalid struct GEP");
                offset+=LLVMOffsetOfElement(layout,t,(unsigned)member);t=LLVMStructGetTypeAtIndex(t,(unsigned)member);continue;
            }
            if (n>1) { if (k!=LLVMVectorTypeKind && k!=LLVMArrayTypeKind) fail("invalid sequential GEP");t=LLVMGetElementType(t); }
            bytes=LLVMABISizeOfType(layout,t);
            if (LLVMIsAConstantInt(index)) {
                if (LLVMGetIntTypeWidth(LLVMTypeOf(index))>64) fail("wide GEP constant");
                offset+=uint64_t(LLVMConstIntGetSExtValue(index))*bytes;
            } else {
                auto v=get(index);auto it=LLVMTypeOf(index);
                if ((v.kind!=Value::Data && v.kind!=Value::WideIndex) || lanes(it)!=1 || (!integer(it,32) && v.kind!=Value::WideIndex))
                    fail("buffer indices require scalar int32 values or their address extensions");
                if (bytes>UINT32_MAX) fail("oversized buffer element");
                if (v.linear==2) p.index=2;
                auto extended=v.kind==Value::WideIndex?v.text:addressExtend(v.text,true);
                address="_q_addr_add("+address+",_q_addr_mul("+extended+","+std::to_string(bytes)+"u))";
            }
        }
        // The access helper validates its own width/alignment; packed half
        // members need byte GEPs even when the enclosing record is word sized.
        p.text="_q_addr_add("+address+","+addressConstant(offset)+")";values[i]=p;
    }
    void call(LLVMValueRef i) {
        auto fn=LLVMGetCalledValue(i);if (!LLVMIsAFunction(fn) || LLVMCountBasicBlocks(fn)) fail("inline helper/indirect calls before AIR import");
        auto n=name(fn);unsigned count=LLVMGetNumArgOperands(i);
        if (n.rfind("llvm.dbg.",0)==0 || n.rfind("llvm.lifetime.",0)==0) return;
        for(unsigned width=2;width<=4;++width)for(bool all:{false,true})
            if(n==std::string(all?"air.all.v":"air.any.v")+std::to_string(width)+"i1") {
                if(count!=1 || !integer(LLVMTypeOf(i),1))fail("invalid boolean reduction signature");
                auto argument=LLVMTypeOf(LLVMGetOperand(i,0));
                if(LLVMGetTypeKind(argument)!=LLVMVectorTypeKind || lanes(argument)!=width || scalar(argument)!=LLVMTypeOf(i))
                    fail("invalid boolean reduction signature");
                assign(i,std::string(all?"all(":"any(")+expr(LLVMGetOperand(i,0))+")");return;
            }
        if(n=="air.discard_fragment") {
            if(stage!=2 || count || LLVMGetTypeKind(LLVMTypeOf(i))!=LLVMVoidTypeKind)fail("invalid fragment discard intrinsic");
            usesDiscard=true;body+="memoryBarrierBuffer(); demote;\n";return;
        }
        AIRAtomic operation;
        if (airAtomic(n,operation)) {
            unsigned wanted=operation.op=="cas"?7:operation.op=="load"?4:5;
            if (stage==1 || count!=wanted) fail("invalid atomic intrinsic stage/signature");
            unsigned order=operation.op=="cas"?3:operation.op=="load"?1:2;
            auto constant=[&](unsigned n,unsigned bits,uint64_t value) { auto a=LLVMGetOperand(i,n);return integer(LLVMTypeOf(a),bits) && LLVMIsAConstantInt(a) && number(a)==value; };
            if (!constant(order,32,0) || (operation.op=="cas" && !constant(order+1,32,0)) || !constant(count-2,32,operation.local?1:2) ||
                !integer(LLVMTypeOf(LLVMGetOperand(i,count-1)),1) || !LLVMIsAConstantInt(LLVMGetOperand(i,count-1)) ||
                LLVMGetTypeKind(LLVMTypeOf(LLVMGetOperand(i,0)))!=LLVMPointerTypeKind ||
                LLVMGetPointerAddressSpace(LLVMTypeOf(LLVMGetOperand(i,0)))!=(operation.local?3u:1u)) fail("unsupported atomic order/scope/address space");
            if (operation.op=="store" ? LLVMGetTypeKind(LLVMTypeOf(i))!=LLVMVoidTypeKind:!integer(LLVMTypeOf(i),32)) fail("integer atomics require int32 return values");
            if (operation.op!="load" && !integer(LLVMTypeOf(LLVMGetOperand(i,operation.op=="cas"?2:1)),32)) fail("integer atomics require int32 operands");
            if (operation.op=="cas") {
                auto expected=get(LLVMGetOperand(i,1));if (expected.kind!=Value::PrivatePointer || !integer(LLVMGetAllocatedType(expected.root),32)) fail("atomic expected pointer requires a thread-local int32 cell");
                assign(i,atomic(LLVMGetOperand(i,0),"cas",expr(LLVMGetOperand(i,2)),expected.text));body+=expected.text+" = "+expr(i)+";\n";
            } else {
                auto result=atomic(LLVMGetOperand(i,0),operation.op,operation.op=="load"?"0u":expr(LLVMGetOperand(i,1)),"0u",operation.sign);
                if (operation.op=="store") body+=result+";\n";else assign(i,result);
            }
            return;
        }
        if (n=="air.wg.barrier") {
            if (stage || count!=2 || !integer(LLVMTypeOf(LLVMGetOperand(i,0)),32) || !integer(LLVMTypeOf(LLVMGetOperand(i,1)),32) || number(LLVMGetOperand(i,0))>7 || number(LLVMGetOperand(i,1))!=1) fail("unsupported workgroup barrier flags/scope");
            body+=workgroupBarrier((unsigned)number(LLVMGetOperand(i,0)));return;
        }
        if(n=="air.get_read_sampler") {
            auto t=LLVMTypeOf(i);
            if(count || LLVMGetTypeKind(t)!=LLVMPointerTypeKind || LLVMGetPointerAddressSpace(t)!=2)fail("invalid AIR read sampler");
            Value sampler;sampler.kind=Value::ReadSampler;values[i]=sampler;return;
        }
        AIRTextureCall textureCall;
        if(airTextureOperation(n,textureCall)) {
            auto &t=textureCall;
            if(!airTextureLayout(t,count))fail("unsupported texture signature");
            auto arg=[&](unsigned index) {return LLVMGetOperand(i,index);};
            auto isInt=[&](unsigned index,unsigned bits) {return integer(LLVMTypeOf(arg(index)),bits);};
            auto zero=[&](unsigned index) {return LLVMIsAConstant(arg(index)) && LLVMIsNull(arg(index));};
            auto immediate=[&](unsigned index,unsigned bits,uint64_t expected) {return LLVMIsAConstantInt(arg(index)) && isInt(index,bits) && number(arg(index))==expected;};
            auto handle=get(arg(0));
            if(handle.kind!=Value::Texture || !imageBindings.count(handle.root))fail("texture operation requires a bound texture");
            const auto binding=imageBindings.at(handle.root);
            if(binding.shape!=t.shape)fail("texture intrinsic dimension disagrees with metadata");
            bool read=t.op==AIRTextureCall::Read,write=t.op==AIRTextureCall::Write,sample=t.op>=AIRTextureCall::Sample;
            if((read || write || sample) && (binding.half!=t.half || binding.unsignedInteger!=t.unsignedInteger))fail("texture intrinsic precision disagrees with metadata");
            auto texelTypeMatches=[&](LLVMTypeRef valueType) {return lanes(valueType)==4 && (t.unsignedInteger?integer(scalar(valueType),32):(floating(valueType) && half(valueType)==t.half));};
            auto result=[&](const std::string &call) {
                auto typeOf=LLVMTypeOf(i);
                if(LLVMGetTypeKind(typeOf)==LLVMStructTypeKind) {
                    if(LLVMCountStructElementTypes(typeOf)!=2)fail("invalid texture result");
                    auto texelType=LLVMStructGetTypeAtIndex(typeOf,0);
                    if(!texelTypeMatches(texelType) || !integer(LLVMStructGetTypeAtIndex(typeOf,1),8))fail("invalid texture result");
                    auto texel=controlFlow?storage.at(i).members[0]:data("v"+std::to_string(serial++));
                    body+=(controlFlow?"":type(texelType)+" ")+texel.text+" = "+(t.half?"f16vec4("+call+")":call)+";\n";
                    Value out;out.kind=Value::Aggregate;Value residency;residency.kind=Value::Unavailable;out.members={texel,residency};values[i]=out;
                } else {if(!texelTypeMatches(typeOf))fail("texture result requires matching float4/half4/uint4");assign(i,t.half?"f16vec4("+call+")":call);}
            };
            if(!read && !write && !sample) {
                if(!integer(LLVMTypeOf(i),32) || (t.lod!=UINT32_MAX && !isInt(t.lod,32)))fail("invalid texture query type");
                assign(i,airTextureQuery(binding,stage,t.op,t.lod==UINT32_MAX?"0u":expr(arg(t.lod))));return;
            }
            if(t.face!=UINT32_MAX && !isInt(t.face,32))fail("texture face requires int32");
            if(t.layer!=UINT32_MAX && !isInt(t.layer,32))fail("texture layer requires int32");
            if(t.offsetFlag!=UINT32_MAX && (!isInt(t.offsetFlag,1) || !LLVMIsAConstantInt(arg(t.offsetFlag))))fail("texture offset mode requires a constant bool");
            std::string offset;
            if(t.offset!=UINT32_MAX) {
                auto value=arg(t.offset);auto valueType=LLVMTypeOf(value);
                if(LLVMGetTypeKind(valueType)!=LLVMVectorTypeKind || lanes(valueType)!=2 || !integer(scalar(valueType),32))fail("texture offset requires int2");
                if(!zero(t.offset)) {
                    auto context=LLVMGetTypeContext(valueType);
                    auto component=[&](unsigned lane) {return LLVMIsAConstant(value)?LLVMConstExtractElement(value,LLVMConstInt(LLVMInt32TypeInContext(context),lane,false)):nullptr;};
                    auto x=component(0),y=component(1);
                    if(!sample || !x || !y || !LLVMIsAConstantInt(x) || !LLVMIsAConstantInt(y) ||
                        LLVMConstIntGetSExtValue(x)<-8 || LLVMConstIntGetSExtValue(x)>7 || LLVMConstIntGetSExtValue(y)<-8 || LLVMConstIntGetSExtValue(y)>7)
                        fail("sample offsets require constant int2 components in -8..7; read planar offsets remain zero");
                    if(t.offsetFlag==UINT32_MAX || !immediate(t.offsetFlag,1,1))fail("nonzero sample offset requires the offset flag");
                    offset="ivec2("+std::to_string(LLVMConstIntGetSExtValue(x))+","+std::to_string(LLVMConstIntGetSExtValue(y))+")";
                }
            }
            unsigned access=binding.access==4?0:binding.access;
            if(!immediate(t.access,32,access))fail("texture access operand disagrees with metadata");
            if(read || write) {
                auto coord=LLVMTypeOf(arg(t.coord));
                if(lanes(coord)!=2 || !integer(scalar(coord),32) || !isInt(t.lod,32) ||
                    (write?(stage || !(binding.access&2)):(binding.access!=4 && !(binding.access&1))))fail("unsupported texture coordinates/access/stage");
                if(t.currentRead && get(arg(1)).kind!=Value::ReadSampler)fail("read requires air.get_read_sampler");
                if(read && (stage || binding.access==4) && fetchSlots.insert(binding.slot).second)
                    header+=stage?graphicsFetchFunctions(binding.slot,binding.shape,stage):binding.shape&2?cubeImageFunctions(binding.slot,1,binding.shape&1):imageFunctions(binding.slot,1,binding.shape&1);
                auto call=airTextureFunction(binding,stage)+(read?"_read(":"_write(");
                if(write) {auto valueType=LLVMTypeOf(arg(t.value));if(!texelTypeMatches(valueType) || LLVMGetTypeKind(LLVMTypeOf(i))!=LLVMVoidTypeKind)fail("texture write requires matching float4/half4/uint4 and void");call+=(t.half?"vec4("+expr(arg(t.value))+")":expr(arg(t.value)))+",";}
                call+="uvec2("+expr(arg(t.coord))+"),";
                if(t.face!=UINT32_MAX)call+=expr(arg(t.face))+",";
                if(t.layer!=UINT32_MAX)call+=expr(arg(t.layer))+",";
                call+=expr(arg(t.lod))+")";
                if(write)body+=call+";\n";else result(call);return;
            }
            bool gradient=t.op==AIRTextureCall::Gradient,cube=binding.shape&2;
            auto sampler=get(arg(1));auto coordinateType=LLVMTypeOf(arg(t.coord));
            if(binding.access!=4 || sampler.kind!=Value::Sampler || !samplerBindings.count(sampler.root) ||
                lanes(coordinateType)!=(cube?3u:2u) || LLVMGetTypeKind(scalar(coordinateType))!=LLVMFloatTypeKind ||
                LLVMGetTypeKind(LLVMTypeOf(arg(t.minLod)))!=LLVMFloatTypeKind || !zero(t.minLod))fail("unsupported texture sampling operands");
            unsigned samplerSlot=samplerBindings.at(sampler.root);bool level=false;
            const auto *literal=samplerSlot>=QGPU_STAGE_SAMPLER_COUNT?&constantSamplers.at(samplerSlot-QGPU_STAGE_SAMPLER_COUNT):nullptr;
            if(cube && literal && !literal->normalized)fail("cube sampling requires normalized coordinates");
            if(gradient) {
                if(LLVMTypeOf(arg(t.dx))!=coordinateType || LLVMTypeOf(arg(t.dy))!=coordinateType)fail("invalid texture gradients");
            } else {
                if(!isInt(t.levelFlag,1) || !LLVMIsAConstantInt(arg(t.levelFlag)) || LLVMGetTypeKind(LLVMTypeOf(arg(t.lod)))!=LLVMFloatTypeKind)fail("invalid texture LOD mode/value");
                level=number(arg(t.levelFlag));
                if(stage!=2 && !level && !zero(t.lod))fail("nonfragment implicit sample requires zero bias");
            }
            if(samplePairs.insert(binding.slot*32+samplerSlot+(level?256:0)).second)
                header+=sampleDeclaration(binding.slot,samplerSlot,level,binding.shape&1,stage,cube,literal);
            result(airTextureSample(binding,samplerSlot,stage,gradient,level,expr(arg(t.coord)),
                t.layer==UINT32_MAX?"":expr(arg(t.layer)),gradient?"":stage!=2 && !level?"0.0":expr(arg(t.lod)),
                gradient?expr(arg(t.dx)):"",gradient?expr(arg(t.dy)):"",literal,offset));return;
        }
        if(n=="air.pack.unorm4x8.v4f16" || n=="air.pack.unorm4x8.v4f32") {
            bool useHalf=n=="air.pack.unorm4x8.v4f16";auto input=count?LLVMTypeOf(LLVMGetOperand(i,0)):LLVMTypeOf(i);
            if(count!=1 || !integer(LLVMTypeOf(i),32) || !floating(input) || half(input)!=useHalf || lanes(input)!=4)fail("invalid unorm4x8 pack signature");
            assign(i,"packUnorm4x8(vec4("+expr(LLVMGetOperand(i,0))+"))");return;
        }
        if(n.rfind("air.convert.",0)==0) {
            std::vector<std::string> bits;size_t start=0;
            for(size_t end;(end=n.find('.',start))!=std::string::npos;start=end+1)bits.push_back(n.substr(start,end-start));
            bits.push_back(n.substr(start));
            if(bits.size()!=6 || count!=1 || lanes(LLVMTypeOf(i))!=lanes(LLVMTypeOf(LLVMGetOperand(i,0))) ||
                lanes(LLVMTypeOf(i))>4 || (bits[2]!="f" && bits[2]!="s" && bits[2]!="u") ||
                (bits[4]!="f" && bits[4]!="s" && bits[4]!="u"))fail("unsupported AIR conversion");
            auto input=LLVMGetOperand(i,0);auto target=LLVMTypeOf(i);
            bool narrow=LLVMIsAConstantInt(input) && (integer(LLVMTypeOf(input),8) || integer(LLVMTypeOf(input),16));
            auto spelling=[&](LLVMTypeRef t,bool isFloat,bool allowNarrow) {
                if(isFloat?!floating(t):!(integer(scalar(t),32) || integer(scalar(t),1) || allowNarrow))fail("AIR conversion type disagrees with its intrinsic name");
                return isFloat?airFloatSuffix(half(t),lanes(t)):(lanes(t)==1?std::string():"v"+std::to_string(lanes(t)))+"i"+std::to_string(LLVMGetIntTypeWidth(scalar(t)));
            };
            if(bits[3]!=spelling(target,bits[2]=="f",false) || bits[5]!=spelling(LLVMTypeOf(input),bits[4]=="f",narrow))fail("AIR conversion mangling disagrees with its types");
            std::string value=expr(input);
            if(bits[4]=="s")value=narrow?std::to_string(LLVMConstIntGetSExtValue(input)):type(LLVMTypeOf(input),true)+"("+value+")";
            value=type(target,bits[2]=="s")+"("+value+")";
            if(bits[2]=="s")value=type(target)+"("+value+")";
            assign(i,value);return;
        }
        if(n=="air.fast_sincos.f32" || n=="air.sincos.f32") {
            auto t=LLVMTypeOf(i);if(count!=2 || LLVMGetTypeKind(t)!=LLVMFloatTypeKind)fail("invalid sincos signature");
            auto input=LLVMGetOperand(i,0),output=LLVMGetOperand(i,1);
            if(LLVMTypeOf(input)!=t ||
                get(output).kind!=Value::PrivatePointer)fail("invalid sincos signature");
            auto target=memory(output,t,true);body+=target.first+" = cos("+expr(input)+");\n";
            assign(i,"sin("+expr(input)+")");return;
        }
        std::string extraMath;auto resultType=LLVMTypeOf(i);
        auto extraCount=airExtraMath(n,airFloatSuffix(half(resultType),lanes(resultType)),extraMath);
        if(extraCount) {
            if(count!=extraCount || !floating(resultType) || lanes(resultType)>4)fail("invalid math intrinsic signature");
            std::vector<std::string> args;
            for(unsigned index=0;index<count;++index) {
                auto argument=LLVMGetOperand(i,index);
                if(LLVMTypeOf(argument)!=resultType)fail("math intrinsic operand/result types disagree");
                args.push_back(expr(argument));
            }
            auto attribute=LLVMGetStringAttributeAtIndex(f,LLVMAttributeFunctionIndex,"no-nans-fp-math",sizeof("no-nans-fp-math")-1);
            unsigned length=0;auto value=attribute?LLVMGetStringAttributeValue(attribute,&length):nullptr;
            const bool noNaNs=n.rfind("air.fast_",0)==0 || (value && std::string(value,length)=="true");
            assign(i,airExtraMathExpression(extraMath,args,half(resultType),lanes(resultType),noNaNs));return;
        }
        std::string math;unsigned argc=1;
        if(n=="air.saturate."+airFloatSuffix(half(LLVMTypeOf(i)),lanes(LLVMTypeOf(i))) ||
            n=="air.fast_saturate."+airFloatSuffix(half(LLVMTypeOf(i)),lanes(LLVMTypeOf(i)))) {
            if(count!=1 || !floating(LLVMTypeOf(i)) || LLVMTypeOf(LLVMGetOperand(i,0))!=LLVMTypeOf(i))fail("invalid saturate signature");
            auto t=type(LLVMTypeOf(i));assign(i,"clamp("+expr(LLVMGetOperand(i,0))+","+t+"(0),"+t+"(1))");return;
        }
        for(const auto &p:std::map<std::string,std::string>{{"dfdx","dFdx"},{"dfdy","dFdy"},{"fwidth","fwidth"}})
            if(n=="air."+p.first+"."+airFloatSuffix(half(LLVMTypeOf(i)),lanes(LLVMTypeOf(i)))) {
                if(stage!=2 || count!=1 || !floating(LLVMTypeOf(i)) || LLVMTypeOf(LLVMGetOperand(i,0))!=LLVMTypeOf(i))fail("invalid fragment derivative signature");
                auto value=expr(LLVMGetOperand(i,0));if(half(LLVMTypeOf(i)))value=airFloatType(false,lanes(LLVMTypeOf(i)))+"("+value+")";
                value=p.second+"("+value+")";assign(i,half(LLVMTypeOf(i))?type(LLVMTypeOf(i))+"("+value+")":value);return;
            }
        const std::map<std::string,std::string> unary={{"sqrt","sqrt"},{"rsqrt","inversesqrt"},{"fabs","abs"},{"floor","floor"},{"ceil","ceil"},{"trunc","trunc"},{"fract","fract"},{"sin","sin"},{"cos","cos"},{"exp2","exp2"},{"log2","log2"},{"normalize","normalize"}};
        auto suffix=[](LLVMTypeRef t) { return airFloatSuffix(half(t),lanes(t)); };
        for (auto &p:unary) for (const char *prefix:{"air.","air.fast_","llvm."}) if (n==prefix+p.first+"."+suffix(LLVMTypeOf(i))) math=p.second;
        for (auto &p:std::map<std::string,std::string>{{"fmin","min"},{"fmax","max"},{"pow","pow"},{"dot","dot"},{"fma","fma"},{"mix","mix"},{"clamp","clamp"}}) {
            auto argType=count?LLVMTypeOf(LLVMGetOperand(i,0)):LLVMTypeOf(i);
            if (n=="air."+p.first+"."+suffix(argType) || n=="air.fast_"+p.first+"."+suffix(argType) || (p.first=="fma" && n=="llvm.fma."+suffix(argType))) {
                math=p.second;argc=(p.first=="fma" || p.first=="mix" || p.first=="clamp")?3:2;
            }
        }
        if (math.empty() || count!=argc || !floating(LLVMTypeOf(i))) fail("unsupported intrinsic: "+n);
        std::string s=math+"(";for (unsigned j=0;j<count;++j) { auto t=LLVMTypeOf(LLVMGetOperand(i,j));if (!floating(t) || half(t)!=half(LLVMTypeOf(i))) fail("invalid math argument");s+=(j?",":"")+expr(LLVMGetOperand(i,j)); }
        assign(i,s+")");
    }
    void instruction(LLVMValueRef i) {
        auto op=LLVMGetInstructionOpcode(i);auto t=LLVMTypeOf(i);
        auto arg=[&](unsigned n) { return LLVMGetOperand(i,n); };
        if (op==LLVMAlloca) { get(i);return; }
        if (op==LLVMAtomicRMW) {
            if (!integer(t,32) || LLVMGetOrdering(i)!=LLVMAtomicOrderingMonotonic) fail("only relaxed int32 atomicrmw is supported");
            const std::map<LLVMAtomicRMWBinOp,std::pair<std::string,bool>> ops={
                {LLVMAtomicRMWBinOpXchg,{"xchg",false}},{LLVMAtomicRMWBinOpAdd,{"add",false}},{LLVMAtomicRMWBinOpSub,{"sub",false}},
                {LLVMAtomicRMWBinOpAnd,{"and",false}},{LLVMAtomicRMWBinOpOr,{"or",false}},{LLVMAtomicRMWBinOpXor,{"xor",false}},
                {LLVMAtomicRMWBinOpMin,{"min",true}},{LLVMAtomicRMWBinOpMax,{"max",true}},{LLVMAtomicRMWBinOpUMin,{"min",false}},{LLVMAtomicRMWBinOpUMax,{"max",false}}};
            auto operation=ops.find(LLVMGetAtomicRMWBinOp(i));if (operation==ops.end()) fail("unsupported atomicrmw operation");
            assign(i,atomic(arg(0),operation->second.first,expr(arg(1)),"0u",operation->second.second));return;
        }
        if (op==LLVMAtomicCmpXchg) {
            if (!integer(LLVMTypeOf(arg(1)),32) || LLVMGetCmpXchgSuccessOrdering(i)!=LLVMAtomicOrderingMonotonic ||
                LLVMGetCmpXchgFailureOrdering(i)!=LLVMAtomicOrderingMonotonic) fail("only relaxed int32 cmpxchg is supported");
            auto wanted=expr(arg(1)),old="_qold"+std::to_string(serial++);
            body+="uint "+old+" = "+atomic(arg(0),"cas",expr(arg(2)),wanted)+";\n";
            Value result;result.kind=Value::Aggregate;result.members={data(old),data("("+old+"=="+wanted+")")};values[i]=result;return;
        }
        if (op==LLVMGetElementPtr) { gep(i);return; }
        if (op==LLVMLoad || op==LLVMStore) {
            auto ordering=LLVMGetOrdering(i);
            if (ordering!=LLVMAtomicOrderingNotAtomic) {
                auto access=op==LLVMLoad?t:LLVMTypeOf(arg(0));
                if (!integer(access,32) || (ordering!=LLVMAtomicOrderingMonotonic && ordering!=LLVMAtomicOrderingUnordered)) fail("only relaxed int32 atomic loads/stores are supported");
                if (op==LLVMLoad) assign(i,atomic(arg(0),"load"));else body+=atomic(arg(1),"store",expr(arg(0)))+";\n";return;
            }
            if (LLVMGetVolatile(i)) fail("non-atomic volatile memory unsupported");
            auto access=op==LLVMLoad?t:LLVMTypeOf(arg(0));auto ref=memory(arg(op==LLVMLoad?0:1),access,op==LLVMStore);
            if (op==LLVMLoad) assign(i,bitcast(ref.first,ref.second,t));
            else { auto p=get(arg(1));if (p.kind==Value::PrivatePointer) body+=p.text+" = "+expr(arg(0))+";\n";
                else if(p.kind==Value::PrivateArray)body+=privateArrayStore(privateArrays.at(p.root),ref.first,bitcast(expr(arg(0)),access,ref.second),lanes(access));
                else { const auto &b=bindings.at(p.root);
                    if(half(access))body+=halfBufferStore("_qhalfbuffer"+std::to_string(b.slot)+".data",ref.first,expr(arg(0)),lanes(access));
                    else body+=wordStore(b.dynamicSlot!=UINT32_MAX?"_qdynamic":b.sharedWords?"_qshared"+std::to_string(b.slot):"_qbuffer"+std::to_string(b.slot)+".data",ref.first,bitcast(expr(arg(0)),access,ref.second),lanes(access),b.sharedWords!=0,b.dynamicSlot); } }return;
        }
        std::string binary;bool sign=false;
        switch (op) {
            case LLVMAdd:case LLVMFAdd:binary="+";break;case LLVMSub:case LLVMFSub:binary="-";break;
            case LLVMMul:case LLVMFMul:binary="*";break;case LLVMUDiv:case LLVMFDiv:binary="/";break;
            case LLVMSDiv:binary="/";sign=true;break;case LLVMURem:binary="%";break;case LLVMSRem:binary="%";sign=true;break;
            case LLVMShl:binary="<<";break;case LLVMLShr:binary=">>";break;case LLVMAShr:binary=">>";sign=true;break;
            case LLVMAnd:binary="&";break;case LLVMOr:binary="|";break;case LLVMXor:binary="^";break;default:break;
        }
        if (!binary.empty()) {
            if (integer(scalar(t),1)) {
                auto value=booleanBinary(binary,expr(arg(0)),expr(arg(1)),lanes(t));
                if (value.empty()) fail("i1 arithmetic supports and/or/xor");assign(i,value);return;
            }
            if (!integer(scalar(t),32) && !integer(scalar(t),8) && !integer(scalar(t),16) && !floating(t)) fail("binary operation requires float16/float32 or integer elements");
            auto a=expr(arg(0)),b=expr(arg(1));if (sign) { a=airSignedInteger(a,LLVMGetIntTypeWidth(scalar(t)),lanes(t));b=airSignedInteger(b,LLVMGetIntTypeWidth(scalar(t)),lanes(t)); }
            auto s="("+a+binary+b+")";assign(i,sign?type(t)+"("+s+")":s);return;
        }
        if (op==LLVMFNeg) { assign(i,"(-"+expr(arg(0))+")");return; }
        if (op==LLVMZExt || op==LLVMSExt || op==LLVMTrunc || op==LLVMUIToFP || op==LLVMSIToFP || op==LLVMFPToUI || op==LLVMFPToSI || op==LLVMFPTrunc || op==LLVMFPExt || op==LLVMBitCast) {
            auto from=LLVMTypeOf(arg(0));auto v=get(arg(0));
            if ((op==LLVMZExt || op==LLVMSExt) && (integer(from,32) || integer(from,8) || integer(from,16)) && integer(t,64)) {
                if(!integer(from,32) && op==LLVMSExt)v.text=airSignedInteger(v.text,LLVMGetIntTypeWidth(from),1);
                v.text=addressExtend(v.text,op==LLVMSExt);v.kind=Value::WideIndex;values[i]=v;return;
            }
            if(op==LLVMBitCast && LLVMGetTypeKind(from)==LLVMPointerTypeKind && LLVMGetTypeKind(t)==LLVMPointerTypeKind &&
                LLVMGetPointerAddressSpace(from)==LLVMGetPointerAddressSpace(t)) {values[i]=v;return;}
            if (op==LLVMBitCast) { assign(i,bitcast(expr(arg(0)),from,t),v.linear);return; }
            if (lanes(from)!=lanes(t)) fail("cast changes vector shape");
            if ((op==LLVMFPToSI || op==LLVMFPToUI) && !integer(scalar(t),32)) fail("floating conversion requires int32 result");
            if (op==LLVMTrunc && integer(scalar(t),1) && integer(scalar(from),32)) {
                auto masked="("+expr(arg(0))+" & "+type(from)+"(1u))";
                assign(i,lanes(t)==1?"("+masked+" != 0u)":"notEqual("+masked+","+type(from)+"(0u))");return;
            }
            std::string s=expr(arg(0));if (op==LLVMSIToFP || op==LLVMSExt)s=airSignedInteger(s,LLVMGetIntTypeWidth(scalar(from)),lanes(from));
            s=type(t,op==LLVMFPToSI)+"("+s+")";if (op==LLVMFPToSI) s=type(t)+"("+s+")";assign(i,s);return;
        }
        if (op==LLVMExtractElement) {
            unsigned n=(unsigned)number(arg(1));auto v=get(arg(0));assign(i,component(v,LLVMTypeOf(arg(0)),n),n==0?v.linear:0);return;
        }
        if (op==LLVMInsertElement || op==LLVMShuffleVector) {
            unsigned n=op==LLVMInsertElement?(unsigned)number(arg(2)):0,width=lanes(LLVMTypeOf(arg(0)));
            if (op==LLVMInsertElement && n>=lanes(t)) fail("invalid vector insertion");
            std::string s=type(t)+"(";
            for (unsigned j=0;j<lanes(t);++j) {
                if (j) s+=",";
                if (op==LLVMInsertElement) s+=j==n?expr(arg(1)):component(get(arg(0)),t,j);
                else { int mask=LLVMGetMaskValue(i,j);s+=mask<0?type(scalar(t))+"(0)":component(get(arg((unsigned)mask>=width?1:0)),LLVMTypeOf(arg(0)),(unsigned)mask%width); }
            }
            assign(i,s+")");return;
        }
        if (op==LLVMSelect) {
            auto c=expr(arg(0)),a=expr(arg(1)),b=expr(arg(2));
            assign(i,lanes(LLVMTypeOf(arg(0)))==1?"("+c+"?"+a+":"+b+")":"mix("+b+","+a+","+c+")");return;
        }
        if(op==LLVMFCmp) {
            auto from=LLVMTypeOf(arg(0));
            if(!floating(from))fail("floating comparison requires float16/float32 elements");
            auto value=airFloatComparison(unsigned(LLVMGetFCmpPredicate(i)),expr(arg(0)),expr(arg(1)),lanes(from));
            if(value.empty())fail("invalid floating comparison");assign(i,value);return;
        }
        if (op==LLVMICmp) {
            auto pred=LLVMGetICmpPredicate(i);auto from=LLVMTypeOf(arg(0));std::string cmp,fn;
            if (!integer(scalar(from),32) && !integer(scalar(from),16) && !integer(scalar(from),8) && !integer(scalar(from),1)) fail("integer comparison requires integer elements");
            switch (pred) {
                case LLVMIntEQ:cmp="==";fn="equal";break;case LLVMIntNE:cmp="!=";fn="notEqual";break;
                case LLVMIntULT:case LLVMIntSLT:cmp="<";fn="lessThan";break;case LLVMIntULE:case LLVMIntSLE:cmp="<=";fn="lessThanEqual";break;
                case LLVMIntUGT:case LLVMIntSGT:cmp=">";fn="greaterThan";break;case LLVMIntUGE:case LLVMIntSGE:cmp=">=";fn="greaterThanEqual";break;default:fail("invalid integer predicate");
            }
            auto a=expr(arg(0)),b=expr(arg(1));if (pred>=LLVMIntSGT) { a=airSignedInteger(a,LLVMGetIntTypeWidth(scalar(from)),lanes(from));b=airSignedInteger(b,LLVMGetIntTypeWidth(scalar(from)),lanes(from)); }
            assign(i,lanes(from)==1?"("+a+cmp+b+")":fn+"("+a+","+b+")");return;
        }
        if (op==LLVMExtractValue || op==LLVMInsertValue) {
            auto v=get(arg(0));auto *part=&v;auto *indices=LLVMGetIndices(i);
            for (unsigned n=0;n<LLVMGetNumIndices(i);++n) {
                if (part->kind!=Value::Aggregate || indices[n]>=part->members.size()) fail("invalid aggregate access");part=&part->members[indices[n]];
            }
            if (op==LLVMExtractValue) { Value result=*part;if (result.kind==Value::Unavailable) fail("residency result is unavailable");values[i]=result; }
            else { *part=get(arg(1));values[i]=v; }return;
        }
        if (op==LLVMCall) { call(i);return; }
        if (op==LLVMRet) {
            if (!stage || (stage==2 && LLVMGetTypeKind(LLVMGetReturnType(LLVMGlobalGetValueType(f)))==LLVMVoidTypeKind)) { if (LLVMGetNumOperands(i)) fail("void shader cannot return a value");return; }
            if (LLVMGetNumOperands(i)!=1) fail("missing graphics output");auto v=get(arg(0));auto result=LLVMTypeOf(arg(0));
            if(LLVMIsPoison(arg(0)) && std::any_of(outputTargets.begin(),outputTargets.end(),[](const auto &s){return !s.empty();}))
                fail("active graphics outputs are poison after specialization");
            if (v.kind!=Value::Aggregate) { Value aggregate;aggregate.kind=Value::Aggregate;aggregate.members.push_back(v);v=std::move(aggregate); }
            if (v.members.size()!=outputTargets.size()) fail("graphics output/metadata count mismatch");
            for (unsigned n=0;n<v.members.size();++n) {
                if(outputTargets[n].empty())continue;
                auto member=LLVMGetTypeKind(result)==LLVMStructTypeKind?LLVMStructGetTypeAtIndex(result,n):result;
                bool mask=outputTargets[n]=="gl_SampleMask[0]";
                if (v.members[n].kind!=Value::Data || (mask?!integer(member,32):(!floating(member) || lanes(member)>4))) fail("invalid graphics output type");
                auto value=v.members[n].text;
                if(outputTargets[n].rfind("_qcolor",0)==0)value=airColorValue(value,half(member),lanes(member));
                else if(half(member))value=airFloatType(false,lanes(member))+"("+value+")";
                body+=outputTargets[n]+" = "+(mask?"int(":"")+value+(mask?")":"")+";\n";
            }return;
        }
        fail("unsupported instruction (inline and simplify AIR first)");
    }
    Value reserve(LLVMTypeRef t,unsigned depth=0) {
        if (depth>8) fail("aggregate nesting limit exceeded");
        Value v;
        if (LLVMGetTypeKind(t)==LLVMStructTypeKind) {
            unsigned count=LLVMCountStructElementTypes(t);if (count>64) fail("aggregate member limit exceeded");
            v.kind=Value::Aggregate;
            for (unsigned n=0;n<count;++n) v.members.push_back(reserve(LLVMStructGetTypeAtIndex(t,n),depth+1));
        } else if (floating(t) || integer(scalar(t),32) || (!depth && (integer(scalar(t),8) || integer(scalar(t),16))) || integer(scalar(t),1)) {
            v.text="v"+std::to_string(serial++);globals+=type(t)+" "+v.text+";\n";
        } else v.kind=Value::Unavailable; // e.g. the unexposed texture residency byte
        return v;
    }
    void copies(const Value &target,const Value &source,LLVMTypeRef t,std::vector<AIRFlow::Copy> &out) {
        if (target.kind==Value::Unavailable) return;
        if (target.kind==Value::Aggregate) {
            if (source.kind!=Value::Aggregate || target.members.size()!=source.members.size()) fail("aggregate SSA shape mismatch");
            for (unsigned n=0;n<target.members.size();++n) copies(target.members[n],source.members[n],LLVMStructGetTypeAtIndex(t,n),out);
        } else {
            if (source.kind!=Value::Data) fail("pointer/resource cannot be copied into a data PHI");
            if (target.text!=source.text) out.push_back({type(t),target.text,source.text});
        }
    }
    void materialize(LLVMValueRef i) {
        auto slot=storage.find(i);if (slot==storage.end()) return;
        auto value=values.at(i);std::vector<AIRFlow::Copy> assignments;copies(slot->second,value,LLVMTypeOf(i),assignments);
        for (const auto &copy:assignments) body+=copy.target+" = "+copy.value+";\n";
        auto stored=slot->second;stored.linear=value.linear;values[i]=std::move(stored);
    }
    void flowBody() {
        std::vector<LLVMBasicBlockRef> blocks;std::map<LLVMBasicBlockRef,unsigned> ids;
        for (auto b=LLVMGetFirstBasicBlock(f);b;b=LLVMGetNextBasicBlock(b)) { ids[b]=unsigned(blocks.size());blocks.push_back(b); }
        std::vector<AIRFlow::Block> graph(blocks.size());std::set<LLVMBasicBlockRef> live;
        std::vector<LLVMBasicBlockRef> pending{blocks.front()};
        for (size_t at=0;at<pending.size();++at) {
            auto b=pending[at];if (!live.insert(b).second) continue;
            auto term=LLVMGetBasicBlockTerminator(b);if (!term) fail("basic block has no terminator");
            auto op=LLVMGetInstructionOpcode(term);
            if (op!=LLVMRet && !branch(term) && op!=LLVMSwitch) fail("unsupported AIR CFG terminator");
            if (op==LLVMRet) continue;
            unsigned count=LLVMGetNumSuccessors(term);if (!count || count>129) fail("successor limit exceeded");
            for (unsigned n=0;n<count;++n) {
                auto next=LLVMGetSuccessor(term,n);if (!ids.count(next)) fail("foreign basic block");pending.push_back(next);
            }
        }
        std::vector<LLVMValueRef> instructions;
        for (auto b:blocks) if (live.count(b)) for (auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i)) {
            instructions.push_back(i);auto t=LLVMTypeOf(i);auto kind=LLVMGetTypeKind(t);
            if (LLVMGetInstructionOpcode(i)==LLVMPHI && (kind==LLVMPointerTypeKind || integer(t,64)))
                fail("pointer/address PHIs are unsupported");
            if (kind==LLVMVoidTypeKind || kind==LLVMPointerTypeKind || integer(t,64)) continue;
            auto slot=reserve(t);values[i]=slot;storage[i]=std::move(slot);
        }
        auto linear=[&](LLVMValueRef v) { auto it=values.find(v);return it==values.end()?0u:it->second.linear; };
        // Propagate only identities of the *unmodified* system index. A loop
        // induction variable never becomes an address proof through a PHI.
        for (size_t pass=0;pass<instructions.size();++pass) {
            bool changed=false;
            for (auto i:instructions) {
                auto op=LLVMGetInstructionOpcode(i);unsigned proof=0;
                if (op==LLVMBitCast || op==LLVMZExt) proof=linear(LLVMGetOperand(i,0));
                else if (op==LLVMExtractElement && LLVMIsAConstantInt(LLVMGetOperand(i,1)) && !number(LLVMGetOperand(i,1))) proof=linear(LLVMGetOperand(i,0));
                else if (op==LLVMSelect && linear(LLVMGetOperand(i,1))==linear(LLVMGetOperand(i,2))) proof=linear(LLVMGetOperand(i,1));
                else if (op==LLVMPHI) {
                    for (unsigned n=0;n<LLVMCountIncoming(i);++n) {
                        auto value=linear(LLVMGetIncomingValue(i,n));
                        if (!value || (proof && value!=proof)) { proof=0;break; }proof=value;
                    }
                }
                if (proof && storage.count(i) && !linear(i)) { values[i].linear=proof;changed=true; }
            }
            if (!changed) break;
        }
        auto prologue=body;
        for (auto b:blocks) if (live.count(b)) {
            auto &block=graph[ids.at(b)];auto term=LLVMGetBasicBlockTerminator(b);body.clear();
            for (auto i=LLVMGetFirstInstruction(b);i && i!=term;i=LLVMGetNextInstruction(i)) {
                if (LLVMGetInstructionOpcode(i)==LLVMPHI) continue;
                instruction(i);materialize(i);
            }
            auto op=LLVMGetInstructionOpcode(term);
            if (op==LLVMRet) { instruction(term);block.term=AIRFlow::Block::Return; }
            else if (branch(term)) {
                block.destination=ids.at(LLVMGetSuccessor(term,0));
                if (LLVMIsConditional(term)) {
                    block.term=AIRFlow::Block::Conditional;block.condition=expr(LLVMGetCondition(term));block.alternate=ids.at(LLVMGetSuccessor(term,1));
                } else block.term=AIRFlow::Block::Jump;
            } else {
                auto t=LLVMTypeOf(LLVMGetOperand(term,0));if (!integer(t,32) && !integer(t,16) && !integer(t,8) && !integer(t,1)) fail("switch requires an integer/bool value");
                block.term=AIRFlow::Block::Switch;block.condition=expr(LLVMGetOperand(term,0));block.destination=ids.at(LLVMGetSuccessor(term,0));
                for (unsigned n=1;n<LLVMGetNumSuccessors(term);++n)
                    block.cases.push_back({uint32_t(number(switchCase(term,n))),ids.at(LLVMGetSuccessor(term,n))});
            }
            block.code=std::move(body);
        }
        for (auto b:blocks) if (live.count(b)) for (auto phi=LLVMGetFirstInstruction(b);phi && LLVMGetInstructionOpcode(phi)==LLVMPHI;phi=LLVMGetNextInstruction(phi)) {
            std::map<LLVMBasicBlockRef,LLVMValueRef> incoming;
            for (unsigned n=0;n<LLVMCountIncoming(phi);++n) {
                auto pred=LLVMGetIncomingBlock(phi,n);if (!live.count(pred)) continue;
                auto value=LLVMGetIncomingValue(phi,n);auto old=incoming.find(pred);
                if (old!=incoming.end()) { if (old->second!=value) fail("inconsistent duplicate PHI edge");continue; }
                incoming[pred]=value;copies(storage.at(phi),get(value),LLVMTypeOf(phi),graph[ids.at(b)].incoming[ids.at(pred)]);
            }
        }
        std::string lowered,error;if (!AIRFlow::emit(graph,0,lowered,error)) fail(error);
        body=prologue+globals+lowered;
    }
public:
    Lower(LLVMValueRef function,unsigned kind,LLVMTargetDataRef data):f(function),layout(data),stage(kind) { e.name=name(f);e.reflection.stage=stage; }
    Entry run(LLVMValueRef metadata) {
        auto blockCount=LLVMCountBasicBlocks(f);
        if (!blockCount || blockCount>128 || e.name.empty() || e.name.size()>=96) fail("entry must be a named function with at most 128 basic blocks");
        unsigned instructionCount=0;
        for (auto b=LLVMGetFirstBasicBlock(f);b;b=LLVMGetNextBasicBlock(b)) for (auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))
            if (++instructionCount>4096) fail("instruction limit exceeded");
        usesHalf=containsHalf(LLVMGetReturnType(LLVMGlobalGetValueType(f)));
        for(unsigned n=0;n<LLVMCountParams(f);++n)usesHalf|=containsHalf(LLVMTypeOf(LLVMGetParam(f,n)));
        for(auto b=LLVMGetFirstBasicBlock(f);b;b=LLVMGetNextBasicBlock(b))for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i))usesHalf|=containsHalf(LLVMTypeOf(i));
        auto desc=node(metadata);if (desc.size()!=3) fail("invalid entry metadata");auto outputs=node(desc[1]);
        if (!stage) { if (!outputs.empty() || LLVMGetTypeKind(LLVMGetReturnType(LLVMGlobalGetValueType(f)))!=LLVMVoidTypeKind) fail("invalid compute output"); }
        else {
            auto result=LLVMGetReturnType(LLVMGlobalGetValueType(f));
            unsigned members=LLVMGetTypeKind(result)==LLVMVoidTypeKind?0:LLVMGetTypeKind(result)==LLVMStructTypeKind?LLVMCountStructElementTypes(result):1;
            if (outputs.size()!=members || (stage==1 && !members) || (stage==2 && members>QGPU_COLOR_TARGET_COUNT+2)) fail("graphics output/metadata count mismatch");
            bool position=false,depth=false,sampleMask=false;std::set<unsigned> colors;
            for (unsigned i=0;i<members;++i) {
                auto t=LLVMGetTypeKind(result)==LLVMStructTypeKind?LLVMStructGetTypeAtIndex(result,i):result;
                if(!AIRSpecialization::enabled(outputs[i])){outputTargets.emplace_back();continue;}
                if(has(outputs[i],"air.raster_order_group"))fail("raster-order groups require fragment interlock support");
                if(has(outputs[i],stage==1?"air.position":"air.render_target")) {
                    if(!floating(t) || lanes(t)>4 || text(field(outputs[i],"air.arg_type_name"))!=airFloatName(half(t),lanes(t)) ||
                        (stage==1 && (half(t) || lanes(t)!=4 || position)))fail("invalid position/color output");
                    position=true;
                    if(stage==1)outputTargets.push_back("gl_Position");
                    else {
                        auto a=node(outputs[i]);size_t target=0;while(target<a.size() && text(a[target])!="air.render_target")++target;
                        if(target+2>=a.size() || number(a[target+1])>=QGPU_COLOR_TARGET_COUNT || number(a[target+2]))fail("color target must be 0..7, dual-source index zero");
                        unsigned slot=unsigned(number(a[target+1]));if(!colors.insert(slot).second)fail("duplicate fragment color target");
                        auto name=fragmentColorName(slot);outputTargets.push_back(name);
                        header+="layout(location="+std::to_string(slot)+") out vec4 "+name+";\n";
                    }
                } else if(stage==2 && has(outputs[i],"air.sample_mask")) {
                    if(sampleMask || !integer(t,32) || text(field(outputs[i],"air.arg_type_name"))!="uint")fail("invalid/duplicate sample mask output");
                    sampleMask=true;outputTargets.push_back("gl_SampleMask[0]");
                } else if(stage==2 && has(outputs[i],"air.depth")) {
                    auto qualifier=text(field(outputs[i],"air.depth_qualifier"));
                    if(depth || LLVMGetTypeKind(t)!=LLVMFloatTypeKind || text(field(outputs[i],"air.arg_type_name"))!="float" ||
                       (qualifier!="air.any" && qualifier!="air.less" && qualifier!="air.greater"))fail("invalid/duplicate fragment depth output");
                    depth=true;outputTargets.push_back("gl_FragDepth");
                    if(qualifier!="air.any")header+="layout(depth_"+qualifier.substr(4)+") out float gl_FragDepth;\n";
                } else if (stage==1 && has(outputs[i],"air.vertex_output")) outputTargets.push_back(varying(outputs[i],t,true));
                else fail("unsupported graphics output semantic");
            }
            if (stage==1 && !position) fail("missing position output");
        }
        std::set<unsigned> seen,slots,threadgroupSlots,attributeSlots;
        std::set<std::string> computeInputs;
        GraphicsTextureBindings textureSlots;
        if(stage) {
            for(auto md:node(desc[2]))if(AIRSpecialization::enabled(md) && has(md,"air.texture"))
                if(!textureSlots.add(number(field(md,"air.location_index"))))fail("invalid/duplicate texture binding or more than eight enabled textures");
            textureSlots.prepare();e.reflection.textureBindings=textureSlots.encoding();
        }
        for (auto md:node(desc[2])) {
            if(has(md,"air.raster_order_group"))fail("raster-order groups require fragment interlock support");
            auto a=node(md);if (a.size()<2) fail("truncated parameter metadata");auto n=number(a[0]);
            if (n>=LLVMCountParams(f) || !seen.insert((unsigned)n).second) fail("invalid/duplicate parameter");
            auto arg=LLVMGetParam(f,(unsigned)n);auto t=LLVMTypeOf(arg);Value v;
            if(!AIRSpecialization::enabled(md)){v.kind=Value::Unavailable;values[arg]=v;continue;}
            if (has(md,"air.buffer")) {
                auto slot=number(field(md,"air.location_index"));
                if (slot>=8 || LLVMGetTypeKind(t)!=LLVMPointerTypeKind) fail("unsupported buffer binding");
                unsigned space=LLVMGetPointerAddressSpace(t);
                if ((space!=1 && space!=2 && space!=3) || (space==3 && stage) ||
                    !(space==3?threadgroupSlots:slots).insert((unsigned)slot).second ||
                    LLVMPointerSizeForAS(layout,space)!=8 || (field(md,"air.address_space") && number(field(md,"air.address_space"))!=space)) fail("invalid or duplicate buffer address space/binding");
                bool readonly=has(md,"air.read") || space==2;
                if ((!readonly && !has(md,"air.write") && !has(md,"air.read_write")) || (stage==1 && !readonly) || (space==3 && readonly)) fail("unsupported buffer access");
                auto kind=text(field(md,"air.arg_type_name"));unsigned n=bufferComponents(kind),stride=4;
                auto ctx=LLVMGetTypeContext(t);auto valueType=LLVMInt32TypeInContext(ctx);
                if (n) stride=(n==3?4:n)*(kind.rfind("half",0)==0?2:4);
                else if (kind.empty() || !field(md,"air.arg_type_size") || !number(field(md,"air.arg_type_size")) || number(field(md,"air.arg_type_size"))>4096)
                    fail("unsupported buffer element type");
                if (n && field(md,"air.arg_type_size") && number(field(md,"air.arg_type_size"))!=stride) fail("buffer layout disagrees with metadata");
                bindings[arg]={(unsigned)slot,stride,readonly,valueType};
                if (space==3) bindings[arg].dynamicSlot=(unsigned)slot;else e.reflection.strides[slot]=stride;
                v.kind=Value::Pointer;v.root=arg;v.text="uvec2(0u)";
                if (space!=3) header+="layout(std430,binding="+std::to_string(slot)+") "+(readonly?"readonly ":"")+"buffer QBuffer"+std::to_string(slot)+" { uint data[]; } _qbuffer"+std::to_string(slot)+";\n";
            } else if (has(md,"air.texture")) {
                auto slot=number(field(md,"air.location_index"));auto kind=text(field(md,"air.arg_type_name"));
                if(stage)slot=textureSlots.physical(unsigned(slot));
                AIRTextureBinding binding;binding.slot=unsigned(slot);
                if(slot>=QGPU_STAGE_TEXTURE_COUNT || !airTextureMetadata(kind,binding) || (stage && (binding.unsignedInteger || (binding.access!=4 && binding.access!=1))) ||
                    LLVMGetTypeKind(t)!=LLVMPointerTypeKind || LLVMGetPointerAddressSpace(t)!=1)fail("unsupported texture metadata");
                for(const auto &bound:imageBindings)if(bound.second.slot==slot)fail("duplicate texture slot");
                bool cube=binding.shape&2,array=binding.shape&1;
                if(stage) {if(graphicsHeaders.insert(binding.shape).second)header+=graphicsTextureHeader(binding.shape,stage);}
                else if(cube) {if(cubeHeaders.insert(unsigned(array)).second)header+=cubeImageHeader(array);}
                else if(!imageIncluded) {header+=imageHeader();imageIncluded=true;}
                if(!stage && binding.access<4)header+=cube?cubeImageFunctions(unsigned(slot),binding.access,array,binding.unsignedInteger):imageFunctions(unsigned(slot),binding.access,array,false,binding.unsignedInteger);
                if(binding.access==4 && !samplerIncluded) {header+=samplerHeader(stage==2,stage==1);samplerIncluded=true;}
                if(binding.unsignedInteger)e.reflection.imageUintMask|=1u<<slot;
                imageBindings[arg]=binding;v.kind=Value::Texture;v.root=arg;
            } else if (has(md,"air.sampler")) {
                auto slot=number(field(md,"air.location_index"));
                if(slot>=QGPU_STAGE_SAMPLER_COUNT || LLVMGetTypeKind(t)!=LLVMPointerTypeKind || LLVMGetPointerAddressSpace(t)!=2)fail("invalid sampler metadata");
                for(const auto &bound:samplerBindings)if(bound.second==slot)fail("duplicate sampler slot");
                samplerBindings[arg]=(unsigned)slot;v.kind=Value::Sampler;v.root=arg;
            } else if (stage==1 && has(md,"air.vertex_input")) {
                auto slot=number(field(md,"air.location_index"));auto kind=text(field(md,"air.arg_type_name"));bool sign=kind.rfind("int",0)==0;
                if (slot>=QGPU_MAX_ATTRIBUTES || !attributeSlots.insert((unsigned)slot).second ||
                    (!floating(t) && !integer(scalar(t),32)) || lanes(t)>4 ||
                    kind!=(floating(t)?airFloatName(half(t),lanes(t)):(sign?"int":"uint")+(lanes(t)==1?std::string():std::to_string(lanes(t))))) fail("invalid vertex attribute type/location");
                std::string input="_qattrib"+std::to_string(slot);
                header+="layout(location="+std::to_string(slot)+") in "+(half(t)?airFloatType(false,lanes(t)):type(t,sign))+" "+input+";\n";
                v.text=sign || half(t)?type(t)+"("+input+")":input;
            } else if(stage==2 && has(md,"air.render_target")) {
                size_t at=0;while(at<a.size() && text(a[at])!="air.render_target")++at;
                if(at+1>=a.size() || !floating(t) || lanes(t)>4 || text(field(md,"air.arg_type_name"))!=airFloatName(half(t),lanes(t)))fail("invalid framebuffer input");
                auto slot=number(a[at+1]);if(slot>=QGPU_COLOR_TARGET_COUNT || (e.reflection.framebufferMask&(1u<<slot)))fail("duplicate/out-of-range framebuffer input");
                if(!e.reflection.framebufferMask)header+=framebufferHeader();e.reflection.framebufferMask|=1u<<slot;header+=framebufferFunctions(slot);
                v.text="_qframebuffer"+std::to_string(slot)+"_read()";
                if(lanes(t)<4)v.text="("+v.text+")."+std::string("xyzw",lanes(t));
                if(half(t))v.text=type(t)+"("+v.text+")";
                // AIR passes the incoming attachment color as one immutable
                // argument. Materialize it once, including its original half
                // conversion, rather than expanding a texture read per use.
                auto input="_qframebuffer_input"+std::to_string(slot);
                framebufferInputs+=type(t)+" "+input+" = "+v.text+";\n";v.text=input;
            } else if (stage==2 && (has(md,"air.sample_id") || has(md,"air.sample_mask"))) {
                bool id=has(md,"air.sample_id");auto key=id?"sample_id":"sample_mask";
                if(!integer(t,32) || text(field(md,"air.arg_type_name"))!="uint" || has(md,"air.post_depth_coverage") ||
                   (id && has(md,"air.sample_mask")) || !computeInputs.insert(key).second)fail("invalid/duplicate fragment sample input");
                v.text=id?"uint(gl_SampleID)":"uint(gl_SampleMaskIn[0])";
            } else if (stage==2 && has(md,"air.fragment_input")) {
                v.text=varying(md,t,false);
            } else if (stage==0 && !computeInputSemantic([&](const char *s) { return has(md,std::string("air.")+s); }).empty()) {
                const auto input=computeInputSemantic([&](const char *s) { return has(md,std::string("air.")+s); });
                const auto value=computeSystemValue(input);
                if (value.empty() || !computeInputs.insert(input).second || !integer(scalar(t),32) ||
                    (lanes(t)!=1 && ((lanes(t)!=2 && lanes(t)!=3) || input=="thread_index_in_threadgroup"))) fail("invalid compute input type/semantic");
                v.text=value+(lanes(t)==1 && input!="thread_index_in_threadgroup"?".x":"");
                if (lanes(t)==2) v.text="uvec2("+v.text+")";
                if (input=="thread_position_in_grid") { system=true;v.linear=1; }
            } else if (has(md,"air.thread_position_in_grid") || has(md,"air.vertex_id")) {
                if (system || stage==2 || !has(md,stage==0?"air.thread_position_in_grid":"air.vertex_id") || !(integer(scalar(t),32) || (stage==1 && integer(t,16))) || (lanes(t)!=1 && (stage!=0 || lanes(t)!=3))) fail("invalid system index");
                system=true;v.text=lanes(t)==1?"_qindex":"uvec3(_qindex,0u,0u)";
                if(integer(t,16))v.text=airIntegerMask(v.text,16,1);v.linear=integer(scalar(t),32);
            } else if (has(md,"air.instance_id") && stage==1 && !instance && integer(t,32)) {
                instance=true;v.text="_qinstance";v.linear=2;
            } else if (has(md,"air.position") && stage==2 && type(t)=="vec4" && text(field(md,"air.arg_type_name"))=="float4") {
                system=true;v.text="gl_FragCoord";
            } else fail("unsupported argument semantic");
            values[arg]=v;
        }
        if (seen.size()!=LLVMCountParams(f)) fail("incomplete AIR interface");
        header=bufferHeader()+(usesHalf?halfBufferHeader():std::string())+header;
        if (!stage) header=computeHeader()+header;
        if(stage!=2)body="uint _qindex = "+std::string(stage==0?"_q_global_id().x":"uint(gl_VertexID)")+";\n";
        if (instance) body+="uint _qinstance = uint(gl_InstanceID);\n";
        controlFlow=blockCount!=1;
        if (controlFlow) flowBody();
        else for (auto i=LLVMGetFirstInstruction(LLVMGetFirstBasicBlock(f));i;i=LLVMGetNextInstruction(i)) instruction(i);
        for (unsigned i=0;i<8;++i) if (!(e.reflection.bufferMask&(1u<<i))) e.reflection.strides[i]=0;
        e.glsl="#version 430\n"+std::string(usesHalf?"#extension GL_AMD_gpu_shader_half_float : require\n":"")+std::string(usesDiscard?"#extension GL_EXT_demote_to_helper_invocation : require\n":"")+header+"void main() {\n"+framebufferInputs+body+"}\n";return e;
    }
};
// Decoded modules are immutable templates. A clone is specialized per call.
// LLVM clones share a context, so keep the context gate for the full lifetime
// of each borrower, including reflection and exception cleanup.
struct DecodedAIR {
    LLVMContextRef context=LLVMContextCreate();
    LLVMModuleRef module=nullptr;
    std::string bytes;
    ~DecodedAIR(){if(module)LLVMDisposeModule(module);LLVMContextDispose(context);}
};
std::mutex &decodeGate(){static std::mutex gate;return gate;}
std::vector<std::shared_ptr<DecodedAIR>> &decodedModules(){static std::vector<std::shared_ptr<DecodedAIR>> entries;return entries;}
struct Module {
    std::unique_lock<std::mutex> gate;
    std::shared_ptr<DecodedAIR> decoded;
    LLVMContextRef context=nullptr;LLVMModuleRef module=nullptr;LLVMTargetDataRef layout=nullptr;
    ~Module(){if(layout)LLVMDisposeTargetData(layout);if(module)LLVMDisposeModule(module);}
};
void initializeModule(const void *bytes,size_t length,Module &m) {
        if (!bytes || !length || length>16*1024*1024) fail("invalid input size");
        m.gate=std::unique_lock<std::mutex>(decodeGate());
        auto &cache=decodedModules();
        if(length>=32768)for(size_t n=0;n<cache.size();++n) {
            auto entry=cache[n];
            if(entry->bytes.size()!=length || memcmp(entry->bytes.data(),bytes,length))continue;
            cache.erase(cache.begin()+n);cache.push_back(entry);
            m.decoded=entry;m.context=entry->context;m.module=LLVMCloneModule(entry->module);
            if(!m.module)fail("AIR module clone failed");
            m.layout=LLVMCreateTargetData(LLVMGetDataLayoutStr(m.module));return;
        }
        m.decoded=std::make_shared<DecodedAIR>();m.context=m.decoded->context;
        char *diagnostic=nullptr;
        auto buffer=LLVMCreateMemoryBufferWithMemoryRangeCopy((const char *)bytes,length,"runtime.air");
        if (LLVMParseIRInContext(m.context,buffer,&m.module,&diagnostic)) {
            std::string error=diagnostic?diagnostic:"LLVM cannot read this AIR version";if (diagnostic) LLVMDisposeMessage(diagnostic);fail(error);
        }
        if (LLVMVerifyModule(m.module,LLVMReturnStatusAction,&diagnostic)) {
            std::string error=diagnostic?diagnostic:"Invalid AIR module";if (diagnostic) LLVMDisposeMessage(diagnostic);fail(error);
        }
        if (diagnostic) LLVMDisposeMessage(diagnostic);
        auto triple=std::string(LLVMGetTarget(m.module));auto dl=LLVMGetDataLayoutStr(m.module);
        if (triple.rfind("air64",0)!=0 || !dl || !dl[0]) fail("AIR64 module with a data layout required");
        m.layout=LLVMCreateTargetData(dl);
        unsigned versions=LLVMGetNamedMetadataNumOperands(m.module,"air.version");if (versions!=1) fail("AIR version metadata required");
        LLVMValueRef version;LLVMGetNamedMetadataOperands(m.module,"air.version",&version);
        auto v=node(version);if (v.size()!=3 || number(v[0])!=2) fail("AIR major version 2 required");
        if(length>=32768) {
            // Four entries and 32 MiB of source bytes bound retained templates.
            // Byte equality, not a hash alone, decides a cache hit.
            size_t retained=length;for(const auto &entry:cache)retained+=entry->bytes.size();
            while(!cache.empty() && (cache.size()>=4 || retained>32u*1024*1024)) {
                retained-=cache.front()->bytes.size();cache.erase(cache.begin());
            }
            m.decoded->bytes.assign(static_cast<const char *>(bytes),length);
            m.decoded->module=m.module;m.module=LLVMCloneModule(m.decoded->module);
            if(!m.module)fail("AIR module clone failed");
            cache.push_back(m.decoded);
        }
}

}
bool inspectAIREntries(const void *bytes,size_t length,std::vector<std::string> &out,std::string &error) {
    out.clear();error.clear();
    try {
        Module m;initializeModule(bytes,length,m);std::set<std::string> names;
        for(const char *key:{"air.kernel","air.vertex","air.fragment"}) {
            unsigned count=LLVMGetNamedMetadataNumOperands(m.module,key);
            if(count>64 || out.size()+count>64)fail("AIR entry limit exceeded");
            std::vector<LLVMValueRef> entries(count);LLVMGetNamedMetadataOperands(m.module,key,entries.data());
            for(auto entry:entries) {
                auto md=node(entry);if(md.size()!=3 || !LLVMIsAFunction(md[0]))fail("invalid AIR stage entry");
                auto n=name(md[0]);if(!names.insert(n).second)fail("duplicate AIR stage entry");
                out.push_back(n);
            }
        }
        if(out.empty())fail("no supported AIR stage entries");return true;
    }catch(const std::exception &e){error=e.what();out.clear();return false;}
}
bool inspectAIRConstants(const void *bytes,size_t length,const std::string &selected,std::vector<FunctionConstant> &out,unsigned &stage,std::string &error) {
    out.clear();stage=UINT32_MAX;
    try {
        if(selected.empty())fail("constant reflection requires one selected entry");
        Module m;initializeModule(bytes,length,m);AIRSpecialization::State state(m.module,selected);state.inspect(out);
        for(unsigned s=0;s<3;++s) {
            auto key=s==0?"air.kernel":s==1?"air.vertex":"air.fragment";unsigned count=LLVMGetNamedMetadataNumOperands(m.module,key);
            std::vector<LLVMValueRef> entries(count);LLVMGetNamedMetadataOperands(m.module,key,entries.data());
            for(auto entry:entries)if(name(node(entry)[0])==selected) {if(stage!=UINT32_MAX)fail("duplicate selected stage entry");stage=s;}
        }
        if(stage==UINT32_MAX)fail("selected entry is absent");return true;
    }catch(const std::exception &e){error=e.what();out.clear();return false;}
}
bool lowerAIR(const void *bytes,size_t length,std::vector<Entry> &out,std::string &error,const std::string &selected,const std::vector<FunctionConstantValue> &constants) {
    out.clear();
    try {
        Module m;initializeModule(bytes,length,m);
        AIRSpecialization::State specialization(m.module,selected);
        if(specialization.present())specialization.apply(constants);
        specialization.lowerHelpers();
        std::set<std::string> names;
        for (unsigned stage=0;stage<3;++stage) {
            auto key=stage==0?"air.kernel":stage==1?"air.vertex":"air.fragment";
            unsigned count=LLVMGetNamedMetadataNumOperands(m.module,key);if (count>64) fail("entry limit exceeded");
            std::vector<LLVMValueRef> entries(count);LLVMGetNamedMetadataOperands(m.module,key,entries.data());
            for (auto entry:entries) {
                auto md=node(entry);if (md.size()!=3 || !LLVMIsAFunction(md[0])) fail("invalid AIR stage entry");
                auto n=name(md[0]);if (!names.insert(n).second) fail("duplicate AIR stage entry");
                if(!selected.empty() && n!=selected)continue;
                if(out.size()==64)fail("AIR entry limit exceeded");
                try { out.push_back(Lower(md[0],stage,m.layout).run(entry)); }
                catch (const std::exception &e) { throw std::runtime_error(n+": "+e.what()); }
            }
        }
        if (out.empty()) fail(selected.empty()?"no supported AIR stage entries":"selected AIR stage entry is absent");return true;
    } catch (const std::exception &e) { error=e.what();out.clear();return false; }
}
} }
