// SPDX-License-Identifier: BSD-2-Clause
// Production metadata, bound-range planner and PM4 ordering; no ISA emulation.
#include "AdrenoPipeline.h"
#include <GPUJITProtocol.h>
#include <GPUPrograms.h>
#include <GPUOperationActivity.h>
#include "../drivers/gpu/QcomMTL/MetalShaderCapture.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
int main() {
    using namespace GPU::Shader;
    static_assert(sizeof(QGPUProgramDescription)==2468 && sizeof(QGPUProgramImage)==83720,"v39");
    static_assert(offsetof(Program,constantWindowCount)==offsetof(QGPUProgramDescription,constantWindowCount) &&
        offsetof(Program,constantWindows)==offsetof(QGPUProgramDescription,constantWindows) &&
        offsetof(Program,code)==((sizeof(QGPUProgramDescription)+alignof(void *)-1)&~(alignof(void *)-1)),"kernel/wire prefix offsets");
    static_assert(QGPU_CONSTANT_WINDOW_BASE==112 && QGPU_MAX_MEMORY_CONSTANT_WORDS==180,"constant layout");
    static_assert(Adreno::ConstantWindowScratch>=GPU::OperationActivity::ScratchOffset+GPU::OperationActivity::CommandCapacity*16 &&
        Adreno::ConstantWindowScratch>=GPU::ShaderCounters::ScratchOffset+GPU::ShaderCounters::Count*16 &&
        Adreno::ConstantWindowScratch+QGPU_MAX_CONSTANT_WINDOWS*QGPU_CONSTANT_WINDOW_BYTES<=Adreno::GMEMOrigin,
        "constant staging cannot alias profiling or GMEM cells");
    static_assert(sizeof(GPU::OperationActivity::Description)==456 && sizeof(GPU::OperationActivity::Record)==496 &&
        offsetof(GPU::OperationActivity::Description,constantWindowState)==324,
        "version 4 dispatch telemetry preserves the constant window field");
    for(unsigned count=1;count<=4;++count) {
        assert(!(qgpu_constant_memory_words(count)&3));
        for(unsigned i=0;i<count;++i)assert(qgpu_constant_window_flag(count,i)>=112+16*count &&
            qgpu_constant_window_flag(count,i)<qgpu_constant_memory_words(count));
    }
    auto fs=*GPUPrograms::lookup(3); // Real constant-color ISA; packet tests add an unused window.
    const auto &vs=*GPUPrograms::lookup(2);
    fs.bufferMask=2;fs.strides[1]=2;fs.flags|=QGPU_BOUNDED_BUFFERS|QGPU_FRAGMENT_BUFFER_BANK|QGPU_BUFFER_SIZE_CONSTANTS;
    fs.constantWindowCount=1;fs.constantWindows[0]={1,160};fs.constlen=40;
    assert(validInterface(fs));
    auto image=std::make_unique<QGPUProgramImage>();image->registry=1;image->version=GPU::JIT::Version;image->gpu=540;
    std::strcpy(image->name,"constant_color_packet_fixture");
    std::memcpy(&image->info,&fs,sizeof(image->info));image->info.id=0;
    std::memcpy(image->code,fs.code,fs.codeWords*4);
    image->msaa.info=image->info;image->msaa.info.sampleFlags|=QGPU_SAMPLE_MSAA;
    std::memcpy(image->msaa.code,fs.code,fs.codeWords*4);
    assert(GPU::JIT::valid(*image,1));
    for(unsigned old:{35u,36u,37u,38u}){image->version=old;assert(!GPU::JIT::valid(*image,1));}
    image->version=GPU::JIT::Version;
    image->constantWindow.info=image->info;image->constantWindow.info.flags|=QGPU_CONSTANT_WINDOWS_REQUIRED;
    image->msaaConstantWindow.info=image->msaa.info;image->msaaConstantWindow.info.flags|=QGPU_CONSTANT_WINDOWS_REQUIRED;
    std::memcpy(image->constantWindow.code,fs.code,fs.codeWords*4);
    std::memcpy(image->msaaConstantWindow.code,fs.code,fs.codeWords*4);
    assert(GPU::JIT::valid(*image,1));
    image->msaaConstantWindow.info.constantWindows[0].byteOffset+=16;assert(!GPU::JIT::valid(*image,1));
    image->msaaConstantWindow.info.constantWindows[0].byteOffset-=16;
    image->constantWindow.info.flags&=~QGPU_CONSTANT_WINDOWS_REQUIRED;assert(!GPU::JIT::valid(*image,1));
    image->constantWindow.info.flags|=QGPU_CONSTANT_WINDOWS_REQUIRED;
    image->info.flags|=QGPU_CONSTANT_WINDOWS_REQUIRED;assert(!GPU::JIT::valid(*image,1));
    image->info.flags&=~QGPU_CONSTANT_WINDOWS_REQUIRED;
    image->constantWindow={};assert(!GPU::JIT::valid(*image,1));image->msaaConstantWindow={};
    assert(GPU::JIT::valid(*image,1));
    assert(GPU::JIT::uploadChunkBytes(0)==65536 && GPU::JIT::uploadChunkBytes(1)==18184 && !GPU::JIT::uploadChunkBytes(2));
    GPU::OperationActivity::Program identity;GPU::OperationActivity::describe(identity,fs);
    assert(identity.hash==GPU::MetalShaderCapture::programHash(*image));
    auto different=fs;different.constantWindows[0].byteOffset+=16;
    GPU::OperationActivity::Program changed;GPU::OperationActivity::describe(changed,different);
    assert(changed.hash!=identity.hash);
    Adreno::ShaderBuffers buffers{};buffers.addresses[0]=0x4000000;buffers.bytes[0]=4096;
    buffers.addresses[9]=0x4500000;buffers.bytes[9]=224;
    uint32_t packet[3];assert(Adreno::constantWindowPacket(fs,0,buffers,packet));
    assert(packet[0]==0x0132001c && packet[1]==0x45000a1 && !packet[2]);
    unsigned checked=0;
    auto specialized=fs;specialized.flags|=QGPU_CONSTANT_WINDOWS_REQUIRED;fs.constantWindow=&specialized;
    for(unsigned bytes=0;bytes<=256;++bytes)for(unsigned align=0;align<32;++align) {
        buffers.bytes[9]=bytes;buffers.addresses[9]=0x4500000+align;
        const bool accepted=Adreno::constantWindowPacket(fs,0,buffers,packet);
        assert(accepted==(bytes>=224 && !(align&15)));
        assert(Adreno::constantWindowRequirements(specialized,buffers)==accepted);
        assert(&Adreno::selectConstantWindowProgram(fs,buffers)==(accepted?&specialized:&fs));
        if(!accepted)assert(!packet[0] && !packet[1] && !packet[2]);
        Adreno::ConstantWindowPlan plan;
        const unsigned stagedBytes=bytes<=160?0:bytes<224?bytes-160:64;
        const bool shortRange=!accepted && bytes>160 && !((stagedBytes|align)&3);
        const bool emptyRange=bytes<=160;
        assert(Adreno::planConstantWindow(fs,0,buffers,0x5400000,4096,plan)==(accepted || shortRange || emptyRange));
        assert(&Adreno::selectConstantWindowProgram(fs,buffers,0x5400000,4096)==
            (accepted || shortRange || emptyRange?&specialized:&fs));
        if(shortRange || emptyRange) {
            assert(plan.source==(emptyRange?0:0x4500000+align+160) && plan.bytes==stagedBytes);
            assert(plan.destination==0x5400000+Adreno::ConstantWindowScratch);
            assert(plan.packet[0]==0x0132001c && plan.packet[1]==(plan.destination|1) && !plan.packet[2]);
            assert(!Adreno::planConstantWindow(fs,0,buffers,0x5400000,4095,plan));
            assert(!Adreno::planConstantWindow(fs,0,buffers,0x5400004,4096,plan));
            assert(!Adreno::planConstantWindow(fs,0,buffers,UINT64_MAX-4095,4096,plan));
        }
        ++checked;
    }
    buffers.addresses[9]=UINT64_MAX-80;buffers.bytes[9]=224;
    assert(!Adreno::constantWindowPacket(fs,0,buffers,packet));
    buffers.addresses[9]=uint64_t(1)<<Adreno::AddressBits;
    assert(!Adreno::constantWindowPacket(fs,0,buffers,packet));
    {
        // Four independent windows of one real binding. Every complete
        // range is checked separately; the short tail must stay disabled.
        auto multi=fs;multi.constantWindowCount=4;multi.constlen=52;
        if(multi.immediateCount)multi.immediateBase=qgpu_constant_memory_words(4);
        for(unsigned i=0;i<4;++i)multi.constantWindows[i]={1,64*i};
        assert(validInterface(multi));buffers.addresses[9]=0x4500000;buffers.bytes[9]=196;
        auto fast=multi;fast.flags|=QGPU_CONSTANT_WINDOWS_REQUIRED;multi.constantWindow=&fast;
        assert(&Adreno::selectConstantWindowProgram(multi,buffers)==&multi);
        for(unsigned i=0;i<4;++i) {
            assert(Adreno::constantWindowPacket(multi,i,buffers,packet)==(i<3));
            if(i<3)assert(packet[1]==(0x4500000+64*i+1) && (packet[0]&65535)==28+4*i);
        }
        buffers.bytes[9]=256;assert(&Adreno::selectConstantWindowProgram(multi,buffers)==&fast);
        auto sampled=multi;sampled.sampleFlags|=QGPU_SAMPLE_MSAA;
        auto fastSampled=fast;fastSampled.sampleFlags|=QGPU_SAMPLE_MSAA;sampled.constantWindow=&fastSampled;multi.msaa=&sampled;
        assert(&Adreno::selectConstantWindowProgram(rasterProgram(multi,4),buffers)==&fastSampled);
        auto bad=multi;bad.constantWindows[1].byteOffset=48;assert(!validInterface(bad));
        bad=multi;bad.constantWindows[1].byteOffset=0;assert(!validInterface(bad));
        bad=multi;std::swap(bad.constantWindows[1],bad.constantWindows[2]);assert(!validInterface(bad));
        bad=multi;bad.constantWindows[2].buffer=0;assert(!validInterface(bad));
    }
    auto invalid=fs;invalid.constantWindowCount=5;assert(!validInterface(invalid));
    invalid=fs;invalid.constantWindows[1]={2,0};assert(!validInterface(invalid));
    invalid=fs;invalid.constantWindows[0].buffer=8;assert(!validInterface(invalid));
    invalid=fs;invalid.constantWindows[0].byteOffset=164;assert(!validInterface(invalid));
    invalid=fs;invalid.constantWindows[0].byteOffset=0xfffffff0;assert(!validInterface(invalid));
    invalid=fs;invalid.immediateCount=1;invalid.immediateBase=128;assert(!validInterface(invalid));
    invalid=fs;invalid.stage=Compute;assert(!validInterface(invalid));
    invalid=fs;invalid.writeMask=2;assert(!validInterface(invalid));
    invalid=fs;invalid.writeMask=1;invalid.bufferMask|=1;invalid.strides[0]=4;
    buffers.addresses[9]=0x4500000;assert(!Adreno::constantWindowPacket(invalid,0,buffers,packet));
    constexpr uint64_t base=0x3200000,scratch=0x5400000;
    {
        // Empty windows never issue source reads. Their zero fill must be
        // visible to indirect LOAD_STATE before any draw consumes the bank.
        auto unbound=fs;unbound.constantWindows[0]={1,0};
        Adreno::ShaderBuffers empty{};Adreno::ConstantWindowPlan plan;
        assert(Adreno::planConstantWindow(unbound,0,empty,scratch,4096,plan));
        assert(!plan.source && !plan.bytes && plan.destination==scratch+Adreno::ConstantWindowScratch);
    }
    std::array<uint32_t,Adreno::IBDwords> words{};Adreno::Encoder e(words.data(),words.size());
    Adreno::PipelineEncoder pipeline(e,base,scratch);
    Draw draw{};draw.primitive=3;draw.count=3;draw.instances=1;
    draw.target={1,0,64,64,256,RGBA8};draw.state=defaultRenderState(64,64);draw.scissor[2]=draw.scissor[3]=64;
    draw.buffers[0]={2,0,4096,0};draw.fragmentBuffers[1]={3,0,224,0};
    {
        auto unbound=fs;unbound.constantWindows[0]={1,0};unbound.flags|=QGPU_CONSTANT_WINDOWS_REQUIRED;
        auto empty=buffers;empty.addresses[9]=empty.bytes[9]=0;
        auto missing=draw;missing.fragmentBuffers[1]={};
        std::array<uint32_t,Adreno::IBDwords> stream{};Adreno::Encoder commands(stream.data(),stream.size());
        Adreno::PipelineEncoder encoder(commands,base,scratch);
        assert(encoder.draw(missing,vs,unbound,empty,0x4200000,{})==GPU::Result::Success);
        assert(encoder.lastConstantWindowState()==0x101);
        unsigned fill=0,wait=0,load=0,drawAt=0;
        for(unsigned i=0;i<commands.size();) {
            unsigned at=i;uint32_t h=stream[i++];
            if((h>>28)==4){i+=h&127;continue;}
            assert((h>>28)==7);unsigned op=(h>>16)&127,n=h&0x3fff;assert(i+n<=commands.size());
            assert(op!=0x73); // No CP_MEM_TO_MEM from the absent binding.
            if(op==0x3d && n==18 && stream[i]==scratch+Adreno::ConstantWindowScratch) {
                fill=at;for(unsigned j=0;j<16;++j)assert(!stream[i+2+j]);
            }
            if(op==0x12)wait=at;
            if(op==0x30 && n==3 && stream[i]==0x0132001c) {
                load=at;assert(fill && fill<wait && wait<load);
                assert(stream[i+1]==(scratch+Adreno::ConstantWindowScratch+1) && !stream[i+2]);
            }
            if(op==0x38)drawAt=at;
            i+=n;
        }
        assert(load && load<drawAt);
    }
    buffers.addresses[9]=0x4500000;buffers.bytes[9]=224;
    buffers.bytes[9]=214; // A two-byte tail cannot be copied with CP_MEM_TO_MEM.
    assert(pipeline.draw(draw,vs,specialized,buffers,0x4200000,{})==GPU::Result::Invalid && !e.size());
    buffers.bytes[9]=224;
    assert(pipeline.draw(draw,vs,fs,buffers,0x4200000,{})==GPU::Result::Success);
    assert(pipeline.lastConstantWindowState()==0x101);
    buffers.bytes[9]=216;draw.fragmentBuffers[1].bytes=216;
    assert(pipeline.draw(draw,vs,fs,buffers,0x4200000,{})==GPU::Result::Success);
    assert(pipeline.lastConstantWindowState()==0x101);
    buffers.bytes[9]=224;draw.fragmentBuffers[1].bytes=224;buffers.addresses[9]=0x4500004;
    assert(pipeline.draw(draw,vs,fs,buffers,0x4200000,{})==GPU::Result::Success);
    assert(pipeline.lastConstantWindowState()==0x101);
    buffers.addresses[9]=0x4600000;
    assert(pipeline.draw(draw,vs,fs,buffers,0x4200000,{})==GPU::Result::Success);
    assert(pipeline.lastConstantWindowState()==0x101);
    assert(pipeline.operation(nullptr,false,[&](Adreno::IndirectBufferAllocator *){return e.barrier();})==GPU::Result::Success);
    assert(!pipeline.lastConstantWindowState());
    std::vector<uint32_t> flags,sources;unsigned draws=0;
    unsigned flushed=0,waited=0,memwait=0,updated=0,direct=0,zeroed=0,copied=0,lastCopy=0;
    for(unsigned i=0;i<e.size();) {
        const unsigned at=i;uint32_t h=words[i++];
        if((h>>28)==4) {
            unsigned n=h&127,reg=(h>>8)&0x3ffff;assert(i+n<=e.size());
            if(reg==REG_A5XX_HLSQ_UPDATE_CNTL)updated=at;
            i+=n;continue;
        }
        assert((h>>28)==7);unsigned op=(h>>16)&127,n=h&0x3fff;assert(i+n<=e.size());
        if(op==0x46 && n==4 && words[i]==4)flushed=at;
        if(op==0x3c && n==6 && words[i+1]==scratch+16)waited=at;
        if(op==0x12)memwait=at;
        if(op==0x3d && n==18 && words[i]==scratch+Adreno::ConstantWindowScratch) {
            assert(waited<at && words[i+1]==0);
            for(unsigned j=0;j<16;++j)assert(words[i+2+j]==0);
            ++zeroed;
        }
        if(op==0x73) {
            assert(n==5 && words[i]==(1u<<30) && (zeroed==1 || zeroed==2));
            const unsigned lane=zeroed==1?copied:copied-14;
            assert(lane<(zeroed==1?14u:16u));
            assert(words[i+1]==scratch+Adreno::ConstantWindowScratch+lane*4 && !words[i+2]);
            assert(words[i+3]==(zeroed==1?0x45000a0:0x45000a4)+lane*4 && !words[i+4]);
            ++copied;lastCopy=at;
        }
        if(op==0x30 && n>=3 && ((words[i]>>18)&15)==12 && (words[i+1]&3)==1) {
            if(((words[i]>>16)&3)==0) {
                direct=at;assert(n==3+fs.constlen*4);flags.push_back(words[i+3+qgpu_constant_window_flag(1,0)]);
            } else {
                assert(n==3 && words[i]==0x0132001c && flags.back()==1);
                assert(flushed<waited && waited<memwait && memwait<updated && updated<direct && direct<at);
                sources.push_back(words[i+1]&~3u);
                if((words[i+1]&~3u)==scratch+Adreno::ConstantWindowScratch)
                    assert(((zeroed==1 && copied==14) || (zeroed==2 && copied==30)) && lastCopy<memwait && memwait<at);
            }
        }
        if(op==0x38)++draws;
        i+=n;
    }
    assert(draws==4 && flags==std::vector<uint32_t>({1,1,1,1}));
    assert(zeroed==2 && copied==30);
    assert(sources==std::vector<uint32_t>({0x45000a0,uint32_t(scratch+Adreno::ConstantWindowScratch),uint32_t(scratch+Adreno::ConstantWindowScratch),0x46000a0}));
    // Real framebuffer metadata with an unused window: packet/dependency
    // tests only. Shift its immediates past the added metadata, without
    // claiming that these synthetic program records execute the original ISA.
    auto fetch=*GPUPrograms::lookup(125);const auto &copy=*GPUPrograms::lookup(124);
    fetch.constantWindowCount=1;fetch.constantWindows[0]={1,160};
    fetch.bufferMask|=2;fetch.strides[1]=2;fetch.constlen=40;
    fetch.immediateBase=qgpu_constant_memory_words(1);
    assert(validInterface(fetch));
    draw.fragmentBuffers[0]={4,0,16,0};buffers.addresses[8]=0x4700000;buffers.bytes[8]=16;
    buffers.addresses[9]=0x4500000;buffers.bytes[9]=224;
    struct Observation {unsigned flushes=0,loads=0,draws=0,writes=0;bool clean=false;uint32_t pending=0;};
    auto observe=[&](const uint32_t *stream,unsigned size,Observation &out) {
        for(unsigned i=0;i<size;) {
            uint32_t h=stream[i++];
            if((h>>28)==4){i+=h&127;continue;}
            assert((h>>28)==7);unsigned op=(h>>16)&127,n=h&0x3fff;assert(i+n<=size);
            if(op==0x46 && n==4 && stream[i]==4){out.pending=stream[i+3];++out.flushes;}
            if(op==0x3c && n==6 && stream[i+1]==scratch+16) {
                assert(out.pending && stream[i+3]==out.pending);out.clean=true;out.pending=0;
            }
            if(op==0x30 && n==3 && stream[i]==0x0132001c) {assert(out.clean);++out.loads;}
            if(op==0x38 || op==0x33 || op==0x2c || op==0x73 || op==0x3d) {
                out.clean=false;++out.writes;if(op==0x38)++out.draws;
            }
            i+=n;
        }
    };
    for(bool inplace:{false,true})for(bool reuse:{false,true})for(bool primitiveSnapshot:{false,true}) {
        std::vector<uint32_t> stream(Adreno::IBDwords);Adreno::Encoder commands(stream.data(),stream.size());
        Adreno::PipelineEncoder encoder(commands,base,scratch,32768);encoder.enableFramebufferInplace(inplace);
        encoder.enableFramebufferStateReuse(reuse);draw.count=6;
        assert(encoder.draw(draw,vs,fetch,buffers,0x4200000,{},0,0,0,nullptr,&copy,nullptr,primitiveSnapshot)==GPU::Result::Success);
        Observation out;observe(stream.data(),commands.size(),out);
        unsigned setups=primitiveSnapshot?1:inplace&&reuse?1:2;
        assert(out.loads==setups && out.draws==(primitiveSnapshot?1:2));
        // Explicit snapshot input has no local pre-publication proof. Plain
        // raster must still emit its own full publish, even with FB metadata.
        assert(out.flushes==(primitiveSnapshot?2u:inplace?3u:5u));
    }
    // Fresh operation after a compute producer cannot inherit a previous
    // draw's publication. Both framebuffer and ordinary paths must publish.
    std::vector<uint32_t> mixed(Adreno::IBDwords);Adreno::Encoder commands(mixed.data(),mixed.size());
    Adreno::PipelineEncoder encoder(commands,base,scratch,32768);encoder.enableFramebufferInplace();draw.count=3;
    assert(encoder.draw(draw,vs,fetch,buffers,0x4200000,{},0,0,0,nullptr,&copy)==GPU::Result::Success);
    Dispatch work{};work.groups[0]=work.groups[1]=work.groups[2]=work.local[0]=work.local[1]=work.local[2]=1;
    work.buffers[0]={1,0,4,0};work.buffers[1]={2,0,4,0};
    auto memory=buffers;memory.addresses[1]=0x4500000;memory.bytes[1]=224;
    assert(encoder.dispatch(work,copy,memory)==GPU::Result::Success);
    assert(encoder.draw(draw,vs,fs,buffers,0x4200000,{})==GPU::Result::Success);
    Observation ordered;observe(mixed.data(),commands.size(),ordered);assert(ordered.loads==2 && ordered.draws==2);
    struct Batch : Adreno::IndirectBufferAllocator {
        std::vector<std::vector<uint32_t>> chunks{std::vector<uint32_t>(Adreno::IBDwords)};
        std::vector<unsigned> sizes;unsigned limit=Adreno::MaxIndirectBuffers;
        GPU::Result next(Adreno::Encoder &stream,uint64_t &address) override {
            if(chunks.size()>=limit)return GPU::Result::NoResources;
            assert(stream.reserveTail(0) && stream.barrier()==GPU::Result::Success);sizes.push_back(stream.size());
            chunks.emplace_back(Adreno::IBDwords);address+=Adreno::IBDwords*4;
            stream=Adreno::Encoder(chunks.back().data(),Adreno::IBDwords,2);return GPU::Result::Success;
        }
    } batch;
    Adreno::Encoder expanded(batch.chunks[0].data(),Adreno::IBDwords,2);
    Adreno::PipelineEncoder chunks(expanded,base,scratch,32768);chunks.enableFramebufferInplace();
    draw.count=3*192;draw.instances=2;chunks.profileNextOperation(0);
    assert(chunks.operation(&batch,true,[&](Adreno::IndirectBufferAllocator *allocator) {
        return chunks.draw(draw,vs,fetch,buffers,0x4200000,{},0,0,0,nullptr,&copy,nullptr,false,allocator);
    })==GPU::Result::Success);
    assert(expanded.reserveTail(0) && expanded.barrier()==GPU::Result::Success);batch.sizes.push_back(expanded.size());
    assert(batch.chunks.size()>1 && !chunks.lastOperationTimed());
    Observation continuity;
    for(unsigned i=0;i<batch.chunks.size();++i)observe(batch.chunks[i].data(),batch.sizes[i],continuity);
    assert(continuity.draws==384 && continuity.flushes==385 && continuity.loads==chunks.fullRasterSetups());
    Batch denied;denied.limit=1;Adreno::Encoder limited(denied.chunks[0].data(),Adreno::IBDwords,2);
    Adreno::PipelineEncoder failing(limited,base,scratch,32768);failing.enableFramebufferInplace();
    assert(failing.operation(&denied,true,[&](Adreno::IndirectBufferAllocator *allocator) {
        return failing.draw(draw,vs,fetch,buffers,0x4200000,{},0,0,0,nullptr,&copy,nullptr,false,allocator);
    })==GPU::Result::NoResources && denied.sizes.empty());
    printf("GPU constant windows: %u bound/alignment cases; metadata rejection, real flush/wait/HLSQ/load/draw ordering and stale-flag/rebind checks passed; local framebuffer publication reuse, mixed compute, IB rollover and trial rollback passed\n",checked);
}
