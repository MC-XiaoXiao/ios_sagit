// SPDX-License-Identifier: BSD-2-Clause
#include "AdrenoGMEM.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    GPU::RenderPass::Description pass;
    pass.registry=1;pass.width=1080;pass.height=1920;pass.colorMask=3;
    pass.colors[0].texture={1,0,1080,1920,4352,GPU::Shader::BGRA8};
    pass.colors[1].texture={2,0,1080,1920,8640,GPU::Shader::RGBA16Float};
    pass.colors[0].load=pass.colors[1].load=GPU::RenderPass::Clear;
    pass.colors[0].store=GPU::RenderPass::Store;
    Adreno::GMEMLayout tile;
    assert(Adreno::gmemLayout(pass,tile));
    assert(tile.width==128 && tile.height==672 && tile.columns==9 && tile.rows==3 && tile.bytes==1032192);
    pass.colors[1].store=GPU::RenderPass::Store;
    assert(!GPU::RenderPass::valid(pass)); // compact RGBA16F rows cannot use block transfers
    pass.colors[1].store=GPU::RenderPass::Discard;
    const uint64_t targets[]={0x11000000,0x12000000,0,0,0,0,0,0},scratch=0x10000000;
    const Adreno::IndirectBuffer children[]={{0x13000000,16,0},{0x13010000,16,0},{0x13020000,16,0}};
    std::vector<uint32_t> words(Adreno::IBDwords);
    Adreno::Encoder out(words.data(),words.size());Adreno::GMEMEncoder encoder(out,scratch,true);
    assert(encoder.encode(pass,tile,targets,children,3)==GPU::Result::Success);
    assert(out.barrier()==GPU::Result::Success);
    assert(out.size()<=GPU::RenderPass::rootWords(pass,tile,3));
    unsigned calls=0,origins=0,resets=0,progress=0,timestamps=0;
    for(unsigned at=0;at<out.size();) {
        const uint32_t header=words[at++];const bool packet=(header>>28)==7;
        const unsigned count=header&(packet?0x3fff:0x7f),op=(header>>16)&0x7f;
        assert(at+count<=out.size());
        if(packet && op==0x3f) {assert(count==3);++calls;}
        if(packet && op==0x3d) {
            const uint64_t address=uint64_t(words[at])|(uint64_t(words[at+1])<<32);
            if(address==scratch+Adreno::GMEMOrigin) {
                assert(count==18 && words[at+2]==(origins%tile.columns)*tile.width && words[at+3]==(origins/tile.columns)*tile.height);
                for(unsigned i=4;i<count;++i)assert(!words[at+i]);
                ++origins;
            } else {
                assert(address==scratch+Adreno::GMEMReplayCells && count==8);++resets;
            }
        }
        if(packet && op==0x3e) {
            assert(count==3 && words[at+1]==scratch+GPU::RenderPass::TimingOffset+timestamps*8);
            ++timestamps;
        }
        if(packet && op==0x46 && count==4 && words[at]==29) {
            assert(words[at+1]==scratch+16 && words[at+3]==++progress);
        }
        at+=count;
    }
    assert(calls==81 && origins==27 && resets==27 && progress==60 && encoder.sequence()==60 && timestamps==108);
    printf("Hello GMEM: %ux%u tiles, %u words, 81 immutable child calls, 60 monotonic flushes, 108 phase timestamps; origin and replay cells independent\n",
        tile.width,tile.height,out.size());
}
