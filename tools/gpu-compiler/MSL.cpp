// SPDX-License-Identifier: BSD-2-Clause
#include "Compiler.h"
#include "Semantic.h"
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
namespace GPU { namespace Compiler {
namespace {
// Parse statements rather than replacing return tokens in a flat body. In
// particular, return inside a branch/loop must publish graphics outputs at
// that point, and for-init variables must not escape their lexical scope.
class Statements {
    const std::vector<std::string> &tokens;
    size_t pos=0;
    unsigned depth=0,loops=0,breakable=0;
    enum { Fall=1, Return=2, Break=4, Continue=8 };
    using Code=std::pair<std::string,unsigned>;
    std::function<void(const std::string &)> fail;
    std::function<std::string(const std::vector<std::string> &)> expression;
    std::function<bool(const std::string &)> isType;
    std::function<std::string(const std::string &)> typeName;
    std::function<std::string(const std::string &,const std::string &)> declare;
    std::function<std::string(const std::vector<std::string> &)> shared;
    std::function<std::string(const std::vector<std::string> &)> sampler;
    std::function<void(bool)> scope;
    std::function<std::string(const std::vector<std::string> &)> result;
    std::function<std::string()> demote;
    bool at(const char *s) const { return pos<tokens.size() && tokens[pos]==s; }
    void take(const char *s) {
        if (!at(s)) fail(std::string("expected ")+s+" in statement");
        ++pos;
    }
    std::vector<std::string> until(const char *delimiter) {
        std::vector<std::string> out;
        unsigned parens=0,brackets=0,ternaries=0;
        while (pos<tokens.size()) {
            auto s=tokens[pos];
            if (!parens && !brackets && !ternaries && s==delimiter) { ++pos;return out; }
            if (s=="{" || s=="}" || s==";") fail("unterminated or unsupported expression");
            if (s=="(") ++parens;
            else if (s==")") { if (!parens) fail("unbalanced expression");--parens; }
            else if (s=="[") ++brackets;
            else if (s=="]") { if (!brackets) fail("unbalanced subscript");--brackets; }
            else if (!parens && !brackets && s=="?") ++ternaries;
            else if (!parens && !brackets && s==":" && ternaries) --ternaries;
            if (parens>64 || brackets>64 || ternaries>64) fail("expression nesting limit exceeded");
            out.push_back(s);++pos;
        }
        fail("unterminated expression");return {};
    }
    std::string condition(const std::vector<std::string> &value) {
        if (value.empty()) fail("empty condition");
        // Metal's scalar integer conditions have C++ boolean conversion.
        return "bool("+expression(value)+")";
    }
    bool declaration() const {
        return at("threadgroup") || at("constexpr") || at("constant") || at("const") || (pos+1<tokens.size() && isType(tokens[pos]) && tokens[pos+1]!="(");
    }
    std::string local(const std::vector<std::string> &part) {
        if (!part.empty() && part[0]=="threadgroup") return shared(part);
        if (!part.empty() && (part[0]=="constexpr" || part[0]=="constant")) return sampler(part);
        size_t i=0;bool constant=i<part.size() && part[i]=="const";
        if (constant) ++i;
        if (i==part.size() || !isType(part[i])) { fail("unsupported local type");return {}; }
        auto type=part[i++];std::string code=constant?"const ":"";code+=typeName(type)+" ";
        bool first=true;
        while (i<part.size()) {
            if (!first) { if (part[i++]!=",") fail("invalid local declaration");code+=","; }
            if (i==part.size()) fail("missing local name");
            code+=declare(type,part[i++]);first=false;
            if (i<part.size() && part[i]=="=") {
                ++i;size_t start=i;unsigned parens=0;
                while (i<part.size() && (parens || part[i]!=",")) {
                    if (part[i]=="(") ++parens;
                    else if (part[i]==")") { if (!parens) fail("unbalanced initializer");--parens; }
                    ++i;
                }
                if (i==start || parens) fail("invalid initializer");
                code+=" = "+expression({part.begin()+start,part.begin()+i});
            } else if (constant) fail("const local needs an initializer");
        }
        if (first) fail("missing local declaration");return code;
    }
    static unsigned sequence(unsigned before,unsigned after) {
        return (before&~Fall)|((before&Fall)?after:0);
    }
    Code list(bool block) {
        std::string code;unsigned flow=Fall;
        while (pos<tokens.size() && !(block && at("}"))) {
            auto s=statement();code+=s.first;flow=sequence(flow,s.second);
        }
        if (block) take("}");return {code,flow};
    }
    Code scoped() {
        scope(true);auto s=statement();scope(false);
        s.first="{\n"+s.first+"}\n";return s;
    }
    Code statement() {
        if (++depth>64) fail("statement nesting limit exceeded");
        auto finish=[&](std::string code,unsigned flow)->Code { --depth;return {std::move(code),flow}; };
        if (at("{")) {
            ++pos;scope(true);auto s=list(true);scope(false);return finish("{\n"+s.first+"}\n",s.second);
        }
        if (at("if")) {
            ++pos;take("(");auto cond=condition(until(")"));auto yes=scoped();
            Code no={"",Fall};if (at("else")) { ++pos;no=scoped(); }
            return finish("if ("+cond+") "+yes.first+(no.first.empty()?"":"else "+no.first),yes.second|no.second);
        }
        if (at("while") || at("for")) {
            bool forLoop=at("for");++pos;scope(true);take("(");std::string init,step,cond;
            bool infinite=false;
            if (forLoop) {
                bool decl=declaration();auto first=until(";");init=first.empty()?"":decl?local(first):expression(first);
                auto test=until(";");infinite=test.empty() || test==std::vector<std::string>{"true"};cond=test.empty()?"true":condition(test);
                auto last=until(")");if (!last.empty()) step=expression(last);
            } else { auto test=until(")");infinite=test==std::vector<std::string>{"true"};cond=condition(test); }
            ++loops;++breakable;auto body=scoped();--loops;--breakable;scope(false);
            unsigned flow=(body.second&Return)|((!infinite || (body.second&Break))?Fall:0);
            return finish(forLoop?"for ("+init+";"+cond+";"+step+") "+body.first:"while ("+cond+") "+body.first,flow);
        }
        if (at("do")) {
            ++pos;++loops;++breakable;auto body=scoped();--loops;--breakable;
            take("while");take("(");auto test=until(")");auto cond=condition(test);take(";");
            unsigned flow=(body.second&Return)|((body.second&Break)?Fall:0);
            if (test!=std::vector<std::string>{"true"} && (body.second&(Fall|Continue))) flow|=Fall;
            return finish("do "+body.first+"while ("+cond+");\n",flow);
        }
        if (at("switch")) {
            ++pos;take("(");auto test=until(")");if (test.empty()) fail("empty switch expression");
            std::string code="switch ("+expression(test)+") {\n";take("{");scope(true);++breakable;
            bool haveDefault=false;std::vector<unsigned> parts;
            while (pos<tokens.size() && !at("}")) {
                if (at("case")) {
                    ++pos;auto value=until(":");if (value.empty()) fail("missing case value");
                    code+="case "+expression(value)+":\n";parts.push_back(Fall);
                } else if (at("default")) {
                    ++pos;take(":");if (haveDefault) fail("duplicate default label");haveDefault=true;
                    code+="default:\n";parts.push_back(Fall);
                } else {
                    if (parts.empty()) fail("statement before first switch label");
                    auto s=statement();code+=s.first;parts.back()=sequence(parts.back(),s.second);
                }
                if (parts.size()>128) fail("switch case limit exceeded");
            }
            take("}");--breakable;scope(false);unsigned tail=Fall,flow=haveDefault?0:Fall;
            for (size_t n=parts.size();n--;) { tail=sequence(parts[n],tail);flow|=(tail&~Break)|((tail&Break)?Fall:0); }
            return finish(code+"}\n",flow);
        }
        if (at("return")) { ++pos;return finish(result(until(";")),Return); }
        if (at("break")) { ++pos;take(";");if (!breakable) fail("break outside loop/switch");return finish("break;\n",Break); }
        if (at("continue")) { ++pos;take(";");if (!loops) fail("continue outside loop");return finish("continue;\n",Continue); }
        if (at("discard_fragment")) {
            ++pos;take("(");take(")");take(";");return finish(demote(),Fall);
        }
        if (at(";")) { ++pos;return finish(";\n",Fall); }
        bool decl=declaration();auto part=until(";");if (part.empty()) fail("empty statement");
        return finish((decl?local(part):expression(part))+";\n",Fall);
    }
public:
    Statements(const std::vector<std::string> &body,
        std::function<void(const std::string &)> error,
        std::function<std::string(const std::vector<std::string> &)> expr,
        std::function<bool(const std::string &)> knownType,
        std::function<std::string(const std::string &)> translatedType,
        std::function<std::string(const std::string &,const std::string &)> localName,
        std::function<std::string(const std::vector<std::string> &)> sharedName,
        std::function<std::string(const std::vector<std::string> &)> samplerName,
        std::function<void(bool)> lexicalScope,
        std::function<std::string(const std::vector<std::string> &)> output,
        std::function<std::string()> discard):
        tokens(body),fail(std::move(error)),expression(std::move(expr)),isType(std::move(knownType)),
        typeName(std::move(translatedType)),declare(std::move(localName)),shared(std::move(sharedName)),sampler(std::move(samplerName)),scope(std::move(lexicalScope)),result(std::move(output)),demote(std::move(discard)) {}
    std::string run(bool graphics) {
        auto s=list(false);if (graphics && (s.second&Fall)) fail("graphics shader has an output path without return");
        return s.first;
    }
};
struct Parser {
    std::vector<std::string> t; size_t pos = 0;
    struct Field { std::string type,name,semantic,depth;unsigned mode=0;bool position=false,sampleMask=false;int attribute=-1,color=-1; };
    std::map<std::string,std::vector<Field>> structures;
    std::map<std::string,QGPUConstantSampler> globalSamplers;
    [[noreturn]] void fail(const std::string &s) { throw std::runtime_error("MSL: " + s); }
    static bool identifier(const std::string &s) {
        if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0]=='_')) return false;
        for (char c:s) if (!std::isalnum((unsigned char)c) && c!='_') return false;
        return true;
    }
    std::string take(const std::string &expected = {}) {
        if (pos==t.size()) fail("truncated entry");
        auto s=t[pos++]; if (!expected.empty() && s!=expected) fail("expected " + expected + ", got " + s);
        return s;
    }
    std::vector<std::string> group(const char *left, const char *right) {
        take(left); unsigned depth=1; std::vector<std::string> result;
        while (depth) { auto s=take(); if (s==left) ++depth; if (s==right) --depth; if (depth) result.push_back(s); }
        return result;
    }
    unsigned width(const std::string &type) {
        for (const auto &base:{std::string("float"),std::string("int"),std::string("uint")}) {
            if (type==base) return 1;
            if (type.size()==base.size()+1 && type.rfind(base,0)==0 && type.back()>='2' && type.back()<='4') return unsigned(type.back()-'0');
        }
        fail("stage members must be 32-bit scalar/vector");
    }
    std::string glslType(const std::string &type) {
        unsigned n=width(type);return n==1?type:(type.rfind("uint",0)==0?"uvec":type.rfind("int",0)==0?"ivec":"vec")+std::to_string(n);
    }
    void structure() {
        take("struct");auto name=take();
        if (!identifier(name) || structures.count(name)) fail("invalid/duplicate interface struct");
        take("{");std::vector<Field> fields;std::set<std::string> names,semantics;
        bool position=false;
        while (pos<t.size() && t[pos]!="}") {
            Field f;f.type=take();width(f.type);f.name=take();f.semantic="field:"+f.name;
            if (!identifier(f.name) || !names.insert(f.name).second) fail("invalid/duplicate struct member");
            bool qualified=false,semanticSet=false;
            while (pos<t.size() && t[pos]=="[[") {
                take("[[");
                do {
                    auto attr=take();
                    if (attr=="position") {
                        if (semanticSet || position || f.type!="float4") fail("invalid/duplicate position member");
                        f.position=position=semanticSet=true;
                    } else if(attr=="color") {
                        if(semanticSet || qualified || (f.type!="float" && f.type!="float2" && f.type!="float3" && f.type!="float4"))fail("color member requires float1..4 without interpolation");
                        take("(");auto number=take();take(")");
                        if(number.size()!=1 || number[0]<'0' || number[0]>'7')fail("color target outside 0..7");
                        f.color=number[0]-'0';f.semantic="color:"+number;semanticSet=true;
                    } else if(attr=="sample_mask") {
                        if(semanticSet || qualified || f.type!="uint")fail("sample mask requires scalar uint without interpolation");
                        f.sampleMask=true;f.semantic="sample_mask";semanticSet=true;
                    } else if(attr=="depth") {
                        if(semanticSet || qualified || f.type!="float")fail("depth output requires scalar float without interpolation");
                        take("(");f.depth=take();take(")");
                        if(f.depth!="any" && f.depth!="less" && f.depth!="greater")fail("unsupported depth qualifier");
                        f.semantic="depth";semanticSet=true;
                    } else if (attr=="attribute") {
                        if (semanticSet || qualified) fail("duplicate or interpolated attribute");
                        take("(");auto number=take();take(")");
                        if (number.empty() || number.size()>2 || number.find_first_not_of("0123456789")!=std::string::npos || std::stoul(number)>=QGPU_MAX_ATTRIBUTES) fail("attribute outside 0..15");
                        f.attribute=int(std::stoul(number));f.semantic="attribute:"+number;semanticSet=true;
                    } else if (attr=="user") {
                        if (semanticSet) fail("duplicate member semantic");
                        take("(");auto value=take();if (!identifier(value)) fail("invalid user semantic");take(")");
                        f.semantic="user:"+value;semanticSet=true;
                    } else if (attr=="center_perspective" || attr=="center_no_perspective" || attr=="flat" || attr=="centroid_perspective" || attr=="centroid_no_perspective" || attr=="sample_perspective" || attr=="sample_no_perspective") {
                        if (qualified) fail("duplicate interpolation qualifier");qualified=true;
                        f.mode=attr=="flat"?2:attr=="sample_perspective"?5:attr=="sample_no_perspective"?6:attr=="centroid_perspective"?3:attr=="centroid_no_perspective"?4:attr=="center_no_perspective"?1:0;
                    } else fail("unsupported struct member attribute: "+attr);
                    if (pos==t.size() || t[pos]!=",") break;take(",");
                } while (true);
                take("]]");
            }
            take(";");
            if (f.attribute>=0 && qualified) fail("vertex attributes cannot have interpolation qualifiers");
            if((f.color>=0 || f.sampleMask) && qualified)fail("color output cannot have interpolation qualifiers");
            if(!f.depth.empty() && qualified)fail("depth output cannot have interpolation qualifiers");
            if (f.position && qualified && f.mode!=1) fail("position only admits center_no_perspective");
            if (!f.position && !semantics.insert(f.semantic).second) fail("duplicate varying semantic");
            fields.push_back(f);if (fields.size()>QGPU_MAX_VARYINGS+1) fail("interface struct exceeds hardware outputs");
        }
        take("}");take(";");if (fields.empty()) fail("empty interface struct");structures[name]=std::move(fields);
    }
    std::string structGLSL(const std::string &name) {
        std::string result="struct _qtype_"+name+" {\n";unsigned index=0;
        for (const auto &f:structures.at(name)) result+=glslType(f.type)+" _qmember"+std::to_string(index++)+";\n";
        return result+"};\n";
    }
    std::string declareFramebuffer(Entry &e,const std::string &kind,unsigned slot,std::string &header,std::string &inputs) {
        if((kind!="float" && kind!="float2" && kind!="float3" && kind!="float4") || slot>=QGPU_COLOR_TARGET_COUNT ||
            (e.reflection.framebufferMask&(1u<<slot)))fail("invalid/duplicate framebuffer input");
        if(!e.reflection.framebufferMask)header+=framebufferHeader();e.reflection.framebufferMask|=1u<<slot;header+=framebufferFunctions(slot);
        auto value="_qframebuffer"+std::to_string(slot)+"_read()";unsigned components=width(kind);
        if(components<4)value="("+value+")."+std::string("xyzw",components);
        auto input="_qframebuffer_input"+std::to_string(slot);
        inputs+=glslType(kind)+" "+input+" = "+value+";\n";return input;
    }
    std::string declareVarying(Entry &e,const Field &f,std::string &header) {
        if (f.attribute>=0 || f.color>=0 || f.sampleMask || !f.depth.empty() || f.type.rfind("float",0)!=0) fail("varyings must be float32 and cannot be vertex attributes or fragment outputs");
        unsigned n=e.reflection.varyingCount;
        if (n>=QGPU_MAX_VARYINGS) fail("too many varying members");
        auto &v=e.reflection.varyings[n];semantic(f.semantic,v.semantic);
        v.components=width(f.type);v.mode=f.mode;
        std::string name="_qvarying"+std::to_string(n);
        header+="layout(location="+std::to_string(n)+") "+interpolationQualifier(f.mode)+
            (e.reflection.stage==1?"out ":"in ")+glslType(f.type)+" "+name+";\n";
        ++e.reflection.varyingCount;return name;
    }
    explicit Parser(const std::string &source) {
        if (source.empty() || source.size()>1024*1024 || source.find('\0')!=std::string::npos) fail("invalid source size or NUL");
        for (size_t i=0;i<source.size();) {
            char c=source[i];
            if (std::isspace((unsigned char)c)) { ++i; continue; }
            if (source.compare(i,2,"//")==0) { i=source.find('\n',i); if (i==std::string::npos) break; continue; }
            if (source.compare(i,2,"/*")==0) { auto end=source.find("*/",i+2); if (end==std::string::npos) fail("unterminated comment"); i=end+2; continue; }
            if (c=='#') {
                auto end=source.find('\n',i); if (end==std::string::npos) end=source.size();
                std::string directive; for (size_t k=i;k<end;++k) if (!std::isspace((unsigned char)source[k])) directive+=source[k];
                if (directive!="#include<metal_stdlib>") fail("unsupported preprocessor directive");
                i=end; continue;
            }
            size_t begin=i;
            if (std::isalpha((unsigned char)c) || c=='_') {
                while (i<source.size() && (std::isalnum((unsigned char)source[i]) || source[i]=='_')) ++i;
            } else if (std::isdigit((unsigned char)c) || (c=='.' && i+1<source.size() && std::isdigit((unsigned char)source[i+1]))) {
                ++i;
                while (i<source.size() && (std::isalnum((unsigned char)source[i]) || source[i]=='.' ||
                    ((source[i]=='+' || source[i]=='-') && (source[i-1]=='e' || source[i-1]=='E')))) ++i;
            } else {
                const std::set<std::string> pairs={"::","[[","]]","<<",">>","<=",">=","==","!=","&&","||","+=","-=","*=","/=","%=","&=","|=","^=","++","--"};
                if (source.compare(i,3,"<<=")==0 || source.compare(i,3,">>=")==0) i+=3;
                else if (pairs.count(source.substr(i,2))) i+=2;
                else if (std::string("{}()[],;.:?+*/%&|^<>!~=-").find(c)!=std::string::npos) ++i;
                else fail("unsupported character");
            }
            t.push_back(source.substr(begin,i-begin));
            if (t.back().rfind("_q",0)==0 || t.back().rfind("gl_",0)==0) fail("reserved compiler identifier");
            if (t.size()>32768) fail("source token limit exceeded");
        }
    }
    std::pair<std::string,QGPUConstantSampler> constantSampler(const std::vector<std::string> &part) {
        if(part.size()<3 || (part[0]!="constexpr" && part[0]!="constant") || part[1]!="sampler" || !identifier(part[2]))fail("expected a constant sampler declaration");
        auto state=defaultConstantSampler();std::set<std::string> fields;
        auto unique=[&](const std::string &key){if(!fields.insert(key).second)fail("duplicate sampler option: "+key);};
        if(part.size()==3)return {part[2],state};
        if(part.size()<5 || part[3]!="(" || part.back()!=")")fail("invalid constant sampler constructor");
        auto number=[&](size_t begin,size_t end) {
            std::string text;for(size_t i=begin;i<end;++i)text+=part[i];
            if(!text.empty() && (text.back()=='f' || text.back()=='u'))text.pop_back();
            size_t consumed=0;double value;
            try{value=std::stod(text,&consumed);}catch(...){fail("sampler options need numeric literals");}
            if(consumed!=text.size() || !std::isfinite(value))fail("invalid sampler numeric literal");return value;
        };
        size_t at=4;
        while(at+1<part.size()) {
            size_t end=at;unsigned depth=0;
            for(;end+1<part.size();++end) {if(part[end]=="(")++depth;else if(part[end]==")"){if(!depth)fail("invalid sampler option");--depth;}
                if(!depth && part[end]==",")break;}
            if(depth || end==at)fail("invalid sampler option");
            size_t start=at;if(end-start>=2 && part[start]=="metal" && part[start+1]=="::")start+=2;
            auto key=part[start];
            if(key=="max_anisotropy" && end-start>=4 && part[start+1]=="(" && part[end-1]==")") {
                unique(key);double n=number(start+2,end-1);if(n<1 || n>16 || n!=std::floor(n))fail("anisotropy outside 1..16");state.maxAnisotropy=uint32_t(n);
            } else if(key=="lod_clamp" && end-start>=6 && part[start+1]=="(" && part[end-1]==")") {
                unique(key);size_t comma=start+2;while(comma<end && part[comma]!=",")++comma;
                if(comma==end)fail("lod_clamp needs min and max");double low=number(start+2,comma),high=number(comma+1,end-1);
                if(low>high || !constantSamplerLOD(low,state.minLOD) || !constantSamplerLOD(high,state.maxLOD))fail("LOD clamp exceeds the A540 8.8 profile");
            } else {
                if(end-start!=3 || part[start+1]!="::")fail("unsupported sampler constructor option");auto value=part[start+2];
                if(key=="filter" || key=="min_filter" || key=="mag_filter") {
                    if(value!="nearest" && value!="linear")fail("unsupported sampler filter");unsigned bit=value=="linear";
                    if(key!="mag_filter"){unique("min_filter");state.filters=(state.filters&~1u)|bit;}
                    if(key!="min_filter"){unique("mag_filter");state.filters=(state.filters&~2u)|(bit<<1);}
                } else if(key=="mip_filter") {
                    unique(key);unsigned valueBits=value=="none"?0:value=="nearest"?1:value=="linear"?2:3;
                    if(valueBits==3)fail("unsupported mip filter");state.filters=(state.filters&~12u)|(valueBits<<2);
                } else if(key=="coord") {
                    unique(key);if(value!="normalized" && value!="pixel")fail("unsupported sampler coordinates");state.normalized=value=="normalized";
                } else if(key=="address" || key=="s_address" || key=="t_address" || key=="r_address") {
                    unsigned mode=value=="clamp_to_edge"?Shader::ClampToEdge:value=="mirrored_repeat"?Shader::MirrorRepeat:value=="repeat"?Shader::Repeat:
                        value=="clamp_to_zero"?Shader::ClampToZero:value=="clamp_to_border"?Shader::ClampToBorderColor:99;
                    if(mode==99)fail("unsupported sampler address mode");
                    for(unsigned axis=0;axis<3;++axis)if(key=="address" || key[0]=="str"[axis]) {
                        unique(std::string(1,"str"[axis])+"_address");state.addressModes=(state.addressModes&~(7u<<(axis*3)))|(mode<<(axis*3));
                    }
                } else if(key=="border_color") {
                    unique(key);state.borderColor=value=="transparent_black"?0:value=="opaque_black"?1:value=="opaque_white"?2:3;
                } else if(key=="reduction") {unique(key);if(value!="weighted_average")fail("reduction samplers are not supported");}
                else if(key=="compare_func") {unique(key);static const char *names[]={"never","less","equal","less_equal","greater","not_equal","greater_equal","always"};unsigned c=0;while(c<8 && value!=names[c])++c;if(c==8)fail("invalid compare function");state.filters=(state.filters&~0x700u)|(c<<8);}
                else fail("unsupported sampler option: "+key);
            }
            if(end+1==part.size())break;at=end+1;if(at+1==part.size())fail("trailing sampler option comma");
        }
        if(!Shader::validSampler(state))fail("sampler state exceeds the A540 profile");return {part[2],state};
    }
    void globalSampler() {
        size_t begin=pos;while(pos<t.size() && t[pos]!=";")++pos;
        if(pos==t.size())fail("unterminated global sampler");auto value=constantSampler({t.begin()+begin,t.begin()+pos});++pos;
        if(structures.count(value.first) || !globalSamplers.emplace(value).second)fail("duplicate global sampler");
    }
    Entry entry() {
        auto stage=take(), result=take(), name=take();
        unsigned s=stage=="kernel"?0:stage=="vertex"?1:stage=="fragment"?2:3;
        bool aggregate=(s==1 || s==2) && structures.count(result);
        if (s==3 || (!aggregate && result!=(s==0?"void":"float4") && !(s==2 && result=="void")) || !identifier(name) || name.size()>=96) fail("unsupported entry signature");
        Entry e; e.name=name; e.reflection.stage=s;
        auto params=group("(",")"), body=group("{","}");
        auto saved=std::move(t); size_t resume=pos; t=std::move(params); pos=0;
        struct Buffer { unsigned slot, index = 0;bool atomic=false,sign=false,reference=false;std::string type;bool dynamic=false; };
        std::map<std::string,Buffer> buffers; std::map<std::string,std::string> system;
        std::map<std::string,unsigned> references;
        std::map<std::string,unsigned> constantStructures;
        std::set<std::string> declaredStructures;
        std::map<std::string,unsigned> indices;
        std::set<std::string> names,attributes; std::string header,prefix,suffix,framebufferInputs;
        std::set<unsigned> threadgroupSlots;
        std::map<std::string,std::pair<unsigned,unsigned>> images;std::map<std::string,unsigned> samplerBindings;std::set<unsigned> samplePairs;std::set<unsigned> imageSlots;
        std::vector<QGPUConstantSampler> constantSamplers;
        for(const auto &item:globalSamplers)samplerBindings[item.first]=UINT32_MAX;
        bool imageIncluded=false,samplerIncluded=false,comparisonIncluded=false,usesDiscard=false;std::set<std::string> depthTextures;
        std::set<std::string> arrayTextures,cubeTextures,multisampleTextures;std::set<bool> cubeHeaders;
        std::set<unsigned> graphicsHeaders,fetchSlots;
        std::map<std::string,std::string> aliases,structVariables;
        if (aggregate) {
            declaredStructures.insert(result);
            header+=structGLSL(result);prefix+="_qtype_"+result+" _qresult;\n";bool havePosition=false;unsigned index=0;
            for (const auto &f:structures.at(result)) {
                std::string target;
                if(s==2) {
                    if(f.sampleMask) target="gl_SampleMask[0]";
                    else if(!f.depth.empty()) {
                        target="gl_FragDepth";
                        if(f.depth!="any")header+="layout(depth_"+f.depth+") out float gl_FragDepth;\n";
                    } else {
                        if(f.color<0 || f.position || f.attribute>=0 || f.mode || f.type!="float4")fail("fragment result members require float4 color(n) or scalar float depth");
                        target=fragmentColorName(unsigned(f.color));header+="layout(location="+std::to_string(f.color)+") out vec4 "+target+";\n";
                    }
                } else target=f.position?"gl_Position":declareVarying(e,f,header);
                if (f.position) havePosition=true;
                suffix+=target+std::string(" = ")+(f.sampleMask?"int(":"")+"_qresult._qmember"+std::to_string(index++)+(f.sampleMask?")":"")+";\n";
            }
            if(s==1 && !havePosition)fail("vertex output struct must contain float4 position");
        }
        while (pos<t.size()) {
            auto kind=take(),space=kind; bool readonly=kind=="constant", pointer=kind=="device" || kind=="constant" || kind=="threadgroup",reference=false;
            if (pointer) {
                kind=take();if (kind=="const") { readonly=true;kind=take(); }
                reference=pos<t.size() && t[pos]=="&";take(reference?"&":"*");
            }
            bool depthTexture=kind=="depth2d" || kind=="depth2d_array" || kind=="depth2d_ms" || kind=="depth2d_ms_array";
            if(depthTexture)kind="texture"+kind.substr(5);
            bool unsignedTexture=false;
            std::string textureAccess=(kind=="texture2d_ms" || kind=="texture2d_ms_array")?"read":"sample";
            if (kind=="texture2d" || kind=="texture2d_array" || kind=="texturecube" || kind=="texturecube_array" || kind=="texture2d_ms" || kind=="texture2d_ms_array") {
                take("<");auto element=take();unsignedTexture=element=="uint";if(element!="float" && !unsignedTexture)fail("unsupported texture element type");if(pos<t.size() && t[pos]==",") {take(",");take("access");take("::");textureAccess=take();}take(">");
            }
            auto variable=take();samplerBindings.erase(variable);
            if (!identifier(variable) || !names.insert(variable).second) fail("duplicate or invalid parameter");
            take("[["); auto attr=take(); unsigned slot=0;
            if (attr=="buffer" || attr=="threadgroup" || attr=="texture" || attr=="sampler" || attr=="attribute" || attr=="color") {
                take("("); auto number=take(); if (number.empty() || number.size()>2 || number.find_first_not_of("0123456789")!=std::string::npos) fail("invalid binding");
                slot=unsigned(std::stoul(number));if (slot>=(attr=="attribute"?QGPU_MAX_ATTRIBUTES:8)) fail("binding outside profile"); take(")");
            }
            take("]]");
            if (pointer && space=="threadgroup" && attr=="threadgroup" && !s &&
                (bufferComponents(kind) || kind=="atomic_uint" || kind=="atomic_int")) {
                if (readonly || !threadgroupSlots.insert(slot).second) fail("duplicate or readonly threadgroup argument");
                buffers[variable]={slot,0,kind=="atomic_uint" || kind=="atomic_int",kind=="atomic_int",reference,kind,true};
            } else if (space=="threadgroup" || attr=="threadgroup") fail("invalid threadgroup argument");
            else if (pointer && attr=="buffer" && (kind=="atomic_uint" || kind=="atomic_int")) {
                if (s==1 || readonly || e.reflection.strides[slot]) fail("atomics require writable device memory in compute/fragment");
                e.reflection.strides[slot]=4;e.reflection.writeMask|=1u<<slot;
                buffers[variable]={slot,0,true,kind=="atomic_int",reference,kind,false};
                header+="layout(std430,binding="+std::to_string(slot)+") buffer QBuffer"+std::to_string(slot)+" { uint data[]; } _qbuffer"+std::to_string(slot)+";\n";
            } else if (pointer && attr=="buffer" && bufferComponents(kind)) {
                if (e.reflection.strides[slot] || (s==1 && !readonly)) fail("duplicate binding or writable vertex buffer");
                unsigned components=width(kind);e.reflection.strides[slot]=(components==3?4:components)*4;
                if (reference) { references[variable]=slot;aliases[variable]="_qbuffer"+std::to_string(slot)+".data[0]"; }
                else buffers[variable]={slot,0,false,false,false,kind,false};
                if (!readonly) e.reflection.writeMask|=1u<<slot;
                header+="layout(std430,binding="+std::to_string(slot)+") "+(readonly?"readonly ":"")+"buffer QBuffer"+std::to_string(slot)+" { "+glslType(kind)+" data[]; } _qbuffer"+std::to_string(slot)+";\n";
            } else if (pointer && reference && readonly && attr=="buffer" && structures.count(kind)) {
                if (e.reflection.strides[slot]) fail("duplicate constant binding");
                e.reflection.strides[slot]=4;references[variable]=slot;constantStructures[variable]=slot;
                structVariables[variable]=kind;aliases[variable]="_qbuffer"+std::to_string(slot)+".data[0]";
                if (declaredStructures.insert(kind).second) header+=structGLSL(kind);
                header+="layout(std430,binding="+std::to_string(slot)+") readonly buffer QBuffer"+std::to_string(slot)+
                    " { uint data[]; } _qbuffer"+std::to_string(slot)+";\n";
            } else if (!pointer && (kind=="texture2d" || kind=="texture2d_array" || kind=="texturecube" || kind=="texturecube_array" || kind=="texture2d_ms" || kind=="texture2d_ms_array") && attr=="texture") {
                unsigned access=textureAccess=="read"?1:textureAccess=="write"?2:textureAccess=="read_write"?3:textureAccess=="sample"?4:0;
                if (!access || (s && access!=4 && access!=1) || !imageSlots.insert(slot).second) fail("textures require distinct bindings and stage-compatible access");
                bool array=kind=="texture2d_array" || kind=="texturecube_array" || kind=="texture2d_ms_array",cube=kind.rfind("texturecube",0)==0;
                if(depthTexture){if(cube || (access!=1 && access!=4))fail("depth textures require read/sample access and 2D shape");depthTextures.insert(variable);e.reflection.depthTextureMask|=1u<<slot;}
                bool ms=kind=="texture2d_ms" || kind=="texture2d_ms_array";if(ms){if(access!=1)fail("multisample textures are read-only");multisampleTextures.insert(variable);}
                if(unsignedTexture){if(s || ms || depthTexture || access==4)fail("uint textures require compute read/write access without multisampling");e.reflection.imageUintMask|=1u<<slot;}
                if(array)arrayTextures.insert(variable);if(cube)cubeTextures.insert(variable);
                unsigned shape=ms?(array?5:4):unsigned(array)+2u*unsigned(cube);
                if(!s) {
                    if(cube){if(cubeHeaders.insert(array).second)header+=cubeImageHeader(array);}
                    else if(!imageIncluded){header+=imageHeader();imageIncluded=true;}
                } else if(graphicsHeaders.insert(shape).second)
                    header+=graphicsTextureHeader(shape,s);
                images[variable]={slot,access};if(!s && (access<4 || depthTexture))header+=ms?msImageFunctions(slot,array,depthTexture):cube?cubeImageFunctions(slot,access,array,unsignedTexture):imageFunctions(slot,depthTexture?1:access,array,depthTexture,unsignedTexture);
                if(access==4 && !samplerIncluded){header+=samplerHeader(s==2,s==1);samplerIncluded=true;}
            } else if (!pointer && kind=="sampler" && attr=="sampler") {
                for(const auto &bound:samplerBindings)if(bound.second==slot)fail("duplicate sampler slot");
                samplerBindings[variable]=slot;
            } else if (!pointer && s==0 && !computeSystemValue(attr).empty() &&
                (kind=="uint" || ((kind=="uint2" || kind=="uint3") && attr!="thread_index_in_threadgroup"))) {
                if (!attributes.insert(attr).second) fail("duplicate compute input");
                system[variable]=computeSystemValue(attr)+(kind=="uint" && attr!="thread_index_in_threadgroup"?".x":"");
                if (kind=="uint2") system[variable]="uvec2("+system[variable]+")";
                if (attr=="thread_position_in_grid") {
                    indices[variable]=1;
                }
            } else if(!pointer && s==2 && attr=="color" && (kind=="float" || kind=="float2" || kind=="float3" || kind=="float4")) {
                aliases[variable]=declareFramebuffer(e,kind,slot,header,framebufferInputs);
            } else if (!pointer && s==2 && kind=="uint" && (attr=="sample_id" || attr=="sample_mask")) {
                if (!attributes.insert(attr).second) fail("duplicate fragment sample input");
                system[variable]=attr=="sample_id"?"uint(gl_SampleID)":"uint(gl_SampleMaskIn[0])";
            } else if (!pointer && (
                (s==1 && kind=="uint" && (attr=="vertex_id" || attr=="instance_id")) || (s==2 && kind=="float4" && attr=="position"))) {
                if (!attributes.insert(attr).second) fail("duplicate system input");
                system[variable]=s==2?"gl_FragCoord":
                    attr=="instance_id"?"uint(gl_InstanceID)":"uint(gl_VertexID)";
                if (s!=2) indices[variable]=attr=="instance_id"?2:1;
            } else if (!pointer && s==1 && attr=="attribute") {
                std::string key="attribute:"+std::to_string(slot);if (!attributes.insert(key).second) fail("duplicate vertex attribute");
                header+="layout(location="+std::to_string(slot)+") in "+glslType(kind)+" _qattrib"+std::to_string(slot)+";\n";
                aliases[variable]="_qattrib"+std::to_string(slot);
            } else if (!pointer && s!=0 && attr=="stage_in" && structures.count(kind) && attributes.insert("stage_in").second) {
                aliases[variable]="_qstage";structVariables[variable]=kind;
                if (declaredStructures.insert(kind).second) header+=structGLSL(kind);
                prefix+="_qtype_"+kind+" _qstage;\n";unsigned index=0;
                for (const auto &f:structures.at(kind)) {
                    std::string value;
                    if (s==1) {
                        if (f.attribute<0 || f.color>=0 || f.sampleMask || !f.depth.empty() || f.position || f.mode || !attributes.insert("attribute:"+std::to_string(f.attribute)).second) fail("vertex stage_in needs unique attribute members");
                        value="_qattrib"+std::to_string(f.attribute);
                        header+="layout(location="+std::to_string(f.attribute)+") in "+glslType(f.type)+" "+value+";\n";
                    } else if(f.sampleMask) {
                        if (!attributes.insert("sample_mask").second) fail("duplicate fragment sample mask input");
                        value="uint(gl_SampleMaskIn[0])";
                    } else if(f.color>=0)value=declareFramebuffer(e,f.type,unsigned(f.color),header,framebufferInputs);
                    else value=f.position?"gl_FragCoord":declareVarying(e,f,header);
                    prefix+="_qstage._qmember"+std::to_string(index++)+" = "+value+";\n";
                }
            } else fail("unsupported parameter or attribute");
            if (pos<t.size()) take(",");
        }
        t=std::move(saved); pos=resume;
        const std::map<std::string,std::string> types={{"uint","uint"},{"int","int"},{"int2","ivec2"},{"int3","ivec3"},{"int4","ivec4"},{"uint2","uvec2"},{"uint3","uvec3"},{"uint4","uvec4"},{"float","float"},{"float2","vec2"},{"float3","vec3"},{"float4","vec4"},{"bool","bool"},{"bool2","bvec2"},{"bool3","bvec3"},{"bool4","bvec4"}};
        const std::set<std::string> calls={"uint","uint2","uint3","uint4","int","int2","int3","int4","float","float2","float3","float4","bool","bool2","bool3","bool4","min","max","clamp","mix","abs","dot","normalize","exp2","dfdx","dfdy","fwidth"};
        const std::set<std::string> forbidden={"if","else","{","}","for","while","do","switch","case","default","discard","threadgroup","atomic","asm","break","continue","return"};
        std::set<std::string> locals,currentNames=names;
        struct Shared { unsigned stride;bool array,atomic=false,sign=false; };
        std::map<std::string,Shared> shared;
        std::map<std::string,std::string> localTypes;
        std::set<std::string> atomicHelpers;
        std::set<std::string> bitcasts;
        std::set<unsigned> barriers;unsigned sharedBytes=0;
        bool dynamicIncluded=false;std::set<std::string> dynamicHelpers;
        struct Scope {
            std::map<std::string,std::string> aliases,structures;
            std::set<std::string> locals,names;
            std::map<std::string,Shared> shared;
            std::map<std::string,std::string> localTypes;
            std::map<std::string,unsigned> samplers;
        };
        std::vector<Scope> scopes;unsigned localSerial=0;
        auto scope=[&](bool enter) {
            if (enter) { scopes.push_back({aliases,structVariables,locals,currentNames,shared,localTypes,samplerBindings});currentNames.clear(); }
            else {
                auto old=std::move(scopes.back());scopes.pop_back();
                aliases=std::move(old.aliases);structVariables=std::move(old.structures);
                locals=std::move(old.locals);currentNames=std::move(old.names);
                shared=std::move(old.shared);
                localTypes=std::move(old.localTypes);
                samplerBindings=std::move(old.samplers);
            }
        };
        auto declare=[&](const std::string &type,const std::string &name) {
            if (!identifier(name) || types.count(name) || structures.count(name) || forbidden.count(name) ||
                name=="true" || name=="false" || !currentNames.insert(name).second) fail("invalid or duplicate local declaration");
            locals.insert(name);aliases[name]="_qlocal"+std::to_string(localSerial++);
            shared.erase(name);
            samplerBindings.erase(name);
            localTypes[name]=type;
            if (structures.count(type)) structVariables[name]=type;else structVariables.erase(name);
            return aliases[name];
        };
        auto declareSampler=[&](const std::vector<std::string> &part) {
            auto value=constantSampler(part);declare("sampler",value.first);locals.erase(value.first);
            unsigned index=internConstantSampler(constantSamplers,value.second);if(index==UINT32_MAX)fail("too many constant sampler states");
            samplerBindings[value.first]=QGPU_STAGE_SAMPLER_COUNT+index;return std::string();
        };
        auto declareShared=[&](const std::vector<std::string> &part) {
            bool atomic=part.size()>1 && (part[1]=="atomic_uint" || part[1]=="atomic_int");
            if (s || (part.size()!=3 && part.size()!=6) || (!atomic && !bufferComponents(part[1]))) fail("threadgroup declarations require compute scalar/vector storage");
            bool array=part.size()==6;unsigned count=1;
            if (array) {
                auto number=part[4];if (!number.empty() && number.back()=='u') number.pop_back();
                if (part[3]!="[" || part[5]!="]" || number.empty() || number.size()>5 || number.find_first_not_of("0123456789")!=std::string::npos) fail("threadgroup arrays require a constant length");
                count=unsigned(std::stoul(number));
            }
            unsigned n=atomic?1:width(part[1]),stride=(n==3?4:n)*4;
            if (!count || count>QGPU_MAX_THREADGROUP_BYTES/stride || sharedBytes>QGPU_MAX_THREADGROUP_BYTES-count*stride) fail("threadgroup storage exceeds A540 capacity");
            sharedBytes+=count*stride;auto alias=declare(part[1],part[2]);
            header+="shared "+(atomic?std::string("uint"):glslType(part[1]))+" "+alias+"["+std::to_string(count)+"];\n";
            shared[part[2]]={stride,array,atomic,part[1]=="atomic_int"};return std::string();
        };
        std::function<std::string(const std::vector<std::string> &)> expression;
        auto dynamicFunctions=[&](const Buffer &memory) {
            if (!dynamicIncluded) {header+=dynamicThreadgroupHeader();dynamicIncluded=true;}
            auto function="_qdynamic"+std::to_string(memory.slot);
            if (!dynamicHelpers.insert(function).second) return function;
            unsigned n=memory.atomic?1:width(memory.type),stride=(n==3?4:n)*4;
            auto address="_q_addr_mul(uvec2(i,0u),"+std::to_string(stride)+"u)";
            header+="uint "+function+"_word(uint i,uint c) {return _q_dynamic_word("+std::to_string(memory.slot)+"u,"+address+",c);}\n"
                "uint "+function+"_word(int i,uint c) {return i<0?8192u:"+function+"_word(uint(i),c);}\n";
            if (memory.atomic) return function;
            auto type=glslType(memory.type),value=wordLoad("_qdynamic",address,n,true,memory.slot),raw=std::string("value");
            if (memory.type.rfind("float",0)==0) {value="uintBitsToFloat("+value+")";raw="floatBitsToUint(value)";}
            else if (memory.type.rfind("int",0)==0) {value=type+"("+value+")";raw=(n==1?"uint":"uvec"+std::to_string(n))+std::string("(value)");}
            header+=type+" "+function+"_load(uint i) {return "+value+";}\n"+
                type+" "+function+"_load(int i) {return i<0?"+type+"(0):"+function+"_load(uint(i));}\n"+
                type+" "+function+"_store(uint i,"+type+" value) {"+wordStore("_qdynamic",address,raw,n,true,memory.slot)+"return value;}\n"+
                type+" "+function+"_store(int i,"+type+" value) {if(i>=0)"+function+"_store(uint(i),value);return value;}\n";
            return function;
        };
        auto sampled=[&](unsigned textureSlot,unsigned samplerSlot,const std::vector<std::string> &coordinate,const std::vector<std::string> *option,const std::vector<std::string> *layer=nullptr,bool cube=false,const std::vector<std::string> *comparison=nullptr) {
            const auto *literal=samplerSlot>=QGPU_STAGE_SAMPLER_COUNT?&constantSamplers.at(samplerSlot-QGPU_STAGE_SAMPLER_COUNT):nullptr;
            if(cube && literal && !literal->normalized)fail("cube sampling requires normalized coordinates");
            bool fragment=s==2;
            std::string vector=cube?"vec3":"vec2";
            std::string raw=vector+"("+expression(coordinate)+")",slot=std::to_string(samplerSlot)+"u";
            std::string coord=cube?raw:sampleCoordinate(samplerSlot,raw,literal);
            if(comparison) {
                if(!comparisonIncluded){header+=comparisonSamplerHeader(fragment);comparisonIncluded=true;}
                const bool explicitLevel=option && !option->empty() && (*option)[0]=="level";
                const unsigned pair=512+textureSlot*32+samplerSlot+(explicitLevel?256:0);
                const auto uniform=sampleUniform(textureSlot,samplerSlot,explicitLevel,s,literal,true);
                if(samplePairs.insert(pair).second)header+=sampleDeclaration(textureSlot,samplerSlot,explicitLevel,layer!=nullptr,s,false,literal,true);
                std::string point=raw;
                if(layer)point="vec3("+point+",float(uint("+expression(*layer)+")))";
                std::string mirror=literal?std::to_string(Shader::samplerMirrorFlags(*literal))+"u":
                    "("+std::string(s==1?"_qvertex_sampler_info":"_qsampler_info")+"["+std::to_string(samplerSlot)+"]&3u)";
                const std::string arguments=uniform+","+mirror+","+point+",float("+expression(*comparison)+")";
                auto atLevel=[&](const std::string &lod){return "_q_compare_level("+arguments+","+lod+")";};
                std::string bias="0.0",dx,dy;
                if(option) {
                    const auto &o=*option;
                    if(o.size()<4 || o[1]!="(" || o.back()!=")")fail("invalid comparison LOD option");
                    if(o[0]=="level")return atLevel("float("+expression({o.begin()+2,o.end()-1})+")");
                    if(o[0]=="bias" && fragment)bias="float("+expression({o.begin()+2,o.end()-1})+")";
                    else if(o[0]=="gradient2d") {
                        size_t split=o.size();unsigned depth=0;
                        for(size_t j=2;j+1<o.size();++j){if(o[j]=="(" || o[j]=="[")++depth;else if(o[j]==")" || o[j]=="]")--depth;
                            else if(o[j]=="," && !depth){if(split!=o.size())fail("gradient2d takes two vectors");split=j;}}
                        if(split==o.size() || split==2 || split+2>=o.size())fail("gradient2d takes two vectors");
                        dx="vec2("+expression({o.begin()+2,o.begin()+split})+")";dy="vec2("+expression({o.begin()+split+1,o.end()-1})+")";
                    } else fail("unsupported comparison LOD option/stage");
                }
                if(dx.empty()) {
                    if(!fragment)return atLevel("0.0");
                    return "_q_compare_implicit("+arguments+","+bias+")";
                }
                return "_q_compare_gradient("+arguments+","+dx+","+dy+")";
            }
            std::string call=fragment?"texture":"textureLod",tail=fragment?"":",0.0",dx,dy;
            bool level=false;
            if(option) {
                const auto &o=*option;
                if(o.size()<4 || o[1]!="(" || o.back()!=")")fail("invalid sample LOD option");
                if(o[0]=="level" || (o[0]=="bias" && fragment)) {
                    level=o[0]=="level";if(level)call="textureLod";
                    tail=",float("+expression({o.begin()+2,o.end()-1})+")";
                } else if(o[0]==(cube?"gradientcube":"gradient2d")) {
                    size_t split=o.size();unsigned depth=0;
                    for(size_t j=2;j+1<o.size();++j){if(o[j]=="(" || o[j]=="[")++depth;else if(o[j]==")" || o[j]=="]")--depth;
                        else if(o[j]=="," && !depth){if(split!=o.size())fail("gradient2d takes two vectors");split=j;}}
                    if(split==o.size() || split==2 || split+2>=o.size())fail("gradient2d takes two vectors");
                    call="textureGrad";
                    dx=vector+"("+expression({o.begin()+2,o.begin()+split})+")";dy=vector+"("+expression({o.begin()+split+1,o.end()-1})+")";
                    tail=","+dx+","+dy;
                } else fail("unsupported sample LOD option/stage");
            }
            unsigned pair=textureSlot*32+samplerSlot+(level?256:0);
            std::string uniform=sampleUniform(textureSlot,samplerSlot,level,s,literal);
            if(samplePairs.insert(pair).second)header+=sampleDeclaration(textureSlot,samplerSlot,level,layer!=nullptr,s,cube,literal);
            if(layer){std::string index="float(uint("+expression(*layer)+"))";coord=(cube?"vec4(":"vec3(")+coord+","+index+")";raw=(cube?"vec4(":"vec3(")+raw+","+index+")";}
            if(!cube && call=="textureGrad")return sampleGradientValue(uniform,samplerSlot,raw,dx,dy,literal);
            if(fragment && !cube && call=="texture")return sampleImplicit(uniform,samplerSlot,raw,tail.empty()?"0.0":tail.substr(1),layer!=nullptr,literal);
            return call+"("+uniform+","+coord+tail+")";
        };
        expression=[&](const std::vector<std::string> &part) {
          std::string lowered;
          for (size_t i=0;i<part.size();++i) {
            const auto &v=part[i];auto next=[&](unsigned n,const char *value) { return i+n<part.size() && part[i+n]==value; };
            if (v=="as_type") {
                if (!next(1,"<") || i+4>=part.size() || !next(3,">") || !next(4,"(")) fail("as_type requires one 32-bit scalar/vector type");
                auto target=part[i+2];unsigned lanes=width(target);
                size_t end=i+5;unsigned depth=1;
                for (;end<part.size();++end) {
                    if (part[end]=="(") ++depth;
                    else if (part[end]==")" && !--depth) break;
                    if (depth==1 && part[end]==",") fail("as_type requires one operand");
                }
                if (end==part.size() || end==i+5) fail("invalid as_type operand");
                if (bitcasts.insert(target).second) header+=bitcastHeader(target,lanes);
                lowered+=" _q_as_"+target+"("+expression({part.begin()+i+5,part.begin()+end})+")";i=end;continue;
            }
            if (v.rfind("atomic_",0)==0 && next(1,"(")) {
                std::string op;
                if (v=="atomic_load_explicit") op="load";
                else if (v=="atomic_store_explicit") op="store";
                else if (v=="atomic_exchange_explicit") op="xchg";
                else if (v=="atomic_compare_exchange_weak_explicit" || v=="atomic_compare_exchange_strong_explicit") op="cas";
                else if (v.rfind("atomic_fetch_",0)==0 && v.size()>22 && v.substr(v.size()-9)=="_explicit") op=v.substr(13,v.size()-22);
                if (s==1 || !atomicOperation(op)) fail("unsupported atomic operation");
                std::vector<std::vector<std::string>> args(1);unsigned depth=1,brackets=0;size_t end=i+2;
                for (;end<part.size();++end) {
                    auto token=part[end];if (token=="(") ++depth;else if (token==")" && !--depth) break;
                    if (token=="[") ++brackets;else if (token=="]") { if (!brackets) fail("invalid atomic argument");--brackets; }
                    if (token=="," && depth==1 && !brackets) args.emplace_back();else args.back().push_back(token);
                }
                unsigned count=op=="load"?2:op=="cas"?5:3;
                if (end==part.size() || args.size()!=count) fail("invalid atomic arguments");
                for (unsigned a=count-(op=="cas"?2:1);a<count;++a)
                    if (args[a]!=std::vector<std::string>{"memory_order_relaxed"}) fail("only relaxed atomics are supported");
                auto pointer=args[0];bool address=!pointer.empty() && pointer[0]=="&";if (address) pointer.erase(pointer.begin());
                if (pointer.empty()) fail("missing atomic pointer");
                auto name=pointer[0];bool group=shared.count(name),sign=false,array=false,dynamic=false;std::string target,index="0u",identity,dynamicFunction;
                if (group) {
                    const auto &memory=shared.at(name);if (!memory.atomic) fail("atomic operation requires an atomic type");
                    sign=memory.sign;array=memory.array;target=aliases.at(name);identity=target;
                } else {
                    auto b=buffers.find(name);if (b==buffers.end() || locals.count(name) || !b->second.atomic) fail("atomic pointer is not a declared atomic resource");
                    sign=b->second.sign;array=!b->second.reference;target="_qbuffer"+std::to_string(b->second.slot)+".data";identity="_qbuffer"+std::to_string(b->second.slot);
                    dynamic=b->second.dynamic;
                    if (dynamic) {dynamicFunction=dynamicFunctions(b->second);target="_qdynamic";identity="_qdynamic";}
                    else e.reflection.bufferMask|=1u<<b->second.slot;
                }
                if (pointer.size()>1) {
                    if (!array || !address || pointer.size()<4 || pointer[1]!="[" || pointer.back()!="]") fail("atomic subscript requires an element address");
                    index=dynamic?expression({pointer.begin()+2,pointer.end()-1}):(group?"_q_shared_index(":"_q_index(")+expression({pointer.begin()+2,pointer.end()-1})+",4u)";
                } else if (address==array) fail("atomic pointer shape disagrees with its declaration");
                if (dynamic) index=dynamicFunction+"_word("+index+",0u)";
                std::string value=op=="load"?"0u":expression(args[op=="cas"?2:1]),result;
                if (op=="store") {
                    auto function=identity+"_store";
                    if (atomicHelpers.insert(function).second) header+="void "+function+"(uint word,uint value) { "+(dynamic?"if(word>=8192u)return;":"")+"atomicExchange("+target+"[word],value); }\n";
                    result=function+"("+index+",uint("+value+"))";
                } else if (op=="cas") {
                    if (args[1].size()!=2 || args[1][0]!="&" || !localTypes.count(args[1][1]) || shared.count(args[1][1]) ||
                        localTypes.at(args[1][1])!=(sign?"int":"uint")) fail("compare-exchange expected must be a matching thread-local scalar");
                    std::string function=identity+"_cas"+(sign?"_s":"_u"),type=sign?"int":"uint";
                    if (atomicHelpers.insert(function).second) header+="bool "+function+"(uint word,inout "+type+" expected,"+type+" desired) { uint wanted=uint(expected);uint old=0u;"+(dynamic?"if(word<8192u)":"")+"old=atomicCompSwap("+target+"[word],wanted,uint(desired));if(old!=wanted)expected="+type+"(old);return old==wanted;}\n";
                    result=function+"("+index+","+aliases.at(args[1][1])+","+value+")";
                } else if (sign && (op=="min" || op=="max")) {
                    auto function=identity+"_s"+op;
                    if (atomicHelpers.insert(function).second) header+=signedAtomicHelper(function,target,op);
                    result="int("+function+"("+index+",uint("+value+")))";
                } else {
                    if (dynamic) {
                        auto function="_qdynamic_atomic_"+op;
                        if (atomicHelpers.insert(function).second) header+=dynamicAtomicHelper(function,op);
                        result=function+"("+index+",uint("+value+"),0u)";
                    } else result=atomicExpression(op,target+"["+index+"]",value);
                    if (sign && op!="store") result="int("+result+")";
                }
                lowered+=" "+result;i=end;continue;
            }
            if (v=="threadgroup_barrier") {
                if (s || !next(1,"(")) fail("threadgroup barrier requires compute");
                unsigned flags=0;size_t end=i+2;bool operand=true;
                while (end<part.size() && part[end]!=")") {
                    if (!operand) { if (part[end++]!="|") fail("barrier flags must be constant");operand=true;continue; }
                    if (part[end]=="mem_flags") {
                        if (end+2<part.size() && part[end+1]=="::") end+=2;
                        else fail("invalid barrier flag namespace");
                    }
                    auto flag=part[end++];
                    if (flag=="mem_device") flags|=1;
                    else if (flag=="mem_threadgroup") flags|=2;
                    else if (flag=="mem_texture") flags|=4;
                    else if (flag!="mem_none" && flag!="0" && flag!="0u") fail("unsupported barrier memory flags");
                    operand=false;
                }
                if (operand || end==part.size()) fail("invalid barrier flags");
                if (barriers.insert(flags).second) header+="void _qbarrier"+std::to_string(flags)+"() {\n"+workgroupBarrier(flags)+"}\n";
                lowered+=" _qbarrier"+std::to_string(flags)+"()";i=end;continue;
            }
            if (forbidden.count(v)) fail("unsupported expression or shared memory/atomic operation");
            // A swizzle/member spelling is not a reference to a same-named
            // local or system input. Struct members are handled below.
            if (i && part[i-1]==".") { lowered+=" "+v;continue; }
            if (structVariables.count(v) && next(1,".")) {
                if (i+2>=part.size()) fail("incomplete struct member access");
                const auto &fields=structures.at(structVariables.at(v));size_t member=0;
                while (member<fields.size() && fields[member].name!=part[i+2]) ++member;
                if (member==fields.size()) fail("unknown struct member");
                if (constantStructures.count(v) && !locals.count(v)) {
                    unsigned offset=0;
                    for (size_t n=0;n<=member;++n) {
                        unsigned components=width(fields[n].type),alignment=components==1?4:components==2?8:16;
                        offset=(offset+alignment-1)&~(alignment-1);
                        if (n!=member) offset+=(components==3?4:components)*4;
                    }
                    unsigned slot=constantStructures.at(v),components=width(fields[member].type);
                    e.reflection.bufferMask|=1u<<slot;
                    auto value=bufferLoad(slot,addressConstant(offset),components);
                    if (fields[member].type.rfind("float",0)==0) value="uintBitsToFloat("+value+")";
                    else if (fields[member].type.rfind("int",0)==0) value=glslType(fields[member].type)+"("+value+")";
                    lowered+=" ("+value+")";i+=2;continue;
                }
                lowered+=" "+aliases.at(v)+"._qmember"+std::to_string(member);i+=2;continue;
            }
            if (shared.count(v)) {
                auto memory=shared.at(v);
                if (memory.atomic) fail("atomic storage requires an atomic operation");
                if (!memory.array) { lowered+=" "+aliases.at(v)+"[0]";continue; }
                if (!next(1,"[")) fail("threadgroup array pointers cannot escape");
                size_t end=i+2;unsigned depth=1;
                for (;end<part.size();++end) { if (part[end]=="[") ++depth;else if (part[end]=="]" && !--depth) break; }
                if (end==part.size() || end==i+2) fail("invalid threadgroup subscript");
                lowered+=" "+aliases.at(v)+"[_q_shared_index("+expression({part.begin()+i+2,part.begin()+end})+","+std::to_string(memory.stride)+"u)]";
                i=end;continue;
            }
            if (locals.count(v)) { lowered+=" "+aliases.at(v);continue; }
            if (constantStructures.count(v)) fail("constant structs require a named field access");
            if (references.count(v)) e.reflection.bufferMask|=1u<<references.at(v);
            auto b=buffers.find(v);
            if (b!=buffers.end()) {
                if (b->second.atomic) fail("atomic storage requires an atomic operation");
                if (b->second.dynamic) {
                    auto function=dynamicFunctions(b->second);size_t end=i;std::string index="0u";
                    if (!b->second.reference) {
                        if (!next(1,"[")) fail("threadgroup pointers may only be subscripted");
                        unsigned depth=1;end=i+2;
                        for (;end<part.size();++end) {if(part[end]=="[")++depth;else if(part[end]=="]" && !--depth)break;}
                        if(end==part.size() || end==i+2)fail("invalid dynamic threadgroup subscript");
                        index=expression({part.begin()+i+2,part.begin()+end});
                    }
                    std::string op=end+1<part.size()?part[end+1]:"";
                    if (op=="=" || op=="+=" || op=="-=" || op=="*=" || op=="/=" || op=="%=" || op=="&=" || op=="|=" || op=="^=") {
                        size_t stop=end+2;unsigned parens=0,brackets=0;
                        for (;stop<part.size();++stop) {
                            const auto &t=part[stop];if(!parens && !brackets && (t=="," || t==")" || t=="]"))break;
                            if(t=="(")++parens;else if(t==")")--parens;else if(t=="[")++brackets;else if(t=="]")--brackets;
                        }
                        if(stop==end+2)fail("missing threadgroup assignment value");
                        auto value=expression({part.begin()+end+2,part.begin()+stop});auto store=function+"_store";
                        if (op!="=") {
                            static const std::map<std::string,std::string> names={{"+=","add"},{"-=","sub"},{"*=","mul"},{"/=","div"},{"%=","mod"},{"&=","and"},{"|=","or"},{"^=","xor"}};
                            store=function+"_"+names.at(op);auto type=glslType(b->second.type);
                            if(dynamicHelpers.insert(store).second) header+=type+" "+store+"(uint i,"+type+" value) {return "+function+"_store(i,"+function+"_load(i)"+op.substr(0,1)+"value);}\n"+
                                type+" "+store+"(int i,"+type+" value) {if(i<0)return value;return "+store+"(uint(i),value);}\n";
                        }
                        lowered+=" "+store+"("+index+","+value+")";i=stop-1;
                    } else {lowered+=" "+function+"_load("+index+")";i=end;}
                    continue;
                }
                if (!next(1,"[")) fail("buffer pointers may only be subscripted");
                size_t end=i+2;unsigned depth=1;
                for (;end<part.size();++end) {
                    if (part[end]=="[") ++depth;
                    else if (part[end]=="]" && !--depth) break;
                    if (depth>64) fail("buffer subscript nesting exceeded");
                }
                if (end==part.size() || end==i+2) fail("invalid buffer subscript");
                unsigned slot=b->second.slot; e.reflection.bufferMask|=1u<<slot;
                if (end==i+3 && indices.count(part[i+2]) && !locals.count(part[i+2]) && indices.at(part[i+2])==2)
                    e.reflection.instanceMask|=1u<<slot;
                lowered+=" _qbuffer"+std::to_string(slot)+".data[_q_index("+expression({part.begin()+i+2,part.begin()+end})+","+
                    std::to_string(e.reflection.strides[slot])+"u)] ";i=end;continue;
            }
            if(images.count(v) && !locals.count(v)) {
                if(!next(1,".") || i+3>=part.size() || !next(3,"("))fail("texture handles cannot escape");
                auto memory=images.at(v);auto method=part[i+2];std::vector<std::vector<std::string>> args(1);unsigned depth=1;size_t end=i+4;
                for(;end<part.size();++end){auto token=part[end];if(token=="(")++depth;else if(token==")" && !--depth)break;
                    if(token=="," && depth==1)args.emplace_back();else args.back().push_back(token);}
                if(end==part.size())fail("unterminated texture operation");
                if(args.size()==1 && args[0].empty())args.clear();
                bool array=arrayTextures.count(v),cube=cubeTextures.count(v),ms=multisampleTextures.count(v),depthTexture=depthTextures.count(v);
                unsigned sampleExtra=array?1:0,imageExtra=sampleExtra+(cube?1:0);
                std::string slot=std::to_string(memory.first)+"u",prefix=cube?cubeImagePrefix(array):"_q";
                std::string function=(cube?prefix:"_qimage")+std::to_string(memory.first),value;
                unsigned shape=ms?(array?5:4):unsigned(array)+2u*unsigned(cube);
                if(ms)function=(array?"_qmsarrayimage":"_qmsimage")+std::to_string(memory.first);
                if(e.reflection.imageUintMask&(1u<<memory.first))function+="_uint";
                if(s) {prefix=graphicsTexturePrefix(shape,s);function=prefix+std::to_string(memory.first);}
                auto indexArguments=[&](unsigned first) {
                    std::string result;
                    for(unsigned j=0;j<imageExtra;++j)result+="uint("+expression(args[first+j])+"),";
                    return result;
                };
                if(ms && method=="read" && args.size()==2+sampleExtra) {
                    if(s && fetchSlots.insert(memory.first).second)header+=graphicsFetchFunctions(memory.first,shape,s);
                    value=function+"_read(uvec2("+expression(args[0])+"),"+(array?"uint("+expression(args[1])+"),":"")+"uint("+expression(args[1+sampleExtra])+")"+")";
                } else if(ms && method=="get_num_samples" && args.empty())value=s?"(1u<<(("+prefix+"_info["+slot+"].w>>28u)&3u))":(array?"_q_ms_array_samples(":"_q_ms_samples(")+slot+")";
                else if(ms && (method=="get_width" || method=="get_height") && args.empty())value=(s?prefix+"_info":array?"_qimage_ms_array_info":"_qimage_ms_info")+"["+slot+"]."+(method=="get_width"?"x":"y");
                else if(ms && array && method=="get_array_size" && args.empty())value=s?prefix+"_info["+slot+"].z":"_q_ms_array_layers("+slot+")";
                else if(ms)fail("unsupported multisample texture operation");
                else if((method=="sample" || (method=="sample_compare" && depthTexture)) && memory.second==4 &&
                    (args.size()==2+sampleExtra+unsigned(method=="sample_compare") || args.size()==3+sampleExtra+unsigned(method=="sample_compare"))) {
                    const unsigned compareExtra=method=="sample_compare";
                    if(args[0].size()!=1 || !samplerBindings.count(args[0][0]) || locals.count(args[0][0]))fail("sample requires an explicit sampler");
                    unsigned samplerSlot=samplerBindings.at(args[0][0]);
                    if(samplerSlot==UINT32_MAX) {
                        unsigned index=internConstantSampler(constantSamplers,globalSamplers.at(args[0][0]));if(index==UINT32_MAX)fail("too many constant sampler states");
                        samplerSlot=QGPU_STAGE_SAMPLER_COUNT+index;samplerBindings[args[0][0]]=samplerSlot;
                    }
                    value=sampled(memory.first,samplerSlot,args[1],args.size()==3+sampleExtra+compareExtra?&args[2+sampleExtra+compareExtra]:nullptr,array?&args[2]:nullptr,cube,compareExtra?&args[2+sampleExtra]:nullptr);
                    if(depthTexture && !compareExtra)value="("+value+").r";
                } else if(method=="read" && ((memory.second&1) || ((s || depthTexture) && memory.second==4)) && (args.size()==1+imageExtra || args.size()==2+imageExtra)) {
                    if(s && fetchSlots.insert(memory.first).second)header+=graphicsFetchFunctions(memory.first,shape,s);
                    value=function+"_read(uvec2("+expression(args[0])+"),"+indexArguments(1)+(args.size()==2+imageExtra?"uint("+expression(args[1+imageExtra])+")":"0u")+")";
                } else if(!s && method=="write" && (memory.second&2) && (args.size()==2+imageExtra || args.size()==3+imageExtra))
                    value=function+"_write("+expression(args[0])+",uvec2("+expression(args[1])+"),"+indexArguments(2)+(args.size()==3+imageExtra?"uint("+expression(args[2+imageExtra])+")":"0u")+")";
                else if(s && (method=="get_width" || method=="get_height") && args.size()<=1)
                    value=prefix+"_size("+slot+","+(args.empty()?"0u":"uint("+expression(args[0])+")")+")."+(method=="get_width"?"x":"y");
                else if(s && method=="get_num_mip_levels" && args.empty())value="(("+prefix+"_info["+slot+"].w>>8u)&255u)";
                else if(s && method=="get_array_size" && array && args.empty())value=prefix+"_info["+slot+"].z";
                else if(!s && (method=="get_width" || method=="get_height") && args.size()<=1)
                    value=prefix+(array?"_array_image_size(":"_image_size(")+slot+","+(args.empty()?"0u":"uint("+expression(args[0])+")")+")."+(method=="get_width"?"x":"y");
                else if(!s && method=="get_num_mip_levels" && args.empty())value=array?prefix+"_array_image_mips("+slot+")":"(("+(cube?prefix+"_info":"_qimage_info")+"["+slot+"].w>>8u)&255u)";
                else if(!s && method=="get_array_size" && array && args.empty())value=cube?"("+prefix+"_face_info["+slot+"].x/6u)":"_qimage_array_info["+slot+"].x";
                else fail("unsupported texture operation/access qualifier");
                if(depthTexture && s && method=="read")value="("+value+").r";
                lowered+=" "+value;i=end;continue;
            }
            if(samplerBindings.count(v) && !locals.count(v))fail("sampler handles cannot escape");
            if (v=="[" || v=="]") fail("local or dynamic arrays are unsupported");
            if (identifier(v) && next(1,"(") && !calls.count(v)) fail("unsupported call: "+v);
            if(v=="dfdx" || v=="dfdy" || v=="fwidth") {
                if(s!=2)fail("derivatives require a fragment shader");
                lowered+=v=="dfdx"?" dFdx":v=="dfdy"?" dFdy":" fwidth";
            }
            else if (system.count(v)) lowered+=" "+system[v];
            else if (aliases.count(v)) lowered+=" "+aliases.at(v);
            else if (structures.count(v)) lowered+=" _qtype_"+v;
            else if (types.count(v)) lowered+=" "+types.at(v);
            else lowered+=" "+v;
          }
          return lowered;
        };
        auto output=[&](const std::vector<std::string> &part) {
            if (result=="void") { if (!part.empty()) fail("void shader cannot return a value");return std::string("return;\n"); }
            if (part.empty()) fail("graphics shader must return its output");
            return std::string(aggregate?"_qresult = ":s==1?"gl_Position = ":"_qcolor = ")+expression(part)+";\n"+suffix+"return;\n";
        };
        Statements statements(body,[&](const std::string &error){fail(error);},expression,
            [&](const std::string &name){return types.count(name) || structures.count(name);},
            [&](const std::string &name){return types.count(name)?types.at(name):"_qtype_"+name;},declare,declareShared,declareSampler,scope,output,
            [&](){if(s!=2)fail("discard_fragment is fragment-only");usesDiscard=true;return std::string("demote;\n");});
        std::string lowered=statements.run(result!="void");
        e.reflection.writeMask&=e.reflection.bufferMask;
        for (unsigned i=0;i<8;++i) if (!(e.reflection.bufferMask&(1u<<i))) e.reflection.strides[i]=0;
        header=bufferHeader()+header;
        if (!s) header=computeHeader()+header;
        if(s==2 && !aggregate && result!="void")header+="layout(location=0) out vec4 _qcolor;\n";
        e.glsl="#version 430\n"+std::string(comparisonIncluded?"#extension GL_EXT_texture_shadow_lod : require\n":"")+std::string(usesDiscard?"#extension GL_EXT_demote_to_helper_invocation : require\n":"")+header+"void main() {"+prefix+framebufferInputs+lowered+"}\n"; return e;
    }
};
}
bool lowerMSL(const std::string &source,std::vector<Entry> &out,std::string &error) {
    out.clear();
    try { Parser p(source); std::set<std::string> names;
        while (p.pos<p.t.size()) {
            if(p.t[p.pos]=="constexpr" || p.t[p.pos]=="constant") {p.globalSampler();continue;}
            if (p.t[p.pos]=="struct") { p.structure();continue; }
            if (p.t[p.pos]=="using") { p.take("using");p.take("namespace");p.take("metal");p.take(";");continue; }
            auto e=p.entry(); if (!names.insert(e.name).second || out.size()==64) p.fail("duplicate entry or entry limit exceeded");
            out.push_back(std::move(e));
        }
        if (out.empty()) p.fail("library has no entries"); return true;
    } catch (const std::exception &e) { error=e.what(); out.clear(); return false; }
}
} }
