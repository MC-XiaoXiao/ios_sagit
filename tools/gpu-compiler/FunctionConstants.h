// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
namespace GPU { namespace Compiler {
// MTLDataType scalar/vector values, independent of Objective-C/LLVM layouts.
// Native Metal stores vector3 function constants tightly (float3=12 bytes).
struct ConstantShape {unsigned bits=0,lanes=0;bool floating=false,boolean=false;};
inline ConstantShape constantShape(uint32_t type) {
    struct Group {uint32_t first;unsigned bits;bool floating,boolean;};
    const Group groups[]={{3,32,true,false},{16,16,true,false},{29,32,false,false},{33,32,false,false},
        {37,16,false,false},{41,16,false,false},{45,8,false,false},{49,8,false,false},{53,8,false,true},
        {81,64,false,false},{85,64,false,false}};
    for(const auto &g:groups)if(type>=g.first && type<g.first+4)return {g.bits,type-g.first+1,g.floating,g.boolean};
    return {};
}
inline uint32_t constantType(std::string name) {
    unsigned lanes=1;
    if(!name.empty() && name.back()>='2' && name.back()<='4'){lanes=unsigned(name.back()-'0');name.pop_back();}
    const char *names[]={"float","half","int","uint","short","ushort","char","uchar","bool","long","ulong"};
    const uint32_t types[]={3,16,29,33,37,41,45,49,53,81,85};
    for(unsigned i=0;i<11;++i)if(name==names[i])return types[i]+lanes-1;
    return 0;
}
inline unsigned constantBytes(uint32_t type) {auto s=constantShape(type);return s.bits*s.lanes/8;}
struct FunctionConstant {std::string name;uint32_t index=0,type=0;bool required=false;};
struct FunctionConstantValue {
    std::string name;
    uint32_t index=UINT32_MAX,type=0;
    std::array<uint8_t,32> bytes{};
};
constexpr unsigned MaxFunctionConstants=256,MaxConstantValues=4096;
inline bool resolveFunctionConstants(const std::vector<FunctionConstant> &declarations,
    const std::vector<FunctionConstantValue> &input,std::vector<FunctionConstantValue> &effective,std::string &error) {
    effective.clear();auto fail=[&](const std::string &s){error=s;effective.clear();return false;};
    if(input.size()>MaxConstantValues)return fail("too many supplied function constants");
    std::map<uint32_t,const FunctionConstantValue *> indexed;std::map<std::string,const FunctionConstantValue *> named;
    for(const auto &v:input) {
        if(!constantBytes(v.type) || (v.index==UINT32_MAX?v.name.empty():v.index>65535))return fail("invalid supplied function constant");
        if(v.index!=UINT32_MAX){if(!indexed.emplace(v.index,&v).second)return fail("duplicate supplied constant index");}
        else if(!named.emplace(v.name,&v).second)return fail("duplicate supplied constant name");
    }
    for(const auto &d:declarations) {
        auto a=indexed.find(d.index);auto b=named.find(d.name);
        auto value=a!=indexed.end()?a->second:b!=named.end()?b->second:nullptr;
        if(!value){if(d.required)return fail("required function constant is missing: "+d.name);continue;}
        if(value->type!=d.type)return fail("function constant type mismatch: "+d.name);
        FunctionConstantValue v;v.index=d.index;v.type=d.type;
        auto size=constantBytes(d.type);std::copy_n(value->bytes.begin(),size,v.bytes.begin());
        if(constantShape(d.type).boolean)for(unsigned i=0;i<size;++i)v.bytes[i]=v.bytes[i]!=0;
        effective.push_back(std::move(v));
    }
    std::sort(effective.begin(),effective.end(),[](const auto &a,const auto &b){return a.index<b.index;});return true;
}
} }
