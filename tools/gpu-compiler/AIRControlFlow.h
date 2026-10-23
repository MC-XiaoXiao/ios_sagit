// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GPU { namespace Compiler { namespace AIRFlow {
struct Copy { std::string type,target,value; };
struct Block {
    enum Term { Return,Jump,Conditional,Switch } term=Return;
    std::string code,condition;
    unsigned destination=0,alternate=0;
    std::vector<std::pair<uint32_t,unsigned>> cases;
    std::map<unsigned,std::vector<Copy>> incoming;
};
inline std::vector<unsigned> successors(const Block &b) {
    if (b.term==Block::Return) return {};
    std::vector<unsigned> out{b.destination};
    if (b.term==Block::Conditional) out.push_back(b.alternate);
    if (b.term==Block::Switch) for (auto c:b.cases) out.push_back(c.second);
    return out;
}
// Shared by the LLVM C++ AOT and exported-C-ABI runtime readers. PHIs are
// parallel copies on the selected predecessor edge, including loop backedges:
// https://llvm.org/docs/LangRef.html#phi-instruction
inline bool emit(const std::vector<Block> &blocks,unsigned entry,std::string &out,std::string &error) {
    out.clear();const unsigned count=unsigned(blocks.size());
    if (!count || count>128 || entry>=count) { error="AIR CFG exceeds 128 blocks";return false; }
    std::set<unsigned> live;bool bad=false,cyclic=false;std::vector<unsigned> color(count);
    std::function<void(unsigned)> visit=[&](unsigned id) {
        if (id>=count) { bad=true;return; }
        if (color[id]) { cyclic|=color[id]==1;return; }
        color[id]=1;live.insert(id);
        for (auto next:successors(blocks[id])) visit(next);
        color[id]=2;
    };
    visit(entry);if (bad) { error="AIR CFG has a foreign successor";return false; }
    unsigned serial=0;
    auto edge=[&](unsigned from,unsigned to) {
        std::string code;auto it=blocks[to].incoming.find(from);
        if (it==blocks[to].incoming.end()) return code;
        std::vector<std::string> temporaries;
        for (const auto &copy:it->second) {
            auto temp="_qedge"+std::to_string(serial++);temporaries.push_back(temp);
            code+=copy.type+" "+temp+" = "+copy.value+";\n";
        }
        for (unsigned i=0;i<temporaries.size();++i) code+=it->second[i].target+" = "+temporaries[i]+";\n";
        return code;
    };
    // Acyclic control flow uses ordinary structured GLSL branches and joins.
    // Bounded duplication handles crossing DAG edges; large/irreducible CFGs
    // and loops use a per-invocation dispatcher, preserving all edges instead
    // of speculating both sides or truncating the loop's iteration count.
    if (!cyclic) {
        auto all=live;all.insert(count);std::vector<std::set<unsigned>> pd(count+1,all);pd[count]={count};
        bool changed=true;
        while (changed) {
            changed=false;
            for (auto id:live) {
                auto next=successors(blocks[id]);if (next.empty()) next.push_back(count);
                auto value=pd[next.front()];
                for (size_t i=1;i<next.size();++i) {
                    std::set<unsigned> common;
                    std::set_intersection(value.begin(),value.end(),pd[next[i]].begin(),pd[next[i]].end(),std::inserter(common,common.end()));
                    value=std::move(common);
                }
                value.insert(id);if (value!=pd[id]) { pd[id]=std::move(value);changed=true; }
            }
        }
        std::vector<unsigned> join(count,count);
        for (auto id:live) for (auto candidate:pd[id]) if (candidate!=id) {
            bool nearest=true;for (auto other:pd[id]) if (other!=id && other!=candidate && !pd[candidate].count(other)) nearest=false;
            if (nearest) { join[id]=candidate;break; }
        }
        unsigned visits=0;bool overflow=false;
        std::function<std::string(unsigned,unsigned)> path=[&](unsigned id,unsigned stop) {
            std::string code;
            while (id!=stop && id!=count && !overflow) {
                if (++visits>512) { overflow=true;break; }
                const auto &b=blocks[id];code+=b.code;
                if (b.term==Block::Return) { code+="return;\n";break; }
                if (b.term==Block::Jump) { code+=edge(id,b.destination);id=b.destination;continue; }
                auto merge=join[id];
                if (b.term==Block::Conditional) {
                    code+="if ("+b.condition+") {\n"+edge(id,b.destination)+path(b.destination,merge)+"} else {\n"+
                        edge(id,b.alternate)+path(b.alternate,merge)+"}\n";
                } else {
                    code+="switch (uint("+b.condition+")) {\n";
                    for (auto c:b.cases) code+="case "+std::to_string(c.first)+"u: {\n"+edge(id,c.second)+path(c.second,merge)+"break;\n}\n";
                    code+="default: {\n"+edge(id,b.destination)+path(b.destination,merge)+"break;\n}\n}\n";
                }
                id=merge;
            }
            return code;
        };
        auto code=path(entry,count);if (!overflow) { out=std::move(code);return true; }
    }
    serial=0;out="uint _qblock = "+std::to_string(entry)+"u;\nfor (;;) {\nswitch (_qblock) {\n";
    auto jump=[&](unsigned from,unsigned to) { return edge(from,to)+"_qblock = "+std::to_string(to)+"u;\n"; };
    for (auto id:live) {
        const auto &b=blocks[id];out+="case "+std::to_string(id)+"u: {\n"+b.code;
        if (b.term==Block::Return) out+="return;\n";
        else if (b.term==Block::Jump) out+=jump(id,b.destination);
        else if (b.term==Block::Conditional) out+="if ("+b.condition+") {\n"+jump(id,b.destination)+"} else {\n"+jump(id,b.alternate)+"}\n";
        else {
            out+="switch (uint("+b.condition+")) {\n";
            for (auto c:b.cases) out+="case "+std::to_string(c.first)+"u: {\n"+jump(id,c.second)+"break;\n}\n";
            out+="default: {\n"+jump(id,b.destination)+"break;\n}\n}\n";
        }
        out+="break;\n}\n";
    }
    out+="default: return;\n}\n}\n";return true;
}
} } }
