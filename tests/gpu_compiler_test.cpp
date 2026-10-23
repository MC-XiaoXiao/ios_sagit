// SPDX-License-Identifier: BSD-2-Clause
// Focused host check of the production in-memory JIT, not a GPU emulator.
#include "Compiler.h"
#include "AIRNIR.h"
#include "air_specialization_test.h"
#include <GPUPrograms.h>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>
#include <atomic>
#include <thread>
static_assert(QGPU_FRAMEBUFFER_TILE_ORIGIN_BASE==80 && QGPU_FRAMEBUFFER_TILE_ORIGIN_WORDS==16 &&
              QGPU_FRAMEBUFFER_TILE_MEMORY_CONSTANT_WORDS==96 && QGPU_BUFFER_SIZE_CONSTANT_BASE==96 &&
              QGPU_CONSTANT_WINDOW_BASE==112,"64B framebuffer-origin ABI keeps later constant ranges fixed");
static std::string read(const std::string &path) {
    std::ifstream f(path,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};
}
int main(int argc,char **argv) {
    if (argc!=2) return 64;
    if (!checkAIRDeadHelpers()) { std::cerr<<"AIR dead-helper pruning failed\n";return 1; }
    if (!qgpu_check_buffer_guards()) { std::cerr<<"NIR buffer-bound semantics failed\n";return 1; }
    std::string root=argv[1],error;std::vector<GPU::Compiler::Entry> entries;
    std::vector<QGPUProgramImage> initial,changed;
    auto msl=read(root+"/drivers/gpu/QcomMTL/Shaders.metal");
    auto air=read(root+"/drivers/gpu/QcomMTL/Shaders.air.ll");
    auto bitcode=read(root+"/build/drivers/generated/GPU/Shaders.air");
    auto check=[&](bool ok) { if (!ok) { std::cerr<<error<<'\n';return false; }return true; };
    {
        GPU::Compiler::AIRCompileInfo info;
        if(!check(GPU::Compiler::compileAIR(bitcode.data(),bitcode.size(),1,changed,error,"air_add",{},&info)) ||
           info.route!=GPU::Compiler::AIRRoute::NIR || changed.size()!=1 || !GPU::JIT::valid(changed[0],1))return 1;
        const auto direct=changed[0];
        if(!check(GPU::Compiler::compileAIR(bitcode.data(),bitcode.size(),1,changed,error,"air_add",{},&info,false)) ||
           info.route!=GPU::Compiler::AIRRoute::GLSL || changed.size()!=1 || !GPU::JIT::valid(changed[0],1))return 1;
        if(!check(GPU::Compiler::compileAIR(bitcode.data(),bitcode.size(),1,changed,error,"air_control_flow",{},&info)) ||
           info.route!=GPU::Compiler::AIRRoute::NIR || info.lowerFailed)return 1;
        // Previously unsupported interfaces now use the direct compiler.
        for(const char *entry:{"air_position","air_atomic_counter_kernel","air_workgroup_coordinates"}) {
            if(!check(GPU::Compiler::compileAIR(bitcode.data(),bitcode.size(),1,changed,error,entry,{},&info)) ||
               info.route!=GPU::Compiler::AIRRoute::NIR || changed.size()!=1 || !GPU::JIT::valid(changed[0],1))return 1;
        }
        // Whole libraries are compiled atomically through the direct path.
        for(unsigned repeat=0;repeat<2;++repeat) {
            if(!check(GPU::Compiler::compileAIR(bitcode.data(),bitcode.size(),1,changed,error,{}, {},&info)) ||
               info.route!=GPU::Compiler::AIRRoute::NIR || changed.size()<2 || info.nirFailureCacheHit)return 1;
        }
        auto multiExit=air;
        const std::string loopBody="body:\n  %next = add i32 %n, 1\n  br label %loop\nchoose:";
        auto loopAt=multiExit.find(loopBody);if(loopAt==std::string::npos)return 1;
        multiExit.replace(loopAt,loopBody.size(),"body:\n  %next = add i32 %n, 1\n  %escape = icmp eq i32 %index, 0\n  br i1 %escape, label %abandon, label %loop\nabandon:\n  store i32 99, ptr addrspace(1) %dst\n  ret void\nchoose:");
        for(unsigned repeat=0;repeat<2;++repeat) {
            if(!check(GPU::Compiler::compileAIR(multiExit.data(),multiExit.size(),1,changed,error,"air_control_flow",{},&info)) ||
                info.route!=GPU::Compiler::AIRRoute::NIR || info.nirFailureCacheHit)return 1;
        }
        if(!check(GPU::Compiler::compileAIR(multiExit.data(),multiExit.size(),1,changed,error,"air_add",{},&info)) ||
            info.route!=GPU::Compiler::AIRRoute::NIR || info.nirFailureCacheHit)return 1;
        multiExit+="\n; source identity changes the failure cache key\n";
        if(!check(GPU::Compiler::compileAIR(multiExit.data(),multiExit.size(),1,changed,error,"air_control_flow",{},&info)) ||
            info.nirFailureCacheHit)return 1;
        // An unsupported ordering still fails safely and caches only the
        // frontend capability decision, with exact source/entry identity.
        auto invalidOrder=air;auto atomicAt=invalidOrder.find("atomicrmw min",invalidOrder.find("define void @air_atomic_values"));
        if(atomicAt==std::string::npos)return 1;auto orderAt=invalidOrder.find("monotonic",atomicAt);
        invalidOrder.replace(orderAt,9,"seq_cst");
        for(unsigned repeat=0;repeat<2;++repeat) {
            if(GPU::Compiler::compileAIR(invalidOrder.data(),invalidOrder.size(),1,changed,error,"air_atomic_values",{},&info) ||
               !changed.empty() || info.nirFailureCacheHit!=(repeat!=0) || info.nirDiagnostic.find("relaxed")==std::string::npos)return 1;
        }
        if(!check(GPU::Compiler::compileAIR(invalidOrder.data(),invalidOrder.size(),1,changed,error,"air_add",{},&info)) ||
           info.route!=GPU::Compiler::AIRRoute::NIR || info.nirFailureCacheHit)return 1;
        invalidOrder+="\n; distinct source identity\n";
        if(GPU::Compiler::compileAIR(invalidOrder.data(),invalidOrder.size(),1,changed,error,"air_atomic_values",{},&info) || info.nirFailureCacheHit)return 1;
        if(GPU::Compiler::compileAIR(invalidOrder.data(),invalidOrder.size(),1,changed,error,{}, {},&info) || !changed.empty())return 1;
        auto remaining=read(root+"/tests/fixtures/gpu/direct-nir-remaining.air.ll");std::vector<std::string> names;
        if(!check(GPU::Compiler::inspectAIREntries(remaining.data(),remaining.size(),names,error)) ||
           !check(GPU::Compiler::compileAIR(remaining.data(),remaining.size(),1,changed,error,{}, {},&info)) ||
           info.route!=GPU::Compiler::AIRRoute::NIR || changed.size()!=names.size())return 1;
        for(size_t n=0;n<names.size();++n)if(changed[n].name!=names[n] || !GPU::JIT::valid(changed[n],1))return 1;
        if(GPU::Compiler::compileAIR(bitcode.data(),bitcode.size(),1,changed,error,"absent_entry",{},&info) ||
           !changed.empty() || !info.lowerFailed || error.empty())return 1;
        QGPUProgramImage invalid=direct;QGPUAIRNIRTiming timing{};char diagnostic[4096]{};
        if(qgpu_compile_air_nir("invalid",7,"air_add",nullptr,0,1,&invalid,&timing,diagnostic,sizeof(diagnostic)) ||
           invalid.info.codeWords || invalid.name[0] || !diagnostic[0])return 1;
        // The two public compiler paths must share their Mesa/IR3 mutex.
        std::atomic<bool> consistent{true};std::vector<std::thread> threads;
        for(unsigned t=0;t<4;++t)threads.emplace_back([&,t]{for(unsigned n=0;n<3;++n){
            std::vector<QGPUProgramImage> output;std::string message;GPU::Compiler::AIRCompileInfo route;
            bool useDirect=(t+n)&1;
            if(!GPU::Compiler::compileAIR(bitcode.data(),bitcode.size(),1,output,message,"air_add",{},&route,useDirect) || output.size()!=1 ||
               route.route!=(useDirect?GPU::Compiler::AIRRoute::NIR:GPU::Compiler::AIRRoute::GLSL) ||
               (useDirect && std::memcmp(&output[0],&direct,sizeof(direct))))consistent=false;
        }});
        for(auto &thread:threads)thread.join();if(!consistent)return 1;
        std::cout<<"AIR direct NIR: loop/vertex/atomic/system inputs, whole-library direct compilation/failure cache, invalid output discard and concurrent serialization passed\n";
    }
    // Every authored entry accepted by the existing GLSL frontend must also
    // compile directly. This detects accidental fallback hidden by success.
    for(const char *file:{"drivers/gpu/QcomMTL/Shaders.air.ll","drivers/gpu/QcomMTL/Textures.air.ll",
        "drivers/gpu/QcomMTL/SampleShading.air.ll","drivers/gpu/QcomMTL/ConstantWindows.air.ll",
        "drivers/gpu/QcomMTL/IntegerTextures.air.ll","tests/fixtures/gpu/packed-fluidity.air.ll","tests/fixtures/gpu/direct-nir-boundaries.air.ll"}) {
        auto source=read(root+"/"+file);std::vector<GPU::Compiler::Entry> declared;
        if(!check(GPU::Compiler::lowerAIR(source.data(),source.size(),declared,error)))return 1;
        for(const auto &entry:declared) {
            GPU::Compiler::AIRCompileInfo route;std::vector<QGPUProgramImage> direct,baseline;
            if(!check(GPU::Compiler::compileAIR(source.data(),source.size(),1,direct,error,entry.name,{},&route)) ||
               route.route!=GPU::Compiler::AIRRoute::NIR || direct.size()!=1) {
                std::cerr<<"direct coverage "<<entry.name<<": "<<route.nirDiagnostic<<"\n";return 1;
            }
            if(!check(GPU::Compiler::compile({entry},1,baseline,error)) || baseline.size()!=1)return 1;
            const auto &a=direct[0].info,&b=baseline[0].info;
            if(a.stage!=b.stage || a.bufferMask!=b.bufferMask || a.writeMask!=b.writeMask || a.instanceMask!=b.instanceMask ||
               a.imageMask!=b.imageMask || a.imageReadMask!=b.imageReadMask || a.imageWriteMask!=b.imageWriteMask || a.imageUintMask!=b.imageUintMask ||
               a.sampleMask!=b.sampleMask || a.samplerMask!=b.samplerMask || a.fetchMask!=b.fetchMask || a.queryMask!=b.queryMask ||
               a.threadgroupMask!=b.threadgroupMask || a.sharedBytes!=b.sharedBytes || a.colorMask!=b.colorMask || a.attributeMask!=b.attributeMask ||
               a.varyingCount!=b.varyingCount || a.textureBindings!=b.textureBindings || std::memcmp(a.strides,b.strides,sizeof(a.strides))) {
                std::cerr<<"direct reflection mismatch "<<entry.name<<"\n";return 1;
            }
        }
    }
    std::cout<<"AIR direct NIR: authored entry coverage and GLSL resource-interface parity passed\n";
    // Integer images retain exact bits, and their native resource interface
    // cannot be rebound to the float image conversion helpers or a sampler.
    {
        auto source=read(root+"/drivers/gpu/QcomMTL/IntegerTextures.air.ll");
        if(!check(GPU::Compiler::lowerAIR(source.data(),source.size(),entries,error,"air_uint_copy")) ||
            !check(GPU::Compiler::compile(entries,1,changed,error)) || changed.size()!=1)return 1;
        auto info=changed[0].info;
        if(info.imageUintMask!=129 || info.imageMask!=129 || info.imageReadMask!=1 || info.imageWriteMask!=128)return 1;
        GPU::Shader::Program p{};std::memcpy(&p,&info,sizeof(info));
        GPU::Shader::Dispatch d{};uint32_t constants[QGPU_IMAGE_CONSTANT_WORDS]{};
        d.images[0]={1,0,17,9,128,GPU::Shader::R32Uint};d.images[7]={2,0,17,9,128,GPU::Shader::R32Uint};
        if(!GPU::Shader::imageFootprint(d,p,constants) || constants[3]!=(GPU::Shader::R32Uint|256u))return 1;
        d.images[7].format=GPU::Shader::BGRA8;if(GPU::Shader::imageFootprint(d,p,constants))return 1;
        d.images[7].format=GPU::Shader::R32Uint;p.imageUintMask=1;if(GPU::Shader::imageFootprint(d,p,constants))return 1;
        p.imageUintMask=129;p.sampleMask=1;if(GPU::Shader::imageFootprint(d,p,constants))return 1;
        auto bad=changed[0];bad.info.imageUintMask|=2;if(GPU::JIT::valid(bad,1))return 1;
        bad=changed[0];bad.info.branchStack=GPU::Shader::MaxBranchStack;if(!GPU::JIT::valid(bad,1))return 1;
        ++bad.info.branchStack;if(GPU::JIT::valid(bad,1))return 1;
        bad=changed[0];bad.info.branchStack=GPU::Shader::MaxBarrierBranchStack+1;
        bad.info.flags|=QGPU_THREADGROUP_BARRIER;if(GPU::JIT::valid(bad,1))return 1;
        for(const auto *name:{"air_half_bits","air_private_array","air_uint_pack"})
            if(!check(GPU::Compiler::lowerAIR(source.data(),source.size(),entries,error,name)) ||
                !check(GPU::Compiler::compile(entries,1,changed,error)) || changed.size()!=1)return 1;
        for(const auto *count:{"0","65","257","4294967304"}) {
            auto oversized=source;std::string from="[8 x <4 x i32>]",to="["+std::string(count)+" x <4 x i32>]";
            for(size_t at=0;(at=oversized.find(from,at))!=std::string::npos;at+=to.size())oversized.replace(at,from.size(),to);
            if(GPU::Compiler::lowerAIR(oversized.data(),oversized.size(),entries,error,"air_private_array"))return 1;
        }
        for(const char *shape:{"texture2d","texture2d_array","texturecube","texturecube_array"}) {
            std::string indices=std::strstr(shape,"cube")?"uint2(id),0u,":"uint2(id),";
            if(std::strstr(shape,"array"))indices+="0u,";
            std::string code="kernel void uint_update("+std::string(shape)+"<uint,access::read_write> image [[texture(3)]],uint2 id [[thread_position_in_grid]]) {"
                "uint4 v=image.read("+indices+"0u);image.write(v^uint4(2147483649u),"+indices+"0u);}";
            if(!check(GPU::Compiler::lowerMSL(code,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
                changed.size()!=1 || changed[0].info.imageUintMask!=8 || changed[0].info.imageReadMask!=8 || changed[0].info.imageWriteMask!=8)return 1;
        }
        for(const auto *code:{
            "kernel void bad(texture2d<uint> image [[texture(0)]]) {}",
            "kernel void bad(texture2d_ms<uint,access::read> image [[texture(0)]]) {}",
            "fragment uint4 bad(texture2d<uint,access::read> image [[texture(0)]]) {return image.read(uint2(0u));}"})
            if(GPU::Compiler::lowerMSL(code,entries,error))return 1;
    }
    // Packed signed uniforms, ushort vertex IDs, constant lookup tables and
    // sincos out parameters must reach the actual production A540 backend.
    auto packedAIR=read(root+"/tests/fixtures/gpu/packed-fluidity.air.ll");
    for(const auto *entry:{"packed_fields","quad_table"}) {
        std::vector<GPU::Compiler::Entry> lowered;std::vector<QGPUProgramImage> images;
        if(!check(GPU::Compiler::lowerAIR(packedAIR.data(),packedAIR.size(),lowered,error,entry)) ||
            !check(GPU::Compiler::compile(lowered,GPUPrograms::Registry,images,error)) || images.size()!=1)return 1;
        if(!std::strcmp(entry,"packed_fields") && (images[0].info.bufferMask!=3 || images[0].info.writeMask!=2))return 1;
    }
    // AIR uses the same native function-constant metadata and static-init
    // convention as QuartzCore. The optional buffer must disappear when off.
    const std::string constantsAIR=R"air(
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
@enabled = internal addrspace(2) externally_initialized constant i8 undef
@scale = internal addrspace(2) externally_initialized constant float undef
@predicate = internal addrspace(2) global i8 0
@configuration = internal addrspace(2) global {i8,[2 x float]} zeroinitializer
@llvm.global_ctors = appending global [1 x {i32,ptr,ptr}] [{i32,ptr,ptr} {i32 65535,ptr @initialize,ptr null}]
declare i1 @air.is_function_constant_defined(ptr addrspace(2))
declare i1 @air.any.v2i1(<2 x i1>)
declare i1 @air.all.v2i1(<2 x i1>)
define internal void @initialize() {
 %a = load i8, ptr addrspace(2) @enabled
 store i8 %a, ptr addrspace(2) @predicate
 %b = load float, ptr addrspace(2) @scale
 %defined = call i1 @air.is_function_constant_defined(ptr addrspace(2) @scale)
 %g = select i1 %defined, float %b, float 1.0
 store float %g, ptr addrspace(2) getelementptr ({i8,[2 x float]}, ptr addrspace(2) @configuration, i64 0, i32 1, i64 1)
 ret void
}
define void @constant_kernel(ptr addrspace(1) %output, ptr addrspace(1) %extra) {
 %p = load i8, ptr addrspace(2) @predicate
 %active = icmp ne i8 %p, 0
 %lanes = insertelement <2 x i1> zeroinitializer, i1 %active, i32 0
 %any = call i1 @air.any.v2i1(<2 x i1> %lanes)
 %mask = insertelement <2 x i1> <i1 true, i1 true>, i1 %any, i32 1
 %all = call i1 @air.all.v2i1(<2 x i1> %mask)
 %g = load float, ptr addrspace(2) getelementptr ({i8,[2 x float]}, ptr addrspace(2) @configuration, i64 0, i32 1, i64 1)
 br i1 %all, label %on, label %off
on:
 %input = load float, ptr addrspace(1) %extra
 %v = fmul float %input, %g
 br label %end
off:
 br label %end
end:
 %result = phi float [%v,%on],[%g,%off]
 store float %result, ptr addrspace(1) %output
 ret void
}
!air.version = !{!0}
!air.kernel = !{!1}
!air.function_constants = !{!6,!7}
!0 = !{i32 2,i32 7,i32 0}
!1 = !{ptr @constant_kernel,!2,!3}
!2 = !{}
!3 = !{!4,!5}
!4 = !{i32 0,!"air.buffer",!"air.location_index",i32 0,!"air.write",!"air.address_space",i32 1,!"air.arg_type_size",i32 4,!"air.arg_type_name",!"float"}
!5 = !{i32 1,!"air.function_constant",!8,!"air.buffer",!"air.location_index",i32 1,!"air.read",!"air.address_space",i32 1,!"air.arg_type_size",i32 4,!"air.arg_type_name",!"float"}
!6 = !{ptr addrspace(2) @enabled,!"bool",!"enabled",i32 0,i1 true}
!7 = !{ptr addrspace(2) @scale,!"float",!"scale",i32 5,i1 false}
!8 = !{ptr addrspace(2) @predicate,!"bool",!"predicate"}
)air";
    std::vector<GPU::Compiler::FunctionConstant> reflected;unsigned reflectedStage=UINT32_MAX;
    if(!check(GPU::Compiler::inspectAIRConstants(constantsAIR.data(),constantsAIR.size(),"constant_kernel",reflected,reflectedStage,error)) ||
        reflected.size()!=2 || reflectedStage!=0 || !reflected[0].required || reflected[1].required)return 1;
    GPU::Compiler::FunctionConstantValue enable;enable.index=0;enable.type=53;
    std::vector<GPU::Compiler::FunctionConstantValue> constants{enable};
    if(!check(GPU::Compiler::lowerAIR(constantsAIR.data(),constantsAIR.size(),entries,error,"constant_kernel",constants)) ||
        entries.size()!=1 || entries[0].reflection.bufferMask!=1 || !check(GPU::Compiler::compile(entries,1,changed,error)))return 1;
    const auto disabledImage=changed[0];constants[0].bytes[0]=1;
    if(!check(GPU::Compiler::lowerAIR(constantsAIR.data(),constantsAIR.size(),entries,error,"constant_kernel",constants)) ||
        entries[0].reflection.bufferMask!=3 || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        !std::memcmp(disabledImage.code,changed[0].code,sizeof(disabledImage.code)))return 1;
    if(GPU::Compiler::lowerAIR(constantsAIR.data(),constantsAIR.size(),entries,error,"constant_kernel") || !entries.empty())return 1;
    std::vector<GPU::Compiler::FunctionConstantValue> effective;
    auto named=enable;named.index=UINT32_MAX;named.name="enabled";named.bytes[0]=1;
    if(!GPU::Compiler::resolveFunctionConstants(reflected,{named,enable},effective,error) || effective.size()!=1 || effective[0].bytes[0]!=0)return 1;
    {
        GPU::Compiler::AIRCompileInfo route;
        if(!check(GPU::Compiler::compileAIR(constantsAIR.data(),constantsAIR.size(),1,changed,error,"constant_kernel",{named},&route)) ||
           route.route!=GPU::Compiler::AIRRoute::NIR || changed[0].info.bufferMask!=3)return 1;
        if(!check(GPU::Compiler::compileAIR(constantsAIR.data(),constantsAIR.size(),1,changed,error,{}, {named,enable},&route)) ||
           route.route!=GPU::Compiler::AIRRoute::NIR || changed[0].info.bufferMask!=1)return 1;
    }
    auto wrong=enable;wrong.type=33;
    if(GPU::Compiler::resolveFunctionConstants(reflected,{wrong},effective,error))return 1;
    // A native interface location can be a constructor-populated constant
    // global. Reflection follows its dependencies and binding bounds still
    // apply to the resulting value, including duplicate binding rejection.
    auto bindingAIR=constantsAIR;
    bindingAIR.insert(bindingAIR.find("@llvm.global_ctors"),
        "@slot = internal addrspace(2) externally_initialized constant i32 undef\n"
        "@location = internal addrspace(2) global i32 0\n");
    bindingAIR.insert(bindingAIR.find(" ret void"),
        " %slot = load i32, ptr addrspace(2) @slot\n"
        " store i32 %slot, ptr addrspace(2) @location\n");
    auto bindingField=bindingAIR.find("!\"air.location_index\",i32 1,!\"air.read\"");
    bindingAIR.replace(bindingField,std::strlen("!\"air.location_index\",i32 1,!\"air.read\""),
        "!\"air.location_index\",ptr addrspace(2) @location,!\"air.read\"");
    bindingAIR.replace(bindingAIR.find("!{!6,!7}"),8,"!{!6,!7,!9}");
    bindingAIR+="!9 = !{ptr addrspace(2) @slot,!\"uint\",!\"slot\",i32 17,i1 true}\n";
    if(!check(GPU::Compiler::inspectAIRConstants(bindingAIR.data(),bindingAIR.size(),"constant_kernel",reflected,reflectedStage,error)) ||
        reflected.size()!=3 || reflected[2].index!=17 || !reflected[2].required)return 1;
    auto bindingEnabled=enable;bindingEnabled.bytes[0]=1;
    GPU::Compiler::FunctionConstantValue slot;slot.index=17;slot.type=33;slot.bytes[0]=6;
    if(!check(GPU::Compiler::lowerAIR(bindingAIR.data(),bindingAIR.size(),entries,error,"constant_kernel",{bindingEnabled,slot})) ||
        entries.size()!=1 || entries[0].reflection.bufferMask!=65 || !check(GPU::Compiler::compile(entries,1,changed,error)))return 1;
    for(unsigned invalid:{0u,8u}) {
        slot.bytes[0]=invalid;
        if(GPU::Compiler::lowerAIR(bindingAIR.data(),bindingAIR.size(),entries,error,"constant_kernel",{bindingEnabled,slot}) || !entries.empty())return 1;
    }
    // VideoToolbox initializes format predicates through switches. The mode
    // is only a control dependency: reflection must still require it. Compare
    // every edge and its PHI against independently compiled literal kernels.
    auto switchAIR=read(root+"/tests/fixtures/gpu/static-switch.air.ll");
    if(!check(GPU::Compiler::inspectAIRConstants(switchAIR.data(),switchAIR.size(),"switch_kernel",reflected,reflectedStage,error)) ||
        reflected.size()!=1 || reflected[0].index!=17 || !reflected[0].required || reflectedStage!=0)return 1;
    if(GPU::Compiler::lowerAIR(switchAIR.data(),switchAIR.size(),entries,error,"switch_kernel") || !entries.empty())return 1;
    {
        // Force the immutable decode-template cache, then alternate constants,
        // reflect again, and run concurrent borrowers. No specialization may
        // mutate the template or leak into the next entry/constant selection.
        auto cached=switchAIR+"\n;"+std::string(32768,' ')+"\n";
        std::map<unsigned,QGPUProgramImage> expected;
        for(unsigned value:{0u,2u,3u,0u,3u,2u}) {
            GPU::Compiler::FunctionConstantValue mode;mode.index=17;mode.type=33;std::memcpy(mode.bytes.data(),&value,4);
            GPU::Compiler::AIRCompileInfo route;
            if(!check(GPU::Compiler::compileAIR(cached.data(),cached.size(),1,changed,error,"switch_kernel",{mode},&route)) ||
                route.route!=GPU::Compiler::AIRRoute::NIR || changed.size()!=1)return 1;
            if(expected.count(value) && std::memcmp(&expected.at(value),&changed[0],sizeof(changed[0])))return 1;
            expected[value]=changed[0];
            if(!check(GPU::Compiler::inspectAIRConstants(cached.data(),cached.size(),"switch_kernel",reflected,reflectedStage,error)) ||
                reflected.size()!=1 || !reflected[0].required)return 1;
        }
        if(!std::memcmp(&expected.at(0),&expected.at(3),sizeof(QGPUProgramImage)))return 1;
        std::atomic<bool> isolated{true};std::vector<std::thread> borrowers;
        for(unsigned value:{0u,2u,3u})borrowers.emplace_back([&,value]{for(unsigned n=0;n<4;++n){
            GPU::Compiler::FunctionConstantValue mode;mode.index=17;mode.type=33;std::memcpy(mode.bytes.data(),&value,4);
            std::vector<QGPUProgramImage> output;std::string message;
            if(!GPU::Compiler::compileAIR(cached.data(),cached.size(),1,output,message,"switch_kernel",{mode}) ||
                output.size()!=1 || std::memcmp(&expected.at(value),&output[0],sizeof(output[0])))isolated=false;
        }});
        for(auto &thread:borrowers)thread.join();if(!isolated)return 1;
        if(GPU::Compiler::compileAIR(cached.data(),cached.size(),1,changed,error,"switch_kernel") || !changed.empty())return 1;
        std::cout<<"AIR decode cache: alternating constants, immutable reflection and concurrent clones passed\n";
    }
    for(const auto &edge:std::vector<std::pair<uint32_t,uint32_t>>{{0,8},{2,11},{UINT32_MAX,11},{3,20}}) {
        GPU::Compiler::FunctionConstantValue mode;mode.index=17;mode.type=33;
        std::memcpy(mode.bytes.data(),&edge.first,sizeof(edge.first));
        if(!check(GPU::Compiler::lowerAIR(switchAIR.data(),switchAIR.size(),entries,error,"switch_kernel",{mode})) ||
            !check(GPU::Compiler::compile(entries,1,changed,error)) || changed.size()!=1)return 1;
        const auto selected=changed[0];
        std::string literal=R"air(
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
define void @switch_kernel(ptr addrspace(1) %output) {
 store float VALUE, ptr addrspace(1) %output
 ret void
}
!air.version = !{!0}
!air.kernel = !{!1}
!0 = !{i32 2,i32 7,i32 0}
!1 = !{ptr @switch_kernel,!2,!3}
!2 = !{}
!3 = !{!4}
!4 = !{i32 0,!"air.buffer",!"air.location_index",i32 0,!"air.write",!"air.address_space",i32 1,!"air.arg_type_size",i32 4,!"air.arg_type_name",!"float"}
)air";
        literal.replace(literal.find("VALUE"),5,std::to_string(edge.second)+".0");
        if(!check(GPU::Compiler::lowerAIR(literal.data(),literal.size(),entries,error,"switch_kernel")) ||
            !check(GPU::Compiler::compile(entries,1,changed,error)) || changed.size()!=1 ||
            std::memcmp(&selected,&changed[0],sizeof(selected)))return 1;
    }
    // Public Metal reflection must include constants read only by an internal
    // helper; the same values must survive nested, noinline AIR calls.
    const std::string helperAIR=R"air(
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
@factor = internal addrspace(2) externally_initialized constant float undef
@option = internal addrspace(2) externally_initialized constant i8 undef
declare i1 @air.is_function_constant_defined(ptr addrspace(2))
define internal float @adjust(float %x) noinline {
 %defined = call i1 @air.is_function_constant_defined(ptr addrspace(2) @option)
 %flag = load i8, ptr addrspace(2) @option
 %on = icmp ne i8 %flag, 0
 %enabled = select i1 %defined, i1 %on, i1 false
 %f = load float, ptr addrspace(2) @factor
 %scaled = fmul float %x, %f
 %result = select i1 %enabled, float %scaled, float %x
 ret float %result
}
define internal float @nested(float %x) noinline {
 %result = call float @adjust(float %x)
 ret float %result
}
define void @helper_kernel(ptr addrspace(1) %output) {
 %x = load float, ptr addrspace(1) %output
 %width = call float @air.convert.f.f32.u.i8(i8 32)
 %normalized = fdiv float %x, %width
 %result = call float @nested(float %normalized)
 store float %result, ptr addrspace(1) %output
 ret void
}
declare float @air.convert.f.f32.u.i8(i8)
!air.version = !{!0}
!air.kernel = !{!1}
!air.function_constants = !{!5,!6}
!0 = !{i32 2,i32 7,i32 0}
!1 = !{ptr @helper_kernel,!2,!3}
!2 = !{}
!3 = !{!4}
!4 = !{i32 0,!"air.buffer",!"air.location_index",i32 0,!"air.read_write",!"air.address_space",i32 1,!"air.arg_type_size",i32 4,!"air.arg_type_name",!"float"}
!5 = !{ptr addrspace(2) @factor,!"float",!"factor",i32 9,i1 true}
!6 = !{ptr addrspace(2) @option,!"bool",!"option",i32 2,i1 false}
)air";
    if(!check(GPU::Compiler::inspectAIRConstants(helperAIR.data(),helperAIR.size(),"helper_kernel",reflected,reflectedStage,error)) ||
        reflected.size()!=2 || reflected[0].index!=2 || reflected[1].index!=9 || !reflected[1].required)return 1;
    if(GPU::Compiler::lowerAIR(helperAIR.data(),helperAIR.size(),entries,error,"helper_kernel") || !entries.empty())return 1;
    GPU::Compiler::FunctionConstantValue helperFactor;helperFactor.index=9;helperFactor.type=3;
    const float twice=2.0f;std::memcpy(helperFactor.bytes.data(),&twice,sizeof(twice));
    auto helperOption=enable;helperOption.index=2;helperOption.bytes[0]=1;
    if(!check(GPU::Compiler::lowerAIR(helperAIR.data(),helperAIR.size(),entries,error,"helper_kernel",{helperFactor,helperOption})) ||
        !check(GPU::Compiler::compile(entries,1,changed,error)))return 1;
    const auto helperImage=changed[0];
    if(!check(GPU::Compiler::lowerAIR(helperAIR.data(),helperAIR.size(),entries,error,"helper_kernel",{helperFactor})) ||
        !check(GPU::Compiler::compile(entries,1,changed,error)) || !std::memcmp(helperImage.code,changed[0].code,sizeof(helperImage.code)))return 1;
    auto recursive=helperAIR;auto recursiveCall=recursive.find("call float @adjust(float %x)");
    recursive.replace(recursiveCall,std::strlen("call float @adjust(float %x)"),"call float @nested(float %x)");
    if(GPU::Compiler::lowerAIR(recursive.data(),recursive.size(),entries,error,"helper_kernel",{helperFactor,helperOption}) || !entries.empty())return 1;
    if (!check(GPU::Compiler::lowerMSL(msl,entries,error)) || entries.size()!=58 ||
        !check(GPU::Compiler::compile(entries,1,initial,error))) return 1;
    const auto initialEntries=entries;
    for (const auto &image:initial) if (image.info.stage==GPU::Shader::Compute && !(image.info.flags&QGPU_VARIABLE_LOCAL_SIZE)) return 1;
    auto save=[&](const char *name,const std::string &text) {
        std::ofstream file(root+"/build/gpu-compiler/"+name);file<<text;return bool(file);
    };
    // Runtime predicates must work without function-constant specialization.
    // These math forms occur in QuartzCore's gradients/color programs. Keep
    // real varying inputs so the production backend cannot fold the workload.
    const std::string displayMathAIR=R"air(
target datalayout = "e-p:64:64-i16:16-i32:32-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
declare <4 x float> @air.fast_exp.v4f32(<4 x float>)
declare <4 x float> @air.fast_log.v4f32(<4 x float>)
declare <4 x float> @air.fast_atan2.v4f32(<4 x float>, <4 x float>)
declare <4 x float> @air.fast_powr.v4f32(<4 x float>, <4 x float>)
declare <4 x float> @air.fmin3.v4f32(<4 x float>, <4 x float>, <4 x float>)
declare <4 x float> @air.fmax3.v4f32(<4 x float>, <4 x float>, <4 x float>)
declare i1 @air.any.v4i1(<4 x i1>)
declare i1 @air.all.v4i1(<4 x i1>)
define <4 x float> @display_math(<4 x float> %value) {
 %e = call <4 x float> @air.fast_exp.v4f32(<4 x float> %value)
 %l = call <4 x float> @air.fast_log.v4f32(<4 x float> %e)
 %a = call <4 x float> @air.fast_atan2.v4f32(<4 x float> %l, <4 x float> %e)
 %p = call <4 x float> @air.fast_powr.v4f32(<4 x float> %e, <4 x float> %value)
 %m = call <4 x float> @air.fmax3.v4f32(<4 x float> %p, <4 x float> %a, <4 x float> %e)
 %r = call <4 x float> @air.fmin3.v4f32(<4 x float> %m, <4 x float> %l, <4 x float> %e)
 %positive = fcmp ogt <4 x float> %value, zeroinitializer
 %any = call i1 @air.any.v4i1(<4 x i1> %positive)
 %all = call i1 @air.all.v4i1(<4 x i1> %positive)
 %mixed = xor i1 %any, %all
 %out = select i1 %mixed, <4 x float> %r, <4 x float> %m
 ret <4 x float> %out
}
!air.version = !{!0}
!air.fragment = !{!1}
!0 = !{i32 2, i32 7, i32 0}
!1 = !{ptr @display_math, !2, !3}
!2 = !{!4}
!3 = !{!5}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{i32 0, !"air.fragment_input", !"generated(5valueDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4"}
)air";
    auto replaceAll=[](std::string source,const std::string &from,const std::string &to) {
        size_t at=0;while((at=source.find(from,at))!=std::string::npos){source.replace(at,from.size(),to);at+=to.size();}return source;
    };
    for(bool half:{false,true}) {
        auto source=half?replaceAll(replaceAll(replaceAll(displayMathAIR,"float","half"),"f32","f16"),"display_math","display_math_half"):displayMathAIR;
        if(!check(GPU::Compiler::lowerAIR(source.data(),source.size(),entries,error)) || entries.size()!=1 ||
            !check(GPU::Compiler::compile(entries,1,changed,error)) || !GPU::JIT::valid(changed[0],1) ||
            entries[0].glsl.find("any(")==std::string::npos || entries[0].glsl.find("all(")==std::string::npos ||
            entries[0].glsl.find("mix(min(")==std::string::npos ||
            !save(half?"display-math-half.air.ll":"display-math-f32.air.ll",source) ||
            !save(half?"display-math-half.frag":"display-math-f32.frag",entries[0].glsl))return 1;
        source+="\nattributes #0 = { \"no-nans-fp-math\"=\"true\" }\n";
        source=replaceAll(source,"%value) {","%value) #0 {");
        if(!check(GPU::Compiler::lowerAIR(source.data(),source.size(),entries,error)) ||
            !check(GPU::Compiler::compile(entries,1,changed,error)) || entries[0].glsl.find("mix(min(")!=std::string::npos)return 1;
    }
    auto malformedMath=replaceAll(displayMathAIR,"air.fast_exp.v4f32","air.fast_exp.v2f32");
    if(GPU::Compiler::lowerAIR(malformedMath.data(),malformedMath.size(),entries,error) || !entries.empty())return 1;
    const std::string gradientOffsetAIR=R"air(
target datalayout = "e-p:64:64-i32:32-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
declare {<4 x float>, i8} @air.sample_texture_2d_grad.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, <2 x float>, <2 x float>, float, i1, <2 x i32>, i32)
define <4 x float> @gradient_offset(ptr addrspace(1) %image, ptr addrspace(2) %filtering, <2 x float> %uv) {
 %sample = call {<4 x float>, i8} @air.sample_texture_2d_grad.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %filtering, <2 x float> %uv, <2 x float> <float 0.125, float 0.0>, <2 x float> <float 0.0, float 0.125>, float 0.0, i1 true, <2 x i32> <i32 -1, i32 1>, i32 0)
 %out = extractvalue {<4 x float>, i8} %sample, 0
 ret <4 x float> %out
}
!air.version = !{!0}
!air.fragment = !{!1}
!0 = !{i32 2, i32 7, i32 0}
!1 = !{ptr @gradient_offset, !2, !3}
!2 = !{!4}
!3 = !{!5, !6, !7}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>"}
!6 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, !"air.arg_type_name", !"sampler"}
!7 = !{i32 2, !"air.fragment_input", !"generated(2uvDv2_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2"}
)air";
    if(!check(GPU::Compiler::lowerAIR(gradientOffsetAIR.data(),gradientOffsetAIR.size(),entries,error)) ||
        !check(GPU::Compiler::compile(entries,1,changed,error)) || !GPU::JIT::valid(changed[0],1) ||
        !save("display-gradient-offset.air.ll",gradientOffsetAIR) || !save("display-gradient-offset.frag",entries[0].glsl))return 1;
    std::cout<<"iOS18 display AIR: runtime any/all, exp/log/atan2/powr/min3/max3, FP16/FP32 and no-NaN fast path passed\n";
    if (initial[4].info.instanceMask!=2 || initial[4].info.instanceID==252 ||
        initial[4].info.vertexID==252 || initial[4].info.driverCount<3) return 1;
    auto program=[](const QGPUProgramImage &image) {
        GPU::Shader::Program p{};std::memcpy(&p,&image.info,sizeof(image.info));
        p.code=image.code;p.immediates=image.immediates;p.name=image.name;return p;
    };
    auto imageNamed=[](const std::vector<QGPUProgramImage> &images,const char *name)->const QGPUProgramImage & {
        for (const auto &image:images) if (!std::strcmp(image.name,name)) return image;
        std::cerr<<"Missing compiled shader "<<name<<'\n';std::abort();
    };
    auto aotNamed=[](const char *name)->const GPU::Shader::Program & {
        for (const auto &p:GPUPrograms::Programs) if (!std::strcmp(p.name,name)) return p;
        std::cerr<<"Missing AOT shader "<<name<<'\n';std::abort();
    };
    // Both real compiler paths must carry both variants through the immutable
    // JIT image. Compare their complete register/resource prefix and ISA.
    for (const char *file:{"SampleShading.metal","SampleShading.air.ll","Resolve.metal","Multisample.metal","Depth.metal","Mipmaps.metal","Compositor.metal"}) {
        auto source=read(root+"/drivers/gpu/QcomMTL/"+file);
        std::vector<GPU::Compiler::Entry> sampleEntries;std::vector<QGPUProgramImage> images;
        bool msl=std::strstr(file,".metal");
        if (!check(msl?GPU::Compiler::lowerMSL(source,sampleEntries,error):GPU::Compiler::lowerAIR(source.data(),source.size(),sampleEntries,error)) ||
            !check(GPU::Compiler::compile(sampleEntries,1,images,error))) return 1;
        for (const auto &image:images) {
            const auto &owner=aotNamed(image.name);
            for (unsigned variant=0;variant<(owner.stage==GPU::Shader::Fragment?2u:1u);++variant) {
                const auto *aot=variant?owner.msaa:&owner;if(!aot)return 1;
                const auto &info=variant?image.msaa.info:image.info;
                const auto *code=variant?image.msaa.code:image.code;
                QGPUProgramDescription expected{};std::memcpy(&expected,aot,sizeof(expected));expected.id=0;
                if(std::memcmp(&info,&expected,sizeof(info)) || std::memcmp(code,aot->code,info.codeWords*4))return 1;
            }
            if(owner.stage==GPU::Shader::Fragment) {
                auto bad=image;bad.msaa={};if(GPU::JIT::valid(bad,1))return 1;
                bad=image;bad.msaa.info.sampleID=252;if((image.msaa.info.sampleFlags&QGPU_SAMPLE_ID) && GPU::JIT::valid(bad,1))return 1;
                bad=image;bad.msaa.info.varyingCount^=1;if(GPU::JIT::valid(bad,1))return 1;
            }
        }
    }
    auto checkGraphicsSamples=[&](const QGPUProgramImage &image,unsigned arrays,unsigned explicitMask) {
        const auto &p=image.info;
        return p.sampleMask==66 && p.samplerMask==132 && p.textureCount==7 && p.imageMask==0 &&
            (p.flags&QGPU_ARRAY_TEXTURE_MASK)==(arrays<<QGPU_ARRAY_TEXTURE_SHIFT) && p.explicitSamplerMask==explicitMask &&
            (!p.driverCount || p.driverBase>=16) && (!p.immediateCount || p.immediateBase>=16) && GPU::JIT::valid(image,1);
    };
    if(!checkGraphicsSamples(imageNamed(initial,"multitextured_vertex"),64,128) ||
        !checkGraphicsSamples(imageNamed(initial,"multitextured_fragment"),0,4))return 1;
    for(const auto &entry:initialEntries) {
        if(entry.name=="multitextured_vertex" && !save("graphics-textures-msl.vert",entry.glsl))return 1;
        if(entry.name=="multitextured_fragment" && !save("graphics-textures-msl.frag",entry.glsl))return 1;
    }
    // Literal state and its physical slot maps agree across both production
    // compiler paths. Native admission rejects collisions and noncanonical data.
    auto constantProgram=[&](const QGPUProgramImage &image) {
        const auto &p=image.info;const auto &aot=aotNamed(image.name);
        if(!p.constantSamplerMask || !GPU::JIT::valid(image,1) || p.codeWords!=aot.codeWords ||
            std::memcmp(image.code,aot.code,p.codeWords*4) ||
            std::memcmp(&p.implicitSamplerMask,&aot.implicitSamplerMask,600))return false;
        auto bad=image;unsigned index=unsigned(__builtin_ctz(p.constantSamplerMask));
        bad.info.constantSamplers[index].filters|=16;if(GPU::JIT::valid(bad,1))return false;
        bad=image;bad.info.framebufferMask=1;if(GPU::JIT::valid(bad,1))return false;
        bad=image;
        if(p.constantExplicitSamplerMask&(1u<<index))bad.info.constantSamplerLevelSlots[index]=16;
        else bad.info.constantSamplerSlots[index]=16;
        if(GPU::JIT::valid(bad,1))return false;
        return true;
    };
    for(const char *name:{"constant_sample","constant_sample_fragment","constant_cube_vertex","constant_array_fragment"}) {
        const auto &image=imageNamed(initial,name);if(!constantProgram(image))return 1;
        for(const auto &entry:initialEntries)if(entry.name==name && !save((entry.name+(image.info.stage==0?".comp":image.info.stage==1?".vert":".frag")).c_str(),entry.glsl))return 1;
    }
    const auto &mixedConstant=imageNamed(initial,"constant_sample_fragment").info;
    if(mixedConstant.samplerMask!=128 || mixedConstant.implicitSamplerMask!=128 || mixedConstant.constantSamplerMask!=3 ||
        mixedConstant.constantImplicitSamplerMask!=1 || mixedConstant.constantExplicitSamplerMask!=3 ||
        mixedConstant.constantSamplers[0].minLOD!=64 || mixedConstant.constantSamplers[0].maxLOD!=1024 || mixedConstant.constantSamplers[1].filters)return 1;
    auto collision=imageNamed(initial,"constant_sample_fragment");collision.info.constantSamplerLevelSlots[0]=collision.info.constantSamplerSlots[0];
    if(GPU::JIT::valid(collision,1))return 1;
    auto colorProgram=[&](const QGPUProgramImage &image,uint32_t mask) {
        const auto &p=image.info;const auto &aot=aotNamed(image.name);
        if(p.colorMask!=mask || !GPU::JIT::valid(image,1) || p.codeWords!=aot.codeWords ||
            std::memcmp(image.code,aot.code,p.codeWords*4) || std::memcmp(p.colorRegisters,aot.colorRegisters,sizeof(p.colorRegisters))) {
            std::cerr<<image.name<<" MRT mismatch: mask "<<p.colorMask<<" expected "<<mask<<" words "<<p.codeWords<<" AOT "<<aot.codeWords<<" regs ";
            for(unsigned i=0;i<8;++i)std::cerr<<p.colorRegisters[i]<<"/"<<aot.colorRegisters[i]<<" ";std::cerr<<'\n';return false;
        }
        auto bad=image;bad.info.depthReg=1;if(GPU::JIT::valid(bad,1))return false;
        bad=image;bad.info.colorRegisters[unsigned(__builtin_ctz(mask))]=252;if(GPU::JIT::valid(bad,1))return false;
        return true;
    };
    for(const auto &test:std::initializer_list<std::pair<const char *,uint32_t>>{{"mrt_fragment",137},{"all_colors_fragment",255},{"color_only_fragment",128}}) {
        if(!colorProgram(imageNamed(initial,test.first),test.second))return 1;
        for(const auto &entry:initialEntries)if(entry.name==test.first && !save((entry.name+".frag").c_str(),entry.glsl))return 1;
    }
    auto fragmentProgram=[&](const QGPUProgramImage &image,uint32_t flags,uint32_t colors) {
        const auto &p=image.info;const auto &aot=aotNamed(image.name);
        if(!GPU::JIT::valid(image,1) || (p.flags&QGPU_FRAGMENT_FLAGS)!=flags || p.colorMask!=colors ||
            p.depthReg!=aot.depthReg || p.flags!=aot.flags || p.codeWords!=aot.codeWords ||
            std::memcmp(image.code,aot.code,p.codeWords*4))return false;
        auto bad=image;bad.info.depthReg=252;if(GPU::JIT::valid(bad,1))return false;
        return true;
    };
    for(const char *name:{"resolve_color","multisample_fragment","multisample_vertex"}) {
        const auto &image=imageNamed(initial,name);const auto &p=image.info;const auto &aot=aotNamed(name);
        uint32_t mask=p.stage==0?1:p.stage==1?4:32;
        if(p.multisampleMask!=mask || !GPU::JIT::valid(image,1) || p.codeWords!=aot.codeWords ||
            std::memcmp(image.code,aot.code,p.codeWords*4))return 1;
        auto bad=image;bad.info.depthTextureMask=256;if(GPU::JIT::valid(bad,1))return 1;
        bad=image;bad.info.multisampleMask=256;if(GPU::JIT::valid(bad,1))return 1;
        for(const auto &entry:initialEntries)if(entry.name==name && !save((entry.name+(p.stage==0?".comp":p.stage==1?".vert":".frag")).c_str(),entry.glsl))return 1;
    }
    for(const char *name:{"raster_depth_only","depth_fragment","depth_only_fragment","discard_depth_fragment","discard_only_fragment"}) {
        bool depth=std::strcmp(name,"raster_depth_only") && std::strcmp(name,"discard_only_fragment");
        bool discard=!std::strncmp(name,"discard_",8);
        uint32_t colors=!std::strcmp(name,"depth_fragment")?1:!std::strcmp(name,"discard_depth_fragment")?128:0;
        if(!fragmentProgram(imageNamed(initial,name),(depth?QGPU_FRAGMENT_DEPTH:0)|(discard?QGPU_FRAGMENT_DISCARD:0),colors))return 1;
        for(const auto &entry:initialEntries)if(entry.name==name && !save((entry.name+".frag").c_str(),entry.glsl))return 1;
    }
    for(const char *name:{"cube_textured","cube_array_textured","cube_sampled_vertex","cube_sample"}) {
        const auto &image=imageNamed(initial,name);unsigned texture=std::strstr(name,"array") || !std::strcmp(name,"cube_sample")?6:3;
        unsigned sampler=!std::strcmp(name,"cube_array_textured")?7:2;
        if(image.info.sampleMask!=(1u<<texture) || image.info.cubeSamplerMask!=(1u<<sampler) ||
            (image.info.flags&QGPU_CUBE_TEXTURE_MASK)!=(1u<<(QGPU_CUBE_TEXTURE_SHIFT+texture)) || !GPU::JIT::valid(image,1))return 1;
        auto malformed=image;malformed.info.cubeSamplerMask=0;if(GPU::JIT::valid(malformed,1))return 1;
        malformed=image;malformed.reserved2=1;if(GPU::JIT::valid(malformed,1))return 1;
        for(const auto &entry:initialEntries)if(entry.name==name) {
            std::string file=std::string(name)+(image.info.stage==0?".comp":image.info.stage==1?".vert":".frag");
            if(!save(file.c_str(),entry.glsl))return 1;
        }
    }
    for(const char *name:{"cube_copy","cube_array_copy","cube_image_queries","cube_image_update"}) {
        const auto &image=imageNamed(initial,name);const auto &p=image.info;
        unsigned mask=!std::strcmp(name,"cube_image_update")?8:!std::strcmp(name,"cube_image_queries")?72:129;
        if(p.imageMask!=mask || p.sampleMask || p.samplerMask || p.cubeSamplerMask || p.textureCount ||
            ((p.flags&QGPU_CUBE_TEXTURE_MASK)>>QGPU_CUBE_TEXTURE_SHIFT)!=mask || p.constlen*4<96 || !GPU::JIT::valid(image,1))return 1;
        if(!std::strcmp(name,"cube_copy") && (p.imageReadMask!=1 || p.imageWriteMask!=128 || (p.flags&QGPU_ARRAY_TEXTURE_MASK)))return 1;
        if(!std::strcmp(name,"cube_array_copy") && (p.imageReadMask!=1 || p.imageWriteMask!=128 || ((p.flags&QGPU_ARRAY_TEXTURE_MASK)>>QGPU_ARRAY_TEXTURE_SHIFT)!=129))return 1;
        if(!std::strcmp(name,"cube_image_queries") && (p.imageReadMask || p.imageWriteMask))return 1;
        if(!std::strcmp(name,"cube_image_update") && (p.imageReadMask!=8 || p.imageWriteMask!=8))return 1;
        for(const auto &entry:initialEntries)if(entry.name==name && !save((std::string(name)+".comp").c_str(),entry.glsl))return 1;
    }
    for(const char *name:{"fetch_vertex","fetch_array_fragment","fetch_cube_fragment","fetch_cube_array_vertex","texture_queries_fragment"}) {
        const auto &image=imageNamed(initial,name);const auto &p=image.info;
        unsigned fetch=!std::strcmp(name,"fetch_array_fragment")?2:!std::strcmp(name,"fetch_cube_fragment")?8:!std::strcmp(name,"texture_queries_fragment")?0:64;
        unsigned query=!std::strcmp(name,"fetch_array_fragment")?130:!std::strcmp(name,"texture_queries_fragment")?139:fetch;
        unsigned sample=!std::strcmp(name,"fetch_cube_array_vertex")?64:0;
        if(p.fetchMask!=fetch || p.queryMask!=query || p.sampleMask!=sample || p.samplerMask!=(sample?128u:0u) || p.imageMask ||
            p.textureCount!=(fetch?32u-__builtin_clz(fetch<<8):0u) || GPU::Shader::memoryConstantWords(p)!=(p.constantWindowCount?QGPU_CONSTANT_WINDOW_BASE+p.constantWindowCount*QGPU_CONSTANT_WINDOW_WORDS+4u:(p.flags&QGPU_BUFFER_SIZE_CONSTANTS)?QGPU_MEMORY_CONSTANT_WORDS:48u) || !GPU::JIT::valid(image,1))return 1;
        auto malformed=image;malformed.info.fetchMask|=1u<<7;if(GPU::JIT::valid(malformed,1))return 1;
        malformed=image;malformed.info.queryMask=0;if(GPU::JIT::valid(malformed,1))return 1;
        for(const auto &entry:initialEntries)if(entry.name==name && !save((std::string(name)+(p.stage==1?".vert":".frag")).c_str(),entry.glsl))return 1;
    }
    if (!imageNamed(initial,"control_flow").info.branchStack) return 1;
    auto invalidLocal=initial[1];invalidLocal.info.flags|=QGPU_VARIABLE_LOCAL_SIZE;
    if (GPU::JIT::valid(invalidLocal,1)) return 1;
    const auto &shared=imageNamed(initial,"threadgroup_reduce");
    if (shared.info.sharedBytes!=256 || !(shared.info.flags&QGPU_THREADGROUP_BARRIER)) return 1;
    const auto &dynamic=imageNamed(initial,"dynamic_threadgroup");
    if (dynamic.info.sharedBytes!=16 || dynamic.info.threadgroupMask!=0x89 || (dynamic.info.flags&108)!=108 ||
        dynamic.info.driverBase<16 || dynamic.info.immediateBase<16) return 1;
    auto invalidDynamic=dynamic;invalidDynamic.info.driverBase=0;
    if (GPU::JIT::valid(invalidDynamic,1)) return 1;
    invalidDynamic=dynamic;invalidDynamic.info.sharedBytes=4;
    if (GPU::JIT::valid(invalidDynamic,1)) return 1;
    invalidDynamic=dynamic;invalidDynamic.info.flags&=~QGPU_DYNAMIC_THREADGROUP;
    if (GPU::JIT::valid(invalidDynamic,1)) return 1;
    auto oversized=shared;oversized.info.sharedBytes=QGPU_MAX_THREADGROUP_BYTES+4;
    if (GPU::JIT::valid(oversized,1)) return 1;
    GPU::Shader::Linkage linkage;
    auto vs=program(initial[5]),fs=program(initial[7]);
    if (!GPU::Shader::link(vs,fs,linkage) || linkage.count!=3 || linkage.stride!=10 || fs.totalIn!=6 ||
        !GPU::Shader::link(*GPUPrograms::lookup(6),fs,linkage) ||
        !GPU::Shader::link(vs,aotNamed("air_varying_textured"),linkage)) return 1;
    auto attributeVS=program(initial[8]),attributeFS=program(initial[9]);
    if (attributeVS.attributeMask!=0x89 || attributeVS.bufferMask ||
        !GPU::Shader::link(attributeVS,attributeFS,linkage)) return 1;
    const auto &fragmentBuffers=imageNamed(initial,"buffer_color");
    if (fragmentBuffers.info.bufferMask!=131 || fragmentBuffers.info.writeMask!=128 ||
        (fragmentBuffers.info.flags&26)!=26 || !GPU::JIT::valid(fragmentBuffers,1)) return 1;
    auto invalidBank=fragmentBuffers;invalidBank.info.flags&=~QGPU_FRAGMENT_BUFFER_BANK;
    if (GPU::JIT::valid(invalidBank,1)) return 1;
    const auto &gradient=imageNamed(initial,"sample_gradient");
    if(gradient.info.sampleMask!=32 || gradient.info.samplerMask!=4 || gradient.info.explicitSamplerMask || gradient.info.driverBase<64)return 1;
    for(const auto &entry:entries) {
        if(entry.name=="sample_gradient" && !save("sampler-gradient-runtime.comp",entry.glsl))return 1;
        if(entry.name=="gradient_textured" && !save("sampler-gradient-runtime.frag",entry.glsl))return 1;
        if(entry.name=="mipmap_rgba8" && !save("mipmap-runtime.comp",entry.glsl))return 1;
    }
    const auto &mipmap=imageNamed(initial,"mipmap_rgba8");
    if (mipmap.info.bufferMask || mipmap.info.writeMask || mipmap.info.textureCount!=1 ||
        mipmap.info.sampleMask!=1 || mipmap.info.samplerMask!=1 || mipmap.info.explicitSamplerMask!=1 ||
        mipmap.info.imageMask!=128 || mipmap.info.imageWriteMask!=128 || mipmap.info.imageReadMask) return 1;
    auto mipSource=msl;auto option=mipSource.find("level(pixel.y*0.25)");if(option==std::string::npos)return 1;
    mipSource.replace(option,std::strlen("level(pixel.y*0.25)"),"bias(pixel.y*0.25)");
    if (!check(GPU::Compiler::lowerMSL(mipSource,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        imageNamed(changed,"mip_textured").info.textureCount!=1) return 1;
    for (const auto &entry:entries) if (entry.name=="mip_textured" && !save("mip-bias.frag",entry.glsl)) return 1;
    // Interpolation changes real IR3 sysvals and linkage, rather than just a
    // front-end flag. The two stage structs intentionally order fields apart.
    for (unsigned mode=1;mode<=2;++mode) {
        auto qualified=msl;auto start=qualified.find("struct RasterFragment");
        auto field=qualified.find("float4 tint [[user(color)]];",start);
        if (field==std::string::npos) return 1;
        qualified.replace(field,std::strlen("float4 tint [[user(color)]];"),mode==1?"float4 tint [[user(color)]] [[center_no_perspective]];":"float4 tint [[user(color)]] [[flat]];");
        if (!check(GPU::Compiler::lowerMSL(qualified,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error))) return 1;
        auto flat=program(changed[6]);
        if (!GPU::Shader::link(vs,flat,linkage) || flat.varyings[1].mode!=mode ||
            (mode==1 && flat.linear==252) || (mode==2 && !linkage.interpolation[0])) return 1;
    }
    const std::string mismatch="struct Input { float2 value [[user(color)]]; }; fragment float4 mismatch(Input in [[stage_in]]) { return float4(in.value,0.0,1.0); }";
    if (!check(GPU::Compiler::lowerMSL(mismatch,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        GPU::Shader::link(vs,program(changed[0]),linkage)) return 1;
    // With no vertex_id, only Metal's baseInstance semantics can require a
    // VS driver constant. A raw GL instance ID would incorrectly need none.
    const std::string instanceOnly="vertex float4 instance_only(uint i [[instance_id]]) { return float4(float(i),0.0,0.0,1.0); }";
    if (!check(GPU::Compiler::lowerMSL(instanceOnly,entries,error)) ||
        !check(GPU::Compiler::compile(entries,1,changed,error)) || changed[0].info.vertexID!=252 ||
        changed[0].info.instanceID==252 || changed[0].info.driverCount<3) return 1;
    auto replace=msl.find("7u");if (replace==std::string::npos) return 1;msl.replace(replace,2,"19u");
    if (!check(GPU::Compiler::lowerMSL(msl,entries,error)) ||
        !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        !std::memcmp(initial[0].code,changed[0].code,sizeof(initial[0].code))) return 1;
    // Repeated imports exercise Mesa program/context/NIR ownership cleanup.
    if(!check(GPU::Compiler::lowerAIR(bitcode.data(),bitcode.size(),entries,error,"air_add")) ||
        entries.size()!=1 || entries[0].name!="air_add" ||
        !check(GPU::Compiler::compile(entries,1,changed,error)) || changed.size()!=1)return 1;
    if(GPU::Compiler::lowerAIR(bitcode.data(),bitcode.size(),entries,error,"missing_entry") || !entries.empty())return 1;
    for (unsigned i=0;i<2;++i) for (auto *input:{&air,&bitcode}) {
        if (!check(GPU::Compiler::lowerAIR(input->data(),input->size(),entries,error)) || entries.size()!=24 ||
            !check(GPU::Compiler::compile(entries,1,changed,error))) return 1;
        if(!checkGraphicsSamples(imageNamed(changed,"air_multitextured_vertex"),0,128) ||
            !checkGraphicsSamples(imageNamed(changed,"air_multitextured_fragment"),0,132))return 1;
        for(const auto &entry:entries) {
            if(entry.name=="air_multitextured_vertex" && !save("graphics-textures-air.vert",entry.glsl))return 1;
            if(entry.name=="air_multitextured_fragment" && !save("graphics-textures-air.frag",entry.glsl))return 1;
        }
        const auto &fragmentBuffers=imageNamed(changed,"air_buffer_color");
        if (imageNamed(changed,"air_threadgroup_reduce").info.sharedBytes!=256 ||
            !(imageNamed(changed,"air_threadgroup_reduce").info.flags&QGPU_THREADGROUP_BARRIER)) return 1;
        for (const auto &entry:entries) if (entry.name=="air_threadgroup_reduce" && !save("shared-runtime.comp",entry.glsl)) return 1;
        for (const auto &entry:entries) if (entry.name=="air_atomic_counter_kernel" && !save("atomic-counter-runtime.comp",entry.glsl)) return 1;
        for (const auto &entry:entries) if (entry.name=="air_atomic_values" && !save("atomic-values-runtime.comp",entry.glsl)) return 1;
        for (const auto &entry:entries) if (entry.name=="air_dynamic_threadgroup" && !save("dynamic-runtime.comp",entry.glsl)) return 1;
        for (const auto &entry:entries) if (entry.name=="air_mip_textured" && !save("mip-runtime.frag",entry.glsl)) return 1;
        for (const auto &entry:entries) if (entry.name=="air_image_transform" && !save("images-runtime.comp",entry.glsl)) return 1;
        for (const auto &entry:entries) if (entry.name=="air_sample_texture" && !save("sampling-runtime.comp",entry.glsl)) return 1;
        const auto &vertexTexture=imageNamed(changed,"air_sampled_vertex");
        if(vertexTexture.info.textureCount!=1 || vertexTexture.info.sampleMask!=1 || vertexTexture.info.samplerMask!=1 ||
            vertexTexture.info.explicitSamplerMask!=1 || vertexTexture.info.driverBase<16 || !GPU::JIT::valid(vertexTexture,1))return 1;
        for(const auto &entry:entries)if(entry.name=="air_sampled_vertex" && !save("vertex-texture-air-runtime.vert",entry.glsl))return 1;
        const auto &sample=imageNamed(changed,"air_sample_texture");
        if(sample.info.sampleMask!=32 || sample.info.samplerMask!=4 || sample.info.textureCount!=6 ||
            sample.info.imageMask!=128 || sample.info.imageWriteMask!=128 || sample.info.explicitSamplerMask!=4 || !GPU::JIT::valid(sample,1))return 1;
        const auto &image=imageNamed(changed,"air_image_transform");
        if(image.info.imageMask!=129 || image.info.imageReadMask!=1 || image.info.imageWriteMask!=128 ||
            image.info.bufferMask!=1 || image.info.driverBase<48 || image.info.immediateBase<48)return 1;
        const auto &dynamic=imageNamed(changed,"air_dynamic_threadgroup");
        if (dynamic.info.sharedBytes!=16 || dynamic.info.threadgroupMask!=0x81 || (dynamic.info.flags&108)!=108) return 1;
        if (fragmentBuffers.info.bufferMask!=131 || fragmentBuffers.info.writeMask!=128 ||
            (fragmentBuffers.info.flags&26)!=26 || !GPU::JIT::valid(fragmentBuffers,1)) return 1;
        for (const auto &entry:entries) if (entry.name=="air_buffer_color" && !save("fragment-runtime.frag",entry.glsl)) return 1;
        bool instanced=false;
        for (const auto &image:changed) if (!std::strcmp(image.name,"air_instanced_position"))
            instanced=image.info.instanceMask==2 && image.info.instanceID!=252;
        if (!instanced || imageNamed(changed,"air_attribute_position").info.attributeMask!=0x89 ||
            !GPU::Shader::link(program(imageNamed(changed,"air_attribute_position")),attributeFS,linkage) ||
            !GPU::Shader::link(attributeVS,program(imageNamed(changed,"air_attribute_color")),linkage)) return 1;
        if (!GPU::Shader::link(program(imageNamed(changed,"air_varying_position")),fs,linkage) ||
            !GPU::Shader::link(vs,program(imageNamed(changed,"air_varying_textured")),linkage)) return 1;
        for (const auto &entry:entries) if (entry.name=="air_control_flow")
            if (!save("flow-runtime.comp",entry.glsl) || !imageNamed(changed,"air_control_flow").info.branchStack) return 1;
        for (const auto &entry:entries) if (entry.name=="air_buffer_indexing" && !save("buffers-runtime.comp",entry.glsl)) return 1;
        for (const auto &entry:entries) if (entry.name=="air_workgroup_coordinates")
            if (!save("compute-runtime.comp",entry.glsl) || !(imageNamed(changed,entry.name.c_str()).info.flags&QGPU_VARIABLE_LOCAL_SIZE)) return 1;
    }
    // The modern read ABI has a builtin sampler. Both textual IR and real
    // bitcode must produce the same admitted AOT images and stage metadata.
    auto textureAIR=read(root+"/drivers/gpu/QcomMTL/Textures.air.ll");
    auto textureBitcode=read(root+"/build/drivers/generated/GPU/Textures.air");
    // Hardware texel offsets preserve mip/filter/address semantics. Exercise
    // implicit and explicit LOD, an external sampler, and a literal array
    // sampler. Zero-offset shader text remains on its original fast path.
    for(const auto &item:std::initializer_list<std::pair<std::string,std::string>>{
            {air,"air_textured"},{textureAIR,"air_array_sample_fragment"},{textureAIR,"air_constant_sample"}}) {
        auto source=replaceAll(item.first,"i1 false, <2 x i32> zeroinitializer","i1 true, <2 x i32> zeroinitializer");
        source=replaceAll(source,"i1 true, <2 x i32> zeroinitializer","i1 true, <2 x i32> <i32 -8, i32 7>");
        if(!check(GPU::Compiler::lowerAIR(source.data(),source.size(),entries,error,item.second)) ||
            !check(GPU::Compiler::compile(entries,1,changed,error)) || !GPU::JIT::valid(changed[0],1))return 1;
        for(const auto &invalid:std::vector<std::string>{
                replaceAll(source,"i32 -8, i32 7","i32 -9, i32 7"),
                replaceAll(source,"i1 true, <2 x i32> <i32 -8, i32 7>","i1 false, <2 x i32> <i32 -8, i32 7>")})
            if(GPU::Compiler::lowerAIR(invalid.data(),invalid.size(),entries,error,item.second) || !entries.empty())return 1;
    }
    std::cout<<"iOS18 sample offsets: native LOD/implicit2D/array sampling and offset range/flag rejection passed\n";
    struct TextureCase {const char *name;unsigned stage,fetch,query,sample,sampler,image,read,write,array,cube,level;};
    const TextureCase textureCases[]={
        {"air_fetch_vertex",1,128,128,0,0,0,0,0,0,0,0},
        {"air_fetch_cube_fragment",2,8,8,0,0,0,0,0,0,8,0},
        {"air_fetch_cube_array_fragment",2,64,64,0,0,0,0,0,64,64,0},
        {"air_array_image_transform",0,0,0,0,0,8,8,8,8,0,0},
        {"air_cube_image_copy",0,0,0,0,0,129,1,128,0,129,0},
        {"air_cube_array_sample_vertex",1,0,64,64,128,0,0,0,64,64,0},
        {"air_array_sample_fragment",2,0,2,2,4,0,0,0,2,0,4},
        {"air_cube_sample_fragment",2,0,8,8,4,0,0,0,0,8,4}};
    for(auto *input:{&textureAIR,&textureBitcode}) {
        if(!check(GPU::Compiler::lowerAIR(input->data(),input->size(),entries,error)) || entries.size()!=17 ||
            !check(GPU::Compiler::compile(entries,1,changed,error)))return 1;
        for(const char *name:{"air_constant_sample","air_constant_cube_vertex","air_constant_array_fragment"})
            if(!constantProgram(imageNamed(changed,name)))return 1;
        if(!colorProgram(imageNamed(changed,"air_mrt_fragment"),137) || !colorProgram(imageNamed(changed,"air_color_only_fragment"),128))return 1;
        for(const char *name:{"air_depth_fragment","air_depth_only_fragment","air_discard_fragment","air_void_fragment"}) {
            bool depth=std::strstr(name,"depth")!=nullptr,discard=std::strstr(name,"discard")!=nullptr;
            uint32_t colors=!std::strcmp(name,"air_depth_fragment")?128:discard?1:0;
            if(!fragmentProgram(imageNamed(changed,name),(depth?QGPU_FRAGMENT_DEPTH:0)|(discard?QGPU_FRAGMENT_DISCARD:0),colors))return 1;
            for(const auto &entry:entries)if(entry.name==name && !save((entry.name+".frag").c_str(),entry.glsl))return 1;
        }
        for(const auto &test:textureCases) {
            const auto &image=imageNamed(changed,test.name);const auto &p=image.info;const auto &aot=aotNamed(test.name);
            if(p.stage!=test.stage || p.fetchMask!=test.fetch || p.queryMask!=test.query || p.sampleMask!=test.sample || p.samplerMask!=test.sampler ||
                p.imageMask!=test.image || p.imageReadMask!=test.read || p.imageWriteMask!=test.write || p.explicitSamplerMask!=test.level ||
                ((p.flags&QGPU_ARRAY_TEXTURE_MASK)>>QGPU_ARRAY_TEXTURE_SHIFT)!=test.array || ((p.flags&QGPU_CUBE_TEXTURE_MASK)>>QGPU_CUBE_TEXTURE_SHIFT)!=test.cube ||
                GPU::Shader::memoryConstantWords(p)!=(p.constantWindowCount?QGPU_CONSTANT_WINDOW_BASE+p.constantWindowCount*QGPU_CONSTANT_WINDOW_WORDS+4u:(p.flags&QGPU_BUFFER_SIZE_CONSTANTS)?QGPU_MEMORY_CONSTANT_WORDS:(test.stage?48u:96u)) || p.codeWords!=aot.codeWords ||
                std::memcmp(image.code,aot.code,p.codeWords*4) || !GPU::JIT::valid(image,1))return 1;
            for(const auto &entry:entries)if(entry.name==test.name && !save((entry.name+(test.stage==0?".comp":test.stage==1?".vert":".frag")).c_str(),entry.glsl))return 1;
        }
    }
    // Keep the public Metal index while allocating a compact A540 descriptor.
    // Native CoreAnimation's dither texture uses binding 9.
    {
        auto remapped=textureAIR;
        const std::string from="!62 = !{i32 0, !\"air.texture\", !\"air.location_index\", i32 1,",
            to="!62 = !{i32 0, !\"air.texture\", !\"air.location_index\", i32 9,";
        auto at=remapped.find(from);if(at==std::string::npos)return 1;remapped.replace(at,from.size(),to);
        if(!check(GPU::Compiler::lowerAIR(remapped.data(),remapped.size(),entries,error,"air_array_sample_fragment")) ||
            !check(GPU::Compiler::compile(entries,1,changed,error)) || changed.size()!=1)return 1;
        const auto &image=changed[0];
        if(image.info.sampleMask!=1 || image.info.queryMask!=1 || !GPU::JIT::valid(image,1) ||
            GPU::Shader::textureBinding(image.info,0)!=9 || image.msaa.info.textureBindings!=image.info.textureBindings)return 1;
        remapped.replace(at,to.size(),"!62 = !{i32 0, !\"air.texture\", !\"air.location_index\", i32 16,");
        if(GPU::Compiler::lowerAIR(remapped.data(),remapped.size(),entries,error,"air_array_sample_fragment"))return 1;
    }
    for(const auto &replacement:std::initializer_list<std::pair<std::string,std::string>>{
        {"ptr addrspace(2) %rs, <2 x i32> %coord","ptr addrspace(2) null, <2 x i32> %coord"},
        {"i32 %lod, i32 1)","i32 %lod, i32 2)"},
        {"texturecube<float, read>","texture2d<float, read>"},
        {"<2 x i32> zeroinitializer, i32 %lod","<2 x i32> <i32 1, i32 0>, i32 %lod"},
        {"float %lod, float 0.0, i32 0)","float %lod, float %lod, i32 0)"},
        {"%color = extractvalue { <4 x float>, i8 } %sample, 0","%unusedResidency = extractvalue { <4 x float>, i8 } %sample, 1\n  %color = extractvalue { <4 x float>, i8 } %sample, 0"}}) {
        auto invalid=textureAIR;auto pos=invalid.find(replacement.first);if(pos==std::string::npos)return 1;
        invalid.replace(pos,replacement.first.size(),replacement.second);
        if(GPU::Compiler::lowerAIR(invalid.data(),invalid.size(),entries,error))return 1;
    }
    auto biasedAIR=air;auto mode=biasedAIR.find("i1 true, float %lod");if(mode==std::string::npos)return 1;
    biasedAIR.replace(mode,std::strlen("i1 true, float %lod"),"i1 false, float %lod");
    if (!check(GPU::Compiler::lowerAIR(biasedAIR.data(),biasedAIR.size(),entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error))) return 1;
    for (const auto &entry:entries) if (entry.name=="air_mip_textured" && !save("mip-bias-runtime.frag",entry.glsl)) return 1;
    // A diamond uses edge PHIs without the loop dispatch path. Retain the
    // actual lowered shader for a focused software-GL semantic check.
    auto diamond=air;
    const std::string tail="  %sum = add i32 %value, 7\n  store i32 %sum, ptr addrspace(1) %dst, align 4\n  ret void";
    auto at=diamond.find(tail);if (at==std::string::npos) return 1;
    diamond.replace(at,tail.size(),
        "  %odd = and i32 %value, 1\n  %test = icmp ne i32 %odd, 0\n"
        "  br i1 %test, label %yes, label %no\nyes:\n  %y = add i32 %value, 13\n  br label %merge\n"
        "no:\n  %n = xor i32 %value, 31\n  br label %merge\nmerge:\n"
        "  %choice = phi i32 [ %y, %yes ], [ %n, %no ]\n"
        "  store i32 %choice, ptr addrspace(1) %dst, align 4\n  ret void");
    if (!check(GPU::Compiler::lowerAIR(diamond.data(),diamond.size(),entries,error)) ||
        !check(GPU::Compiler::compile(entries,1,changed,error))) return 1;
    for (const auto &entry:entries) if (entry.name=="air_add" && !save("flow-diamond.comp",entry.glsl)) return 1;
    const std::string scopes=
        "kernel void scopes(device const uint *input [[buffer(0)]], device uint *output [[buffer(1)]], uint i [[thread_position_in_grid]]) {"
        "uint value=input[i]; { uint i=2u; while(i) { value+=i; --i; } } uint n=0u;"
        "do { ++n; if(n==2u) continue; value+=n; if(n==3u) break; } while(n<4u); output[i]=value; }"
        "fragment float4 returns(float4 pixel [[position]]) { if(pixel.x<1.0) return float4(1.0);"
        "else if(pixel.y<1.0) return float4(0.5); return float4(0.0); }";
    if (!check(GPU::Compiler::lowerMSL(scopes,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        !save("flow-scopes.comp",entries[0].glsl)) return 1;
    const std::string sharedVector="kernel void shared_vector(device float3 *out [[buffer(0)]], uint lane [[thread_index_in_threadgroup]]) {"
        "threadgroup float3 values[2]; threadgroup uint counter; values[lane]=float3(float(lane));"
        "if(lane==0u) counter=9u; threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);"
        "{ uint values=1u; out[lane]=float3(float(values)); } out[lane]=values[1u-lane]+float3(float(counter)); }";
    // IR3 eliminates the constant counter; the two padded float3 slots remain.
    if (!check(GPU::Compiler::lowerMSL(sharedVector,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.sharedBytes!=32 || !(changed[0].info.flags&QGPU_THREADGROUP_BARRIER)) {
        if (!changed.empty()) std::cerr<<"shared vector allocation "<<changed[0].info.sharedBytes<<" flags "<<changed[0].info.flags<<'\n';return 1;
    }
    const std::string sharedOverflow="kernel void overflow() { threadgroup uint values[8193]; }";
    if (GPU::Compiler::lowerMSL(sharedOverflow,entries,error) || !entries.empty()) return 1;
    const std::string atomicOps="kernel void atomic_ops(device atomic_uint *value [[buffer(0)]], device uint *output [[buffer(1)]], uint i [[thread_position_in_grid]]) {"
        "atomic_store_explicit(&value[i],255u,memory_order_relaxed); uint a=atomic_exchange_explicit(&value[i],25u,memory_order_relaxed);"
        "uint b=atomic_fetch_sub_explicit(&value[i],7u,memory_order_relaxed); uint c=atomic_fetch_and_explicit(&value[i],15u,memory_order_relaxed);"
        "uint d=atomic_fetch_or_explicit(&value[i],32u,memory_order_relaxed); uint e=atomic_fetch_xor_explicit(&value[i],3u,memory_order_relaxed);"
        "uint f=atomic_fetch_min_explicit(&value[i],8u,memory_order_relaxed); uint g=atomic_fetch_max_explicit(&value[i],23u,memory_order_relaxed);"
        "output[i*8u]=a;output[i*8u+1u]=b;output[i*8u+2u]=c;output[i*8u+3u]=d;output[i*8u+4u]=e;output[i*8u+5u]=f;output[i*8u+6u]=g;"
        "output[i*8u+7u]=atomic_load_explicit(&value[i],memory_order_relaxed); }";
    if (!check(GPU::Compiler::lowerMSL(atomicOps,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) || !save("atomic-ops.comp",entries[0].glsl)) return 1;
    const std::string fragmentAtomic="fragment float4 atomic_color(device atomic_uint *value [[buffer(0)]]) { return float4(float(atomic_fetch_add_explicit(value,1u,memory_order_relaxed))); }";
    if (!check(GPU::Compiler::lowerMSL(fragmentAtomic,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.writeMask!=1 || (changed[0].info.flags&26)!=26) return 1;
    auto strongOrder=atomicOps;auto order=strongOrder.find("memory_order_relaxed");strongOrder.replace(order,20,"memory_order_seq_cst");
    if (GPU::Compiler::lowerMSL(strongOrder,entries,error) || !entries.empty()) return 1;
    const std::string dynamicAtomic="kernel void dynamic_atomic(device uint *out [[buffer(0)]], threadgroup atomic_int *cell [[threadgroup(5)]]) {"
        "atomic_store_explicit(cell,-9,memory_order_relaxed); int old=atomic_fetch_min_explicit(cell,-11,memory_order_relaxed); int expected=-11;"
        "bool swapped=atomic_compare_exchange_weak_explicit(cell,&expected,7,memory_order_relaxed,memory_order_relaxed);"
        "out[0]=uint(old)+uint(swapped)+uint(expected); }";
    if (!check(GPU::Compiler::lowerMSL(dynamicAtomic,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.threadgroupMask!=32 || changed[0].info.sharedBytes || !save("dynamic-atomic.comp",entries[0].glsl)) return 1;
    const std::string imageRW="kernel void image_rw(texture2d<float,access::read_write> image [[texture(3)]], uint2 id [[thread_position_in_grid]]) {"
        "image.write(image.read(id).bgra,id);threadgroup_barrier(mem_flags::mem_texture); }";
    if(!check(GPU::Compiler::lowerMSL(imageRW,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.imageMask!=8 || changed[0].info.imageReadMask!=8 || changed[0].info.imageWriteMask!=8 || !save("images-rw.comp",entries[0].glsl))return 1;
    const std::string imageQuery="kernel void image_query(texture2d<float,access::read> image [[texture(0)]],device uint *out [[buffer(0)]]) {"
        "out[0]=image.get_width();out[1]=image.get_num_mip_levels();out[2]=image.get_width(image.get_num_mip_levels());}";
    if(!check(GPU::Compiler::lowerMSL(imageQuery,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.imageMask!=1 || changed[0].info.imageReadMask || changed[0].info.imageWriteMask || changed[0].info.constlen*4<48 ||
        !save("images-query.comp",entries[0].glsl))return 1;
    const std::string imageShared="kernel void image_shared(texture2d<float,access::write> image [[texture(1)]],"
        "threadgroup uint *scratch [[threadgroup(2)]],uint lane [[thread_index_in_threadgroup]],uint2 id [[thread_position_in_grid]]) {"
        "scratch[lane]=image.get_width();threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_texture);"
        "image.write(float4(float(scratch[lane])/255.0),id);}";
    if(!check(GPU::Compiler::lowerMSL(imageShared,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.imageMask!=2 || changed[0].info.imageWriteMask!=2 || changed[0].info.threadgroupMask!=4 ||
        changed[0].info.driverBase<48 || changed[0].info.constlen*4<48)return 1;
    const std::string partial="kernel void partial_groups(device uint *out [[buffer(0)]],device uint *sums [[buffer(1)]],"
        "threadgroup uint *scratch [[threadgroup(3)]],uint3 pos [[thread_position_in_grid]],uint3 lid [[thread_position_in_threadgroup]],"
        "uint3 gid [[threadgroup_position_in_grid]],uint3 size [[threads_per_threadgroup]],uint3 requested [[dispatch_threads_per_threadgroup]],"
        "uint3 groups [[threadgroups_per_grid]],uint3 grid [[threads_per_grid]],uint lane [[thread_index_in_threadgroup]]) {"
        "uint at=pos.x+grid.x*(pos.y+grid.y*pos.z);scratch[lane]=at+1u;"
        "out[at]=pos.x+pos.y*3u+pos.z*5u+lid.x*7u+lid.y*11u+lid.z*13u+gid.x*17u+gid.y*19u+gid.z*23u+"
        "size.x*29u+size.y*31u+size.z*37u+groups.x*41u+groups.y*43u+groups.z*47u+grid.x*53u+grid.y*59u+grid.z*61u+"
        "lane*67u+requested.x*71u+requested.y*73u+requested.z*79u;threadgroup_barrier(mem_flags::mem_threadgroup);"
        "if(lane==0u){uint sum=0u;for(uint i=0u;i<size.x*size.y*size.z;++i)sum+=scratch[i];"
        "sums[gid.x+groups.x*(gid.y+groups.y*gid.z)]=sum;} }";
    if(!check(GPU::Compiler::lowerMSL(partial,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        (changed[0].info.flags&(QGPU_DISPATCH_GEOMETRY|QGPU_THREADGROUP_BARRIER|QGPU_DYNAMIC_THREADGROUP))!=
        (QGPU_DISPATCH_GEOMETRY|QGPU_THREADGROUP_BARRIER|QGPU_DYNAMIC_THREADGROUP) || changed[0].info.driverBase<80 ||
        changed[0].info.threadgroupMask!=8 || !save("nonuniform-runtime.comp",entries[0].glsl))return 1;
    const auto &arrayCopy=imageNamed(initial,"array_copy");
    for(const char *name:{"sampled_vertex","sampled_array_vertex"}) {
        const auto &sample=imageNamed(initial,name);
        if(sample.info.stage!=GPU::Shader::Vertex || sample.info.textureCount!=1 || sample.info.sampleMask!=1 ||
            sample.info.samplerMask!=1 || sample.info.driverBase<16 || sample.info.immediateBase<16 || !GPU::JIT::valid(sample,1))return 1;
        if((sample.info.flags&QGPU_ARRAY_TEXTURE_MASK)!=(std::strcmp(name,"sampled_vertex")?256u:0u))return 1;
        for(const auto &entry:initialEntries)if(entry.name==name && !save(std::strcmp(name,"sampled_vertex")?"vertex-texture-array-runtime.vert":"vertex-texture-msl-runtime.vert",entry.glsl))return 1;
    }
    const std::string vertexImplicit="struct Probe {float4 position [[position]];float4 tint [[user(color)]];};"
        "vertex Probe vertex_implicit(device const float4 *p [[buffer(0)]],texture2d<float> image [[texture(0)]],sampler filtering [[sampler(0)]],uint i [[vertex_id]]) {"
        "Probe o;o.position=p[i];o.tint=image.sample(filtering,(p[i].xy+float2(1.0))*0.5);return o;}";
    if(!check(GPU::Compiler::lowerMSL(vertexImplicit,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.explicitSamplerMask || !save("vertex-texture-implicit-runtime.vert",entries[0].glsl))return 1;
    auto vertexGradient=vertexImplicit;auto sampleCall=vertexGradient.find("(p[i].xy+float2(1.0))*0.5)");
    vertexGradient.replace(sampleCall,std::strlen("(p[i].xy+float2(1.0))*0.5)"),"(p[i].xy+float2(1.0))*0.5,gradient2d(float2(0.125,0.0),float2(0.0,0.125)))");
    if(!check(GPU::Compiler::lowerMSL(vertexGradient,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.explicitSamplerMask || !save("vertex-texture-gradient-runtime.vert",entries[0].glsl))return 1;
    if(((arrayCopy.info.flags&QGPU_ARRAY_TEXTURE_MASK)>>QGPU_ARRAY_TEXTURE_SHIFT)!=129 ||
        arrayCopy.info.imageReadMask!=1 || arrayCopy.info.imageWriteMask!=128 || arrayCopy.info.driverBase<96 ||
        !GPU::JIT::valid(arrayCopy,1))return 1;
    for(const auto &entry:initialEntries)if(entry.name=="array_copy" && !save("array-copy-runtime.comp",entry.glsl))return 1;
    const std::string arraySample="kernel void array_sample(texture2d_array<float> image [[texture(5)]],sampler filtering [[sampler(2)]],"
        "device float4 *out [[buffer(0)]],constant uint2 &control [[buffer(1)]],uint2 id [[thread_position_in_grid]]) {"
        "out[id.x+16u*id.y]=image.sample(filtering,(float2(id)+float2(0.5))/float2(16.0),control.x,level(float(control.y)));}";
    if(!check(GPU::Compiler::lowerMSL(arraySample,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        (changed[0].info.flags&QGPU_ARRAY_TEXTURE_MASK)!=(32u<<QGPU_ARRAY_TEXTURE_SHIFT) || changed[0].info.sampleMask!=32 ||
        changed[0].info.samplerMask!=4 || changed[0].info.driverBase<96 || !save("array-sample-runtime.comp",entries[0].glsl))return 1;
    auto arrayGradient=arraySample;auto levelOption=arrayGradient.find("level(float(control.y))");
    arrayGradient.replace(levelOption,std::strlen("level(float(control.y))"),"gradient2d(float2(0.125,0.0),float2(0.0,0.125))");
    if(!check(GPU::Compiler::lowerMSL(arrayGradient,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.explicitSamplerMask || !save("array-gradient-runtime.comp",entries[0].glsl))return 1;
    const std::string arrayQuery="kernel void array_query(texture2d_array<float,access::read> image [[texture(3)]],device uint *out [[buffer(0)]]) {"
        "out[0]=image.get_width();out[1]=image.get_height(1u);out[2]=image.get_array_size();out[3]=image.get_num_mip_levels();}";
    if(!check(GPU::Compiler::lowerMSL(arrayQuery,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.imageMask!=8 || changed[0].info.imageReadMask || changed[0].info.constlen*4<96 ||
        (changed[0].info.flags&QGPU_ARRAY_TEXTURE_MASK)!=(8u<<QGPU_ARRAY_TEXTURE_SHIFT) || !save("array-query-runtime.comp",entries[0].glsl))return 1;
    const std::string arrayRender="fragment float4 array_render(texture2d_array<float> image [[texture(0)]],sampler filtering [[sampler(0)]],"
        "constant uint2 &control [[buffer(0)]],float4 pixel [[position]]) {return image.sample(filtering,pixel.xy/float2(16.0),control.x,level(float(control.y)));}";
    if(!check(GPU::Compiler::lowerMSL(arrayRender,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        !(changed[0].info.flags&(1u<<QGPU_ARRAY_TEXTURE_SHIFT)) || !save("array-render-runtime.frag",entries[0].glsl))return 1;
    auto arrayImplicit=arrayRender;levelOption=arrayImplicit.find(",level(float(control.y))");arrayImplicit.erase(levelOption,std::strlen(",level(float(control.y))"));
    if(!check(GPU::Compiler::lowerMSL(arrayImplicit,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        !save("array-implicit-runtime.frag",entries[0].glsl))return 1;
    const std::string mixedLOD="fragment float4 mixed_lod(texture2d<float> image [[texture(0)]],sampler filtering [[sampler(0)]],float4 pixel [[position]]) {"
        "return (image.sample(filtering,(pixel.xy-float2(1.0))*0.25)+image.sample(filtering,(pixel.xy-float2(1.0))*0.25,level(2.0)))*0.5;}";
    if(!check(GPU::Compiler::lowerMSL(mixedLOD,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.samplerMask!=1 || changed[0].info.explicitSamplerMask!=1 || changed[0].info.constlen*4<16 || !save("sampler-mixed.frag",entries[0].glsl))return 1;
    for(unsigned stage=1;stage<=2;++stage) {
        std::string many=stage==1?"vertex float4 eight_textures(":"fragment float4 eight_textures(";
        for(unsigned i=0;i<8;++i)many+=(i?",":"")+std::string("texture2d<float> t")+std::to_string(i)+" [[texture("+std::to_string(i)+")]],sampler s"+std::to_string(i)+" [[sampler("+std::to_string(i)+")]]";
        many+=") {return ";for(unsigned i=0;i<8;++i)many+=(i?"+":"")+std::string("t")+std::to_string(i)+".sample(s"+std::to_string(7-i)+",float2(0.5),level(1.0))";
        many+=";}";
        if(!check(GPU::Compiler::lowerMSL(many,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
            changed[0].info.sampleMask!=255 || changed[0].info.samplerMask!=255 || changed[0].info.explicitSamplerMask!=255 ||
            changed[0].info.textureCount!=8 || !GPU::JIT::valid(changed[0],1) ||
            !save(stage==1?"graphics-textures-eight.vert":"graphics-textures-eight.frag",entries[0].glsl))return 1;
        auto invalid=many;auto slot=invalid.find("texture(7)");invalid.replace(slot,10,"texture(8)");
        if(GPU::Compiler::lowerMSL(invalid,entries,error))return 1;
        invalid=many;slot=invalid.find("sampler(7)");invalid.replace(slot,10,"sampler(6)");
        if(GPU::Compiler::lowerMSL(invalid,entries,error))return 1;
    }
    for(unsigned mode=0;mode<2;++mode) {
        std::string source="kernel void cube_modes(texturecube_array<float> image [[texture(6)]],sampler filtering [[sampler(2)]],device float4 *out [[buffer(0)]],"
            "constant float4 &direction [[buffer(1)]],constant uint &cubeIndex [[buffer(2)]],uint index [[thread_position_in_grid]]) {out[index]=image.sample(filtering,direction.xyz,cubeIndex";
        if(mode)source+=",gradientcube(float3(0.125,0.0,0.0),float3(0.0,0.125,0.0))";
        source+=");}";
        if(!check(GPU::Compiler::lowerMSL(source,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
            changed[0].info.cubeSamplerMask!=4 || changed[0].info.explicitSamplerMask ||
            !save(mode?"cube-gradient-runtime.comp":"cube-implicit-runtime.comp",entries[0].glsl))return 1;
    }
    const std::string cubeReadProbe="kernel void cube_read_probe(texturecube_array<float,access::read> image [[texture(0)]],device float4 *out [[buffer(0)]],"
        "constant uint4 &point [[buffer(1)]],constant uint &lod [[buffer(2)]]) {out[0]=image.read(point.xy,point.z,point.w,lod);}";
    if(!check(GPU::Compiler::lowerMSL(cubeReadProbe,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.imageReadMask!=1 || changed[0].info.cubeSamplerMask || !save("cube-read-probe.comp",entries[0].glsl))return 1;
    const std::string samplerPairs="kernel void sampler_pairs(texture2d<float> a [[texture(6)]],texture2d<float> b [[texture(1)]],"
        "sampler x [[sampler(7)]],sampler y [[sampler(2)]],device float4 *out [[buffer(0)]]) {"
        "out[0]=a.sample(x,float2(0.5))+a.sample(y,float2(0.25),level(1.0))+b.sample(x,float2(0.75));}";
    if(!check(GPU::Compiler::lowerMSL(samplerPairs,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.sampleMask!=66 || changed[0].info.samplerMask!=132 || changed[0].info.textureCount!=7 ||
        changed[0].info.imageMask || changed[0].info.explicitSamplerMask!=4 || !save("sampling-pairs.comp",entries[0].glsl))return 1;
    auto sampleAlias=changed[0];sampleAlias.info.imageMask=64;sampleAlias.info.imageWriteMask=64;
    if(GPU::JIT::valid(sampleAlias,1))return 1;
    const std::string dynamicAIR="target datalayout=\"e-p:64:64-i32:32:32\"\ntarget triple=\"air64_v27-apple-ios18.6.0\"\n"
        "define void @dynamic_atomic(ptr addrspace(3) %cell, ptr addrspace(1) %output) {\n"
        "store atomic i32 -9, ptr addrspace(3) %cell monotonic, align 4\n"
        "%old=atomicrmw min ptr addrspace(3) %cell, i32 -11 monotonic\n"
        "%cas=cmpxchg ptr addrspace(3) %cell, i32 -11, i32 7 monotonic monotonic\n"
        "%previous=extractvalue {i32,i1} %cas, 0\nstore i32 %previous, ptr addrspace(1) %output, align 4\nret void }\n"
        "!air.kernel=!{!0}\n!air.version=!{!5}\n!5=!{i32 2,i32 7,i32 0}\n!0=!{ptr @dynamic_atomic,!1,!2}\n!1=!{}\n!2=!{!3,!4}\n"
        "!3=!{i32 0,!\"air.buffer\",!\"air.location_index\",i32 5,!\"air.read_write\",!\"air.address_space\",i32 3,!\"air.arg_type_size\",i32 4,!\"air.arg_type_name\",!\"atomic_int\"}\n"
        "!4=!{i32 1,!\"air.buffer\",!\"air.location_index\",i32 0,!\"air.read_write\",!\"air.address_space\",i32 1,!\"air.arg_type_name\",!\"uint\"}\n";
    if (!check(GPU::Compiler::lowerAIR(dynamicAIR.data(),dynamicAIR.size(),entries,error)) ||
        !check(GPU::Compiler::compile(entries,1,changed,error)) || changed[0].info.threadgroupMask!=32 ||
        !save("dynamic-air-atomic.comp",entries[0].glsl)) return 1;
    const std::string noReturn="fragment float4 missing(float4 p [[position]]) { if(p.x<1.0) return float4(1.0); }";
    const std::string axes="kernel void axes(device uint *out [[buffer(0)]], uint2 id [[thread_position_in_grid]], uint2 size [[threads_per_grid]]) { if(id.y!=size.y-1u) return; out[id.x]=id.x+id.y*17u; }";
    if (!check(GPU::Compiler::lowerMSL(axes,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        !save("compute-uint2.comp",entries[0].glsl)) return 1;
    if (GPU::Compiler::lowerMSL(noReturn,entries,error) || !entries.empty()) return 1;
    const std::string shadow="kernel void shadow(device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]]) { for(uint i=0u;i<2u;++i) out[i]=0u; }";
    if (!check(GPU::Compiler::lowerMSL(shadow,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error))) return 1;
    const std::string integerAttribute="vertex float4 integer_attribute(uint4 input [[attribute(15)]]) { return float4(float(input.x),0.0,0.0,1.0); }";
    if (!check(GPU::Compiler::lowerMSL(integerAttribute,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error)) ||
        changed[0].info.attributeMask!=0x8000 || changed[0].info.attributes[15].type!=2) return 1;
    auto invalidAttribute=initial[8];invalidAttribute.info.attributeMask|=0x10000;
    if (GPU::JIT::valid(invalidAttribute,1)) return 1;
    const std::string invalid="kernel void invalid(device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]]) { out[i+1]=9u; }";
    if (!check(GPU::Compiler::lowerMSL(invalid,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error))) return 1;
    const std::string mixed="vertex float4 mixed(device const float4 *v [[buffer(0)]], uint i [[vertex_id]], uint j [[instance_id]]) { return v[i]+v[j]; }";
    if (!check(GPU::Compiler::lowerMSL(mixed,entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error))) return 1;
    auto shift=air.find("%wide = zext i32 %index");if (shift==std::string::npos) return 1;
    air.replace(shift,23,"%shift = add i32 %index, 1\n  %wide = zext i32 %shift");
    if (!check(GPU::Compiler::lowerAIR(air.data(),air.size(),entries,error)) || !check(GPU::Compiler::compile(entries,1,changed,error))) return 1;
    const std::string readonly="kernel void invalid(device const uint *in [[buffer(0)]], uint i [[thread_position_in_grid]]) { in[i]=1u; }";
    if (!GPU::Compiler::lowerMSL(readonly,entries,error) || GPU::Compiler::compile(entries,1,changed,error) || changed.size() || error.empty()) return 1;
    std::string budget="fragment float4 budget(texture2d<float> t [[texture(0)]]";
    for(unsigned i=0;i<8;++i)budget+=",sampler s"+std::to_string(i)+" [[sampler("+std::to_string(i)+")]]";
    budget+=") {constexpr sampler literal(filter::linear,address::clamp_to_edge);float4 value=t.sample(literal,float2(0.5));";
    for(unsigned i=0;i<8;++i)budget+="value=value+t.sample(s"+std::to_string(i)+",float2(0.5))+t.sample(s"+std::to_string(i)+",float2(0.5),level(1.0));";
    budget+="return value;}";
    if(!check(GPU::Compiler::lowerMSL(budget,entries,error)) || GPU::Compiler::compile(entries,1,changed,error) ||
        !changed.empty() || error.find("16 hardware sampler states")==std::string::npos)return 1;
    const std::string halfAIR=R"air(
target datalayout = "e-p:64:64-i16:16-i32:32-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
define void @half_memory(ptr addrspace(1) %output,ptr addrspace(2) %input) {
 %next = getelementptr half,ptr addrspace(2) %input,i64 1
 %h = load half,ptr addrspace(2) %next,align 2
 %wide = fpext half %h to float
 %sum = fadd float %wide,1.250000e-01
 %narrow = fptrunc float %sum to half
 %scaled = fmul half %narrow,0xH3555
 %positive = fcmp ogt half %scaled,0xH0000
 br i1 %positive,label %on,label %off
on:
 br label %end
off:
 %negated = fneg half %scaled
 br label %end
end:
 %selected = phi half [%scaled,%on],[%negated,%off]
 %seed = insertelement <3 x half> poison,half %selected,i32 0
 %vector = shufflevector <3 x half> %seed,<3 x half> poison,<3 x i32> zeroinitializer
 %clamped = call <3 x half> @air.clamp.v3f16(<3 x half> %vector,<3 x half> zeroinitializer,<3 x half> <half 0xH3C00,half 0xH3C00,half 0xH3C00>)
 %first = extractelement <3 x half> %clamped,i32 0
 store half %first,ptr addrspace(1) %output,align 2
 ret void
}
declare <3 x half> @air.clamp.v3f16(<3 x half>,<3 x half>,<3 x half>)
!air.version=!{!0}
!0=!{i32 2,i32 7,i32 0}
!air.kernel=!{!1}
!1=!{ptr @half_memory,!2,!3}
!2=!{}
!3=!{!4,!5}
!4=!{i32 0,!"air.buffer",!"air.location_index",i32 0,!"air.read_write",!"air.address_space",i32 1,!"air.arg_type_size",i32 2,!"air.arg_type_name",!"half"}
!5=!{i32 1,!"air.buffer",!"air.location_index",i32 1,!"air.read",!"air.address_space",i32 2,!"air.arg_type_size",i32 2,!"air.arg_type_name",!"half"}
)air";
    if(!check(GPU::Compiler::lowerAIR(halfAIR.data(),halfAIR.size(),entries,error)) ||
        !check(GPU::Compiler::compile(entries,1,changed,error)) || changed[0].info.bufferMask!=3 ||
        changed[0].info.writeMask!=1 || changed[0].info.strides[0]!=2 || changed[0].info.strides[1]!=2 ||
        !GPU::JIT::valid(changed[0],1) || !save("half-memory-runtime.comp",entries[0].glsl))return 1;
    // Incoming attachment colors are values, including across branches and
    // PHIs. Reusing one argument must not issue another framebuffer fetch.
    // Real multi-buffer fragment ISA checks generic windows and flag layout.
    for(unsigned count=1;count<=4;++count) {
        GPU::Compiler::Entry entry;entry.name="constant_windows";entry.reflection.stage=2;
        entry.glsl="#version 430\n#extension GL_AMD_gpu_shader_half_float : require\nlayout(location=0) out vec4 color;\n";
        for(unsigned slot=0;slot<count;++slot) {
            entry.reflection.bufferMask|=1u<<slot;entry.reflection.strides[slot]=2;
            entry.glsl+="layout(std430,binding="+std::to_string(slot)+") readonly buffer B"+std::to_string(slot)+" { float16_t data[]; } b"+std::to_string(slot)+";\n";
        }
        const auto declarations=entry.glsl;
        entry.glsl+="void main(){ f16vec4 sum=f16vec4(0);\n";
        for(unsigned slot=0;slot<count;++slot)for(unsigned row=0;row<5;++row) {
            entry.glsl+="sum+=f16vec4(";
            for(unsigned lane=0;lane<4;++lane)entry.glsl+=(lane?",":"")+std::string("b")+std::to_string(slot)+".data["+std::to_string(88+4*row+lane)+"]";
            entry.glsl+=");\n";
        }
        entry.glsl+="color=vec4(sum);}\n";
        if(!check(GPU::Compiler::compile({entry},1,changed,error)))return 1;
        const auto &image=changed[0];
        if(image.info.constantWindowCount!=count || image.msaa.info.constantWindowCount!=count ||
           !GPU::JIT::valid(image,1) || image.info.immediateBase<qgpu_constant_memory_words(count))return 1;
        for(unsigned slot=0;slot<count;++slot)
            if(image.info.constantWindows[slot].buffer!=slot || image.info.constantWindows[slot].byteOffset!=160)return 1;
        if(!save(("constant-windows-"+std::to_string(count)+".frag").c_str(),entry.glsl))return 1;
        if(count==1) {
            for(const std::string qualifier:{"readonly volatile","readonly coherent"}) {
                auto excluded=entry;auto at=excluded.glsl.find("readonly buffer");
                excluded.glsl.replace(at,8,qualifier);
                if(!check(GPU::Compiler::compile({excluded},1,changed,error)) || changed[0].info.constantWindowCount)return 1;
            }
            auto written=entry;written.glsl=declarations+"layout(std430,binding=7) buffer W { uint data[]; } w;\n"+
                entry.glsl.substr(declarations.size());
            written.glsl.insert(written.glsl.find("color=vec4(sum);"),"w.data[0]=1u;\n");
            written.reflection.bufferMask|=128;written.reflection.writeMask=128;written.reflection.strides[7]=4;
            if(!check(GPU::Compiler::compile({written},1,changed,error)) || changed[0].info.constantWindowCount)return 1;
        }
    }
    {
        GPU::Compiler::Entry word;word.name="word_windows";word.reflection.stage=2;
        word.reflection.bufferMask=8;word.reflection.strides[3]=16;
        word.glsl="#version 430\nlayout(location=0) out vec4 color;\nlayout(std430,binding=3) readonly buffer B { vec4 data[]; } input0;\nvoid main(){vec4 sum=vec4(0);\n";
        for(unsigned row=0;row<16;++row)word.glsl+="sum+=input0.data["+std::to_string(row)+"];\n";
        word.glsl+="color=sum;}\n";
        if(!check(GPU::Compiler::compile({word},1,changed,error)) || !GPU::JIT::valid(changed[0],1))return 1;
        for(const auto *p:{&changed[0].info,&changed[0].msaa.info,&changed[0].constantWindow.info,&changed[0].msaaConstantWindow.info}) {
            if(p->constantWindowCount!=4)return 1;
            for(unsigned i=0;i<4;++i)if(p->constantWindows[i].buffer!=3 || p->constantWindows[i].byteOffset!=64*i)return 1;
        }
        if(!save("word-windows.frag",word.glsl))return 1;
        for(const std::string qualifier:{"readonly volatile","readonly coherent"}) {
            auto excluded=word;auto at=excluded.glsl.find("readonly buffer");excluded.glsl.replace(at,8,qualifier);
            // The unpromoted sixteen-vector fixture exceeds the existing
            // register profile. Eight vectors still cover all four windows
            // while keeping both excluded variants executable on A540.
            for(unsigned row=0;row<16;++row)if(row%4==1 || row%4==2) {
                auto line="sum+=input0.data["+std::to_string(row)+"];\n";
                auto at=excluded.glsl.find(line);if(at==std::string::npos)return 1;
                excluded.glsl.erase(at,line.size());
            }
            if(!check(GPU::Compiler::compile({excluded},1,changed,error)) || changed[0].info.constantWindowCount)return 1;
        }
        const auto native=read(root+"/tools/gpuctl/metalctl.mm");
        const std::string begin="static const char wordWindowSource[]=R\"QGPU(";
        auto first=native.find(begin);if(first==std::string::npos)return 1;first+=begin.size();
        auto last=native.find(")QGPU\";",first);if(last==std::string::npos)return 1;
        if(!check(GPU::Compiler::lowerMSL(native.substr(first,last-first),entries,error)) ||
           !check(GPU::Compiler::compile(entries,1,changed,error)))return 1;
        bool found=false;
        for(const auto &image:changed)if(std::string(image.name)=="word_consumer") {
            found=true;if(!GPU::JIT::valid(image,1) || image.info.constantWindowCount!=4 || image.msaa.info.constantWindowCount!=4)return 1;
            if(!(image.constantWindow.info.flags&QGPU_CONSTANT_WINDOWS_REQUIRED) ||
               !(image.msaaConstantWindow.info.flags&QGPU_CONSTANT_WINDOWS_REQUIRED) ||
               image.constantWindow.info.instrlen>=image.info.instrlen ||
               image.msaaConstantWindow.info.instrlen>=image.msaa.info.instrlen)return 1;
            for(unsigned i=0;i<4;++i)if(image.info.constantWindows[i].buffer!=1 || image.info.constantWindows[i].byteOffset!=64*i)return 1;
        }
        if(!found)return 1;
        std::cout<<"32-bit constant windows: four independent ranges in one buffer, base/MSAA and coherent/volatile exclusion passed\n";
    }
    {
        const auto source=read(root+"/tools/gpuctl/metalctl.mm");
        const std::string begin="static const char constantWindowFramebufferSource[]=R\"QGPU(";
        auto first=source.find(begin);if(first==std::string::npos)return 1;first+=begin.size();
        auto last=source.find(")QGPU\";",first);if(last==std::string::npos)return 1;
        const auto airSource=source.substr(first,last-first);
        if(!check(GPU::Compiler::lowerAIR(airSource.data(),airSource.size(),entries,error)) ||
            !check(GPU::Compiler::compile(entries,1,changed,error)) || changed.size()!=1 ||
            !GPU::JIT::valid(changed[0],1) || changed[0].info.framebufferMask!=1 ||
            changed[0].info.constantWindowCount!=1 || changed[0].info.constantWindows[0].buffer!=1 ||
            changed[0].info.constantWindows[0].byteOffset!=160 || changed[0].msaa.info.constantWindowCount!=1 ||
            !save("constant-window-framebuffer.frag",entries[0].glsl))return 1;
        std::cout<<"Native constant-window framebuffer source: real base/MSAA ISA,window1[160,224),framebuffer fetch and ABI validation passed\n";
    }
    const std::string framebufferAIR=R"air(
target datalayout = "e-p:64:64-i16:16-i32:32-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
define <4 x half> @framebuffer_snapshot(<4 x half> %a, <4 x half> %b, i32 %sample) {
entry:
 %twice = fadd <4 x half> %a, %a
 %first = icmp eq i32 %sample, 0
 br i1 %first, label %on, label %off
on:
 %sum = fadd <4 x half> %twice, %b
 br label %end
off:
 %difference = fsub <4 x half> %twice, %b
 br label %end
end:
 %result = phi <4 x half> [%sum, %on], [%difference, %off]
 ret <4 x half> %result
}
!air.version = !{!0}
!0 = !{i32 2, i32 7, i32 0}
!air.fragment = !{!1}
!1 = !{ptr @framebuffer_snapshot, !2, !3}
!2 = !{!4}
!3 = !{!5, !6, !7}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"half4"}
!5 = !{i32 0, !"air.render_target", i32 0, !"air.arg_type_name", !"half4"}
!6 = !{i32 1, !"air.render_target", i32 1, !"air.arg_type_name", !"half4"}
!7 = !{i32 2, !"air.sample_id", !"air.arg_type_name", !"uint"}
)air";
    const std::string framebufferMSL="#include <metal_stdlib>\nusing namespace metal;\n"
        "fragment float4 framebuffer_snapshot(float4 a [[color(0)]], float4 b [[color(1)]], uint sample [[sample_id]]) {"
        "float4 twice=a+a;if(sample==0)return twice+b;return twice-b;}";
    for(bool fromAIR:{false,true}) {
        if(!check(fromAIR?GPU::Compiler::lowerAIR(framebufferAIR.data(),framebufferAIR.size(),entries,error):
                         GPU::Compiler::lowerMSL(framebufferMSL,entries,error)) || entries.size()!=1 ||
            !check(GPU::Compiler::compile(entries,1,changed,error)) || changed[0].info.framebufferMask!=3 ||
            changed[0].msaa.info.framebufferMask!=3)return 1;
        // No buffer-size or constant-window prefix can mask a missing origin
        // reservation here. A 64B indirect load at word 80 must not overwrite
        // compiler immediates or driver parameters in either sample variant.
        const auto &image=changed[0];
        if(!GPU::JIT::valid(image,1))return 1;
        for(const auto *p:{&image.info,&image.msaa.info}) {
            if(!(p->flags&QGPU_FRAMEBUFFER_TILE_ORIGIN) || p->bufferMask || p->constantWindowCount ||
               GPU::Shader::memoryConstantWords(*p)!=96 || p->constlen*4<96 ||
               (p->immediateCount && p->immediateBase<96) || (p->driverCount && p->driverBase<96))return 1;
            auto bad=*p;bad.constlen=20;if(GPU::JIT::validDescription(bad))return 1;
            bad=*p;bad.immediateBase=80;bad.immediateCount=1;if(GPU::JIT::validDescription(bad))return 1;
            bad=*p;bad.driverBase=80;bad.driverCount=1;if(GPU::JIT::validDescription(bad))return 1;
            bad=*p;bad.framebufferMask=0;if(GPU::JIT::validDescription(bad))return 1;
        }
        auto inconsistent=image;inconsistent.msaa.info.flags^=QGPU_FRAMEBUFFER_TILE_ORIGIN;
        if(GPU::JIT::valid(inconsistent,1))return 1;
        const auto body=entries[0].glsl.substr(entries[0].glsl.find("void main()"));
        for(unsigned slot=0;slot<2;++slot) {
            auto read="_qframebuffer"+std::to_string(slot)+"_read()";
            auto at=body.find(read);
            if(at==std::string::npos || body.find(read,at+read.size())!=std::string::npos)return 1;
        }
    }
    std::cout<<"Framebuffer tile origin: isolated 64B constants at words 80..95, base/MSAA prefix and overlap rejection passed\n";
    auto image=initial[0];image.info.immediateCount=513;
    if (GPU::JIT::valid(image,1)) return 1;
    std::cout<<"MSL + AIR IR/bitcode -> real A540 code; control flow/PHIs, changed source, cleanup and rejection paths passed\n";
}
