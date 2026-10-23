// SPDX-License-Identifier: BSD-2-Clause
// AIR/LLVM to the driver's bounded GLSL profile. LLVM owns bitcode parsing,
// verification, SSA, types and metadata; no shader name selects an
// implementation.
#include <array>
#include "Semantic.h"
#include "AIRControlFlow.h"
#include "LibraryArchive.h"
#include "AIRSpecialization.h"
#include <GPUShaderInterface.h>
#include <cstdlib>
#include <llvm/ADT/MapVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace llvm;
namespace AIRFlow = GPU::Compiler::AIRFlow;
static std::string entryName;
[[noreturn]] static void fail(const Twine &s) {
  errs() << "AIR" << (entryName.empty() ? "" : " [" + entryName + "]") << ": "
         << s << '\n';
  std::exit(1);
}
static MDNode *node(Metadata *m) {
  auto *n = dyn_cast_or_null<MDNode>(m);
  if (!n)
    fail("expected metadata node");
  return n;
}
static uint64_t number(Metadata *m) {
  auto *c = dyn_cast_or_null<ConstantAsMetadata>(m);
  if(c && LLVMIsAGlobalVariable(GPU::Compiler::AIRSpecialization::root(wrap(c->getValue())))) {
    try {return GPU::Compiler::AIRSpecialization::interfaceNumber(wrap(c->getValue()));}
    catch(const std::exception &error){fail(error.what());}
  }
  auto *i = c ? dyn_cast<ConstantInt>(c->getValue()) : nullptr;
  if (!i || i->getBitWidth() > 64)
    fail("expected metadata integer");
  return i->getZExtValue();
}
static bool has(MDNode *n, StringRef key) {
  for (auto &v : n->operands())
    if (auto *s = dyn_cast_or_null<MDString>(v.get()))
      if (s->getString() == key)
        return true;
  return false;
}
static Metadata *field(MDNode *n, StringRef key) {
  for (unsigned i = 0; i + 1 < n->getNumOperands(); ++i) {
    if (auto *s = dyn_cast_or_null<MDString>(n->getOperand(i)))
      if (s->getString() == key)
        return n->getOperand(i + 1);
  }
  return nullptr;
}
static std::string stringField(MDNode *n, StringRef key) {
  auto *s = dyn_cast_or_null<MDString>(field(n, key));
  if (!s)
    fail("missing AIR string field " + key);
  return s->getString().str();
}
static bool enabled(MDNode *n) {
  return GPU::Compiler::AIRSpecialization::enabled(LLVMMetadataAsValue(wrap(&n->getContext()),wrap(n)));
}
static unsigned lanes(Type *t) {
  auto *v = dyn_cast<FixedVectorType>(t);
  return v ? v->getNumElements() : 1;
}
static Type *scalar(Type *t) { return t->getScalarType(); }
static bool half(Type *t) {return scalar(t)->isHalfTy();}
static bool floating(Type *t) {return half(t) || scalar(t)->isFloatTy();}
static bool containsHalf(Type *t) {
  if(half(t))return true;
  if(auto *s=dyn_cast<StructType>(t))for(auto *member:s->elements())if(containsHalf(member))return true;
  return false;
}
static std::string type(Type *t, bool sign = false) {
  unsigned n = lanes(t);
  if (n > 4 || (t->isVectorTy() && n < 2))
    fail("unsupported vector width");
  Type *s = scalar(t);
  std::string a, b;
  if (floating(t))return GPU::Compiler::airFloatType(half(t),n);
  if (s->isIntegerTy(32) || s->isIntegerTy(8) || s->isIntegerTy(16)) {
    a = sign ? "int" : "uint";
    b = sign ? "ivec" : "uvec";
  } else if (s->isIntegerTy(1)) {
    a = "bool";
    b = "bvec";
  } else
    fail("only float16/float32, int32 and bool values are supported (int64 is reserved "
         "for proven indices)");
  return n == 1 ? a : b + std::to_string(n);
}
struct AIRValue {
  enum Kind {
    Data,
    Pointer,
    PrivatePointer,
    PrivateArray,
    Texture,
    Sampler,
    ReadSampler,
    Aggregate,
    WideIndex,
    Unavailable
  } kind = Data;
  std::string text;
  const Value *root = nullptr;
  unsigned linear = 0, index = 0;
  std::vector<AIRValue> members;
};
struct Buffer {
  unsigned slot = 0, stride = 0;
  bool readonly = true;
  Type *valueType = nullptr;
  unsigned sharedWords=0,dynamicSlot=UINT32_MAX;
};
class Lower {
  Function &f;
  const DataLayout &layout;
  unsigned stage, serial = 0,framebufferMask=0,imageUintMask=0;
  std::map<const Value *, AIRValue> values;
  std::map<const Value *, Buffer> bindings;
  std::map<const Value *,std::pair<std::string,Type *>> constantTables;
  std::map<const Value *,std::string> privateArrays;
  unsigned privateWords=0;
  std::map<const Value *,GPU::Compiler::AIRTextureBinding> imageBindings;
  std::map<const Value *,unsigned> samplerBindings;std::set<unsigned> samplePairs;
  std::vector<QGPUConstantSampler> constantSamplers;
  bool imageIncluded=false,samplerIncluded=false,usesDiscard=false;
  bool usesHalf=false;std::set<unsigned> halfBuffers;
  std::set<unsigned> graphicsHeaders,cubeHeaders,fetchSlots;
  unsigned sharedSerial=0,sharedBytes=0;
  bool dynamicIncluded=false;
  std::array<unsigned, 8> strides{};
  uint32_t bufferMask = 0, writeMask = 0, instanceMask = 0;
  std::string declarations, body, framebufferInputs;
  bool controlFlow=false;
  std::string globals;
  std::map<const Value *,AIRValue> storage;
  std::set<const Value *> resolving;
  std::set<std::string> atomicHelpers;
  bool haveSystem = false, haveInstance = false;
  std::vector<QGPUVarying> varyings;
  std::set<unsigned> attributeSlots;
  std::vector<std::string> outputTargets;
  Type *f32, *i32;
  std::string varying(MDNode *desc,Type *t,bool output) {
    unsigned width=lanes(t);
    if (!floating(t) || !width || width>4 || stringField(desc,"air.arg_type_name")!=GPU::Compiler::airFloatName(half(t),width))
      fail("stage varying must be a float16/float32 scalar/vector");
    std::string identity;
    auto *tag=field(desc,output?"air.vertex_output":"air.fragment_input");
    auto *name=dyn_cast_or_null<MDString>(tag);
    if (!name || !GPU::Compiler::airSemantic(name->getString().str(),identity)) fail("unsupported AIR varying semantic");
    if(half(t))identity+=":half";
    if (varyings.size()>=QGPU_MAX_VARYINGS) fail("too many varyings");
    QGPUVarying v{};GPU::Compiler::semantic(identity,v.semantic);v.components=width;
    if (!GPU::Compiler::airInterpolation(has(desc,"air.flat"),has(desc,"air.no_perspective"),has(desc,"air.perspective"),
        has(desc,"air.center"),has(desc,"air.centroid"),has(desc,"air.sample"),v.mode)) fail("conflicting interpolation qualifiers");
    for (const auto &other:varyings) if (!std::memcmp(v.semantic,other.semantic,sizeof(v.semantic))) fail("duplicate varying semantic");
    auto index=varyings.size();varyings.push_back(v);
    std::string variable="_qvarying"+std::to_string(index);
    declarations+="layout(location="+std::to_string(index)+") "+GPU::Compiler::interpolationQualifier(v.mode)+
        (output?"out ":"in ")+GPU::Compiler::airFloatType(false,width)+" "+variable+";\n";return output || !half(t)?variable:type(t)+"("+variable+")";
  }
  AIRValue data(std::string s) {
    AIRValue x;
    x.text = std::move(s);
    return x;
  }
  AIRValue zero(Type *t) {
    AIRValue out;
    if (auto *s = dyn_cast<StructType>(t)) {
      out.kind = AIRValue::Aggregate;
      for (auto *e : s->elements())
        out.members.push_back(zero(e));
    } else
      out.text = type(t) + "(0)";
    return out;
  }
  AIRValue get(const Value *v) {
    auto it = values.find(v);
    if (it != values.end())
      return it->second;
    if (auto *allocation=dyn_cast<AllocaInst>(v)) {
      auto *count=dyn_cast<ConstantInt>(allocation->getArraySize());
      if (allocation->getParent()!=&f.getEntryBlock() || !count || !count->isOne() || allocation->getAddressSpace()!=0)
        fail("thread-local allocations require a fixed entry-block object");
      auto *allocated=allocation->getAllocatedType();
      if(!allocated->isIntegerTy(32) && !allocated->isFloatTy()) {
        auto *leaf=allocated;uint64_t elements=1;unsigned depth=0;
        while(auto *array=dyn_cast<ArrayType>(leaf)) {
          auto count=array->getNumElements();
          if(++depth>8 || !count || count>256/elements)fail("thread-local array shape exceeds the bounded profile");
          elements*=count;leaf=array->getElementType();
        }
        if((!leaf->isIntegerTy(32) && !leaf->isFloatTy() && !isa<FixedVectorType>(leaf)) ||
            !(scalar(leaf)->isIntegerTy(32) || scalar(leaf)->isFloatTy()) || lanes(leaf)>4)
          fail("thread-local array elements require fixed int32/float32 scalar/vector values");
        auto bytes=uint64_t(layout.getTypeAllocSize(allocated));
        if(!(scalar(leaf)->isIntegerTy(32) || scalar(leaf)->isFloatTy()) || lanes(leaf)>4 ||
            !bytes || (bytes&3) || bytes>1024 || privateWords>256-bytes/4)fail("thread-local array exceeds the bounded int32/float32 profile");
        privateWords+=unsigned(bytes/4);auto name="_qprivate_array"+std::to_string(serial++);
        declarations+=GPU::Compiler::privateArrayHeader(name,unsigned(bytes/4));privateArrays[v]=name;
        AIRValue p;p.kind=AIRValue::PrivateArray;p.root=v;p.text="uvec2(0u)";values[v]=p;return p;
      }
      AIRValue p;p.kind=AIRValue::PrivatePointer;p.root=allocation;p.text="_qprivate"+std::to_string(serial++);values[v]=p;
      (controlFlow?globals:body)+=type(allocation->getAllocatedType())+" "+p.text+";\n";return p;
    }
    if(auto *c=dyn_cast<ConstantExpr>(v)) {
      if(c->getOpcode()!=Instruction::BitCast || !c->getType()->isPointerTy() ||
          c->getType()->getPointerAddressSpace()!=2)fail("unsupported constant resource expression");
      auto value=get(c->getOperand(0));if(value.kind!=AIRValue::Sampler)fail("constant resource cast requires a sampler");return value;
    }
    if (auto *g=dyn_cast<GlobalVariable>(v)) {
      if(g->getAddressSpace()==2 && g->isConstant() && g->hasInitializer()) {
        auto *initial=g->getInitializer();
        auto *array=dyn_cast<ArrayType>(initial->getType());
        if(array && floating(array->getElementType()) && lanes(array->getElementType())<=4) {
          if(!array->getNumElements() || array->getNumElements()>256)fail("constant table size exceeds AIR profile");
          auto *element=array->getElementType();std::string name="_qconst"+std::to_string(serial++),init;
          for(unsigned i=0;i<array->getNumElements();++i)init+=(i+1==array->getNumElements()?"default:":"case "+std::to_string(i)+"u:")+"return "+get(initial->getAggregateElement(i)).text+";";
          declarations+=type(element)+" "+name+"(uint index){switch(index){"+init+"}}\n";
          constantTables[g]={name,element};AIRValue p;p.kind=AIRValue::Pointer;p.root=g;p.text=GPU::Compiler::addressConstant(0);values[g]=p;return p;
        }
        uint64_t words[2]={};
        if(initial->getType()->isIntegerTy(64)) {auto *word=dyn_cast<ConstantInt>(initial);if(!word)fail("sampler initializer must contain integer words");words[0]=word->getZExtValue();}
        else {
          auto *array=dyn_cast<ArrayType>(initial->getType());
          if(!array || array->getNumElements()!=2 || !array->getElementType()->isIntegerTy(64))fail("invalid AIR sampler constant shape");
          for(unsigned i=0;i<2;++i){auto *word=dyn_cast_or_null<ConstantInt>(initial->getAggregateElement(i));if(!word)fail("sampler initializer must contain integer words");words[i]=word->getZExtValue();}
        }
        QGPUConstantSampler state{};if(!GPU::Compiler::airConstantSampler(words[0],words[1],state))fail("unsupported constexpr sampler encoding/state");
        unsigned index=GPU::Compiler::internConstantSampler(constantSamplers,state);if(index==UINT32_MAX)fail("too many constant sampler states");
        AIRValue value;value.kind=AIRValue::Sampler;value.root=g;values[g]=value;samplerBindings[g]=QGPU_STAGE_SAMPLER_COUNT+index;return value;
      }
      if (stage || g->getAddressSpace()!=3 || !g->hasInitializer() ||
          (!isa<UndefValue>(g->getInitializer()) && !isa<PoisonValue>(g->getInitializer()))) fail("only uninitialized compute threadgroup globals are supported");
      uint64_t bytes=layout.getTypeAllocSize(g->getValueType());
      if (!bytes || (bytes&3) || bytes>QGPU_MAX_THREADGROUP_BYTES || sharedBytes>QGPU_MAX_THREADGROUP_BYTES-bytes) fail("invalid threadgroup global size");
      sharedBytes+=bytes;Buffer b;b.slot=sharedSerial++;b.sharedWords=bytes/4;b.readonly=false;
      bindings[g]=b;AIRValue p;p.kind=AIRValue::Pointer;p.root=g;p.text="uvec2(0u)";values[g]=p;
      declarations+="shared uint _qshared"+std::to_string(b.slot)+"["+std::to_string(b.sharedWords)+"];\n";return p;
    }
    if (controlFlow && isa<Instruction>(v)) {
      auto *i=cast<Instruction>(v);
      if ((isa<GetElementPtrInst>(i) || ((isa<ZExtInst>(i) || isa<SExtInst>(i)) && i->getType()->isIntegerTy(64))) && resolving.insert(v).second) {
        // Address aliases may be resolved in any source block order; loads,
        // samples and arithmetic remain in their own blocks.
        instruction(*const_cast<Instruction *>(i));resolving.erase(v);
        if (values.count(v)) return values.at(v);
      }
      fail("unresolved or cyclic resource/wide-index value in AIR CFG");
    }
    if (auto *c = dyn_cast<ConstantInt>(v)) {
      if (c->getBitWidth() > 64)
        fail("integer constants wider than 64 bits are unsupported");
      if (scalar(c->getType())->isIntegerTy(1)) {
        std::string value=c->isZero()?"false":"true";
        return data(lanes(c->getType())==1?value:type(c->getType())+"("+value+")");
      }
      if (c->getBitWidth() > 32 && c->getZExtValue() > UINT32_MAX)
        fail("large integer outside index lowering");
      auto value=std::to_string(c->getZExtValue())+"u";
      return data(lanes(c->getType())==1?value:type(c->getType())+"("+value+")");
    }
    if (auto *c = dyn_cast<ConstantFP>(v)) {
      if(half(c->getType())) {
        auto value="float16_t(unpackHalf2x16("+std::to_string(c->getValueAPF().bitcastToAPInt().getZExtValue())+"u).x)";
        return data(lanes(c->getType())==1?value:type(c->getType())+"("+value+")");
      }
      if (!scalar(c->getType())->isFloatTy())
        fail("only float32 constants are supported");
      auto text =
          "uintBitsToFloat(" +
          std::to_string(c->getValueAPF().bitcastToAPInt().getZExtValue()) +
          "u)";
      return data(lanes(c->getType()) == 1
                      ? text
                      : type(c->getType()) + "(" + text + ")");
    }
    if (isa<UndefValue>(v) || isa<PoisonValue>(v) ||
        isa<ConstantAggregateZero>(v))
      return zero(v->getType());
    if (auto *c = dyn_cast<Constant>(v)) {
      if (c->getType()->isVectorTy()) {
        std::string text = type(c->getType()) + "(";
        for (unsigned i = 0; i < lanes(c->getType()); ++i) {
          if (i)
            text += ",";
          text += get(c->getAggregateElement(i)).text;
        }
        return data(text + ")");
      }
      if (auto *s = dyn_cast<ConstantStruct>(c)) {
        AIRValue out;
        out.kind = AIRValue::Aggregate;
        for (auto &op : s->operands())
          out.members.push_back(get(op));
        return out;
      }
    }
    fail("unlowered value, global memory or unsupported constant");
  }
  std::string expr(const Value *v) {
    auto x = get(v);
    if (x.kind != AIRValue::Data)
      fail("resource/index escaped its supported operation");
    return x.text;
  }
  void assign(const Instruction &i, std::string text, unsigned linear = 0) {
    if (controlFlow && !linear) linear=values.at(&i).linear;
    auto x = controlFlow?storage.at(&i):data("v" + std::to_string(serial++));
    if (x.kind!=AIRValue::Data) fail("arithmetic result is not a data value");
    x.linear = linear;
    if(scalar(i.getType())->isIntegerTy() && !scalar(i.getType())->isIntegerTy(1))
      text=GPU::Compiler::airIntegerMask(text,scalar(i.getType())->getIntegerBitWidth(),lanes(i.getType()));
    body += (controlFlow?"":type(i.getType())+" ") + x.text + " = " + text + ";\n";
    values[&i] = x;
  }
  unsigned constantIndex(const Value *v, unsigned limit) {
    auto *i = dyn_cast<ConstantInt>(v);
    if (!i || i->getBitWidth() > 64 || i->getZExtValue() >= limit)
      fail("dynamic/out-of-range component index");
    return i->getZExtValue();
  }
  std::string component(AIRValue v, Type *t, unsigned index) {
    if (v.kind != AIRValue::Data || index >= lanes(t))
      fail("invalid vector component");
    return lanes(t) == 1 ? v.text
                         : "(" + v.text + ")[" + std::to_string(index) + "]";
  }
  std::string bitcast(std::string x, Type *from, Type *to) {
    if (from == to)
      return x;
    if(half(from) && scalar(to)->isIntegerTy(32) && lanes(from)==2*lanes(to)) {
      auto result=GPU::Compiler::airHalfBitcast(x,lanes(to),false);if(!result.empty())return result;
    }
    if(half(to) && scalar(from)->isIntegerTy(32) && lanes(to)==2*lanes(from)) {
      auto result=GPU::Compiler::airHalfBitcast(x,lanes(from),true);if(!result.empty())return result;
    }
    if (lanes(from) != lanes(to))
      fail("bitcast cannot change vector shape");
    if (scalar(from)->isFloatTy() && scalar(to)->isIntegerTy(32))
      return "floatBitsToUint(" + x + ")";
    if (scalar(to)->isFloatTy() && scalar(from)->isIntegerTy(32))
      return "uintBitsToFloat(" + x + ")";
    fail("unsupported bitcast");
  }
  std::pair<std::string, Type *> memory(const Value *pointer, Type *access,
                                        bool write) {
    auto p = get(pointer);
    if (p.kind==AIRValue::PrivatePointer) {
      auto *stored=cast<AllocaInst>(p.root)->getAllocatedType();
      if(access!=stored)fail("thread-local access type disagrees with allocation");
      return {p.text,stored};
    }
    if(p.kind==AIRValue::PrivateArray) {
      if((!scalar(access)->isIntegerTy(32) && !scalar(access)->isFloatTy()) || lanes(access)>4)
        fail("private array access requires int32/float32 scalar/vector values");
      auto *raw=lanes(access)==1?i32:static_cast<Type *>(FixedVectorType::get(i32,lanes(access)));
      return {write?p.text:GPU::Compiler::privateArrayLoad(privateArrays.at(p.root),p.text,lanes(access)),raw};
    }
    auto table=constantTables.find(p.root);
    if(p.kind==AIRValue::Pointer && table!=constantTables.end()) {
      auto *element=table->second.second;const auto stride=uint64_t(layout.getTypeAllocSize(element));
      if(write || (access!=element && access!=scalar(element)))fail("invalid constant table access");
      auto out=table->second.first+"(("+p.text+").x/"+std::to_string(stride)+"u)";
      if(access!=element)out+="[(("+p.text+").x%"+std::to_string(stride)+"u)/"+std::to_string(uint64_t(layout.getTypeStoreSize(access)))+"u]";
      return {out,access};
    }
    auto it = bindings.find(p.root);
    if (p.kind != AIRValue::Pointer || it == bindings.end())
      fail("memory access is not a declared buffer");
    auto &b = it->second;
    if ((!floating(access) && !scalar(access)->isIntegerTy(32) && !scalar(access)->isIntegerTy(8) && !scalar(access)->isIntegerTy(16)) || lanes(access)>4)
      fail("buffer access requires float16/float32 or int32 scalar/vector values");
    if (write && (b.readonly || stage == 1))
      fail("store to readonly or vertex buffer");
    bool dynamic=b.dynamicSlot!=UINT32_MAX;
    if (dynamic && !dynamicIncluded) {declarations+=GPU::Compiler::dynamicThreadgroupHeader();dynamicIncluded=true;}
    if (write && !b.sharedWords && !dynamic)
      writeMask |= 1u << b.slot;
    if (!b.sharedWords && !dynamic) {
      bufferMask |= 1u << b.slot;
      if (p.index == 2) instanceMask |= 1u << b.slot;
    }
    if(scalar(access)->isIntegerTy(8) || scalar(access)->isIntegerTy(16)) {
      if(write || b.sharedWords || dynamic)fail("packed integer loads require device or constant buffers");
      return {GPU::Compiler::narrowBufferLoad("_qbuffer"+std::to_string(b.slot)+".data",p.text,
          scalar(access)->getIntegerBitWidth(),lanes(access)),access};
    }
    if(half(access)) {
      if(b.sharedWords || dynamic)fail("half threadgroup memory is not implemented");
      auto array="_qhalfbuffer"+std::to_string(b.slot)+".data";
      if(halfBuffers.insert(b.slot).second)declarations+="layout(std430,binding="+std::to_string(b.slot)+") "+
          (b.readonly?"readonly ":"")+"buffer QHalfBuffer"+std::to_string(b.slot)+" { float16_t data[]; } _qhalfbuffer"+std::to_string(b.slot)+";\n";
      return {write?p.text:GPU::Compiler::halfBufferLoad(array,p.text,lanes(access)),access};
    }
    Type *stored=lanes(access)==1?i32:FixedVectorType::get(i32,lanes(access));
    return {write?p.text:GPU::Compiler::wordLoad(dynamic?"_qdynamic":b.sharedWords?"_qshared"+std::to_string(b.slot):"_qbuffer"+std::to_string(b.slot)+".data",p.text,lanes(access),b.sharedWords!=0,b.dynamicSlot),stored};
  }
  std::string atomic(const Value *pointer,const std::string &op,const std::string &value="0u",const std::string &compare="0u",bool sign=false) {
    auto p=get(pointer);if (p.kind!=AIRValue::Pointer) fail("atomic pointer is not device/threadgroup memory");
    memory(pointer,i32,true);const auto &b=bindings.at(p.root);
    bool dynamic=b.dynamicSlot!=UINT32_MAX;
    std::string array=dynamic?"_qdynamic":b.sharedWords?"_qshared"+std::to_string(b.slot):"_qbuffer"+std::to_string(b.slot)+".data";
    std::string word=dynamic?"_q_dynamic_word("+std::to_string(b.dynamicSlot)+"u,"+p.text+",0u)":(b.sharedWords?"_q_shared_word(":"_q_word(")+p.text+",0u)";
    if (sign && (op=="min" || op=="max")) {
      std::string function=(dynamic?std::string("_qdynamic"):(b.sharedWords?"_qshared":"_qbuffer")+std::to_string(b.slot))+"_s"+op;
      if (atomicHelpers.insert(function).second) declarations+=GPU::Compiler::signedAtomicHelper(function,array,op);
      return function+"("+word+",uint("+value+"))";
    }
    if (dynamic) {
      auto function="_qdynamic_atomic_"+op;
      if (atomicHelpers.insert(function).second) declarations+=GPU::Compiler::dynamicAtomicHelper(function,op);
      return function+"("+word+",uint("+value+"),uint("+compare+"))";
    }
    return GPU::Compiler::atomicExpression(op,array+"["+word+"]",value,compare);
  }
  void gep(GetElementPtrInst &i) {
    auto p = get(i.getPointerOperand());
    if (p.kind != AIRValue::Pointer && p.kind!=AIRValue::PrivateArray)
      fail("GEP base is not a buffer");
    SmallMapVector<Value *, APInt, 4> variables;
    APInt constant(64, 0);
    if (!i.collectOffset(layout, 64, variables, constant))
      fail("unrepresentable buffer address");
    uint64_t off=constant.getZExtValue();
    std::string address=p.text;
    for (auto &v : variables) {
      auto index = get(v.first);
      if ((index.kind!=AIRValue::Data && index.kind!=AIRValue::WideIndex) || lanes(v.first->getType())!=1 ||
          (!v.first->getType()->isIntegerTy(32) && index.kind!=AIRValue::WideIndex)) fail("buffer indices require scalar int32 values or their address extensions");
      uint64_t scale=v.second.getZExtValue();if (scale>UINT32_MAX) fail("oversized byte buffer indices are unsupported");
      if (index.linear==2) p.index=2;
      auto extended=index.kind==AIRValue::WideIndex?index.text:GPU::Compiler::addressExtend(index.text,true);
      address="_q_addr_add("+address+",_q_addr_mul("+extended+","+std::to_string(scale)+"u))";
    }
    p.text="_q_addr_add("+address+","+GPU::Compiler::addressConstant(off)+")";
    values[&i] = p;
  }
  void compare(CmpInst &i) {
    Type *t = i.getOperand(0)->getType();
    std::string a = expr(i.getOperand(0)), b = expr(i.getOperand(1));
    std::string op, fn;
    bool sign = false;
    auto predicate = i.getPredicate();
    if (isa<FCmpInst>(&i)) {
      if(!floating(t))fail("floating comparison requires float16/float32 elements");
      auto value=GPU::Compiler::airFloatComparison(unsigned(predicate),a,b,lanes(t));
      if(value.empty())fail("invalid floating comparison");assign(i,value);return;
    } else {
      sign = ICmpInst::isSigned(predicate);
      switch (predicate) {
      case CmpInst::ICMP_EQ:
        op = "==";
        fn = "equal";
        break;
      case CmpInst::ICMP_NE:
        op = "!=";
        fn = "notEqual";
        break;
      case CmpInst::ICMP_ULT:
      case CmpInst::ICMP_SLT:
        op = "<";
        fn = "lessThan";
        break;
      case CmpInst::ICMP_ULE:
      case CmpInst::ICMP_SLE:
        op = "<=";
        fn = "lessThanEqual";
        break;
      case CmpInst::ICMP_UGT:
      case CmpInst::ICMP_SGT:
        op = ">";
        fn = "greaterThan";
        break;
      case CmpInst::ICMP_UGE:
      case CmpInst::ICMP_SGE:
        op = ">=";
        fn = "greaterThanEqual";
        break;
      default:
        fail("unsupported integer predicate");
      }
      if (sign) {
        a=GPU::Compiler::airSignedInteger(a,scalar(t)->getIntegerBitWidth(),lanes(t));
        b=GPU::Compiler::airSignedInteger(b,scalar(t)->getIntegerBitWidth(),lanes(t));
      }
    }
    assign(i, lanes(t) == 1 ? "(" + a + op + b + ")"
                            : fn + "(" + a + "," + b + ")");
  }
  void call(CallInst &i) {
    auto *function = i.getCalledFunction();
    if (!function || !function->isDeclaration())
      fail("indirect calls and user helper functions require inlining before "
           "AIR import");
    StringRef name = function->getName();
    if (name.starts_with("llvm.dbg.") || name.starts_with("llvm.lifetime."))
      return;
    for(unsigned width=2;width<=4;++width)for(bool all:{false,true})
      if(name==std::string(all?"air.all.v":"air.any.v")+std::to_string(width)+"i1") {
        if(i.arg_size()!=1 || !i.getType()->isIntegerTy(1) ||
            i.getArgOperand(0)->getType()!=FixedVectorType::get(i.getType(),width))fail("invalid boolean reduction signature");
        assign(i,std::string(all?"all(":"any(")+expr(i.getArgOperand(0))+")");return;
      }
    if(name=="air.discard_fragment") {
      if(stage!=2 || i.arg_size() || !i.getType()->isVoidTy())fail("invalid fragment discard intrinsic");
      // Keep earlier SSBO writes observable before helper-lane suppression.
      usesDiscard=true;body+="memoryBarrierBuffer(); demote;\n";return;
    }
    GPU::Compiler::AIRAtomic operation;
    if (GPU::Compiler::airAtomic(name.str(),operation)) {
      unsigned count=operation.op=="cas"?7:operation.op=="load"?4:5;
      if (stage==1 || i.arg_size()!=count) fail("invalid atomic intrinsic stage/signature");
      auto constant=[&](unsigned n,unsigned bits,uint64_t value) { auto *c=dyn_cast<ConstantInt>(i.getArgOperand(n));return c && c->getBitWidth()==bits && c->getZExtValue()==value; };
      unsigned order=operation.op=="cas"?3:operation.op=="load"?1:2;
      if (!constant(order,32,0) || (operation.op=="cas" && !constant(order+1,32,0)) ||
          !constant(count-2,32,operation.local?1:2) || !i.getArgOperand(count-1)->getType()->isIntegerTy(1) ||
          !isa<ConstantInt>(i.getArgOperand(count-1)) || !i.getArgOperand(0)->getType()->isPointerTy() ||
          i.getArgOperand(0)->getType()->getPointerAddressSpace()!=(operation.local?3u:1u)) fail("unsupported atomic order/scope/address space");
      if (operation.op=="store" ? !i.getType()->isVoidTy():!i.getType()->isIntegerTy(32)) fail("integer atomics require int32 return values");
      if (operation.op!="load" && !i.getArgOperand(operation.op=="cas"?2:1)->getType()->isIntegerTy(32)) fail("integer atomics require int32 operands");
      if (operation.op=="cas") {
        auto expected=get(i.getArgOperand(1));if (expected.kind!=AIRValue::PrivatePointer || !cast<AllocaInst>(expected.root)->getAllocatedType()->isIntegerTy(32)) fail("atomic expected pointer requires a thread-local int32 cell");
        std::string result=atomic(i.getArgOperand(0),"cas",expr(i.getArgOperand(2)),expected.text);
        assign(i,result);body+=expected.text+" = "+expr(&i)+";\n";
      } else {
        auto result=atomic(i.getArgOperand(0),operation.op,operation.op=="load"?"0u":expr(i.getArgOperand(1)),"0u",operation.sign);
        if (operation.op=="store") body+=result+";\n";else assign(i,result);
      }
      return;
    }
    if (name=="air.wg.barrier") {
      auto *flags=i.arg_size()==2?dyn_cast<ConstantInt>(i.getArgOperand(0)):nullptr;
      auto *scope=i.arg_size()==2?dyn_cast<ConstantInt>(i.getArgOperand(1)):nullptr;
      if (stage || !flags || flags->getBitWidth()!=32 || !scope || scope->getBitWidth()!=32 || !scope->isOne() || flags->getZExtValue()>7) fail("unsupported workgroup barrier flags/scope");
      body+=GPU::Compiler::workgroupBarrier(flags->getZExtValue());return;
    }
    if(name=="air.get_read_sampler") {
      if(i.arg_size() || !i.getType()->isPointerTy() || i.getType()->getPointerAddressSpace()!=2)fail("invalid AIR read sampler");
      AIRValue sampler;sampler.kind=AIRValue::ReadSampler;values[&i]=sampler;return;
    }
    using TextureCall=GPU::Compiler::AIRTextureCall;
    TextureCall textureCall;
    if(GPU::Compiler::airTextureOperation(name.str(),textureCall)) {
      auto &t=textureCall;
      if(!GPU::Compiler::airTextureLayout(t,unsigned(i.arg_size())))fail("unsupported texture signature");
      auto arg=[&](unsigned index) {return i.getArgOperand(index);};
      auto isInt=[&](unsigned index,unsigned bits) {return arg(index)->getType()->isIntegerTy(bits);};
      auto zero=[&](unsigned index) {auto *c=dyn_cast<Constant>(arg(index));return c && c->isNullValue();};
      auto immediate=[&](unsigned index,unsigned bits,uint64_t expected) {auto *c=dyn_cast<ConstantInt>(arg(index));return c && c->getBitWidth()==bits && c->getZExtValue()==expected;};
      auto handle=get(arg(0));
      if(handle.kind!=AIRValue::Texture || !imageBindings.count(handle.root))fail("texture operation requires a bound texture");
      const auto binding=imageBindings.at(handle.root);
      if(binding.shape!=t.shape)fail("texture intrinsic dimension disagrees with metadata");
      bool read=t.op==TextureCall::Read,write=t.op==TextureCall::Write,sample=t.op>=TextureCall::Sample;
      if((read || write || sample) && (binding.half!=t.half || binding.unsignedInteger!=t.unsignedInteger))fail("texture intrinsic precision disagrees with metadata");
      auto texelTypeMatches=[&](Type *valueType) {return lanes(valueType)==4 && (t.unsignedInteger?scalar(valueType)->isIntegerTy(32):(floating(valueType) && half(valueType)==t.half));};
      auto result=[&](const std::string &call) {
        if(auto *aggregate=dyn_cast<StructType>(i.getType())) {
          if(aggregate->getNumElements()!=2)fail("invalid texture result");
          auto *texelType=aggregate->getElementType(0);
          if(!texelTypeMatches(texelType) || !aggregate->getElementType(1)->isIntegerTy(8))fail("invalid texture result");
          // A resource read is an SSA value at this call, even when its
          // texel is extracted after a write or in divergent control flow.
          auto texel=controlFlow?storage.at(&i).members[0]:data("v"+std::to_string(serial++));
          body+=(controlFlow?"":type(texelType)+" ")+texel.text+" = "+(t.half?"f16vec4("+call+")":call)+";\n";
          AIRValue out;out.kind=AIRValue::Aggregate;out.members={texel,AIRValue()};out.members[1].kind=AIRValue::Unavailable;values[&i]=out;
        } else {if(!texelTypeMatches(i.getType()))fail("texture result requires matching float4/half4/uint4");assign(i,t.half?"f16vec4("+call+")":call);}
      };
      if(!read && !write && !sample) {
        if(!i.getType()->isIntegerTy(32) || (t.lod!=UINT32_MAX && !isInt(t.lod,32)))fail("invalid texture query type");
        assign(i,GPU::Compiler::airTextureQuery(binding,stage,t.op,t.lod==UINT32_MAX?"0u":expr(arg(t.lod))));return;
      }
      if(t.face!=UINT32_MAX && !isInt(t.face,32))fail("texture face requires int32");
      if(t.layer!=UINT32_MAX && !isInt(t.layer,32))fail("texture layer requires int32");
      if(t.offsetFlag!=UINT32_MAX && (!isInt(t.offsetFlag,1) || !isa<ConstantInt>(arg(t.offsetFlag))))fail("texture offset mode requires a constant bool");
      std::string offset;
      if(t.offset!=UINT32_MAX) {
        if(arg(t.offset)->getType()!=FixedVectorType::get(i32,2))fail("texture offset requires int2");
        if(!zero(t.offset)) {
          auto *constant=dyn_cast<Constant>(arg(t.offset));
          auto *x=constant?dyn_cast_or_null<ConstantInt>(constant->getAggregateElement(0u)):nullptr;
          auto *y=constant?dyn_cast_or_null<ConstantInt>(constant->getAggregateElement(1u)):nullptr;
          if(!sample || !x || !y || x->getSExtValue()<-8 || x->getSExtValue()>7 || y->getSExtValue()<-8 || y->getSExtValue()>7)
            fail("sample offsets require constant int2 components in -8..7; read planar offsets remain zero");
          if(t.offsetFlag==UINT32_MAX || !immediate(t.offsetFlag,1,1))fail("nonzero sample offset requires the offset flag");
          offset="ivec2("+std::to_string(x->getSExtValue())+","+std::to_string(y->getSExtValue())+")";
        }
      }
      unsigned access=binding.access==4?0:binding.access;
      if(!immediate(t.access,32,access))fail("texture access operand disagrees with metadata");
      if(read || write) {
        if(arg(t.coord)->getType()!=FixedVectorType::get(i32,2) || !isInt(t.lod,32) ||
            (write?(stage || !(binding.access&2)):(binding.access!=4 && !(binding.access&1))))fail("unsupported texture coordinates/access/stage");
        if(t.currentRead && get(arg(1)).kind!=AIRValue::ReadSampler)fail("read requires air.get_read_sampler");
        if(read && (stage || binding.access==4) && fetchSlots.insert(binding.slot).second)
          declarations+=stage?GPU::Compiler::graphicsFetchFunctions(binding.slot,binding.shape,stage):
              binding.shape&2?GPU::Compiler::cubeImageFunctions(binding.slot,1,binding.shape&1):GPU::Compiler::imageFunctions(binding.slot,1,binding.shape&1);
        auto call=GPU::Compiler::airTextureFunction(binding,stage)+(read?"_read(":"_write(");
        if(write) {auto *valueType=arg(t.value)->getType();if(!texelTypeMatches(valueType) || !i.getType()->isVoidTy())fail("texture write requires matching float4/half4/uint4 and void");call+=(t.half?"vec4("+expr(arg(t.value))+")":expr(arg(t.value)))+",";}
        call+="uvec2("+expr(arg(t.coord))+"),";
        if(t.face!=UINT32_MAX)call+=expr(arg(t.face))+",";
        if(t.layer!=UINT32_MAX)call+=expr(arg(t.layer))+",";
        call+=expr(arg(t.lod))+")";
        if(write)body+=call+";\n";else result(call);return;
      }
      bool gradient=t.op==TextureCall::Gradient,cube=binding.shape&2;
      auto sampler=get(arg(1));
      if(binding.access!=4 || sampler.kind!=AIRValue::Sampler || !samplerBindings.count(sampler.root) ||
          arg(t.coord)->getType()!=FixedVectorType::get(f32,cube?3:2) || arg(t.minLod)->getType()!=f32 || !zero(t.minLod))fail("unsupported texture sampling operands");
      unsigned samplerSlot=samplerBindings.at(sampler.root);bool level=false;
      const auto *literal=samplerSlot>=QGPU_STAGE_SAMPLER_COUNT?&constantSamplers.at(samplerSlot-QGPU_STAGE_SAMPLER_COUNT):nullptr;
      if(cube && literal && !literal->normalized)fail("cube sampling requires normalized coordinates");
      if(gradient) {
        if(arg(t.dx)->getType()!=arg(t.coord)->getType() || arg(t.dy)->getType()!=arg(t.coord)->getType())fail("invalid texture gradients");
      } else {
        if(!isInt(t.levelFlag,1) || !isa<ConstantInt>(arg(t.levelFlag)) || arg(t.lod)->getType()!=f32)fail("invalid texture LOD mode/value");
        level=cast<ConstantInt>(arg(t.levelFlag))->isOne();
        if(stage!=2 && !level && !zero(t.lod))fail("nonfragment implicit sample requires zero bias");
      }
      if(samplePairs.insert(binding.slot*32+samplerSlot+(level?256:0)).second)
        declarations+=GPU::Compiler::sampleDeclaration(binding.slot,samplerSlot,level,binding.shape&1,stage,cube,literal);
      result(GPU::Compiler::airTextureSample(binding,samplerSlot,stage,gradient,level,expr(arg(t.coord)),
          t.layer==UINT32_MAX?"":expr(arg(t.layer)),gradient?"":stage!=2 && !level?"0.0":expr(arg(t.lod)),
          gradient?expr(arg(t.dx)):"",gradient?expr(arg(t.dy)):"",literal,offset));return;
    }
    if(name=="air.pack.unorm4x8.v4f16" || name=="air.pack.unorm4x8.v4f32") {
      bool useHalf=name.ends_with("v4f16");
      if(i.arg_size()!=1 || !i.getType()->isIntegerTy(32) || !floating(i.getArgOperand(0)->getType()) ||
          half(i.getArgOperand(0)->getType())!=useHalf || lanes(i.getArgOperand(0)->getType())!=4)fail("invalid unorm4x8 pack signature");
      assign(i,"packUnorm4x8(vec4("+expr(i.getArgOperand(0))+"))");return;
    }
    if (name.starts_with("air.convert.")) {
      SmallVector<StringRef, 8> bits;
      name.split(bits, '.');
      if (bits.size() != 6 || i.arg_size() != 1 ||
          lanes(i.getType()) != lanes(i.getArgOperand(0)->getType()) ||
          lanes(i.getType()) > 4)
        fail("unsupported AIR conversion");
      bool signIn = bits[4] == "s", signOut = bits[2] == "s";
      if (!((bits[2] == "f" || bits[2] == "s" || bits[2] == "u") &&
            (bits[4] == "f" || bits[4] == "s" || bits[4] == "u")))
        fail("unsupported AIR conversion signedness");
      auto *inputConstant=dyn_cast<ConstantInt>(i.getArgOperand(0));
      bool narrow=inputConstant && (inputConstant->getBitWidth()==8 || inputConstant->getBitWidth()==16);
      auto spelling = [](Type *t, bool isFloat,bool allowNarrow) {
        if (isFloat ? !floating(t) : !(scalar(t)->isIntegerTy(32) || scalar(t)->isIntegerTy(1) || allowNarrow))
          fail("AIR conversion type disagrees with its intrinsic name");
        return isFloat?GPU::Compiler::airFloatSuffix(half(t),lanes(t)):(lanes(t) == 1 ? std::string()
                              : "v" + std::to_string(lanes(t))) +
               "i"+std::to_string(scalar(t)->getIntegerBitWidth());
      };
      if (bits[3] != spelling(i.getType(), bits[2] == "f",false) ||
          bits[5] != spelling(i.getArgOperand(0)->getType(), bits[4] == "f",narrow))
        fail("AIR conversion mangling disagrees with its types");
      std::string value = expr(i.getArgOperand(0));
      if (signIn)
        value = narrow?std::to_string(inputConstant->getSExtValue()):type(i.getArgOperand(0)->getType(), true) + "(" + value + ")";
      std::string target = type(i.getType(), signOut);
      std::string out = target + "(" + value + ")";
      if (signOut)
        out = type(i.getType()) + "(" + out + ")";
      assign(i, out);
      return;
    }
    if(name=="air.fast_sincos.f32" || name=="air.sincos.f32") {
      if(i.arg_size()!=2 || !i.getType()->isFloatTy() || i.getArgOperand(0)->getType()!=i.getType() ||
          get(i.getArgOperand(1)).kind!=AIRValue::PrivatePointer)fail("invalid sincos signature");
      auto output=memory(i.getArgOperand(1),i.getType(),true);
      body+=output.first+" = cos("+expr(i.getArgOperand(0))+");\n";
      assign(i,"sin("+expr(i.getArgOperand(0))+")");return;
    }
    std::string extraMath;
    auto extraCount=GPU::Compiler::airExtraMath(name.str(),GPU::Compiler::airFloatSuffix(half(i.getType()),lanes(i.getType())),extraMath);
    if(extraCount) {
      if(i.arg_size()!=extraCount || !floating(i.getType()) || lanes(i.getType())>4)fail("invalid math intrinsic signature");
      std::vector<std::string> args;
      for(auto &argument:i.args()) {
        if(argument->getType()!=i.getType())fail("math intrinsic operand/result types disagree");
        args.push_back(expr(argument));
      }
      const bool noNaNs=name.starts_with("air.fast_") || (f.hasFnAttribute("no-nans-fp-math") &&
          f.getFnAttribute("no-nans-fp-math").getValueAsString()=="true");
      assign(i,GPU::Compiler::airExtraMathExpression(extraMath,args,half(i.getType()),lanes(i.getType()),noNaNs));return;
    }
    std::string fn;
    unsigned count = 1;
    if(name=="air.saturate."+GPU::Compiler::airFloatSuffix(half(i.getType()),lanes(i.getType())) ||
        name=="air.fast_saturate."+GPU::Compiler::airFloatSuffix(half(i.getType()),lanes(i.getType()))) {
      if(i.arg_size()!=1 || !floating(i.getType()) || i.getArgOperand(0)->getType()!=i.getType())fail("invalid saturate signature");
      auto t=type(i.getType());assign(i,"clamp("+expr(i.getArgOperand(0))+","+t+"(0),"+t+"(1))");return;
    }
    for(const auto &p:std::map<std::string,std::string>{{"dfdx","dFdx"},{"dfdy","dFdy"},{"fwidth","fwidth"}})
      if(name=="air."+p.first+"."+GPU::Compiler::airFloatSuffix(half(i.getType()),lanes(i.getType()))) {
        if(stage!=2 || i.arg_size()!=1 || !floating(i.getType()) || i.getArgOperand(0)->getType()!=i.getType())fail("invalid fragment derivative signature");
        auto value=expr(i.getArgOperand(0));if(half(i.getType()))value=GPU::Compiler::airFloatType(false,lanes(i.getType()))+"("+value+")";
        value=p.second+"("+value+")";assign(i,half(i.getType())?type(i.getType())+"("+value+")":value);return;
      }
    auto matches = [&](StringRef base) {
      if (!name.starts_with((base + ".").str()))
        return false;
      auto suffix = name.drop_front(base.size() + 1);
      auto *t=i.arg_size()?i.getArgOperand(0)->getType():i.getType();
      return suffix==GPU::Compiler::airFloatSuffix(half(t),lanes(t));
    };
    for (auto pair : {std::pair<const char *, const char *>{"sqrt", "sqrt"},
                      {"rsqrt", "inversesqrt"},
                      {"fabs", "abs"},
                      {"floor", "floor"},
                      {"ceil", "ceil"},
                      {"trunc", "trunc"},
                      {"fract", "fract"},
                      {"sin", "sin"},
                      {"cos", "cos"},
                      {"exp2", "exp2"},
                      {"log2", "log2"},
                      {"normalize", "normalize"}})
      if (matches(std::string("air.") + pair.first) ||
          matches(std::string("air.fast_") + pair.first) ||
          matches(std::string("llvm.") + pair.first))
        fn = pair.second;
    for (auto pair : {std::pair<const char *, const char *>{"fmin", "min"},
                      {"fmax", "max"},
                      {"pow", "pow"},
                      {"dot", "dot"}})
      if (matches(std::string("air.") + pair.first) ||
          matches(std::string("air.fast_") + pair.first)) {
        fn = pair.second;
        count = 2;
      }
    if (matches("air.fma") || matches("llvm.fma")) {
      fn = "fma";
      count = 3;
    }
    if (matches("air.mix") || matches("air.fast_mix")) {
      fn = "mix";
      count = 3;
    }
    if (matches("air.clamp") || matches("air.fast_clamp")) {
      fn = "clamp";
      count = 3;
    }
    if (fn.empty() || i.arg_size() != count ||
        !floating(i.getType()))
      fail("unsupported AIR/LLVM intrinsic: " + name);
    std::string text = fn + "(";
    for (unsigned j = 0; j < count; ++j) {
      if (j)
        text += ",";
      if (!floating(i.getArgOperand(j)->getType()) || half(i.getArgOperand(j)->getType())!=half(i.getType()))
        fail("invalid math argument");
      text += expr(i.getArgOperand(j));
    }
    assign(i, text + ")");
  }
  void instruction(Instruction &i) {
    if (isa<AllocaInst>(&i)) { get(&i);return; }
    if (auto *rmw=dyn_cast<AtomicRMWInst>(&i)) {
      if (!i.getType()->isIntegerTy(32) || rmw->getOrdering()!=AtomicOrdering::Monotonic) fail("only relaxed int32 atomicrmw is supported");
      const std::map<AtomicRMWInst::BinOp,std::pair<std::string,bool>> ops={
        {AtomicRMWInst::Xchg,{"xchg",false}},{AtomicRMWInst::Add,{"add",false}},{AtomicRMWInst::Sub,{"sub",false}},
        {AtomicRMWInst::And,{"and",false}},{AtomicRMWInst::Or,{"or",false}},{AtomicRMWInst::Xor,{"xor",false}},
        {AtomicRMWInst::Min,{"min",true}},{AtomicRMWInst::Max,{"max",true}},{AtomicRMWInst::UMin,{"min",false}},{AtomicRMWInst::UMax,{"max",false}}};
      auto op=ops.find(rmw->getOperation());if (op==ops.end()) fail("unsupported atomicrmw operation");
      assign(i,atomic(rmw->getPointerOperand(),op->second.first,expr(rmw->getValOperand()),"0u",op->second.second));return;
    }
    if (auto *cas=dyn_cast<AtomicCmpXchgInst>(&i)) {
      if (!cas->getCompareOperand()->getType()->isIntegerTy(32) || cas->getSuccessOrdering()!=AtomicOrdering::Monotonic ||
          cas->getFailureOrdering()!=AtomicOrdering::Monotonic) fail("only relaxed int32 cmpxchg is supported");
      auto wanted=expr(cas->getCompareOperand()),old="_qold"+std::to_string(serial++);
      body+="uint "+old+" = "+atomic(cas->getPointerOperand(),"cas",expr(cas->getNewValOperand()),wanted)+";\n";
      AIRValue result;result.kind=AIRValue::Aggregate;result.members={data(old),data("("+old+"=="+wanted+")")};values[&i]=result;return;
    }
    if (auto *g = dyn_cast<GetElementPtrInst>(&i)) {
      gep(*g);
      return;
    }
    if (auto *l = dyn_cast<LoadInst>(&i)) {
      if (l->isAtomic()) {
        if (!l->getType()->isIntegerTy(32) || (l->getOrdering()!=AtomicOrdering::Monotonic && l->getOrdering()!=AtomicOrdering::Unordered)) fail("only relaxed int32 atomic loads are supported");
        assign(i,atomic(l->getPointerOperand(),"load"));return;
      }
      if (l->isVolatile()) fail("non-atomic volatile load unsupported");
      auto ref = memory(l->getPointerOperand(), l->getType(), false);
      assign(i, bitcast(ref.first, ref.second, l->getType()));
      return;
    }
    if (auto *s = dyn_cast<StoreInst>(&i)) {
      if (s->isAtomic()) {
        if (!s->getValueOperand()->getType()->isIntegerTy(32) || (s->getOrdering()!=AtomicOrdering::Monotonic && s->getOrdering()!=AtomicOrdering::Unordered)) fail("only relaxed int32 atomic stores are supported");
        body+=atomic(s->getPointerOperand(),"store",expr(s->getValueOperand()))+";\n";return;
      }
      if (s->isVolatile()) fail("non-atomic volatile store unsupported");
      auto ref =
          memory(s->getPointerOperand(), s->getValueOperand()->getType(), true);
      auto p=get(s->getPointerOperand());
      if (p.kind==AIRValue::PrivatePointer) { body+=p.text+" = "+expr(s->getValueOperand())+";\n";return; }
      if(p.kind==AIRValue::PrivateArray) {body+=GPU::Compiler::privateArrayStore(privateArrays.at(p.root),ref.first,
          bitcast(expr(s->getValueOperand()),s->getValueOperand()->getType(),ref.second),lanes(s->getValueOperand()->getType()));return;}
      const auto &b=bindings.at(p.root);
      if(half(s->getValueOperand()->getType()))body+=GPU::Compiler::halfBufferStore("_qhalfbuffer"+std::to_string(b.slot)+".data",ref.first,expr(s->getValueOperand()),lanes(s->getValueOperand()->getType()));
      else body+=GPU::Compiler::wordStore(b.dynamicSlot!=UINT32_MAX?"_qdynamic":b.sharedWords?"_qshared"+std::to_string(b.slot):"_qbuffer"+std::to_string(b.slot)+".data",ref.first,
          bitcast(expr(s->getValueOperand()),s->getValueOperand()->getType(),ref.second),lanes(s->getValueOperand()->getType()),b.sharedWords!=0,b.dynamicSlot);
      return;
    }
    if (auto *b = dyn_cast<BinaryOperator>(&i)) {
      std::string op;
      bool sign = false;
      auto *t = scalar(i.getType());
      switch (b->getOpcode()) {
      case Instruction::Add:
      case Instruction::FAdd:
        op = "+";
        break;
      case Instruction::Sub:
      case Instruction::FSub:
        op = "-";
        break;
      case Instruction::Mul:
      case Instruction::FMul:
        op = "*";
        break;
      case Instruction::UDiv:
      case Instruction::FDiv:
        op = "/";
        break;
      case Instruction::SDiv:
        op = "/";
        sign = true;
        break;
      case Instruction::URem:
        op = "%";
        break;
      case Instruction::SRem:
        op = "%";
        sign = true;
        break;
      case Instruction::Shl:
        op = "<<";
        break;
      case Instruction::LShr:
        op = ">>";
        break;
      case Instruction::AShr:
        op = ">>";
        sign = true;
        break;
      case Instruction::And:
        op = "&";
        break;
      case Instruction::Or:
        op = "|";
        break;
      case Instruction::Xor:
        op = "^";
        break;
      default:
        fail("unsupported binary opcode");
      }
      if (t->isIntegerTy(1)) {
        auto value=GPU::Compiler::booleanBinary(op,expr(b->getOperand(0)),expr(b->getOperand(1)),lanes(i.getType()));
        if (value.empty()) fail("i1 arithmetic supports and/or/xor");assign(i,value);return;
      }
      if (!floating(t) && !t->isIntegerTy(32) && !t->isIntegerTy(8) && !t->isIntegerTy(16))
        fail("binary arithmetic requires float16/float32 or int32 elements");
      std::string a = expr(b->getOperand(0)), c = expr(b->getOperand(1));
      if (sign) {
        a=GPU::Compiler::airSignedInteger(a,t->getIntegerBitWidth(),lanes(i.getType()));
        c=GPU::Compiler::airSignedInteger(c,t->getIntegerBitWidth(),lanes(i.getType()));
      }
      std::string out = "(" + a + op + c + ")";
      if (sign)
        out = type(i.getType()) + "(" + out + ")";
      assign(i, out);
      return;
    }
    if (i.getOpcode() == Instruction::FNeg) {
      assign(i, "(-" + expr(i.getOperand(0)) + ")");
      return;
    }
    if (auto *c = dyn_cast<CastInst>(&i)) {
      auto source = get(c->getOperand(0));
      Type *from = c->getOperand(0)->getType(), *to = c->getType();
      if ((c->getOpcode() == Instruction::ZExt || c->getOpcode()==Instruction::SExt) && (from->isIntegerTy(32) || from->isIntegerTy(8) || from->isIntegerTy(16)) &&
          to->isIntegerTy(64)) {
        if(from->getIntegerBitWidth()<32 && c->getOpcode()==Instruction::SExt)
          source.text=GPU::Compiler::airSignedInteger(source.text,from->getIntegerBitWidth(),1);
        source.text=GPU::Compiler::addressExtend(source.text,c->getOpcode()==Instruction::SExt);
        source.kind = AIRValue::WideIndex;
        values[&i] = source;
        return;
      }
      if(c->getOpcode()==Instruction::BitCast && from->isPointerTy() && to->isPointerTy() &&
          from->getPointerAddressSpace()==to->getPointerAddressSpace()) {values[&i]=source;return;}
      if (c->getOpcode() == Instruction::BitCast) {
        assign(i, bitcast(expr(c->getOperand(0)), from, to), source.linear);
        return;
      }
      bool fromSigned = false, toSigned = false;
      switch (c->getOpcode()) {
      case Instruction::SExt:
      case Instruction::SIToFP:
        fromSigned = true;
        break;
      case Instruction::FPToSI:
        toSigned = true;
        break;
      case Instruction::UIToFP:
      case Instruction::FPToUI:
      case Instruction::FPTrunc:
      case Instruction::FPExt:
      case Instruction::ZExt:
      case Instruction::Trunc:
        break;
      default:
        fail("unsupported cast");
      }
      if (lanes(from) != lanes(to))
        fail("cast changes vector shape");
      std::string out = expr(c->getOperand(0));
      if (fromSigned)
        out=GPU::Compiler::airSignedInteger(out,scalar(from)->getIntegerBitWidth(),lanes(from));
      out = type(to, toSigned) + "(" + out + ")";
      if (toSigned)
        out = type(to) + "(" + out + ")";
      assign(i, out);
      return;
    }
    if (auto *e = dyn_cast<ExtractElementInst>(&i)) {
      unsigned index = constantIndex(e->getIndexOperand(),
                                     lanes(e->getVectorOperand()->getType()));
      auto x = get(e->getVectorOperand());
      assign(i, component(x, e->getVectorOperand()->getType(), index),
             index == 0 ? x.linear : 0);
      return;
    }
    if (auto *e = dyn_cast<InsertElementInst>(&i)) {
      unsigned index = constantIndex(e->getOperand(2), lanes(i.getType()));
      std::string out = type(i.getType()) + "(";
      for (unsigned n = 0; n < lanes(i.getType()); ++n) {
        if (n)
          out += ",";
        out += n == index ? expr(e->getOperand(1))
                          : component(get(e->getOperand(0)), i.getType(), n);
      }
      assign(i, out + ")");
      return;
    }
    if (auto *s = dyn_cast<ShuffleVectorInst>(&i)) {
      unsigned width = lanes(s->getOperand(0)->getType());
      std::string out = type(i.getType()) + "(";
      for (unsigned j = 0; j < lanes(i.getType()); ++j) {
        if (j)
          out += ",";
        int index = s->getMaskValue(j);
        out += index < 0
                   ? type(scalar(i.getType())) + "(0)"
                   : component(get(s->getOperand(index >= int(width) ? 1 : 0)),
                               s->getOperand(0)->getType(),
                               unsigned(index) % width);
      }
      assign(i, out + ")");
      return;
    }
    if (auto *s = dyn_cast<SelectInst>(&i)) {
      std::string cond = expr(s->getCondition()), a = expr(s->getTrueValue()),
                  b = expr(s->getFalseValue());
      assign(i, lanes(s->getCondition()->getType()) == 1
                    ? "(" + cond + "?" + a + ":" + b + ")"
                    : "mix(" + b + "," + a + "," + cond + ")");
      return;
    }
    if (auto *c = dyn_cast<CmpInst>(&i)) {
      compare(*c);
      return;
    }
    if (auto *e = dyn_cast<ExtractValueInst>(&i)) {
      auto x = get(e->getAggregateOperand());
      for (auto index : e->getIndices()) {
        if (x.kind != AIRValue::Aggregate || index >= x.members.size())
          fail("invalid aggregate extraction");
        x = x.members[index];
      }
      values[&i] = x;
      return;
    }
    if (auto *e = dyn_cast<InsertValueInst>(&i)) {
      auto x = get(e->getAggregateOperand());
      AIRValue *part = &x;
      for (auto index : e->getIndices()) {
        if (part->kind != AIRValue::Aggregate || index >= part->members.size())
          fail("invalid aggregate insertion");
        part = &part->members[index];
      }
      *part = get(e->getInsertedValueOperand());
      values[&i] = x;
      return;
    }
    if (auto *c = dyn_cast<CallInst>(&i)) {
      call(*c);
      return;
    }
    if (auto *r = dyn_cast<ReturnInst>(&i)) {
      if (stage == 0 || (stage==2 && f.getReturnType()->isVoidTy())) {
        if (r->getReturnValue())
          fail("compute must return void");
        return;
      }
      if (!r->getReturnValue())
        fail("graphics shader must return its output");
      if(isa<PoisonValue>(r->getReturnValue()) && std::any_of(outputTargets.begin(),outputTargets.end(),[](const auto &s){return !s.empty();}))
        fail("active graphics outputs are poison after specialization");
      auto x = get(r->getReturnValue());
      Type *t = r->getReturnValue()->getType();
      if (x.kind!=AIRValue::Aggregate) { AIRValue aggregate;aggregate.kind=AIRValue::Aggregate;aggregate.members.push_back(x);x=std::move(aggregate); }
      if (x.members.size()!=outputTargets.size()) fail("graphics output/metadata count mismatch");
      for (unsigned n=0;n<x.members.size();++n) {
        if(outputTargets[n].empty())continue;
        Type *member=t->isStructTy()?cast<StructType>(t)->getElementType(n):t;
        bool mask=outputTargets[n]=="gl_SampleMask[0]";
        if (x.members[n].kind!=AIRValue::Data || (mask?!member->isIntegerTy(32):(!floating(member) || lanes(member)>4))) fail("invalid graphics output type");
        auto value=x.members[n].text;
        if(outputTargets[n].rfind("_qcolor",0)==0)value=GPU::Compiler::airColorValue(value,half(member),lanes(member));
        else if(half(member))value=GPU::Compiler::airFloatType(false,lanes(member))+"("+value+")";
        body+=outputTargets[n]+" = "+(mask?"int(":"")+value+(mask?")":"")+";\n";
      }
      return;
    }
    fail("unsupported instruction: " + Twine(i.getOpcodeName()));
  }

  AIRValue reserve(Type *t,unsigned depth=0) {
    if (depth>8) fail("aggregate nesting limit exceeded");
    AIRValue value;
    if (auto *aggregate=dyn_cast<StructType>(t)) {
      if (aggregate->getNumElements()>64) fail("aggregate member limit exceeded");
      value.kind=AIRValue::Aggregate;
      for (auto *member:aggregate->elements()) value.members.push_back(reserve(member,depth+1));
    } else if (floating(t) || scalar(t)->isIntegerTy(32) || (!depth && (scalar(t)->isIntegerTy(8) || scalar(t)->isIntegerTy(16))) || scalar(t)->isIntegerTy(1)) {
      value.text="v"+std::to_string(serial++);globals+=type(t)+" "+value.text+";\n";
    } else value.kind=AIRValue::Unavailable;
    return value;
  }
  void copies(const AIRValue &target,const AIRValue &source,Type *t,std::vector<AIRFlow::Copy> &out) {
    if (target.kind==AIRValue::Unavailable) return;
    if (target.kind==AIRValue::Aggregate) {
      if (source.kind!=AIRValue::Aggregate || target.members.size()!=source.members.size()) fail("aggregate SSA shape mismatch");
      for (unsigned n=0;n<target.members.size();++n) copies(target.members[n],source.members[n],cast<StructType>(t)->getElementType(n),out);
    } else {
      if (source.kind!=AIRValue::Data) fail("pointer/resource cannot be copied into a data PHI");
      if (target.text!=source.text) out.push_back({type(t),target.text,source.text});
    }
  }
  void materialize(Instruction &i) {
    auto slot=storage.find(&i);if (slot==storage.end()) return;
    auto value=values.at(&i);std::vector<AIRFlow::Copy> assignments;copies(slot->second,value,i.getType(),assignments);
    for (const auto &copy:assignments) body+=copy.target+" = "+copy.value+";\n";
    auto stored=slot->second;stored.linear=value.linear;values[&i]=std::move(stored);
  }
  void flowBody() {
    std::vector<BasicBlock *> blocks;std::map<BasicBlock *,unsigned> ids;
    for (auto &b:f) { ids[&b]=unsigned(blocks.size());blocks.push_back(&b); }
    std::vector<AIRFlow::Block> graph(blocks.size());std::set<BasicBlock *> live;
    std::vector<BasicBlock *> pending{blocks.front()};
    for (size_t at=0;at<pending.size();++at) {
      auto *b=pending[at];if (!live.insert(b).second) continue;
      auto *term=b->getTerminator();
      if (!term || (!isa<ReturnInst>(term) && !isa<CondBrInst>(term) && !isa<UncondBrInst>(term) && !isa<SwitchInst>(term)))
        fail("unsupported AIR CFG terminator");
      if (term->getNumSuccessors()>129) fail("successor limit exceeded");
      for (unsigned n=0;n<term->getNumSuccessors();++n) {
        auto *next=term->getSuccessor(n);if (!ids.count(next)) fail("foreign basic block");pending.push_back(next);
      }
    }
    std::vector<Instruction *> instructions;
    for (auto *b:blocks) if (live.count(b)) for (auto &i:*b) {
      instructions.push_back(&i);auto *t=i.getType();
      if (isa<PHINode>(i) && (t->isPointerTy() || t->isIntegerTy(64))) fail("pointer/address PHIs are unsupported");
      if (t->isVoidTy() || t->isPointerTy() || t->isIntegerTy(64)) continue;
      auto slot=reserve(t);values[&i]=slot;storage[&i]=std::move(slot);
    }
    auto linear=[&](const Value *v) { auto it=values.find(v);return it==values.end()?0u:it->second.linear; };
    for (size_t pass=0;pass<instructions.size();++pass) {
      bool changed=false;
      for (auto *i:instructions) {
        unsigned proof=0;
        if (isa<BitCastInst>(i) || isa<ZExtInst>(i)) proof=linear(i->getOperand(0));
        else if (auto *extract=dyn_cast<ExtractElementInst>(i)) {
          auto *index=dyn_cast<ConstantInt>(extract->getIndexOperand());if (index && index->isZero()) proof=linear(extract->getVectorOperand());
        } else if (auto *select=dyn_cast<SelectInst>(i)) {
          if (linear(select->getTrueValue())==linear(select->getFalseValue())) proof=linear(select->getTrueValue());
        } else if (auto *phi=dyn_cast<PHINode>(i)) {
          for (auto &incoming:phi->incoming_values()) {
            auto value=linear(incoming);if (!value || (proof && value!=proof)) { proof=0;break; }proof=value;
          }
        }
        if (proof && storage.count(i) && !linear(i)) { values[i].linear=proof;changed=true; }
      }
      if (!changed) break;
    }
    auto prologue=body;
    for (auto *b:blocks) if (live.count(b)) {
      auto &block=graph[ids.at(b)];auto *term=b->getTerminator();body.clear();
      for (auto &i:*b) {
        if (&i==term || isa<PHINode>(i)) continue;
        instruction(i);materialize(i);
      }
      if (isa<ReturnInst>(term)) { instruction(*term);block.term=AIRFlow::Block::Return; }
      else if (auto *branch=dyn_cast<CondBrInst>(term)) {
        block.term=AIRFlow::Block::Conditional;block.condition=expr(branch->getCondition());
        block.destination=ids.at(branch->getSuccessor(0));block.alternate=ids.at(branch->getSuccessor(1));
      } else if (isa<UncondBrInst>(term)) { block.term=AIRFlow::Block::Jump;block.destination=ids.at(term->getSuccessor(0)); }
      else {
        auto *choice=cast<SwitchInst>(term);auto *t=choice->getCondition()->getType();
        if (!t->isIntegerTy(32) && !t->isIntegerTy(16) && !t->isIntegerTy(8) && !t->isIntegerTy(1)) fail("switch requires an integer/bool value");
        block.term=AIRFlow::Block::Switch;block.condition=expr(choice->getCondition());block.destination=ids.at(choice->getDefaultDest());
        for (auto c:choice->cases()) block.cases.push_back({uint32_t(c.getCaseValue()->getZExtValue()),ids.at(c.getCaseSuccessor())});
      }
      block.code=std::move(body);
    }
    for (auto *b:blocks) if (live.count(b)) for (auto &phi:b->phis()) {
      std::map<BasicBlock *,const Value *> incoming;
      for (unsigned n=0;n<phi.getNumIncomingValues();++n) {
        auto *pred=phi.getIncomingBlock(n);if (!live.count(pred)) continue;
        auto *value=phi.getIncomingValue(n);auto old=incoming.find(pred);
        if (old!=incoming.end()) { if (old->second!=value) fail("inconsistent duplicate PHI edge");continue; }
        incoming[pred]=value;copies(storage.at(&phi),get(value),phi.getType(),graph[ids.at(b)].incoming[ids.at(pred)]);
      }
    }
    std::string lowered,error;if (!AIRFlow::emit(graph,0,lowered,error)) fail(error);body=prologue+globals+lowered;
  }

public:
  Lower(Function &function, unsigned shaderStage)
      : f(function), layout(f.getParent()->getDataLayout()),
        stage(shaderStage) {
    f32 = Type::getFloatTy(f.getContext());
    i32 = Type::getInt32Ty(f.getContext());
  }
  json::Object run(MDNode *entry) {
    if (f.isDeclaration() || f.size()>128 || f.getInstructionCount()>4096)
      fail("entry must have at most 128 blocks and 4096 instructions (inline helper functions before AIR import)");
    usesHalf=containsHalf(f.getReturnType());
    for(const auto &arg:f.args())usesHalf|=containsHalf(arg.getType());
    for(const auto &block:f)for(const auto &i:block)usesHalf|=containsHalf(i.getType());
    if (entry->getNumOperands() != 3)
      fail("unsupported entry metadata layout");
    auto *outputs = node(entry->getOperand(1)),
         *args = node(entry->getOperand(2));
    if (stage == 0) {
      if (!f.getReturnType()->isVoidTy() || outputs->getNumOperands())
        fail("invalid compute result");
    } else {
      Type *result=f.getReturnType();auto *aggregate=dyn_cast<StructType>(result);
      unsigned members=result->isVoidTy()?0:aggregate?aggregate->getNumElements():1;
      if (outputs->getNumOperands()!=members || (stage==1 && !members) || (stage==2 && members>QGPU_COLOR_TARGET_COUNT+2)) fail("graphics output/metadata count mismatch");
      bool position=false,depth=false,sampleMask=false;std::set<unsigned> colors;
      for (unsigned i=0;i<members;++i) {
        Type *t=aggregate?aggregate->getElementType(i):result;auto *output=node(outputs->getOperand(i));
        if(!enabled(output)){outputTargets.emplace_back();continue;}
        if(has(output,"air.raster_order_group"))fail("raster-order groups require fragment interlock support");
        if(has(output,stage==1?"air.position":"air.render_target")) {
          if(!floating(t) || lanes(t)>4 || stringField(output,"air.arg_type_name")!=GPU::Compiler::airFloatName(half(t),lanes(t)) ||
              (stage==1 && (half(t) || lanes(t)!=4 || position)))fail("invalid position/color output");
          position=true;
          if(stage==1)outputTargets.push_back("gl_Position");
          else {
            unsigned target=0;for(;target<output->getNumOperands();++target) {
              auto *key=dyn_cast_or_null<MDString>(output->getOperand(target));if(key && key->getString()=="air.render_target")break;
            }
            if(target+2>=output->getNumOperands() || number(output->getOperand(target+1))>=QGPU_COLOR_TARGET_COUNT || number(output->getOperand(target+2)))fail("color target must be 0..7, dual-source index zero");
            unsigned slot=number(output->getOperand(target+1));if(!colors.insert(slot).second)fail("duplicate fragment color target");
            auto name=GPU::Compiler::fragmentColorName(slot);outputTargets.push_back(name);
            declarations+="layout(location="+std::to_string(slot)+") out vec4 "+name+";\n";
          }
        } else if(stage==2 && has(output,"air.sample_mask")) {
          if(sampleMask || t!=i32 || stringField(output,"air.arg_type_name")!="uint")fail("invalid/duplicate sample mask output");
          sampleMask=true;outputTargets.push_back("gl_SampleMask[0]");
        } else if(stage==2 && has(output,"air.depth")) {
          auto qualifier=stringField(output,"air.depth_qualifier");
          if(depth || t!=f32 || stringField(output,"air.arg_type_name")!="float" ||
             (qualifier!="air.any" && qualifier!="air.less" && qualifier!="air.greater"))fail("invalid/duplicate fragment depth output");
          depth=true;outputTargets.push_back("gl_FragDepth");
          if(qualifier!="air.any")declarations+="layout(depth_"+qualifier.substr(4)+") out float gl_FragDepth;\n";
        } else if (stage==1 && has(output,"air.vertex_output")) outputTargets.push_back(varying(output,t,true));
        else fail("unsupported graphics output semantic");
      }
      if (stage==1 && !position) fail("missing position output");
    }
    std::set<unsigned> seen, slots, threadgroupSlots;
    std::set<std::string> computeInputs;
    GPU::Compiler::GraphicsTextureBindings textureSlots;
    if(stage) {
      for(auto &operand:args->operands()) {
        auto *desc=node(operand);
        if(enabled(desc) && has(desc,"air.texture"))
          if(!textureSlots.add(number(field(desc,"air.location_index"))))fail("invalid/duplicate texture binding or more than eight enabled textures");
      }
      textureSlots.prepare();
    }
    for (auto &operand : args->operands()) {
      auto *desc = node(operand);
      if(has(desc,"air.raster_order_group"))fail("raster-order groups require fragment interlock support");
      const auto computeInput=GPU::Compiler::computeInputSemantic([&](const char *s) { return has(desc,std::string("air.")+s); });
      if (desc->getNumOperands() < 2)
        fail("truncated argument metadata");
      uint64_t n = number(desc->getOperand(0));
      if (n >= f.arg_size() || !seen.insert(unsigned(n)).second)
        fail("invalid/duplicate argument index");
      Argument *arg = f.getArg(unsigned(n));
      AIRValue x;
      if(!enabled(desc)){x.kind=AIRValue::Unavailable;values[arg]=x;continue;}
      if (has(desc, "air.buffer")) {
        auto *location = field(desc, "air.location_index");
        if (!location)
          fail("missing buffer binding");
        auto slot = number(location);
        if (slot >= 8 || !arg->getType()->isPointerTy())
          fail("unsupported buffer slot/type/stage");
        unsigned space = arg->getType()->getPointerAddressSpace();
        if ((space != 1 && space != 2 && space != 3) || (space==3 && stage) ||
            !(space==3?threadgroupSlots:slots).insert(slot).second)
          fail("invalid or duplicate device/constant/threadgroup binding");
        if (layout.getPointerSizeInBits(space) != 64)
          fail("buffer address space must use 64-bit AIR pointers");
        if (auto *addressSpace = field(desc, "air.address_space"))
          if (number(addressSpace) != space)
            fail("buffer address-space metadata disagrees with its pointer");
        auto kind = stringField(desc, "air.arg_type_name");
        Buffer b;
        b.slot = slot;
        b.readonly = has(desc, "air.read") || space == 2;
        if (space==3) {if (b.readonly) fail("readonly threadgroup arguments are unsupported");b.dynamicSlot=slot;}
        if (!b.readonly && !has(desc, "air.write") &&
            !has(desc, "air.read_write"))
          fail("buffer access metadata missing");
        if (stage == 1 && !b.readonly)
          fail("vertex buffers must be readonly");
        unsigned components=GPU::Compiler::bufferComponents(kind);
        if (components) {
          b.stride=(components==3?4:components)*(kind.rfind("half",0)==0?2:4);
          b.valueType=components==1?i32:FixedVectorType::get(i32,components);
          if (auto *size=field(desc,"air.arg_type_size")) if (number(size)!=b.stride) fail("AIR buffer layout disagrees with metadata");
        } else if (!kind.empty() && field(desc,"air.arg_type_size") && number(field(desc,"air.arg_type_size")) &&
            number(field(desc,"air.arg_type_size"))<=4096) { b.valueType=i32;b.stride=4; }
        else fail("unsupported AIR buffer type: "+Twine(kind));
        bindings[arg] = b;
        if (space!=3) strides[slot] = b.stride;
        x.kind = AIRValue::Pointer;
        x.root = arg;x.text="uvec2(0u)";
        if (space!=3) declarations += "layout(std430,binding=" + std::to_string(slot) + ") " +
                        (b.readonly ? "readonly " : "") + "buffer QBuffer" +
                        std::to_string(slot) + " { uint" +
                        " data[]; } _qbuffer" + std::to_string(slot) + ";\n";
      } else if (has(desc,"air.texture")) {
        auto *location=field(desc,"air.location_index");auto slot=location?number(location):uint64_t(8);
        if(stage)slot=textureSlots.physical(unsigned(slot));
        GPU::Compiler::AIRTextureBinding binding;binding.slot=unsigned(slot);
        if(slot>=QGPU_STAGE_TEXTURE_COUNT || !GPU::Compiler::airTextureMetadata(stringField(desc,"air.arg_type_name"),binding) ||
            (stage && (binding.unsignedInteger || (binding.access!=4 && binding.access!=1))) || !arg->getType()->isPointerTy() || arg->getType()->getPointerAddressSpace()!=1)fail("unsupported texture metadata");
        for(const auto &bound:imageBindings)if(bound.second.slot==slot)fail("duplicate texture slot");
        bool cube=binding.shape&2,array=binding.shape&1;
        if(stage) {if(graphicsHeaders.insert(binding.shape).second)declarations+=GPU::Compiler::graphicsTextureHeader(binding.shape,stage);}
        else if(cube) {if(cubeHeaders.insert(unsigned(array)).second)declarations+=GPU::Compiler::cubeImageHeader(array);}
        else if(!imageIncluded) {declarations+=GPU::Compiler::imageHeader();imageIncluded=true;}
        if(!stage && binding.access<4)declarations+=cube?GPU::Compiler::cubeImageFunctions(unsigned(slot),binding.access,array,binding.unsignedInteger):GPU::Compiler::imageFunctions(unsigned(slot),binding.access,array,false,binding.unsignedInteger);
        if(binding.access==4 && !samplerIncluded) {declarations+=GPU::Compiler::samplerHeader(stage==2,stage==1);samplerIncluded=true;}
        if(binding.unsignedInteger)imageUintMask|=1u<<slot;
        imageBindings[arg]=binding;x.kind=AIRValue::Texture;x.root=arg;
      } else if (has(desc,"air.sampler")) {
        auto *location=field(desc,"air.location_index");auto slot=location?number(location):uint64_t(8);
        if(slot>=QGPU_STAGE_SAMPLER_COUNT || !arg->getType()->isPointerTy() || arg->getType()->getPointerAddressSpace()!=2)fail("invalid sampler metadata");
        for(const auto &bound:samplerBindings)if(bound.second==slot)fail("duplicate sampler slot");
        samplerBindings[arg]=slot;x.kind=AIRValue::Sampler;x.root=arg;
      } else if (stage==1 && has(desc,"air.vertex_input")) {
        auto slot=number(field(desc,"air.location_index"));auto *t=arg->getType();
        auto kind=stringField(desc,"air.arg_type_name");bool sign=kind.rfind("int",0)==0;
        if (slot>=QGPU_MAX_ATTRIBUTES || !attributeSlots.insert((unsigned)slot).second ||
            (!floating(t) && !scalar(t)->isIntegerTy(32)) || lanes(t)>4 ||
            kind!=(floating(t)?GPU::Compiler::airFloatName(half(t),lanes(t)):(sign?"int":"uint")+(lanes(t)==1?std::string():std::to_string(lanes(t))))) fail("invalid vertex attribute type/location");
        std::string input="_qattrib"+std::to_string(slot);
        std::string inputType=floating(t)?GPU::Compiler::airFloatType(false,lanes(t)):lanes(t)==1?(sign?"int":"uint"):(sign?"ivec":"uvec")+std::to_string(lanes(t));
        declarations+="layout(location="+std::to_string(slot)+") in "+inputType+" "+input+";\n";
        x.text=sign || half(t)?type(t)+"("+input+")":input;
      } else if(stage==2 && has(desc,"air.render_target")) {
        unsigned at=0;while(at<desc->getNumOperands()) {auto key=dyn_cast_or_null<MDString>(desc->getOperand(at));if(key && key->getString()=="air.render_target")break;++at;}
        auto *t=arg->getType();
        if(at+1>=desc->getNumOperands() || !floating(t) || lanes(t)>4 ||
            stringField(desc,"air.arg_type_name")!=GPU::Compiler::airFloatName(half(t),lanes(t)))fail("invalid framebuffer input");
        auto slot=number(desc->getOperand(at+1));if(slot>=QGPU_COLOR_TARGET_COUNT || (framebufferMask&(1u<<slot)))fail("duplicate/out-of-range framebuffer input");
        if(!framebufferMask)declarations+=GPU::Compiler::framebufferHeader();framebufferMask|=1u<<slot;
        declarations+=GPU::Compiler::framebufferFunctions(slot);
        x.text="_qframebuffer"+std::to_string(slot)+"_read()";
        if(lanes(t)<4)x.text="("+x.text+")."+std::string("xyzw",lanes(t));
        if(half(t))x.text=type(t)+"("+x.text+")";
        auto input="_qframebuffer_input"+std::to_string(slot);
        framebufferInputs+=type(t)+" "+input+" = "+x.text+";\n";x.text=input;
      } else if (stage==2 && (has(desc,"air.sample_id") || has(desc,"air.sample_mask"))) {
        bool id=has(desc,"air.sample_id");auto key=id?"sample_id":"sample_mask";
        if(!arg->getType()->isIntegerTy(32) || stringField(desc,"air.arg_type_name")!="uint" || has(desc,"air.post_depth_coverage") ||
           (id && has(desc,"air.sample_mask")) || !computeInputs.insert(key).second)fail("invalid/duplicate fragment sample input");
        x.text=id?"uint(gl_SampleID)":"uint(gl_SampleMaskIn[0])";
      } else if (stage==2 && has(desc,"air.fragment_input")) {
        x.text=varying(desc,arg->getType(),false);
      } else if (stage==0 && !computeInput.empty()) {
        auto *t=arg->getType();const auto value=GPU::Compiler::computeSystemValue(computeInput);
        if (value.empty() || !computeInputs.insert(computeInput).second || !scalar(t)->isIntegerTy(32) ||
            (lanes(t)!=1 && ((lanes(t)!=2 && lanes(t)!=3) || computeInput=="thread_index_in_threadgroup"))) fail("invalid compute input type/semantic");
        x.text=value+(lanes(t)==1 && computeInput!="thread_index_in_threadgroup"?".x":"");
        if (lanes(t)==2) x.text="uvec2("+x.text+")";
        if (computeInput=="thread_position_in_grid") { haveSystem=true;x.linear=1; }
      } else if (has(desc, "air.thread_position_in_grid") ||
                 has(desc, "air.vertex_id")) {
        if (haveSystem ||
            !has(desc, stage == 0 ? "air.thread_position_in_grid"
                                  : "air.vertex_id") ||
            stage == 2)
          fail("invalid system index");
        auto *t = arg->getType();
        if (!(scalar(t)->isIntegerTy(32) || (stage==1 && scalar(t)->isIntegerTy(16))) || (lanes(t) != 1 && lanes(t) != 3))
          fail("invalid grid/vertex index type");
        if (stage == 1 && lanes(t) != 1)
          fail("vertex ID must be scalar");
        haveSystem = true;
        x.text = lanes(t) == 1 ? "_qindex" : "uvec3(_qindex,0u,0u)";
        if(scalar(t)->isIntegerTy(16))x.text=GPU::Compiler::airIntegerMask(x.text,16,1);
        x.linear = scalar(t)->isIntegerTy(32);
      } else if (has(desc, "air.instance_id") && stage == 1 && !haveInstance && arg->getType()->isIntegerTy(32)) {
        haveInstance = true;
        x.text = "_qinstance"; x.linear = 2;
      } else if (has(desc, "air.position") && stage == 2) {
        // Native AIR can carry both the ordinary and a conditional alias of
        // the same position builtin. Both refer to one hardware value.
        if (arg->getType() != FixedVectorType::get(f32, 4) || stringField(desc,"air.arg_type_name")!="float4")
          fail("invalid fragment position");
        haveSystem = true;
        x.text = "gl_FragCoord";
      } else if (arg->use_empty()) {
        // An unused unsupported interface argument still cannot change the
        // active stage linkage. Only dead system inputs are harmless.
        if (!has(desc, "air.instance_id"))
          fail("unsupported argument semantic");
        x = zero(arg->getType());
      } else
        fail("unsupported AIR argument semantic");
      values[arg] = x;
    }
    if (seen.size() != f.arg_size())
      fail("AIR metadata must describe every argument");
    declarations=GPU::Compiler::bufferHeader()+(usesHalf?GPU::Compiler::halfBufferHeader():std::string())+declarations;
    if (stage == 0) declarations=GPU::Compiler::computeHeader()+declarations;
    if (stage != 2)
      body = "uint _qindex = " +
             std::string(stage == 0 ? "_q_global_id().x"
                                    : "uint(gl_VertexID)") +
             ";\n";
    if (haveInstance) body += "uint _qinstance = uint(gl_InstanceID);\n";
    controlFlow=f.size()!=1;
    if (controlFlow) flowBody();
    else for (auto &instruction : f.getEntryBlock()) this->instruction(instruction);
    for(unsigned i=0;i<strides.size();++i)if(!(bufferMask&(1u<<i)))strides[i]=0;
    json::Array sizes,interface;
    for (const auto &v:varyings) {
      json::Array hash;for (auto word:v.semantic) hash.push_back(word);
      interface.push_back(json::Object{{"semantic",std::move(hash)},{"components",v.components},{"mode",v.mode}});
    }
    for (auto stride : strides)
      sizes.push_back(stride);
    return json::Object{{"name", f.getName().str()},
                        {"stage", stage},
                        {"extension", stage == 0   ? "comp"
                                      : stage == 1 ? "vert"
                                                   : "frag"},
                        {"bufferMask", bufferMask},
                        {"writeMask", writeMask},
                        {"instanceMask", instanceMask},
                        {"framebufferMask",framebufferMask},
                        {"imageUintMask",imageUintMask},
                        {"textureBindings",textureSlots.encoding()},
                        {"varyings",std::move(interface)},
                        {"strides", std::move(sizes)},
                        {"glsl", "#version 430\n" + std::string(usesHalf?"#extension GL_AMD_gpu_shader_half_float : require\n":"") + std::string(usesDiscard?"#extension GL_EXT_demote_to_helper_invocation : require\n":"") + declarations +
                                     "void main() {\n" + framebufferInputs + body + "}\n"}};
  }
};
int main(int argc, char **argv) {
  if(argc==2 && StringRef(argv[1])=="--library-index") {
    auto input=MemoryBuffer::getSTDIN();if(!input)fail(input.getError().message());
    auto bytes=(*input)->getBuffer();std::vector<GPU::Compiler::LibraryArchive::Function> functions;std::string error;
    if(!GPU::Compiler::LibraryArchive::parse(bytes.data(),bytes.size(),functions,error))fail(error);
    json::Array index;
    for(const auto &f:functions) {
      auto hash=SHA256::hash(ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(bytes.data()+f.offset),f.size));
      if(hash!=f.hash)fail("metallib AIR hash mismatch");
      std::string hex;for(uint8_t b:hash){hex+="0123456789abcdef"[b>>4];hex+="0123456789abcdef"[b&15];}
      index.push_back(json::Object{{"name",f.name},{"type",f.type},{"offset",int64_t(f.offset)},
          {"size",int64_t(f.size)},{"hash",hex}});
    }
    outs()<<formatv("{0}\n",json::Value(std::move(index)));return 0;
  }
  if (argc < 3) {
    errs()
        << "usage: air-frontend input.air|input.ll output.json [entry ...]\n";
    return 64;
  }
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseIRFile(argv[1], diagnostic, context);
  if (!module) {
    diagnostic.print("air-frontend", errs());
    return 1;
  }
  if (verifyModule(*module, &errs()))
    fail("LLVM verifier rejected AIR");
  auto triple = module->getTargetTriple().str();
  if (triple.rfind("air64", 0) != 0)
    fail("module is not AIR64");
  if (module->getDataLayoutStr().empty() ||
      module->getDataLayout().getPointerSizeInBits(1) != 64)
    fail("AIR64 data layout required");
  auto *version = module->getNamedMetadata("air.version");
  if (!version || version->getNumOperands() != 1 ||
      version->getOperand(0)->getNumOperands() != 3 ||
      number(version->getOperand(0)->getOperand(0)) != 2)
    fail("AIR major version 2 required");
  std::set<std::string> selected;
  std::vector<GPU::Compiler::FunctionConstantValue> constantValues;bool reflectionOnly=false,explicitConstants=false;
  for (int i = 3; i < argc; ++i) {
    if(StringRef(argv[i])=="--reflect-constants"){reflectionOnly=true;continue;}
    if(StringRef(argv[i])=="--function-constants") {
      explicitConstants=true;
      if(++i==argc)fail("missing function constant JSON path");
      auto data=MemoryBuffer::getFile(argv[i]);if(!data)fail(data.getError().message());
      auto parsed=json::parse((*data)->getBuffer());if(!parsed)fail(toString(parsed.takeError()));
      auto array=parsed->getAsArray();if(!array || array->size()>GPU::Compiler::MaxConstantValues)fail("invalid function constant array");
      for(const auto &item:*array) {
        auto object=item.getAsObject();if(!object)fail("invalid function constant record");
        auto type=object->getInteger("type");auto bytes=object->getString("data");
        if(!type || *type<0 || *type>UINT32_MAX || !bytes)fail("function constants require type and hexadecimal data");
        GPU::Compiler::FunctionConstantValue v;v.type=uint32_t(*type);
        unsigned size=GPU::Compiler::constantBytes(v.type);if(!size || bytes->size()!=size*2)fail("invalid function constant byte size");
        if(auto index=object->getInteger("index")){if(*index<0 || *index>65535)fail("constant index outside Metal range");v.index=uint32_t(*index);}
        else if(auto name=object->getString("name"))v.name=name->str();else fail("constant name or index required");
        auto digit=[](char c)->unsigned {if(c>='0' && c<='9')return c-'0';if(c>='a' && c<='f')return c-'a'+10;if(c>='A' && c<='F')return c-'A'+10;fail("invalid constant hexadecimal data");};
        for(unsigned j=0;j<size;++j)v.bytes[j]=uint8_t((digit((*bytes)[j*2])<<4)|digit((*bytes)[j*2+1]));constantValues.push_back(std::move(v));
      }
    }else if(!selected.insert(argv[i]).second)fail("duplicate selected entry");
  }
  try {
    if(module->getNamedMetadata("air.function_constants") && selected.size()>1)fail("function constant specialization selects one entry at a time");
    GPU::Compiler::AIRSpecialization::State state(wrap(module.get()),selected);
    if(reflectionOnly) {
      std::vector<GPU::Compiler::FunctionConstant> constants;state.inspect(constants);json::Array result;
      for(const auto &c:constants)result.push_back(json::Object{{"name",c.name},{"index",c.index},{"type",c.type},{"required",c.required}});
      std::error_code error;raw_fd_ostream out(argv[2],error);if(error)fail(error.message());out<<formatv("{0:2}\n",json::Value(std::move(result)));return 0;
    }
    if(state.present()) {
      // Embedded AOT aliases have no specialization metadata/key. Require an
      // explicit CLI value set rather than silently registering default code
      // that a later Metal constantValues request could mistake for a variant.
      if(!explicitConstants)fail("AIR function constants require --function-constants; embedded AOT aliases do not support specialization");
      state.apply(constantValues);
    }
    state.lowerHelpers();
  }catch(const std::exception &error){fail(error.what());}
  std::set<std::string> found;
  json::Array entries;
  for (unsigned stage = 0; stage < 3; ++stage)
    if (auto *metadata =
            module->getNamedMetadata(stage == 0   ? "air.kernel"
                                     : stage == 1 ? "air.vertex"
                                                  : "air.fragment")) {
      for (auto *entry : metadata->operands()) {
        if (!entry->getNumOperands())
          fail("truncated AIR entry metadata");
        auto *value = dyn_cast_or_null<ValueAsMetadata>(entry->getOperand(0));
        auto *f = value ? dyn_cast<Function>(value->getValue()) : nullptr;
        if (!f)
          fail("entry metadata does not name a function");
        auto name = f->getName().str();
        if (!selected.empty() && !selected.count(name))
          continue;
        if (!found.insert(name).second)
          fail("duplicate stage entry");
        entryName = name;
        entries.push_back(Lower(*f, stage).run(entry));
      }
    }
  if (entries.empty() || entries.size() > 64)
    fail("no supported stage entry selected, or more than 64 entries");
  if (!selected.empty() && found != selected)
    fail("selected entry is absent");
  std::error_code error;
  raw_fd_ostream out(argv[2], error);
  if (error)
    fail(error.message());
  out << formatv("{0:2}\n", json::Value(std::move(entries)));
  out.close();
  if (out.has_error())
    fail("cannot write AIR reflection");
  return 0;
}
