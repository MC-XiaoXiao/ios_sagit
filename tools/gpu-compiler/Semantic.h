// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <cstdint>
#include <cstring>
#include <GPUShaderInterface.h>
#include <GPUShaderProtocol.h>
#include <cmath>
#include <vector>
#include <initializer_list>
#include <string>
#include <map>
#include <set>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>
#endif
namespace GPU { namespace Compiler {
inline std::string airIntegerType(unsigned lanes,bool sign=false) {
    return lanes==1?(sign?"int":"uint"):(sign?"ivec":"uvec")+std::to_string(lanes);
}
inline std::string airIntegerMask(const std::string &value,unsigned bits,unsigned lanes) {
    if(bits!=8 && bits!=16)return value;
    return "("+value+" & "+airIntegerType(lanes)+"("+std::to_string((1u<<bits)-1)+"u))";
}
inline std::string airSignedInteger(const std::string &value,unsigned bits,unsigned lanes) {
    if(bits==1)return "(-"+airIntegerType(lanes,true)+"("+value+"))";
    if(bits==32)return airIntegerType(lanes,true)+"("+value+")";
    const auto shift=std::to_string(32-bits);
    return "("+airIntegerType(lanes,true)+"("+value+" << "+shift+"u) >> "+shift+")";
}
inline std::string airHalfBitcast(const std::string &value,unsigned lanes,bool toHalf) {
    if(lanes!=1 && lanes!=2)return {};
    if(toHalf)return lanes==1?"unpackFloat2x16("+value+")":
        "f16vec4(unpackFloat2x16(("+value+").x),unpackFloat2x16(("+value+").y))";
    return lanes==1?"packFloat2x16("+value+")":
        "uvec2(packFloat2x16(("+value+").xy),packFloat2x16(("+value+").zw))";
}
// Fixed private arrays are per invocation, not shared memory. Bound each raw
// word access before GLSL lowers an indirect index; invalid addresses cannot
// alias another local, device buffer, or padding lane.
inline std::string privateArrayHeader(const std::string &name,unsigned words) {
    auto limit=std::to_string(words)+"u";
    return "uint "+name+"["+std::to_string(words)+"];\n"
        "uint "+name+"_load(uvec2 address,uint component){uint word=_q_word(address,component);"
        "if(word>="+limit+")return 0u;return "+name+"[word];}\n"
        "void "+name+"_store(uvec2 address,uint component,uint value){uint word=_q_word(address,component);"
        "if(word<"+limit+")"+name+"[word]=value;}\n";
}
inline std::string privateArrayLoad(const std::string &name,const std::string &offset,unsigned lanes) {
    auto component=[&](unsigned lane){return name+"_load("+offset+","+std::to_string(lane)+"u)";};
    if(lanes==1)return component(0);
    std::string out=airIntegerType(lanes)+"(";
    for(unsigned lane=0;lane<lanes;++lane)out+=(lane?",":"")+component(lane);
    return out+")";
}
inline std::string privateArrayStore(const std::string &name,const std::string &offset,const std::string &value,unsigned lanes) {
    std::string out;
    for(unsigned lane=0;lane<lanes;++lane)out+=name+"_store("+offset+","+std::to_string(lane)+"u,"+
        (lanes==1?value:"("+value+")["+std::to_string(lane)+"]")+");\n";
    return out;
}
inline std::string narrowBufferLoad(const std::string &array,const std::string &offset,unsigned bits,unsigned lanes) {
    auto component=[&](unsigned n) {
        auto address="_q_addr_add("+offset+",uvec2("+std::to_string(n*(bits/8))+"u,0u))";
        auto aligned="uvec2(("+address+").x & ~3u,("+address+").y)";
        auto low="("+array+"[_q_word("+aligned+",0u)] >> ((("+address+").x & 3u)*8u))";
        if(bits==16)low="("+low+" | ((("+address+").x & 3u)==3u ? ("+array+"[_q_word(_q_addr_add("+aligned+",uvec2(4u,0u)),0u)] << 8u) : 0u))";
        return "("+low+" & "+std::to_string((1u<<bits)-1)+"u)";
    };
    if(lanes==1)return component(0);
    std::string value=airIntegerType(lanes)+"(";
    for(unsigned n=0;n<lanes;++n)value+=(n?",":"")+component(n);
    return value+")";
}
class GraphicsTextureBindings {
    std::set<unsigned> logical;
    std::map<unsigned,unsigned> slots;
    uint32_t packed=0;
public:
    bool add(uint64_t index) {
        return index<QGPU_METAL_TEXTURE_BINDING_COUNT && logical.insert(unsigned(index)).second &&
            logical.size()<=QGPU_STAGE_TEXTURE_COUNT;
    }
    void prepare() {
        unsigned occupied=0;
        for(auto index:logical)if(index<QGPU_STAGE_TEXTURE_COUNT){slots[index]=index;occupied|=1u<<index;}
        uint32_t encoded=0x76543210u;bool changed=false;
        for(auto index:logical)if(index>=QGPU_STAGE_TEXTURE_COUNT) {
            unsigned slot=0;while(occupied&(1u<<slot))++slot;
            slots[index]=slot;occupied|=1u<<slot;
            encoded=(encoded&~(15u<<(slot*4)))|(index<<(slot*4));changed=true;
        }
        packed=changed?encoded:0;
    }
    unsigned physical(unsigned index)const{return slots.at(index);}
    uint32_t encoding()const{return packed;}
};
inline std::string fragmentColorName(unsigned slot) {return slot?"_qcolor"+std::to_string(slot):"_qcolor";}
inline std::string colorZero(const std::string &format) {
    return "vec4(0.0,0.0,0.0,(("+format+")=="+std::to_string(Shader::R8)+"u || ("+format+")=="+std::to_string(Shader::RG8)+"u)?1.0:0.0)";
}
// Private descriptor aliases refer only to the kernel's per-primitive color
// snapshots. They never consume an application's Metal texture/sampler slot.
inline std::string framebufferHeader() {return "uniform uvec4 _qframebuffer_info[8];\n";}
inline std::string framebufferFunctions(unsigned slot) {
    auto n=std::to_string(slot),name="_qframebuffer"+n;
    return "layout(binding="+std::to_string(176+slot)+") uniform sampler2D "+name+";\n"+
        "layout(binding="+std::to_string(184+slot)+") uniform sampler2DMS _qframebuffer_ms"+n+";\n"+
        "vec4 "+name+"_read(){uvec4 info=_qframebuffer_info["+n+"];ivec2 pixel=ivec2(gl_FragCoord.xy);"
        "if(any(lessThan(pixel,ivec2(0))) || any(greaterThanEqual(uvec2(pixel),info.xy)))return "+colorZero("info.w")+";"
        "return info.z>1u?texelFetch(_qframebuffer_ms"+n+",pixel,gl_SampleID):texelFetch("+name+",pixel,0);}\n";
}
inline std::string bitcastHeader(const std::string &target,unsigned lanes) {
    const auto scalar=target.substr(0,target.size()-(lanes>1?1:0));
    auto type=[&](const std::string &s) {return lanes==1?s:(s=="float"?"vec":s=="uint"?"uvec":"ivec")+std::to_string(lanes);};
    std::string result;
    for (const std::string source:{"float","uint","int"}) {
        std::string value="v";
        if (scalar!=source) value=scalar=="float"?(source=="uint"?"uintBitsToFloat(v)":"intBitsToFloat(v)"):
            source=="float"?(scalar=="uint"?"floatBitsToUint(v)":"floatBitsToInt(v)"):type(scalar)+"(v)";
        result+=type(scalar)+" _q_as_"+target+"("+type(source)+" v){return "+value+";}\n";
    }
    return result;
}
inline std::string interpolationQualifier(unsigned mode) {
    static const char *const names[]={"smooth ","noperspective ","flat ",
        "centroid smooth ","centroid noperspective ","sample smooth ","sample noperspective "};
    return mode<7?names[mode]:"";
}
inline bool airInterpolation(bool flat,bool linear,bool perspective,bool center,bool centroid,bool sample,unsigned &mode) {
    if ((linear && perspective) || (unsigned(center)+unsigned(centroid)+unsigned(sample)>1) || (flat && (linear || perspective || center || centroid || sample))) return false;
    mode=flat?2:sample?5+unsigned(linear):centroid?3+unsigned(linear):unsigned(linear);return true;
}
inline QGPUConstantSampler defaultConstantSampler() {
    return {0,0,4095,0,1,0,1};
}
// A5xx LOD fields are unsigned 8.8. Large finite clamps saturate above every
// admitted mip chain; smaller values must be exactly representable.
inline bool constantSamplerLOD(double value,uint32_t &out) {
    if(!std::isfinite(value) || value<0)return false;
    if(value>=4095.0/256.0){out=4095;return true;}
    double fixed=value*256;if(fixed!=std::floor(fixed))return false;
    out=uint32_t(fixed);return true;
}
inline double samplerHalf(unsigned bits) {
    unsigned exponent=(bits>>10)&31,fraction=bits&1023;
    if(exponent==31)return NAN;
    double value=exponent?std::ldexp(double(1024+fraction),int(exponent)-25):std::ldexp(double(fraction),-24);
    return bits&32768?-value:value;
}
// Apple AIR one-word and two-word constexpr encodings. The second word's
// bias, comparison/reduction filters and unknown bits are rejected until the
// native backend can reproduce them; they never become raw A5xx state.
inline bool airConstantSampler(uint64_t word,uint64_t extra,QGPUConstantSampler &s) {
    if((word&(7ull<<60)) || extra)return false;
    unsigned border=(word>>56)&3,compare=(word>>16)&15,mag=(word>>9)&3,min=(word>>11)&3,mip=(word>>13)&3;
    if(border>2 || (compare!=0 && compare!=8) || mag>1 || min>1 || mip>2 || ((word>>58)&3))return false;
    s={min|(mag<<1)|(mip<<2),0,0,0,unsigned((word>>20)&15)+1,border,unsigned(!(word&(1ull<<15)))};
    for(unsigned axis=0;axis<3;++axis) {
        unsigned mode=(word>>(axis*3))&7;
        if(mode>3)return false;
        unsigned address=mode==0?(border?Shader::ClampToBorderColor:Shader::ClampToZero):mode==1?Shader::ClampToEdge:mode==2?Shader::Repeat:Shader::MirrorRepeat;
        s.addressModes|=address<<(axis*3);
    }
    double low=samplerHalf((word>>24)&65535),high=samplerHalf((word>>40)&65535);
    return low<=high && constantSamplerLOD(low,s.minLOD) && constantSamplerLOD(high,s.maxLOD) && Shader::validSampler(s);
}
inline unsigned internConstantSampler(std::vector<QGPUConstantSampler> &states,const QGPUConstantSampler &s) {
    for(unsigned i=0;i<states.size();++i)if(!std::memcmp(&states[i],&s,sizeof(s)))return i;
    if(states.size()==QGPU_MAX_CONSTANT_SAMPLERS)return UINT32_MAX;
    states.push_back(s);return unsigned(states.size()-1);
}
inline std::string sampleCoordinate(unsigned sampler,const std::string &p,const QGPUConstantSampler *literal) {
    if(!literal)return "_q_sample_coord("+std::to_string(sampler)+"u,"+p+")";
    unsigned flags=Shader::samplerMirrorFlags(*literal);
    return flags?"_q_sample_coord_flags("+std::to_string(flags)+"u,"+p+")":p;
}
inline std::string sampleGradientValue(const std::string &uniform,unsigned sampler,const std::string &raw,
    const std::string &dx,const std::string &dy,const QGPUConstantSampler *literal) {
    const auto flags=literal?std::to_string(Shader::samplerMirrorFlags(*literal))+"u":"_q_sampler_mirror("+std::to_string(sampler)+"u)";
    return "_q_sample_gradient_value("+uniform+","+flags+","+raw+","+dx+","+dy+")";
}
inline std::string sampleImplicit(const std::string &uniform,unsigned sampler,const std::string &raw,const std::string &bias,bool array,const QGPUConstantSampler *literal) {
    if(!literal)return "_q_sample_implicit("+uniform+","+std::to_string(sampler)+"u,"+raw+","+bias+")";
    if(!Shader::samplerMirrorFlags(*literal))return "texture("+uniform+","+raw+","+bias+")";
    (void)array;
    return "_q_sample_implicit_flags("+uniform+","+std::to_string(Shader::samplerMirrorFlags(*literal))+"u,"+raw+","+bias+")";
}
struct AIRTextureBinding { unsigned slot=0,access=0,shape=0;bool half=false,unsignedInteger=false; };
inline bool airTextureMetadata(std::string name,AIRTextureBinding &out) {
    // AIR's type spelling is semantic metadata, never a GPU address/handle.
    for(size_t i=0;i<name.size();) {
        if(name[i]==' ' || name[i]=='\t')name.erase(i,1);else ++i;
    }
    static const char *shapes[]={"texture2d","texture2d_array","texturecube","texturecube_array"};
    for(unsigned shape=0;shape<4;++shape)for(unsigned access=1;access<=4;++access) {
        const char *qualifier=access==1?"read":access==2?"write":access==3?"read_write":"sample";
        for(unsigned type=0;type<3;++type) {
            if(type==2 && access==4)continue; // Integer filtering has no lowering.
            std::string stem=std::string(shapes[shape])+(type==2?"<uint":type==1?"<half":"<float");
            if(name==stem+","+qualifier+">" || name==stem+",access::"+qualifier+">" || (access==4 && name==stem+">")) {
                out.shape=shape;out.access=access;out.half=type==1;out.unsignedInteger=type==2;return true;
            }
        }
    }
    return false;
}
// Layouts independently implemented from pinned public Apple AIR outputs.
// Current reads add get_read_sampler and a planar offset; legacy reads omit
// both. Every position is validated by each LLVM API adapter before lowering.
struct AIRTextureCall {
    enum Operation { Read,Write,Width,Height,MipLevels,ArraySize,Sample,Gradient } op=Read;
    unsigned shape=0,coord=UINT32_MAX,face=UINT32_MAX,layer=UINT32_MAX,value=UINT32_MAX,
        lod=UINT32_MAX,access=UINT32_MAX,offset=UINT32_MAX,offsetFlag=UINT32_MAX,
        levelFlag=UINT32_MAX,minLod=UINT32_MAX,dx=UINT32_MAX,dy=UINT32_MAX;
    bool currentRead=false,half=false,unsignedInteger=false;
};
inline bool airTextureOperation(const std::string &name,AIRTextureCall &out) {
    static const char *shapes[]={"2d","2d_array","cube","cube_array"};
    static const char *operations[]={"read","write","get_width","get_height","get_num_mip_levels","get_array_size","sample","sample"};
    for(unsigned shape=0;shape<4;++shape)for(unsigned op=0;op<8;++op) {
        if(op==AIRTextureCall::ArraySize && !(shape&1))continue;
        std::string candidate=std::string("air.")+operations[op]+"_texture_"+shapes[shape];
        if(op==AIRTextureCall::Gradient)candidate+="_grad";
        bool texel=op==AIRTextureCall::Read || op==AIRTextureCall::Write || op>=AIRTextureCall::Sample;
        bool unsignedInteger=(op==AIRTextureCall::Read || op==AIRTextureCall::Write) && name==candidate+".u.v4i32";
        if(name==candidate+(texel?".v4f32":"") || (texel && name==candidate+".v4f16") || unsignedInteger) {
            out.op=AIRTextureCall::Operation(op);out.shape=shape;out.half=texel && name==candidate+".v4f16";out.unsignedInteger=unsignedInteger;return true;
        }
    }
    return false;
}
inline bool airTextureLayout(AIRTextureCall &out,unsigned count) {
    if(out.op==AIRTextureCall::Width || out.op==AIRTextureCall::Height) {out.lod=1;return count==2;}
    if(out.op==AIRTextureCall::MipLevels || out.op==AIRTextureCall::ArraySize)return count==1;
    bool array=out.shape&1,cube=out.shape&2;
    unsigned at=1;
    if(out.op==AIRTextureCall::Read) {
        unsigned legacy=4+unsigned(array)+unsigned(cube),current=legacy+1+unsigned(!cube);
        if(count!=legacy && count!=current)return false;
        out.currentRead=count==current;if(out.currentRead)++at;
    } else if(out.op>=AIRTextureCall::Sample)++at;
    out.coord=at++;
    if(cube && out.op<AIRTextureCall::Sample)out.face=at++;
    if(array)out.layer=at++;
    if(out.op==AIRTextureCall::Write)out.value=at++;
    if(out.op==AIRTextureCall::Read && out.currentRead && !cube)out.offset=at++;
    if(out.op==AIRTextureCall::Sample) {
        if(!cube) {out.offsetFlag=at++;out.offset=at++;}
        out.levelFlag=at++;out.lod=at++;out.minLod=at++;
    } else if(out.op==AIRTextureCall::Gradient) {
        out.dx=at++;out.dy=at++;out.minLod=at++;
        if(!cube) {out.offsetFlag=at++;out.offset=at++;}
    } else out.lod=at++;
    out.access=at++;return count==at;
}
inline bool atomicOperation(const std::string &op) {
    return op=="load" || op=="store" || op=="xchg" || op=="cas" || op=="add" || op=="sub" ||
        op=="and" || op=="or" || op=="xor" || op=="min" || op=="max";
}
struct AIRAtomic { std::string op;bool local=false,sign=false; };
inline bool airAtomic(const std::string &name,AIRAtomic &out) {
    const std::string global="air.atomic.global.",local="air.atomic.local.";
    size_t start=name.rfind(global,0)==0?global.size():name.rfind(local,0)==0?local.size():0;
    if (!start) return false;
    out.local=name.rfind(local,0)==0;std::string suffix=name.substr(start);
    if (suffix=="cmpxchg.weak.i32" || suffix=="cmpxchg.strong.i32") out.op="cas";
    else if (suffix=="load.i32" || suffix=="store.i32" || suffix=="xchg.i32") out.op=suffix.substr(0,suffix.find('.'));
    else if (suffix.size()>6 && (suffix.substr(suffix.size()-6)==".s.i32" || suffix.substr(suffix.size()-6)==".u.i32")) {
        out.sign=suffix[suffix.size()-5]=='s';out.op=suffix.substr(0,suffix.size()-6);
    } else return false;
    return atomicOperation(out.op);
}
inline std::string atomicExpression(const std::string &op,const std::string &memory,
    const std::string &value="0u",const std::string &compare="0u") {
    if (op=="load") return "atomicOr("+memory+",0u)";
    if (op=="store" || op=="xchg") return "atomicExchange("+memory+",uint("+value+"))";
    if (op=="cas") return "atomicCompSwap("+memory+",uint("+compare+"),uint("+value+"))";
    if (op=="add" || op=="sub") return "atomicAdd("+memory+","+
        (op=="sub"?"(0u-uint("+value+"))":"uint("+value+")")+")";
    const std::string name=op=="and"?"And":op=="or"?"Or":op=="xor"?"Xor":op=="min"?"Min":op=="max"?"Max":"";
    return name.empty()?std::string():"atomic"+name+"("+memory+",uint("+value+"))";
}
// Signed min/max over a raw uint view retain exact bits and linearizability.
// Resource-specific helpers keep the actual SSBO/shared lvalue visible to GL.
inline std::string signedAtomicHelper(const std::string &function,const std::string &array,const std::string &op) {
    return "uint "+function+"(uint word,uint value) { "+(array=="_qdynamic"?"if(word>=8192u)return 0u;":"")+"uint old=atomicOr("+array+"[word],0u);\n"
        "while (int(old)"+(op=="min"?">":"<")+"int(value)) { uint seen=atomicCompSwap("+array+"[word],old,value);"
        "if (seen==old) break;old=seen;}return old;}\n";
}
inline std::string dynamicAtomicHelper(const std::string &function,const std::string &op) {
    return "uint "+function+"(uint word,uint value,uint compare) { if(word>=8192u)return 0u;return "+
        atomicExpression(op,"_qdynamic[word]","value","compare")+";}\n";
}
inline std::string workgroupBarrier(unsigned flags) {
    if (flags&~7u) return {}; // device buffers, threadgroup and linear images
    return std::string(flags&5u?"memoryBarrierBuffer();\n":"")+
        (flags&2u?"memoryBarrierShared();\n":"")+"barrier();\n";
}
inline std::string imageHeader() {
    return "uniform uvec4 _qimage_info[8];\nuniform uvec2 _qimage_array_info[8];\nuniform uvec4 _qimage_ms_info[8];\n"
        "uniform uvec4 _qimage_ms_array_info[8];\nuniform uvec2 _qimage_ms_array_layout[8];\n"
        "uint _q_image_cpp(uint format){return format=="+std::to_string(Shader::R8)+"u?1u:format=="+std::to_string(Shader::RG8)+"u?2u:format=="+std::to_string(Shader::RGBA16Float)+"u?8u:4u;}\n"
        "uint _q_image_shift(uint slot,uvec2 coord){return ((coord.x*_q_image_cpp(_qimage_info[slot].w&255u))&3u)*8u;}\n"
        "uint _q_ms_array_samples(uint slot){return 1u<<((_qimage_ms_array_info[slot].w>>28u)&3u);}\n"
        "uint _q_ms_array_layers(uint slot){return _qimage_ms_array_layout[slot].x;}\n"
        "uint _q_ms_array_word(uint slot,uvec2 coord,uint slice,uint sampleIndex){uvec4 p=_qimage_ms_array_info[slot];uint n=_q_ms_array_samples(slot);"
        "if(slice>=_q_ms_array_layers(slot) || sampleIndex>=n || any(greaterThanEqual(coord,p.xy)))return 0xffffffffu;"
        "return (slice*_qimage_ms_array_layout[slot].y+coord.y*p.z+(coord.x*n+sampleIndex)*_q_image_cpp(p.w&255u))/4u;}\n"
        "uint _q_ms_array_shift(uint slot,uvec2 coord,uint sampleIndex){return (((coord.x*_q_ms_array_samples(slot)+sampleIndex)*_q_image_cpp(_qimage_ms_array_info[slot].w&255u))&3u)*8u;}\n"
        "uint _q_ms_samples(uint slot){return 1u<<((_qimage_ms_info[slot].w>>28u)&3u);}\n"
        "uint _q_ms_word(uint slot,uvec2 coord,uint sampleIndex){uvec4 p=_qimage_ms_info[slot];uint n=_q_ms_samples(slot);"
        "if(sampleIndex>=n || any(greaterThanEqual(coord,p.xy)))return 0xffffffffu;return (coord.y*p.z+(coord.x*n+sampleIndex)*_q_image_cpp(p.w&255u))/4u;}\n"
        "uint _q_ms_shift(uint slot,uvec2 coord,uint sampleIndex){return (((coord.x*_q_ms_samples(slot)+sampleIndex)*_q_image_cpp(_qimage_ms_info[slot].w&255u))&3u)*8u;}\n"
        // Match the sRGB transfer function for RGB only. Image storage uses
        // guarded SSBOs, so it does not inherit hardware texture conversion.
        "vec3 _q_srgb_decode(vec3 s){return mix(s/12.92,pow((s+0.055)/1.055,vec3(2.4)),greaterThan(s,vec3(0.04045)));}\n"
        "vec3 _q_srgb_encode(vec3 c){return mix(c*12.92,1.055*pow(c,vec3(1.0/2.4))-0.055,greaterThan(c,vec3(0.0031308)));}\n"
        "float _q_channel(vec4 v,uint c){return c==0u?0.0:c==1u?1.0:c==2u?v.r:c==3u?v.g:c==4u?v.b:v.a;}\n"
        "vec4 _q_swizzle(vec4 v,uint s){if(s==0u)return v;s-=1u;return vec4(_q_channel(v,s&7u),_q_channel(v,(s>>3u)&7u),_q_channel(v,(s>>6u)&7u),_q_channel(v,(s>>9u)&7u));}\n"
        "uint _q_uint_channel(uvec4 v,uint c){return c==0u?0u:c==1u?1u:c==2u?v.r:c==3u?v.g:c==4u?v.b:v.a;}\n"
        "uvec4 _q_uint_swizzle(uvec4 v,uint s){if(s==0u)return v;s-=1u;return uvec4(_q_uint_channel(v,s&7u),_q_uint_channel(v,(s>>3u)&7u),_q_uint_channel(v,(s>>6u)&7u),_q_uint_channel(v,(s>>9u)&7u));}\n"
        "uvec2 _q_image_size(uint slot,uint level){uvec4 p=_qimage_info[slot];if(level>=((p.w>>8)&255u))return uvec2(0u);return max(p.xy>>level,uvec2(1u));}\n"
        "uint _q_image_word(uint slot,uvec2 coord,uint level){uvec4 p=_qimage_info[slot];uint levels=(p.w>>8)&255u;"
        "if(level>=levels)return 0xffffffffu;uvec2 size=max(p.xy>>level,uvec2(1u));"
        "if(any(greaterThanEqual(coord,size)))return 0xffffffffu;uint offset=0u,cpp=_q_image_cpp(p.w&255u),alignment=64u*cpp;"
        "for(uint i=0u;i<level;++i){uint pitch=(max(p.z>>i,1u)+alignment-1u)&~(alignment-1u);offset+=pitch*max(p.y>>i,1u);}"
        "uint pitch=levels>1u?(max(p.z>>level,1u)+alignment-1u)&~(alignment-1u):p.z;return (offset+coord.y*pitch+coord.x*cpp)/4u;}\n"
        "uvec2 _q_array_image_size(uint slot,uint level){return _qimage_array_info[slot].x>0u?_q_image_size(slot,level):uvec2(0u);}\n"
        "uint _q_array_image_mips(uint slot){return _qimage_array_info[slot].x>0u?((_qimage_info[slot].w>>8u)&255u):0u;}\n"
        "uint _q_array_image_word(uint slot,uvec2 coord,uint slice,uint level){uvec2 p=_qimage_array_info[slot];"
        "if(slice>=p.x)return 0xffffffffu;uint word=_q_image_word(slot,coord,level);"
        "return word==0xffffffffu?word:word+slice*(p.y/4u);}\n";
}
inline std::string samplerHeader(bool fragment=false,bool vertex=false) {
    std::string header="uniform uint _qsampler_info[8];\n"
        "vec2 _q_sample_coord(uint slot,vec2 p){uint f=_qsampler_info[slot];return vec2((f&1u)!=0u?abs(p.x):p.x,(f&2u)!=0u?abs(p.y):p.y);}\n"
        "vec2 _q_sample_gradient(uint slot,vec2 p,vec2 g){uint f=_qsampler_info[slot];return g*vec2((f&1u)!=0u && p.x<0.0?-1.0:1.0,(f&2u)!=0u && p.y<0.0?-1.0:1.0);}\n";
    header+="uint _q_sampler_mirror(uint slot){return _qsampler_info[slot]&3u;}\n"
        "vec2 _q_sample_coord_flags(uint flags,vec2 p){return vec2((flags&1u)!=0u?abs(p.x):p.x,(flags&2u)!=0u?abs(p.y):p.y);}\n";
    for(bool array:{false,true}) {
        const std::string sampler=array?"sampler2DArray":"sampler2D",point=array?"vec3":"vec2";
        const std::string xy=array?"p.xy":"p",coord=array?"vec3(c,p.z)":"c";
        const std::string parameters=sampler+" image,uint flags,"+point+" p";
        header+="vec4 _q_sample_gradient_value("+parameters+",vec2 dx,vec2 dy){vec2 q="+xy+";"
            "vec2 c=_q_sample_coord_flags(flags,q);vec2 direction=vec2((flags&1u)!=0u && q.x<0.0?-1.0:1.0,(flags&2u)!=0u && q.y<0.0?-1.0:1.0);"
            "return textureGrad(image,"+coord+",dx*direction,dy*direction);}\n";
        if(fragment)header+="vec4 _q_sample_implicit_flags("+parameters+",float bias){float scale=exp2(bias);"
            "return _q_sample_gradient_value(image,flags,p,dFdx("+xy+")*scale,dFdy("+xy+")*scale);}\n";
    }
    if(fragment)header+="vec4 _q_sample_implicit(sampler2D image,uint slot,vec2 p,float bias){float scale=exp2(bias);"
        "vec2 dx=dFdx(p)*scale,dy=dFdy(p)*scale;if((_qsampler_info[slot]&3u)==0u)return texture(image,p,bias);"
        "return textureGrad(image,_q_sample_coord(slot,p),"
        "_q_sample_gradient(slot,p,dx),_q_sample_gradient(slot,p,dy));}\n"
        "vec4 _q_sample_implicit(sampler2DArray image,uint slot,vec3 p,float bias){float scale=exp2(bias);"
        "vec2 dx=dFdx(p.xy)*scale,dy=dFdy(p.xy)*scale;if((_qsampler_info[slot]&3u)==0u)return texture(image,p,bias);"
        "return textureGrad(image,vec3(_q_sample_coord(slot,p.xy),p.z),"
        "_q_sample_gradient(slot,p.xy,dx),_q_sample_gradient(slot,p.xy,dy));}\n";
    if(vertex) {
        size_t at=0;
        while((at=header.find("_qsampler_info",at))!=std::string::npos) {
            header.replace(at,14,"_qvertex_sampler_info");at+=21;
        }
    }
    return header;
}
inline std::string comparisonSamplerHeader(bool fragment=false) {
    // Function parameters evaluate coordinates/reference/LOD once, including
    // operands with atomics. Derivatives and mirror transforms reuse values.
    std::string header;
    for(bool array:{false,true}) {
        const std::string sampler=array?"sampler2DArrayShadow":"sampler2DShadow",point=array?"vec3":"vec2";
        const std::string xy=array?"p.xy":"p",coord=array?"vec4(c,p.z,reference)":"vec3(c,reference)";
        const std::string parameters=sampler+" image,uint flags,"+point+" p,float reference";
        header+="float _q_compare_gradient("+parameters+",vec2 dx,vec2 dy){vec2 q="+xy+";"
            "vec2 c=_q_sample_coord_flags(flags,q);"
            "vec2 direction=vec2((flags&1u)!=0u && q.x<0.0?-1.0:1.0,(flags&2u)!=0u && q.y<0.0?-1.0:1.0);"
            "return textureGrad(image,"+coord+",dx*direction,dy*direction);}\n";
        // EXT_texture_shadow_lod supplies the array-shadow overload. Keep
        // explicit LOD as NIR txl / IR3 saml.s; deriving a gradient changes
        // trilinear weights through the backend's logarithm approximation.
        header+="float _q_compare_level("+parameters+",float lod){vec2 c=_q_sample_coord_flags(flags,"+xy+");"
            "return textureLod(image,"+coord+",lod);}\n";
        if(fragment)header+="float _q_compare_implicit("+parameters+",float bias){float scale=exp2(bias);"
            "return _q_compare_gradient(image,flags,p,reference,dFdx("+xy+")*scale,dFdy("+xy+")*scale);}\n";
    }
    return header;
}
inline std::string sampleUniform(unsigned texture,unsigned sampler,bool explicitLevel=false,unsigned stage=0,const QGPUConstantSampler *literal=nullptr,bool shadow=false) {
    auto name=std::string(stage==1?"_qvertex_sample":"_qsample")+std::to_string(texture)+"_"+std::to_string(sampler);
    if(literal)name+="_c_"+std::to_string(literal->filters)+"_"+std::to_string(literal->minLOD)+"_"+std::to_string(literal->maxLOD)+"_"+
        std::to_string(literal->addressModes)+"_"+std::to_string(literal->maxAnisotropy)+"_"+std::to_string(literal->borderColor)+"_"+std::to_string(literal->normalized);
    return name+(explicitLevel?"_level":"")+(shadow?"_shadow":"");
}
inline std::string sampleDeclaration(unsigned texture,unsigned sampler,bool explicitLevel=false,bool array=false,unsigned stage=0,bool cube=false,const QGPUConstantSampler *literal=nullptr,bool shadow=false) {
    return "layout(binding="+std::to_string(texture*8+(sampler%8)+(explicitLevel?64:0)+(shadow?192:0))+") uniform "+
        (shadow?(array?"sampler2DArrayShadow ":"sampler2DShadow "):cube?(array?"samplerCubeArray ":"samplerCube "):(array?"sampler2DArray ":"sampler2D "))+
        sampleUniform(texture,sampler,explicitLevel,stage,literal,shadow)+";\n";
}
inline std::string graphicsTexturePrefix(unsigned shape,unsigned stage) {
    static const char *names[]={"texture2d","texture2d_array","texturecube","texturecube_array","texture2d_ms","texture2d_ms_array"};
    return std::string(stage==1?"_qvertex_":"_q")+names[shape];
}
inline std::string graphicsTextureHeader(unsigned shape,unsigned stage) {
    auto p=graphicsTexturePrefix(shape,stage);
    return "uniform uvec4 "+p+"_info[8];\n"
        "uvec2 "+p+"_size(uint slot,uint lod){uvec4 p="+p+"_info[slot];"
        "if(lod>=((p.w>>8u)&255u))return uvec2(0u);return max(p.xy>>lod,uvec2(1u));}\n"
        "vec4 "+p+"_zero(uint slot){uvec4 p="+p+"_info[slot];vec4 value="+colorZero("p.w&255u")+";uint s=(p.w>>16u)&4095u;"
        "if(s==0u)return value;s-=1u;vec4 result;for(uint i=0u;i<4u;++i){uint c=(s>>(i*3u))&7u;result[i]=c==0u?0.0:c==1u?1.0:value[c-2u];}return result;}\n";
}
inline std::string graphicsFetchFunctions(unsigned slot,unsigned shape,unsigned stage) {
    auto p=graphicsTexturePrefix(shape,stage),s=std::to_string(slot);
    auto uniform=std::string(stage==1?"_qvertex_fetch":"_qfetch")+std::to_string(shape)+"_"+s;
    if(shape>=4)return "layout(binding="+std::to_string(128+shape*8+slot)+") uniform "+(shape==5?"sampler2DMSArray ":"sampler2DMS ")+uniform+";\n"
        "vec4 "+p+s+"_read(uvec2 coord,"+(shape==5?std::string("uint slice,"):std::string())+"uint sampleIndex){uvec4 info="+p+"_info["+s+"];"
        +(shape==5?"if(slice>=info.z)return "+p+"_zero("+s+"u);":std::string())+
        "if(any(greaterThanEqual(coord,info.xy)) || sampleIndex>=(1u<<((info.w>>28u)&3u)))return "+p+"_zero("+s+"u);"
        "return texelFetch("+uniform+(shape==5?",ivec3(coord,slice)":",ivec2(coord)")+",int(sampleIndex));}\n";
    bool cube=shape&2,array=shape&1;
    std::string indices=cube?"uint face,":"";if(array)indices+="uint slice,";
    std::string bounds=cube?"if(face>=6u)return "+p+"_zero("+s+"u);":"";
    if(array)bounds+="if(slice>="+p+"_info["+s+"].z)return "+p+"_zero("+s+"u);";
    std::string coordinate=shape?"ivec3(coord,":"ivec2(coord)";
    if(shape)coordinate+=(cube?(array?"slice*6u+face":"face"):"slice")+std::string(")");
    return "layout(binding="+std::to_string(128+shape*8+slot)+") uniform "+(shape?"sampler2DArray ":"sampler2D ")+uniform+";\n"
        "vec4 "+p+s+"_read(uvec2 coord,"+indices+"uint lod){uvec2 size="+p+"_size("+s+"u,lod);"
        "if(any(greaterThanEqual(coord,size)))return "+p+"_zero("+s+"u);"+bounds+
        "return texelFetch("+uniform+","+coordinate+",int(lod));}\n";
}
inline std::string imageFunctions(unsigned slot,unsigned access,bool array=false,bool depth=false,bool unsignedInteger=false) {
    std::string s=std::to_string(slot),name="_qimage"+s+(unsignedInteger?"_uint":""),memory="_qimage_words"+s+".data";
    std::string out="layout(std430,binding="+std::to_string(QGPU_COMPUTE_IMAGE_BASE+slot)+") coherent "+
        (access==1?"readonly ":"")+"buffer QImage"+s+" {uint data[];} _qimage_words"+s+";\n";
    std::string parameters=array?"uvec2 coord,uint slice,uint level":"uvec2 coord,uint level";
    std::string address=array?"_q_array_image_word("+s+"u,coord,slice,level)":"_q_image_word("+s+"u,coord,level)";
    if(unsignedInteger) {
        // R32Uint is a raw word, including values above 2^24 and the sign bit.
        // Write ignores read swizzles and preserves every other texel/padding.
        if(access&1)out+="uvec4 "+name+"_read("+parameters+"){uint word="+address+";"
            "uvec4 value=uvec4(word==0xffffffffu?0u:"+memory+"[word],0u,0u,1u);"
            "return _q_uint_swizzle(value,_qimage_info["+s+"].w>>16u);}\n";
        if(access&2)out+="void "+name+"_write(uvec4 value,"+parameters+"){uint word="+address+";"
            "if(word!=0xffffffffu)"+memory+"[word]=value.x;}\n";
        return out;
    }
    if(depth)out+="float "+name+"_read("+parameters+"){uint word="+address+";return word==0xffffffffu?0.0:uintBitsToFloat("+memory+"[word]);}\n";
    else if(access&1) out+="vec4 "+name+"_read("+parameters+"){uint word="+address+";"
        "uint format=_qimage_info["+s+"].w&255u;vec4 value="+colorZero("format")+";"
        "if(word!=0xffffffffu){if(format=="+std::to_string(Shader::RGBA16Float)+"u)value=vec4(unpackHalf2x16("+memory+"[word]),unpackHalf2x16("+memory+"[word+1u]));"
        "else {uint cpp=_q_image_cpp(format);value=unpackUnorm4x8("+memory+"[word]>>_q_image_shift("+s+"u,coord));"
        "if(cpp==1u)value=vec4(value.r,0.0,0.0,1.0);else if(cpp==2u)value=vec4(value.rg,0.0,1.0); }}"
        "if(format==5u || format==6u)value.rgb=_q_srgb_decode(value.rgb);"
        "if(format==2u || format==6u)value=value.bgra;return _q_swizzle(value,_qimage_info["+s+"].w>>16u);}\n";
    if(access&2) out+="void "+name+"_write(vec4 value,"+parameters+"){uint word="+address+";"
        "if(word==0xffffffffu)return;uint format=_qimage_info["+s+"].w&255u;"
        "if(format=="+std::to_string(Shader::RGBA16Float)+"u){"+memory+"[word]=packHalf2x16(value.rg);"+memory+"[word+1u]=packHalf2x16(value.ba);return;}"
        "value=clamp(mix(value,vec4(0.0),isnan(value)),0.0,1.0);"
        "if(format==5u || format==6u)value.rgb=_q_srgb_encode(value.rgb);"
        "if(format==2u || format==6u)value=value.bgra;uint encoded=packUnorm4x8(value),cpp=_q_image_cpp(format);"
        "if(cpp==4u){"+memory+"[word]=encoded;return;}"
        // Adjacent packed texels may be written by different invocations.
        // CAS preserves their lanes, including row padding in the last word.
        "uint shift=_q_image_shift("+s+"u,coord),mask=(cpp==1u?255u:65535u)<<shift,expected=0u;"
        "while(true){uint desired=(expected&~mask)|((encoded<<shift)&mask);uint observed=atomicCompSwap("+memory+"[word],expected,desired);"
        "if(observed==expected)break;expected=observed;} }\n";
    return out;
}
// Reuse the planar storage conversion/address helpers with a shape-specific
// uniform namespace. NIR owns those constants; they never contain GPU VAs.
inline void replaceImageName(std::string &code,const std::string &from,const std::string &to) {
    size_t at=0;while((at=code.find(from,at))!=std::string::npos){code.replace(at,from.size(),to);at+=to.size();}
}
inline std::string msImageFunctions(unsigned slot,bool array=false,bool depth=false) {
    auto code=imageFunctions(slot,1,array,depth),s=std::to_string(slot);
    replaceImageName(code,"uint level","uint sampleIndex");
    replaceImageName(code,array?"_q_array_image_word("+s+"u,coord,slice,level)":"_q_image_word("+s+"u,coord,level)",
        array?"_q_ms_array_word("+s+"u,coord,slice,sampleIndex)":"_q_ms_word("+s+"u,coord,sampleIndex)");
    replaceImageName(code,"_q_image_shift("+s+"u,coord)",(array?"_q_ms_array_shift(":"_q_ms_shift(")+s+"u,coord,sampleIndex)");
    replaceImageName(code,"_qimage_info",array?"_qimage_ms_array_info":"_qimage_ms_info");
    replaceImageName(code,"_qimage"+s,(array?"_qmsarrayimage":"_qmsimage")+s);
    replaceImageName(code,".w>>16u",".w>>16u&4095u");
    return code;
}
inline std::string cubeImagePrefix(bool array) {return array?"_qcubearray":"_qcube";}
inline std::string cubeImageNames(std::string code,bool array) {
    auto prefix=cubeImagePrefix(array);
    replaceImageName(code,"_qimage_array_info",prefix+"_face_info");
    replaceImageName(code,"_qimage_info",prefix+"_info");
    replaceImageName(code,"_qimage",prefix);
    replaceImageName(code,"_q_",prefix+"_");
    replaceImageName(code,"QImage",array?"QCubeArrayImage":"QCubeImage");
    return code;
}
inline std::string cubeImageHeader(bool array) {
    auto p=cubeImagePrefix(array);
    return cubeImageNames(imageHeader(),array)+
        "uint "+p+"_face_word(uint slot,uvec2 coord,uint face,uint cube,uint level){"
        "if(face>=6u || cube>="+p+"_face_info[slot].x/6u)return 0xffffffffu;"
        "return "+p+"_array_image_word(slot,coord,cube*6u+face,level); }\n";
}
inline std::string cubeImageFunctions(unsigned slot,unsigned access,bool array,bool unsignedInteger=false) {
    auto code=imageFunctions(slot,access,true,false,unsignedInteger),s=std::to_string(slot);
    replaceImageName(code,"uvec2 coord,uint slice,uint level","uvec2 coord,uint face,"+std::string(array?"uint cube,":"")+"uint level");
    replaceImageName(code,"_q_array_image_word("+s+"u,coord,slice,level)","_q_face_word("+s+"u,coord,face,"+(array?"cube":"0u")+",level)");
    return cubeImageNames(code,array);
}
inline std::string airTextureFunction(const AIRTextureBinding &binding,unsigned stage) {
    return (stage?graphicsTexturePrefix(binding.shape,stage):binding.shape&2?cubeImagePrefix(binding.shape&1):"_qimage")+std::to_string(binding.slot)+(binding.unsignedInteger?"_uint":"");
}
inline std::string airTextureQuery(const AIRTextureBinding &binding,unsigned stage,AIRTextureCall::Operation op,const std::string &lod) {
    auto s=std::to_string(binding.slot)+"u";bool array=binding.shape&1,cube=binding.shape&2;
    auto p=stage?graphicsTexturePrefix(binding.shape,stage):cube?cubeImagePrefix(array):"_q";
    if(op==AIRTextureCall::Width || op==AIRTextureCall::Height)
        return p+(stage?"_size(":array?"_array_image_size(":"_image_size(")+s+","+lod+")."+(op==AIRTextureCall::Width?"x":"y");
    if(op==AIRTextureCall::MipLevels) {
        if(!stage && array)return p+"_array_image_mips("+s+")";
        return "(("+(stage || cube?p+"_info":"_qimage_info")+"["+s+"].w>>8u)&255u)";
    }
    return stage?p+"_info["+s+"].z":cube?"("+p+"_face_info["+s+"].x/6u)":"_qimage_array_info["+s+"].x";
}
inline std::string airTextureSample(const AIRTextureBinding &binding,unsigned sampler,unsigned stage,bool gradient,bool level,
    const std::string &coordinate,const std::string &layer,const std::string &lod,const std::string &dx,const std::string &dy,const QGPUConstantSampler *literal=nullptr,
    const std::string &offset={}) {
    bool cube=binding.shape&2,array=binding.shape&1;auto slot=std::to_string(sampler)+"u";
    std::string vector=cube?"vec3":"vec2",raw=vector+"("+coordinate+")";
    auto coord=cube?raw:sampleCoordinate(sampler,raw,literal);
    if(array)coord=std::string(cube?"vec4(":"vec3(")+coord+",float("+layer+"))";
    auto uniform=sampleUniform(binding.slot,sampler,level,stage,literal);
    if(!offset.empty()) {
        // AIR values are already materialized SSA operands. Keep offsets in
        // the texture instruction: shifting normalized coordinates would
        // change mip filtering, addressing and pixel-coordinate samplers.
        if(!gradient && (level || stage!=2))return "textureLodOffset("+uniform+","+coord+","+lod+","+offset+")";
        const auto flags=literal?Shader::samplerMirrorFlags(*literal):3u;
        if(!gradient && !flags)return "textureOffset("+uniform+","+coord+","+offset+","+lod+")";
        auto gx=gradient?"vec2("+dx+")":"(dFdx("+raw+")*exp2("+lod+"))";
        auto gy=gradient?"vec2("+dy+")":"(dFdy("+raw+")*exp2("+lod+"))";
        if(flags) {
            auto direction=literal?"vec2("+std::string(flags&1?"("+raw+".x<0.0?-1.0:1.0)":"1.0")+","+
                (flags&2?"("+raw+".y<0.0?-1.0:1.0)":"1.0")+")":"";
            gx=literal?"("+gx+"*"+direction+")":"_q_sample_gradient("+slot+","+raw+","+gx+")";
            gy=literal?"("+gy+"*"+direction+")":"_q_sample_gradient("+slot+","+raw+","+gy+")";
        }
        auto sampled="textureGradOffset("+uniform+","+coord+","+gx+","+gy+","+offset+")";
        if(!gradient && !literal)return "(_q_sampler_mirror("+slot+")==0u?textureOffset("+uniform+","+coord+","+offset+","+lod+"):"+sampled+")";
        return sampled;
    }
    if(gradient) {
        const auto gx=vector+"("+dx+")",gy=vector+"("+dy+")";
        if(!cube)return sampleGradientValue(uniform,sampler,array?"vec3("+raw+",float("+layer+"))":raw,gx,gy,literal);
        return "textureGrad("+uniform+","+coord+","+gx+","+gy+")";
    }
    if(stage==2 && !level && !cube) {
        auto original=array?"vec3("+raw+",float("+layer+"))":raw;
        return sampleImplicit(uniform,sampler,original,lod,array,literal);
    }
    return std::string(stage==2 && !level?"texture(":"textureLod(")+uniform+","+coord+","+lod+")";
}
inline unsigned bufferComponents(const std::string &name) {
    for (const std::string base:{"uint","int","float","half"}) {
        if (name==base) return 1;
        if (name.size()==base.size()+1 && name.rfind(base,0)==0 && name.back()>='2' && name.back()<='4') return name.back()-'0';
    }
    return 0;
}
inline std::string airFloatType(bool half,unsigned lanes) {
    return lanes==1?(half?"float16_t":"float"):(half?"f16vec":"vec")+std::to_string(lanes);
}
inline std::string airFloatName(bool half,unsigned lanes) {
    return std::string(half?"half":"float")+(lanes==1?std::string():std::to_string(lanes));
}
inline std::string airFloatSuffix(bool half,unsigned lanes) {
    return (lanes==1?std::string():"v"+std::to_string(lanes))+(half?"f16":"f32");
}
// Additional math observed in iOS18 QuartzCore AIR. Both LLVM readers check
// exact signatures before using this shared expression builder.
inline unsigned airExtraMath(const std::string &name,const std::string &suffix,std::string &operation) {
    static constexpr struct {const char *name;unsigned count;} operations[]={
        {"exp",1},{"log",1},{"atan2",2},{"powr",2},{"fmin3",3},{"fmax3",3}};
    for(const auto &item:operations)
        if(name==std::string("air.")+item.name+"."+suffix || name==std::string("air.fast_")+item.name+"."+suffix) {
            operation=item.name;return item.count;
        }
    return 0;
}
inline std::string airExtraMathExpression(const std::string &op,const std::vector<std::string> &args,bool half,unsigned lanes,bool fast) {
    // Transcendentals use the existing f32 hardware path and round once at
    // a half result. Min/max retain half operands and nested Metal ordering.
    const bool extrema=op=="fmin3" || op=="fmax3";
    auto arg=[&](unsigned i){return half && !extrema?airFloatType(false,lanes)+"("+args[i]+")":args[i];};
    std::string value;
    if(extrema) {
        auto combine=[&](const std::string &a,const std::string &b) {
            auto result=std::string(op=="fmin3"?"min(":"max(")+a+","+b+")";
            // Unlike GLSL min/max, Metal returns the numeric operand if
            // exactly one is NaN. Fast AIR permits the existing no-NaN path.
            return fast?result:"mix(mix("+result+","+a+",isnan("+b+")),"+b+",isnan("+a+"))";
        };
        value=combine(arg(0),combine(arg(1),arg(2)));
    } else {
        auto function=op=="atan2"?"atan":op=="powr"?"pow":op.c_str();
        value=std::string(function)+"("+arg(0)+(args.size()==2?","+arg(1):"")+")";
    }
    return half && !extrema?airFloatType(true,lanes)+"("+value+")":value;
}
inline std::string airColorValue(const std::string &value,bool half,unsigned lanes) {
    auto full=half?airFloatType(false,lanes)+"("+value+")":value;
    return lanes==4?full:"vec4("+full+(lanes==1?",0.0,0.0,1.0)":lanes==2?",0.0,1.0)":",1.0)");
}
inline std::string halfBufferHeader() {
    return "uint _q_half_word(uvec2 address,uint component){uvec2 p=_q_addr_add(address,uvec2(component*2u,0u));"
        "return p.y==0u && (p.x&1u)==0u?p.x/2u:0x7fffffffu;}\n";
}
inline std::string halfBufferLoad(const std::string &array,const std::string &offset,unsigned lanes) {
    auto component=[&](unsigned n){return array+"[_q_half_word("+offset+","+std::to_string(n)+"u)]";};
    if(lanes==1)return component(0);
    std::string out=airFloatType(true,lanes)+"(";
    for(unsigned i=0;i<lanes;++i)out+=(i?",":"")+component(i);
    return out+")";
}
inline std::string halfBufferStore(const std::string &array,const std::string &offset,const std::string &value,unsigned lanes) {
    std::string out;
    for(unsigned i=0;i<lanes;++i)out+=array+"[_q_half_word("+offset+","+std::to_string(i)+"u)] = "+
        (lanes==1?value:"("+value+")["+std::to_string(i)+"]")+";\n";
    return out;
}
inline std::string bufferHeader() {
    return "uint _q_index(uint i,uint stride) { return min(i,0xffffffffu/stride); }\n"
        "uint _q_index(int i,uint stride) { return i<0?0xffffffffu/stride:_q_index(uint(i),stride); }\n"
        "uint _q_index(bool i,uint stride) { return i?1u:0u; }\n"
        // Bound invalid shared offsets before NIR adds each variable's base.
        // 32 KiB plus any admitted base fits uint32 without wrapping.
        "uint _q_shared_index(uint i,uint stride) { return min(i,32768u/stride); }\n"
        "uint _q_shared_index(int i,uint stride) { return i<0?32768u/stride:_q_shared_index(uint(i),stride); }\n"
        "uint _q_shared_index(bool i,uint stride) { return i?1u:0u; }\n"
        "uvec2 _q_addr_add(uvec2 a,uvec2 b) { uint lo=a.x+b.x;return uvec2(lo,a.y+b.y+uint(lo<a.x)); }\n"
        "uvec2 _q_addr_mul(uvec2 a,uint b) { uint hi,lo;umulExtended(a.x,b,hi,lo);return uvec2(lo,hi+a.y*b); }\n"
        "uint _q_word(uvec2 address,uint component) { uvec2 p=_q_addr_add(address,uvec2(component*4u,0u));"
        "return p.y==0u && (p.x&3u)==0u?p.x/4u:0x3fffffffu; }\n"
        "uint _q_shared_word(uvec2 address,uint component) { return min(_q_word(address,component),8192u); }\n";
}
inline std::string addressConstant(uint64_t value) {
    return "uvec2("+std::to_string(uint32_t(value))+"u,"+std::to_string(uint32_t(value>>32))+"u)";
}
inline std::string addressExtend(const std::string &value,bool sign) {
    return "uvec2(uint("+value+"),"+(sign?"(int("+value+")<0?0xffffffffu:0u)":"0u")+")";
}
inline std::string dynamicThreadgroupHeader() {
    return "uniform uvec2 _qthreadgroup_info[8];\nshared uint _qdynamic[8192];\n"
        "uint _q_dynamic_word(uint slot,uvec2 address,uint component) { uvec2 p=_q_addr_add(address,uvec2(component*4u,0u));"
        "uvec2 binding=_qthreadgroup_info[slot];return p.y==0u && (p.x&3u)==0u && binding.y>=4u && p.x<=binding.y-4u?"
        "(binding.x+p.x)/4u:8192u;}\n"
        "uint _q_dynamic_load(uint slot,uvec2 address,uint component) {uint word=_q_dynamic_word(slot,address,component);"
        "if(word>=8192u)return 0u;return _qdynamic[word];}\n"
        "void _q_dynamic_store(uint slot,uvec2 address,uint component,uint value) {uint word=_q_dynamic_word(slot,address,component);"
        "if(word<8192u)_qdynamic[word]=value;}\n";
}
inline std::string wordLoad(const std::string &name,const std::string &offset,unsigned components,bool shared=false,unsigned dynamicSlot=UINT32_MAX) {
    if (dynamicSlot!=UINT32_MAX) {
        std::string prefix="_q_dynamic_load("+std::to_string(dynamicSlot)+"u,"+offset+",";
        if (components==1) return prefix+"0u)";
        std::string value="uvec"+std::to_string(components)+"(";
        for (unsigned i=0;i<components;++i) value+=(i?",":"")+prefix+std::to_string(i)+"u)";
        return value+")";
    }
    std::string prefix=name+(shared?"[_q_shared_word(":"[_q_word(")+offset+",";
    if (components==1) return prefix+"0u)]";
    std::string value="uvec"+std::to_string(components)+"(";
    for (unsigned i=0;i<components;++i) value+=(i?",":"")+prefix+std::to_string(i)+"u)]";
    return value+")";
}
inline std::string wordStore(const std::string &name,const std::string &offset,const std::string &value,unsigned components,bool shared=false,unsigned dynamicSlot=UINT32_MAX) {
    std::string code;
    if (dynamicSlot!=UINT32_MAX) {
        for (unsigned i=0;i<components;++i) code+="_q_dynamic_store("+std::to_string(dynamicSlot)+"u,"+offset+","+
            std::to_string(i)+"u,"+(components==1?value:"("+value+")["+std::to_string(i)+"]")+");\n";
        return code;
    }
    for (unsigned i=0;i<components;++i) code+=name+(shared?"[_q_shared_word(":"[_q_word(")+offset+","+
        std::to_string(i)+"u)] = "+(components==1?value:"("+value+")["+std::to_string(i)+"]")+";\n";
    return code;
}
inline std::string bufferLoad(unsigned slot,const std::string &offset,unsigned components) {
    return wordLoad("_qbuffer"+std::to_string(slot)+".data",offset,components);
}
inline std::string bufferStore(unsigned slot,const std::string &offset,const std::string &value,unsigned components) {
    return wordStore("_qbuffer"+std::to_string(slot)+".data",offset,value,components);
}
inline std::string computeHeader() {
    return "#extension GL_ARB_compute_variable_group_size : require\nlayout(local_size_variable) in;\n"
        "uniform uvec4 _qdispatch_info[4];\n"
        "uvec3 _q_global_id(){return _qdispatch_info[0].xyz+gl_GlobalInvocationID;}\n"
        "uvec3 _q_group_id(){return _qdispatch_info[1].xyz+gl_WorkGroupID;}\n"
        "uvec3 _q_grid_size(){return _qdispatch_info[2].xyz;}\n"
        "uvec3 _q_group_count(){return _qdispatch_info[3].xyz;}\n"
        "uvec3 _q_dispatch_local_size(){return uvec3(_qdispatch_info[0].w,_qdispatch_info[1].w,_qdispatch_info[2].w);}\n";
}
inline std::string computeSystemValue(const std::string &attribute) {
    if (attribute=="thread_position_in_grid") return "_q_global_id()";
    if (attribute=="thread_position_in_threadgroup") return "gl_LocalInvocationID";
    if (attribute=="threadgroup_position_in_grid") return "_q_group_id()";
    if (attribute=="threads_per_threadgroup") return "gl_LocalGroupSizeARB";
    if (attribute=="threadgroups_per_grid") return "_q_group_count()";
    if (attribute=="threads_per_grid") return "_q_grid_size()";
    if (attribute=="dispatch_threads_per_threadgroup") return "_q_dispatch_local_size()";
    if (attribute=="thread_index_in_threadgroup") return "gl_LocalInvocationIndex";
    return {};
}
template<class Has> inline std::string computeInputSemantic(Has has) {
    std::string input;
    for (auto semantic:{"thread_position_in_grid","thread_position_in_threadgroup","threadgroup_position_in_grid",
        "threads_per_threadgroup","dispatch_threads_per_threadgroup","threadgroups_per_grid","threads_per_grid","thread_index_in_threadgroup"})
        if (has(semantic)) { if (!input.empty()) return "?";input=semantic; }
    return input;
}
inline std::string booleanBinary(const std::string &op,const std::string &a,const std::string &b,unsigned lanes) {
    const std::string logical=op=="&"?"&&":op=="|"?"||":op=="^"?"!=":"";
    if (logical.empty() || !lanes || lanes>4) return {};
    if (lanes==1) return "("+a+logical+b+")";
    std::string value="bvec"+std::to_string(lanes)+"(";
    for (unsigned i=0;i<lanes;++i) {
        auto component=std::string(".")+"xyzw"[i];
        value+=(i?",":"")+std::string("((")+a+")"+component+logical+"("+b+")"+component+")";
    }
    return value+")";
}
// LLVM FCmp predicates 0..15 have the same stable numbering in its C/C++
// APIs. Keep ordered/unordered NaN behavior identical in both AIR readers,
// without relying on optional fast-math C ABI exports in system libLLVM.
inline std::string airFloatComparison(unsigned predicate,const std::string &a,const std::string &b,unsigned lanes) {
    if(predicate>15 || !lanes || lanes>4)return {};
    static const char *ops[]={"","==",">",">=","<","<=","!=","","","==",">",">=","<","<=","!=",""};
    std::string result=lanes==1?"":"bvec"+std::to_string(lanes)+"(";
    for(unsigned i=0;i<lanes;++i) {
        auto component=lanes==1?std::string():std::string(".")+"xyzw"[i];
        auto x="("+a+")"+component,y="("+b+")"+component;
        auto nan="(isnan("+x+")||isnan("+y+"))";
        std::string value;
        if(!predicate || predicate==15)value=predicate?"true":"false";
        else if(predicate==7)value="(!"+nan+")";
        else if(predicate==8)value=nan;
        else value="("+(predicate<7?"!"+nan+"&&":nan+"||")+"("+x+ops[predicate]+y+"))";
        result+=(i?",":"")+value;
    }
    return lanes==1?result:result+")";
}
inline void semantic(const std::string &name,uint32_t (&words)[4]) {
    uint8_t digest[32];
#ifdef __APPLE__
    CC_SHA256(name.data(),static_cast<CC_LONG>(name.size()),digest);
#else
    auto hash=llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(name.data()),name.size()));
    std::memcpy(digest,hash.data(),sizeof(digest));
#endif
    for (unsigned i=0;i<4;++i) words[i]=uint32_t(digest[i*4])|(uint32_t(digest[i*4+1])<<8)|
        (uint32_t(digest[i*4+2])<<16)|(uint32_t(digest[i*4+3])<<24);
}
// Apple AIR identifies explicit user names and compiler-generated field names
// separately. Normalize generated(length+field+type) to field identity; the
// scalar/vector type is validated independently when linking the stages.
inline bool airSemantic(const std::string &value,std::string &out) {
    if (value.size()>6 && value.rfind("user(",0)==0 && value.back()==')') {
        out="user:"+value.substr(5,value.size()-6);return out.size()>5;
    }
    if (value.rfind("generated(",0)!=0 || value.back()!=')') return false;
    size_t pos=10,length=0;
    while (pos<value.size() && value[pos]>='0' && value[pos]<='9') {
        length=length*10+unsigned(value[pos++]-'0');if (length>256) return false;
    }
    if (!length || length>value.size()-pos-1) return false;
    out="field:"+value.substr(pos,length);return true;
}
} }
