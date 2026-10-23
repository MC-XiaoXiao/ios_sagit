// SPDX-License-Identifier: BSD-2-Clause
// This executable deliberately links no compiler/LLVM library.
#include <GPUSystemShaders.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
int main(int argc,char **argv) {
    if(argc!=2)return 64;
    using namespace GPU;using Clock=std::chrono::steady_clock;
    std::vector<double> first,steady;size_t count=0;auto image=std::make_unique<QGPUProgramImage>();
    for(const auto &e:GPUSystemShaders::Entries) {
        if(SystemShaders::find(GPUSystemShaders::Entries,GPUSystemShaders::Count,e.airHash,e.name)!=&e)return 1;
        auto wrong=e.airHash;wrong[0]^=255;
        if(SystemShaders::find(GPUSystemShaders::Entries,GPUSystemShaders::Count,wrong,e.name) ||
           SystemShaders::find(GPUSystemShaders::Entries,GPUSystemShaders::Count,e.airHash,"absent entry"))return 2;
        std::vector<Compiler::FunctionConstant> declarations;unsigned stage;
        SystemShaders::reflect(e,declarations,stage);if(stage!=e.stage)return 3;
        for(size_t n=0;n<e.variantCount;++n) {
            const auto &v=e.variants[n];std::vector<Compiler::FunctionConstantValue> input,values;
            for(size_t i=0;i<v.count;++i) {
                Compiler::FunctionConstantValue c;c.index=v.constants[i].index;c.type=v.constants[i].type;
                std::memcpy(c.bytes.data(),v.constants[i].bytes,sizeof(v.constants[i].bytes));input.push_back(c);
            }
            std::reverse(input.begin(),input.end());std::string diagnostic;
            for(unsigned repeat=0;repeat<32;++repeat) {
                const auto begin=Clock::now();
                const auto *found=SystemShaders::find(GPUSystemShaders::Entries,GPUSystemShaders::Count,e.airHash,e.name);
                SystemShaders::reflect(*found,declarations,stage);
                if(!Compiler::resolveFunctionConstants(declarations,input,values,diagnostic))return 4;
                const auto *matched=SystemShaders::match(*found,values);
                if(matched!=&v || !SystemShaders::restore(*found,*matched,GPUSystemShaders::Registry,*image))return 5;
                double us=std::chrono::duration<double,std::micro>(Clock::now()-begin).count();
                (repeat?steady:first).push_back(us);
            }
            std::ofstream out(std::string(argv[1])+"/"+std::to_string(count)+".bin",std::ios::binary);
            out.write(reinterpret_cast<const char *>(image.get()),sizeof(*image));if(!out)return 6;
            if(SystemShaders::restore(e,v,GPUSystemShaders::Registry^1,*image))return 7;
            // Unknown profile/type and omitted required constants must not be
            // silently substituted with the nearest available specialization.
            auto mismatch=values;Compiler::FunctionConstantValue extra;extra.index=65535;extra.type=33;mismatch.push_back(extra);
            if(SystemShaders::match(e,mismatch))return 8;
            if(!values.empty()){mismatch=values;mismatch[0].type=0;if(SystemShaders::match(e,mismatch))return 9;}
            for(size_t length:{size_t(0),size_t(1),size_t(7),v.bytes-1})
                if(SystemShaders::unpack(v.packed,length,*image))return 10;
            uint8_t overflow[]={255,255,255,255,1,0,0,0,0};
            if(SystemShaders::unpack(overflow,sizeof(overflow),*image))return 11;
            ++count;
        }
    }
    if(!count)return 12;
    std::sort(first.begin(),first.end());std::sort(steady.begin(),steady.end());
    std::cout<<"{\"variants\":"<<count<<",\"first_touch_median_us\":"<<first[first.size()/2]
             <<",\"first_touch_max_us\":"<<first.back()<<",\"steady_median_us\":"<<steady[steady.size()/2]
             <<",\"steady_p95_us\":"<<steady[steady.size()*95/100]<<",\"compiler_linked\":false,\"hardware_tested\":false}\n";
}
