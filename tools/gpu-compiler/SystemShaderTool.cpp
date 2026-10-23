// SPDX-License-Identifier: BSD-2-Clause
// Host-only catalog builder. Uses the same C-ABI AIR reader and JIT validation
// as the driver; never loads IOKit or submits GPU work.
#include "Compiler.h"
#include "LibraryArchive.h"
#include <GPUPrograms.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <iomanip>
static std::string read(const char *path) {std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static std::string hex(const void *p,size_t n) {
    std::ostringstream s;s<<std::hex<<std::setfill('0');auto b=static_cast<const unsigned char *>(p);
    for(size_t i=0;i<n;++i)s<<std::setw(2)<<unsigned(b[i]);return s.str();
}
int main(int argc,char **argv) {
    if(argc<3)return 64;auto bytes=read(argv[2]);std::string error,mode=argv[1];
    if(mode=="index") {
        std::vector<GPU::Compiler::LibraryArchive::Function> functions;
        if(!GPU::Compiler::LibraryArchive::parse(bytes.data(),bytes.size(),functions,error)){std::cerr<<error;return 1;}
        for(const auto &f:functions)std::cout<<hex(f.hash.data(),32)<<'\t'<<hex(f.name.data(),f.name.size())<<'\t'<<f.type<<'\t'<<f.offset<<'\t'<<f.size<<'\n';
        return 0;
    }
    if(argc<4)return 64;
    std::vector<GPU::Compiler::FunctionConstant> declarations;unsigned stage;
    if(!GPU::Compiler::inspectAIRConstants(bytes.data(),bytes.size(),argv[3],declarations,stage,error)){std::cerr<<error;return 1;}
    if(mode=="inspect") {
        std::cout<<stage<<'\n';
        for(const auto &d:declarations)
            std::cout<<d.index<<'\t'<<d.type<<'\t'<<d.required<<'\t'<<hex(d.name.data(),d.name.size())<<'\n';
        return 0;
    }
    if(mode!="compile" || argc!=6)return 64;
    std::ifstream file(argv[4]);std::vector<GPU::Compiler::FunctionConstantValue> supplied,effective;std::string line;
    while(std::getline(file,line)) {
        if(line.empty())continue;
        std::istringstream s(line);GPU::Compiler::FunctionConstantValue v;std::string data,extra;
        if(!(s>>v.index>>v.type>>data) || (s>>extra) || data.size()!=2*GPU::Compiler::constantBytes(v.type))return 64;
        for(size_t i=0;i<data.size()/2;++i){auto pair=data.substr(2*i,2);char *end=nullptr;unsigned long b=std::strtoul(pair.c_str(),&end,16);if(*end)return 64;v.bytes[i]=uint8_t(b);}
        supplied.push_back(v);
    }
    if(!GPU::Compiler::resolveFunctionConstants(declarations,supplied,effective,error)){std::cerr<<error;return 1;}
    std::vector<QGPUProgramImage> images;GPU::Compiler::AIRCompileInfo info;
    if(!GPU::Compiler::compileAIR(bytes.data(),bytes.size(),GPUPrograms::Registry,images,error,argv[3],effective,&info) ||
       images.size()!=1 || !GPU::JIT::valid(images[0],GPUPrograms::Registry)){std::cerr<<error;return 1;}
    std::ofstream out(argv[5],std::ios::binary);out.write(reinterpret_cast<const char *>(&images[0]),sizeof(images[0]));
    if(!out)return 1;
    std::cerr<<"route="<<unsigned(info.route)<<" lower_ns="<<info.lowerNS<<" backend_ns="<<info.backendNS<<'\n';
    for(const auto &v:effective)std::cout<<v.index<<'\t'<<v.type<<'\t'<<hex(v.bytes.data(),GPU::Compiler::constantBytes(v.type))<<'\n';
}
