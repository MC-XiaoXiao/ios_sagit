// SPDX-License-Identifier: BSD-2-Clause
#include "../drivers/gpu/QcomMTL/MetalTexture.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using GPU::MetalTexture::MemoryLayout;
using GPU::MetalTexture::CopyRegion;
using namespace GPU::Shader;

static void mipArrayCopies(uint32_t format)
{
    const uint32_t cpp=formatBytes(format),guard=64;
    Texture texture{1,0,130,65,192*cpp,format,8,0,2};
    // Independent frozen A540 row/offset expectations, including the final
    // 32-row mip and 4096-byte slice rounding; these are not a round-trip oracle.
    const uint32_t offsets[]={0,12480,16576,17600,18112,18368,18496,18560};
    const uint32_t pitches[]={192,128,64,64,64,64,64,64};
    const uint32_t stride=(20608*cpp+4095)&~4095u;
    MemoryLayout layout;assert(layout.initialize(texture));
    assert(layout.dataSize()==2*stride && layout.size()==2*stride+16 && layout.alignment()==64);
    std::vector<uint8_t> memory(layout.size()+2*guard,0xa5),expected(memory);
    auto *base=memory.data()+guard;
    assert(layout.initializeMemory(base));
    std::fill(expected.begin()+guard,expected.end()-guard,0);
    assert(memory==expected);
    for(uint32_t slice=0;slice<2;++slice)for(uint32_t level=0;level<8;++level) {
        uint32_t width=std::max(1u,130u>>level),height=std::max(1u,65u>>level);
        CopyRegion r{width>2?1u:0u,height>3?2u:0u,0,width>2?width-2:width,height>3?height-3:height,1};
        uint64_t row=r.width*cpp,pitch=row+5,span=(r.height-1)*pitch+row,offset=7;
        std::vector<uint8_t> source(offset+span+19,0x69);
        for(uint32_t y=0;y<r.height;++y)for(uint32_t x=0;x<row;++x)
            source[offset+y*pitch+x]=uint8_t(17*format+31*slice+13*level+7*y+3*x);
        auto sourceBefore=source;
        assert(layout.copyToTexture(source.data(),offset,pitch,span,base,slice,level,r));
        for(uint32_t y=0;y<r.height;++y) {
            uint64_t at=guard+slice*stride+offsets[level]*cpp+(r.y+y)*pitches[level]*cpp+r.x*cpp;
            memcpy(expected.data()+at,source.data()+offset+y*pitch,row);
        }
        assert(memory==expected && source==sourceBefore);
        // Read to a different row stride and offset. All gaps remain sentinels.
        uint64_t readPitch=row+11,readSpan=(r.height-1)*readPitch+row;
        std::vector<uint8_t> result(readSpan+37,0xd3),want(result);
        assert(layout.copyFromTexture(base,slice,level,r,result.data(),9,readPitch,0));
        for(uint32_t y=0;y<r.height;++y)
            memcpy(want.data()+9+y*readPitch,source.data()+offset+y*pitch,row);
        assert(result==want && memory==expected);
    }
    assert(layout.finalizeMemory(base));
    const auto watermark=GPU::MetalTexture::LinearLayoutWatermark;
    assert(watermark.words[0]==0x51434f4d4c494e31ull && watermark.words[1]==0x413534304d495031ull);
    memcpy(expected.data()+guard+layout.dataSize(),&watermark,16);
    assert(memory==expected && layout.finalizeMemory(base) && memory==expected);
}

static void masksAndCubes()
{
    // Original shutdown masks are GA8 768x48 at2x. Runtime format selection is
    // separate; cover both alpha-only and gray/alpha data without color conversion.
    for(auto format:{R8,RG8,A8,RGBA8}) {
        uint32_t cpp=formatBytes(format);
        Texture t{1,0,768,48,768*cpp,format};
        t.swizzle=format==RG8?packSwizzle(ChannelRed,ChannelRed,ChannelRed,ChannelGreen):0;
        MemoryLayout layout;assert(layout.initialize(t));
        assert(layout.dataSize()==768*48*cpp);
        std::vector<uint8_t> data(layout.size(),0),source(768*48*cpp);
        for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*7+i/768);
        assert(layout.copyToTexture(source.data(),0,768*cpp,0,data.data(),0,0,{0,0,0,768,48,1}));
        assert(!memcmp(data.data(),source.data(),source.size()));
    }
    Texture cube{1,0,16,16,64,RG8,1,0,2,0,1};MemoryLayout layout;
    assert(layout.initialize(cube) && layout.dataSize()==12*4096);
    std::vector<uint8_t> bytes(layout.size(),0),row(32,0x7b);
    assert(layout.copyToTexture(row.data(),0,32,0,bytes.data(),11,0,{0,15,0,16,1,1}));
    for(size_t i=0;i<bytes.size();++i)
        assert(bytes[i]==(i>=11*4096+15*64 && i<11*4096+15*64+32?0x7b:0));
    assert(!layout.copyToTexture(row.data(),0,32,0,bytes.data(),12,0,{0,0,0,16,1,1}));
}

static void rejectionAndOverflow()
{
    MemoryLayout layout;Texture t{1,0,13,5,64,RGBA8};assert(layout.initialize(t));
    std::vector<uint8_t> memory(layout.size(),0xc7),source(512,0x53);auto before=memory;
    const CopyRegion region{1,1,0,7,3,1};
    auto reject=[&](uint64_t offset,uint64_t pitch,uint64_t image,uint64_t slice,uint64_t level,CopyRegion r) {
        assert(!layout.copyToTexture(source.data(),offset,pitch,image,memory.data(),slice,level,r));
        assert(memory==before);
        assert(!layout.copyFromTexture(memory.data(),slice,level,r,source.data(),offset,pitch,image));
        assert(memory==before && std::all_of(source.begin(),source.end(),[](auto c){return c==0x53;}));
    };
    reject(0,27,0,0,0,region);reject(0,32,91,0,0,region);
    reject(UINT64_MAX,32,0,0,0,region);reject(0,UINT64_MAX,0,0,0,region);
    reject(0,32,0,1,0,region);reject(0,32,0,0,1,region);
    for(auto r:{CopyRegion{UINT64_MAX,0,0,1,1,1},CopyRegion{0,0,0,UINT64_MAX,1,1},
        CopyRegion{0,5,0,1,1,1},CopyRegion{0,0,1,1,1,1},CopyRegion{0,0,0,1,1,2}})
        reject(0,32,0,0,0,r);
    assert(!layout.initializeMemory(nullptr) && !layout.finalizeMemory(nullptr));
    auto *bad=reinterpret_cast<void *>(UINTPTR_MAX-3);
    assert(!layout.initializeMemory(bad) && !layout.finalizeMemory(bad));
    assert(!layout.copyToTexture(bad,0,32,0,memory.data(),0,0,region) && memory==before);
    assert(!layout.copyFromTexture(memory.data(),0,0,region,bad,0,32,0) && memory==before);
    assert(!layout.copyToTexture(source.data(),0,32,0,bad,0,0,region));
    assert(!layout.copyToTexture(memory.data(),0,64,0,memory.data(),0,0,{0,1,0,13,4,1}) && memory==before);
    assert(layout.copyToTexture(memory.data(),0,64,0,memory.data(),0,0,{0,0,0,13,5,1}) && memory==before);
    assert(layout.copyToTexture(nullptr,UINT64_MAX,UINT64_MAX,0,nullptr,0,0,{13,5,0,0,0,1}));
    for(unsigned field=0;field<6;++field) {
        auto invalid=t;
        if(field==0)invalid.resource=0;
        if(field==1)invalid.offset=64;
        if(field==2)invalid.samples=2;
        if(field==3)invalid.format=Depth32;
        if(field==4)invalid.width=UINT32_MAX;
        if(field==5)invalid.pitch=UINT32_MAX;
        assert(!layout.initialize(invalid) && !layout.size() && !layout.dataSize() && !layout.alignment());
    }
}

static void compressedCopies()
{
    for(auto format:{ASTC4x4,ASTC4x4SRGB}) {
        Texture t{1,0,13,9,1024,format,4,0,2};TextureLayout checked;
        assert(textureLayout(t,checked) && checked.bytes==81920 && checked.layerStride==40960);
        const uint32_t offsets[]={0,3072,4096,5120};
        for(unsigned i=0;i<4;++i)assert(checked.level[i].offset==offsets[i] && checked.level[i].pitch==1024);
        MemoryLayout layout;assert(layout.initialize(t) && layout.copyGranularity()==4);
        std::vector<uint8_t> memory(layout.size()+128,0xa5),expected(memory),source(256,0x69);
        const CopyRegion edge{4,4,0,9,5,1};
        assert(layout.copyToTexture(source.data(),5,57,105,memory.data()+64,1,0,edge));
        for(unsigned y=0;y<2;++y)memcpy(expected.data()+64+40960+1024+y*1024+16,source.data()+5+y*57,48);
        assert(memory==expected);
        std::vector<uint8_t> result(256,0x7b),want(result);
        assert(layout.copyFromTexture(memory.data()+64,1,0,edge,result.data(),3,61,0));
        for(unsigned y=0;y<2;++y)memcpy(want.data()+3+y*61,source.data()+5+y*57,48);
        assert(result==want && memory==expected);
        for(auto invalid:{CopyRegion{1,4,0,8,4,1},CopyRegion{4,1,0,4,4,1},
            CopyRegion{4,4,0,5,4,1},CopyRegion{4,4,0,4,3,1}}) {
            assert(!layout.copyToTexture(source.data(),0,64,0,memory.data()+64,1,0,invalid));
            assert(memory==expected);
        }
        auto bad=t;bad.pitch=960;assert(!layout.initialize(bad));
        bad=t;bad.samples=2;assert(!layout.initialize(bad));
        Texture full{1,0,1080,1920,5120,format};
        assert(textureLayout(full,checked) && checked.bytes==2457600 && checked.accessBytes==2457600);
    }
}

int main()
{
    for(auto format:{RGBA8,BGRA8,RGBA8SRGB,BGRA8SRGB,R8,RG8,A8,RGBA16Float,RG32Float,RGBA16Unorm})
        mipArrayCopies(format);
    masksAndCubes();rejectionAndOverflow();compressedCopies();
    puts("Metal texture layout: independent mip/slice bytes, mask channels, guarded bidirectional copies, watermark and overflow checks passed");
}
