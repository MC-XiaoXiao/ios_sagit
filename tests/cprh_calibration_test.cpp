// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/clock/QcomOSM/CPRhCalibration.h"
#include <cassert>
#include <cstdio>
#include <cstring>

extern "C" int vendor_calibrate(unsigned,unsigned,unsigned,const unsigned*,const unsigned*,
    const unsigned*,const unsigned*,const int64_t*,const int64_t*,const int64_t*,
    const int64_t*,const int64_t*,const int64_t*,const int64_t*,const int64_t*,
    const int64_t*,const int64_t*,uint32_t*);
using namespace QcomCPRh;
static void set(uint64_t &row,unsigned bit,unsigned n,unsigned v) {
    uint64_t mask=((uint64_t(1)<<n)-1)<<bit;
    row=(row&~mask)|((uint64_t(v)<<bit)&mask);
}
static FuseRows pack(unsigned bin,unsigned rev,unsigned cluster,const DecodedFuses &f) {
    FuseRows r={}; set(r.row38,29,3,bin); set(r.row39,51,3,rev);
    set(r.row100,45,1,f.forceHighest);
    if (!cluster) {
        for(unsigned i=0;i<4;i++) {
            set(r.row67,12-4*i,4,f.ro[i]); set(r.row67,34-6*i,6,f.voltage[i]);
        }
        set(r.row68,18,12,f.target[0]);set(r.row68,6,12,f.target[1]);
        set(r.row67,58,6,f.target[2]);set(r.row68,0,6,f.target[2]>>6);
        set(r.row67,46,12,f.target[3]);
        set(r.row68,63,1,f.offset[1]);set(r.row69,0,6,f.offset[1]>>1);
        set(r.row68,56,7,f.offset[2]);set(r.row68,49,7,f.offset[3]);
    } else {
        for(unsigned i=0;i<4;i++) {
            set(r.row69,26-4*i,4,f.ro[i]);set(r.row69,48-6*i,6,f.voltage[i]);
        }
        set(r.row70,32,12,f.target[0]);set(r.row70,20,12,f.target[1]);
        set(r.row70,8,12,f.target[2]);set(r.row69,60,4,f.target[3]);
        set(r.row70,0,8,f.target[3]>>4);
        set(r.row71,13,3,f.offset[1]);set(r.row71,21,4,f.offset[1]>>3);
        set(r.row71,6,7,f.offset[2]);set(r.row70,63,1,f.offset[3]);
        set(r.row71,0,6,f.offset[3]>>1);
    }
    return r;
}
static uint32_t rng=0x89981234;
static unsigned next() { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static void compare(unsigned cluster,unsigned bin,unsigned rev,DecodedFuses f) {
    Identity id={292,0x20000,30}; Plan p;
    FuseRows rows=pack(bin,rev,cluster,f); DecodedFuses decoded;
    assert(decode(rows,cluster,decoded));
    assert(std::memcmp(f.ro,decoded.ro,sizeof(f.ro))==0);
    assert(std::memcmp(f.voltage,decoded.voltage,sizeof(f.voltage))==0);
    assert(std::memcmp(f.target,decoded.target,sizeof(f.target))==0);
    assert(std::memcmp(f.offset,decoded.offset,sizeof(f.offset))==0);
    if (!calibrate(id,rows,cluster,p)) {
        std::fprintf(stderr,"rejected cluster %u bin %u rev %u\n",cluster,bin,rev);
        assert(false);
    }
    const Data::Cluster &d=cluster ? Data::cluster1 : Data::cluster0;
    unsigned combo=bin*8+rev;
    auto a=[&](const Data::Array &x){return Detail::corners(x,d,combo);};
    auto t=[&](const Data::Array &x,unsigned n){return Detail::tuple(x,n,combo);};
    uint32_t expected[40*4]={};
    assert(vendor_calibrate(cluster,p.activeCount,bin,f.ro,f.voltage,f.target,f.offset,
        t(d.fmax,4),a(d.frequency),a(d.floor),a(d.ceiling),a(d.range),
        t(d.openFuse,4),a(d.openAdjust),t(d.closedFuse,4),a(d.closedAdjust),
        t(d.roScale,64),expected)==0);
    for(unsigned i=0;i<p.activeCount;i++) {
        unsigned j=f.forceHighest ? p.activeCount-1 : i;
        const Corner &c=p.corners[i];
        if(c.openLoopUV!=expected[4*j] || c.floorUV!=expected[4*j+1] ||
           c.ceilingUV!=expected[4*j+2] || c.targetQuotient!=expected[4*j+3]) {
            std::fprintf(stderr,"mismatch c%u b%u r%u corner%u: %u/%u %u/%u %u/%u %u/%u\n",
                cluster,bin,rev,i,c.openLoopUV,expected[4*j],c.floorUV,expected[4*j+1],
                c.ceilingUV,expected[4*j+2],c.targetQuotient,expected[4*j+3]);
            assert(false);
        }
        assert(((c.word&255)*4000+352000)==c.openLoopUV);
        assert((((c.word>>8)&255)*4000+352000)==c.floorUV);
        assert(((c.word>>25)&15)==c.ro);
        assert(!(c.word&(1u<<29)));
        assert(c.targetQuotient>=p.baseQuotient[c.ro]);
    }
    assert(p.count==p.activeCount+2);
    assert(p.corners[p.apmCrossoverCorner].word&(1u<<29));
    assert(p.corners[p.memAccCrossoverCorner].word&(1u<<29));
    for(unsigned i=0;i<40;i++) {
        const auto &o=p.osm[i];
        assert(o.voltage>>16<p.activeCount);
        assert((o.voltage&0xffff)*1000==p.corners[o.voltage>>16].ceilingUV);
        if(i>=p.osmCount) assert(std::memcmp(&o,&p.osm[p.osmCount-1],sizeof(o))==0);
    }
    id.chipVersion=0x20001; Plan p21;
    assert(calibrate(id,rows,cluster,p21));
    assert(std::memcmp(p.corners,p21.corners,sizeof(p.corners))==0);
}
int main() {
    unsigned tests=0;
    for(unsigned cluster=0;cluster<2;cluster++)
        for(unsigned bin=0;bin<4;bin++) for(unsigned rev=0;rev<8;rev++)
            for(unsigned sample=0;sample<48;sample++) {
                DecodedFuses f={};
                unsigned sameRO=next()%16;
                for(unsigned i=0;i<4;i++) {
                    f.ro[i]=(sample%3==0) ? sameRO : next()%16;
                    f.voltage[i]=next()%64; f.target[i]=1000+i*350+(next()%200);
                    f.offset[i]=i ? next()%128 : 0;
                }
                f.forceHighest=(sample%8==0);
                compare(cluster,bin,rev,f); tests++;
            }
    DecodedFuses f={};
    for(unsigned i=0;i<4;i++){f.ro[i]=i;f.target[i]=1000+300*i;}
    Plan p; Identity id={292,0x20000,30};
    FuseRows r=pack(0,0,0,f);
    assert(calibrate(id,r,0,p));
    id.chipVersion=0x10000; assert(!calibrate(id,r,0,p)); assert(p.count==0);
    id={292,0x20000,31}; assert(!calibrate(id,r,0,p));
    id={293,0x20000,30}; assert(!calibrate(id,r,0,p));
    id={292,0x20000,30};
    for(unsigned bin=4;bin<8;bin++) assert(!calibrate(id,pack(bin,0,0,f),0,p));
    assert(!calibrate(id,r,2,p)); assert(!calibrate(id,FuseRows{},0,p));
    FuseRows erased; std::memset(&erased,0xff,sizeof(erased));
    assert(!calibrate(id,erased,0,p));
    f.target[1]=1; f.offset[1]=127;
    assert(!calibrate(id,pack(0,0,0,f),0,p));
    std::printf("CPRh: %u vendor differential cases + identity/fuse boundary checks passed\n",tests);
}
