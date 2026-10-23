// SPDX-License-Identifier: BSD-2-Clause
#include <GPUJobActivity.h>
#include <cassert>
#include <memory>
#include <cstdio>
using namespace GPU::JobActivity;
int main() {
    // Exercise the production bounded ring across more than two rollovers.
    auto history=std::make_unique<History>();
    for(unsigned i=0;i<Capacity*3+17;++i) {
        Record r;r.sequence=i+1;r.prepareBegin=uint64_t(i)*100;r.admitted=r.prepareBegin+1;
        r.submitted=r.admitted+1;r.observed=r.submitted+1;r.retireBegin=r.observed+1;r.retireEnd=r.retireBegin+1;
        r.flags=Compositor|Successful|Admitted;r.writtenSurfaces[0]=i%4+1;r.writtenSurfaceCount=1;
        r.pid=32;r.indexBytes=i*4;r.encodingTicks=i*9;r.indexSnapshotTicks=i*3;r.addressSpaceTicks=i*2;
        history->append(r);
        assert(history->count==(i+1<Capacity?i+1:Capacity));
        assert(history->oldest(history->count-1).sequence==i+1);
    }
    for(unsigned i=0;i<history->count;++i) {
        const auto &r=history->oldest(i);
        assert(r.ordinal==history->total-history->count+i+1 && r.sequence==r.ordinal);
        assert(r.writtenSurfaces[0]==(r.ordinal-1)%4+1);
        assert(r.pid==32 && r.indexBytes==(r.ordinal-1)*4 && r.encodingTicks==(r.ordinal-1)*9);
        assert(r.indexSnapshotTicks==(r.ordinal-1)*3 && r.addressSpaceTicks==(r.ordinal-1)*2);
    }
    auto copy=std::make_unique<History>(*history);
    history->append({});assert(copy->total+1==history->total);
    history->total=UINT64_MAX;const auto next=history->next;
    history->append({});assert(history->next==next && history->total==UINT64_MAX);
    // Real native page rounding changes the bin; imports count factory calls
    // even when the same IOSurface backing would be reused by the Family.
    Resources resources;
    resources.sample(true,GPU::Native::Buffer,16384,10,30,true);
    resources.sample(true,GPU::Native::IOSurface,8388608,30,60,true);
    resources.sample(true,GPU::Native::IOSurface,8388608,60,90,true);
    resources.sample(false,GPU::Native::ClientBuffer,65536,100,130,false);
    assert(resources.compositor.bins[0][1].calls==1 && resources.compositor.bins[0][1].bytes==16384);
    assert(resources.compositor.bins[2][5].calls==2 && resources.compositor.bins[2][5].ticks==60);
    assert(resources.native.bins[1][2].failures==1 && resources.native.bins[1][2].bytes==0);
    assert(resources.compositor.bins[1][2].calls==0);
    resources.sample(true,0xff,1,1,2,true);
    resources.sample(true,GPU::Native::Buffer,1,2,1,true);
    assert(resources.native.invalid==2 && resources.compositor.invalid==2);
    const uint64_t boundary[]={4096,16384,65536,262144,1048576};
    for(unsigned i=0;i<5;++i){assert(sizeClass(boundary[i])==i);assert(sizeClass(boundary[i]+1)==i+1);}
    puts("GPU job activity: chronological ring rollover/copy/saturation and native type/rounded-size/failure accounting passed");
}
