// SPDX-License-Identifier: BSD-2-Clause
// Offline inspection only. Include the frozen production reader for its AIR
// parser and specialization, without calling its GLSL Lower implementation.
#include "AIRRuntime.cpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iterator>
int main(int argc,char **argv) {
    if(argc!=5)return 64;
    std::ifstream in(argv[1],std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in),{}};
    std::vector<GPU::Compiler::FunctionConstantValue> constants;
    std::ifstream values(argv[3]);unsigned index,type;std::string hex;
    while(values>>index>>type>>hex) {
        GPU::Compiler::FunctionConstantValue v;v.index=index;v.type=type;
        for(unsigned i=0;i<hex.size()/2;++i)v.bytes[i]=std::stoul(hex.substr(i*2,2),nullptr,16);
        constants.push_back(v);
    }
    try {
        using namespace GPU::Compiler;
        Module m;initializeModule(bytes.data(),bytes.size(),m);
        AIRSpecialization::State specialization(m.module,argv[2]);
        if(specialization.present())specialization.apply(constants);
        specialization.lowerHelpers();
        char *error=nullptr;if(LLVMPrintModuleToFile(m.module,argv[4],&error))throw std::runtime_error(error);
        auto function=LLVMGetNamedFunction(m.module,argv[2]);
        std::map<unsigned,unsigned> opcodes;std::map<std::string,unsigned> calls;
        unsigned instructions=0;
        for(auto b=LLVMGetFirstBasicBlock(function);b;b=LLVMGetNextBasicBlock(b))
            for(auto i=LLVMGetFirstInstruction(b);i;i=LLVMGetNextInstruction(i)) {
                ++instructions;++opcodes[LLVMGetInstructionOpcode(i)];
                if(LLVMGetInstructionOpcode(i)==LLVMCall)++calls[name(LLVMGetCalledValue(i))];
            }
        std::cout<<"blocks="<<LLVMCountBasicBlocks(function)<<" instructions="<<instructions<<"\n";
        for(auto &p:opcodes)std::cout<<"op "<<p.first<<" "<<p.second<<"\n";
        for(auto &p:calls)std::cout<<"call "<<p.first<<" "<<p.second<<"\n";
    }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
