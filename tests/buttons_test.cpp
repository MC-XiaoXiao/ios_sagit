// SPDX-License-Identifier: BSD-2-Clause
#include <ButtonState.h>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace QcomButton;
int main() {
    // Interpret HID short items independently of the button state machine.
    // Verify the actual descriptor exposes three variable bits and padding,
    // rather than an array that would lose simultaneous button presses.
    unsigned page=0,size=0,count=0,bits=0,depth=0; std::vector<unsigned> usages,controls;
    for (unsigned pos=0;pos<sizeof(ReportDescriptor);) {
        unsigned prefix=ReportDescriptor[pos++],length=prefix&3;
        if (length==3) length=4;
        assert(pos+length<=sizeof(ReportDescriptor)); unsigned value=0;
        for (unsigned i=0;i<length;++i) value|=unsigned(ReportDescriptor[pos++])<<(8*i);
        unsigned type=(prefix>>2)&3,tag=prefix>>4;
        if (type==1) {
            if (tag==0) page=value;
            if (tag==7) size=value;
            if (tag==9) count=value;
        } else if (type==2 && tag==0) usages.push_back(value);
        else if (type==0) {
            if (tag==10) { assert(page==12 && usages==std::vector<unsigned>{1} && value==1); ++depth; }
            if (tag==12) { assert(depth); --depth; }
            if (tag==8) {
                if (!(value&1)) {
                    assert(value==2 && page==12 && size==1 && count==usages.size());
                    controls.insert(controls.end(),usages.begin(),usages.end());
                }
                bits+=size*count;
            }
            usages.clear();
        }
    }
    assert(!depth && bits==8 && (controls==std::vector<unsigned>{0x30,0xe9,0xea}));
    State state; state.configure(0,0xe9,15); state.configure(1,0x30,15); state.configure(2,0xea,20);
    bool pending=false;
    assert(state.sample(0,0,pending)==0 && !pending);
    assert(state.sample(1,1000000,pending)==0 && pending);
    assert(state.sample(0,10000000,pending)==0 && !pending);
    assert(state.sample(1,12000000,pending)==0 && pending);
    assert(state.sample(1,26999999,pending)==0 && pending);
    assert(state.sample(1,27000000,pending)==2 && !pending);
    assert(state.sample(1,1000000000,pending)==2 && !pending); // hold has no repeats
    assert(state.sample(7,1001000000,pending)==2 && pending);
    assert(state.sample(7,1016000000,pending)==3 && pending);
    assert(state.sample(7,1021000000,pending)==7 && !pending);
    assert(state.sample(0,1100000000,pending)==7 && pending);
    assert(state.sample(0,1120000000,pending)==0 && !pending);
    state.reset();
    assert(state.sample(2,0,pending)==0 && pending); // held at startup
    assert(state.sample(2,15000000,pending)==1 && !pending);
    state.reset();
    for (unsigned i=0;i<200;++i) assert(state.sample(i&1,i*1000000,pending)==0);
    assert(state.sample(0,250000000,pending)==0 && !pending);
    puts("Buttons: per-key debounce, bounce suppression, boot-held press, simultaneous chords and release passed");
}
