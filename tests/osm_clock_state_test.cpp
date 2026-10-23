// SPDX-License-Identifier: GPL-2.0-only
#include "OSMClockState.h"
#include <assert.h>
#include <stdio.h>
using namespace QcomOSMClock;
int main() {
    State boot{{2,0x301,7,81,0,9,0x10000},true};
    assert(boot.localPLLHz()==1555200000);
    State perf=boot; perf.words[3]=90;
    assert(perf.localPLLHz()==1728000000);
    State early=boot; early.words[1]=0x500;
    assert(early.localPLLHz()==BootHz[0]);
    for (unsigned n=0;n<12;++n) {
        auto bad=boot;
        switch(n) {
        case 0: bad.stable=false; break;
        case 1: bad.words[0]|=1; break;
        case 2: bad.words[0]|=0x80000000; break;
        case 3: bad.words[1]=0x400; break; // Accepted desired state, still on aux.
        case 4: bad.words[2]=6; break;
        case 5: bad.words[6]=0; break;
        case 6: bad.words[4]=1; break;
        case 7: bad.words[5]|=0x100; break;
        case 8: bad.words[5]&=~1U; break;
        case 9: bad.words[1]=0x303; break;
        case 10: bad.words[5]|=0x1000; break;
        case 11: bad.words[5]|=0x1000000; break;
        }
        assert(!bad.localPLLHz());
    }
    unsigned calls=0,waited=0; bool stuck=false, torn=false;
    auto read=[&](uint32_t off) {
        unsigned i=0; while(i<7 && Offsets[i]!=off) ++i; assert(i<7);
        ++calls;
        if(i==1 && (stuck || waited<20)) return 0x400U;
        if(i==3 && torn && calls%14>7) return 90U;
        return boot.words[i];
    };
    auto delay=[&](unsigned us) { assert(us==5); waited+=us; };
    State last{};
    assert(awaitBootRate(0,read,delay,last) && waited==25);
    stuck=true; waited=0;
    assert(!awaitBootRate(0,read,delay,last) && waited==5000);
    stuck=false; torn=true; waited=0; calls=0;
    assert(!awaitBootRate(0,read,delay,last) && waited==5000);
    torn=false; waited=0; calls=0;
    assert(!awaitBootRate(1,read,delay,last) && waited==5000);
    assert(!awaitBootRate(2,read,delay,last));
    puts("OSM physical boot clock qualification: aux stall, torn reads, PLL lock and timeout passed");
}
