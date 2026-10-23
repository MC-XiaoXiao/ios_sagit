// SPDX-License-Identifier: GPL-2.0-only
#include <TLMM.h>
#include <TLMMPinmux.h>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
struct Registers {
    std::map<uint32_t,uint32_t> values;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    uint32_t blocked=UINT32_MAX;
    uint32_t read32(uint32_t offset) { return values[offset]; }
    void write32(uint32_t offset,uint32_t value) { if(offset!=blocked)values[offset]=value; writes.emplace_back(offset,value); }
};
static void pinmux() {
    using namespace QcomTLMM;
    Registers regs;PinmuxLeases leases;
    int ownerObject=0,otherObject=0;
    Pinmux::Request r;r.owner=reinterpret_cast<IOService *>(&ownerObject);r.count=4;
    for(unsigned i=0;i<4;++i){r.pins[i]=45+i;r.configuration[i]=8;regs.values[PinBase[45+i]]=0xa5000000|i;}
    auto check=[](unsigned pin,unsigned function) {
        return pin>=45 && pin<=48 && function==2?MuxResult::Success:MuxResult::Denied;
    };
    auto interrupted=[](unsigned,unsigned){return MuxResult::Busy;};
    assert(leases.apply(r,regs,interrupted)==MuxResult::Busy && regs.writes.empty() && !r.token);
    assert(leases.apply(r,regs,check)==MuxResult::Success && r.token && leases.busy(45));
    const auto token=r.token;
    for(unsigned pin=45;pin<=48;++pin)assert(regs.values[PinBase[pin]]==0xa5000008);
    Pinmux::Request conflict=r;conflict.token=0;
    assert(leases.apply(conflict,regs,check)==MuxResult::Busy && !conflict.token);
    r.action=Pinmux::Action::Validate;
    r.owner=reinterpret_cast<IOService *>(&otherObject);
    assert(leases.apply(r,regs,check)==MuxResult::Denied && leases.busy(45));
    r.owner=reinterpret_cast<IOService *>(&ownerObject);
    assert(leases.apply(r,regs,check)==MuxResult::Success);
    r.action=Pinmux::Action::Release;regs.blocked=PinBase[47];
    assert(leases.apply(r,regs,check)==MuxResult::IOFailure && r.token==token);
    assert(leases.busy(45) && leases.busy(48)); // partial restoration retains the entire lease
    r.action=Pinmux::Action::Validate;
    assert(leases.apply(r,regs,check)==MuxResult::NotReady);
    r.action=Pinmux::Action::Release;regs.blocked=UINT32_MAX;
    assert(leases.apply(r,regs,check)==MuxResult::Success && !r.token && !leases.busy(45));
    for(unsigned i=0;i<4;++i)assert(regs.values[PinBase[45+i]]==(0xa5000000|i));
    r.action=Pinmux::Action::Acquire;regs.blocked=PinBase[46];
    assert(leases.apply(r,regs,check)==MuxResult::IOFailure && r.token>token && leases.busy(46));
    r.action=Pinmux::Action::Release;regs.blocked=UINT32_MAX;
    assert(leases.apply(r,regs,check)==MuxResult::Success && !leases.busy(46));
    r.action=Pinmux::Action::Acquire;r.pins[1]=r.pins[0];
    const auto writes=regs.writes.size();
    assert(leases.apply(r,regs,check)==MuxResult::Invalid && regs.writes.size()==writes);
}
int main() {
    pinmux();
    using namespace QcomTLMM;
    assert(PinCount==150 && PinBase[0]==0x900000 && PinBase[4]==0x104000);
    assert(PinBase[35]==0x523000 && PinBase[124]==0x97c000 && PinBase[149]==0x195000);
    Registers regs; Controller<Registers> gpio(regs);
    SummaryGuard guard;
    for (unsigned i=0;i<7;++i) assert(!guard.fault(false,false));
    assert(!guard.fault(true,false)); // a real interrupt resets the empty budget
    for (unsigned i=0;i<7;++i) assert(!guard.fault(false,false));
    assert(guard.fault(false,false));
    SummaryGuard foreign; assert(foreign.fault(false,true));
    regs.values[PinBase[0]+12]=1; regs.values[PinBase[0]+8]=0x71;
    assert(gpio.routedPending(0));
    regs.values[PinBase[0]+8]=0x70; assert(!gpio.routedPending(0)); // retained masked edge
    regs.values[PinBase[0]+8]=0x31; assert(!gpio.routedPending(0)); // another execution environment
    for (unsigned pin=0;pin<PinCount;++pin) {
        assert(PinBase[pin]+16<=RegionSize);
        for (unsigned other=0;other<pin;++other) assert(PinBase[other]!=PinBase[pin]);
        auto base=PinBase[pin]; regs.values[base]=0xabcdffff; regs.values[base+4]=1;
        regs.writes.clear();
        assert(gpio.configure(pin,3,7,true,true));
        assert(regs.writes.size()==2 && regs.writes[0].first==base+4 && regs.writes[0].second==3);
        assert(regs.values[base]==((0xabcdffff&~0x3ffU)|0x3c3));
        assert(gpio.configure(pin,1,0,false,false) && !(regs.values[base]&0x23c));
        bool high=false; assert(gpio.read(pin,high) && high);
        assert(gpio.set(pin,false) && regs.values[base+4]==1);
        for (unsigned trigger:{1U,2U,3U,4U,8U}) {
            regs.values[base+8]=0xa5a50000;
            assert(gpio.configureIRQ(pin,trigger));
            unsigned expected=0x70 | (trigger<4 ? (trigger<<2)|2 : trigger==4 ? 2 : 0);
            assert(regs.values[base+8]==(0xa5a50000|expected));
            assert(regs.values[base+12]==0);
            gpio.unmask(pin); assert(regs.values[base+8]&1);
            gpio.mask(pin,trigger); assert(!(regs.values[base+8]&1));
            assert(bool(regs.values[base+8]&16)==edge(trigger));
            regs.values[base+12]=1; assert(gpio.pending(pin));
            gpio.ack(pin); assert(!gpio.pending(pin) && regs.writes.back().second==0);
        }
    }
    auto writes=regs.writes.size(); bool value=true;
    assert(!gpio.read(150,value) && !gpio.set(150,1) && !gpio.configure(150,0,0,0,0));
    assert(!gpio.configure(0,4,0,0,0) && !gpio.configure(0,0,8,0,0));
    for (unsigned trigger:{0U,5U,7U,15U,~0U}) assert(!gpio.configureIRQ(0,trigger));
    assert(regs.writes.size()==writes);
    puts("GPIO: 150 pins, IRQ modes/masking, exclusive pinmux leases, failed writes and retryable restoration passed");
}
