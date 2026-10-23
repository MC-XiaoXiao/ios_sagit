// SPDX-License-Identifier: GPL-2.0-only
#include <PMICInputs.h>
#include <SPMIArbiter.h>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
using namespace QcomPMICInput;
struct Bus {
    std::map<unsigned,uint8_t> regs;
    unsigned calls=0,fail=0;
    std::vector<unsigned> writes;
    bool read(unsigned sid,unsigned address,uint8_t *data,unsigned count) {
        if (++calls==fail) return false;
        for (unsigned i=0;i<count;++i) data[i]=regs[(sid<<16)+address+i];
        return true;
    }
    bool write(unsigned sid,unsigned address,const uint8_t *data,unsigned count) {
        if (++calls==fail) return false;
        for (unsigned i=0;i<count;++i) { regs[(sid<<16)+address+i]=data[i]; writes.push_back(address+i); }
        return true;
    }
};
struct MMIO {
    std::map<uint64_t,uint32_t> regs;
    static uint32_t read(void *ctx,unsigned bank,uint32_t off) { return static_cast<MMIO *>(ctx)->regs[(uint64_t(bank)<<32)|off]; }
    static void write(void *ctx,unsigned bank,uint32_t off,uint32_t value) { static_cast<MMIO *>(ctx)->regs[(uint64_t(bank)<<32)|off]=value; }
    static void wait(void *,unsigned) {}
    static void kick(void *) {}
};
int main() {
    Bus bus; Hardware<Bus> hw(bus);
    bus.regs[0x815]=3;
    assert(hw.exclusive({0,8,0,3},3) && !hw.exclusive({0,8,0,3},1));
    for (unsigned trigger:{1U,2U,3U,4U,8U}) {
        bus.regs[0x811]=0x80; bus.regs[0x812]=0x80; bus.regs[0x813]=0x80;
        IRQ irq{0,8,1,trigger}; assert(hw.configureIRQ(irq));
        assert(bus.regs[0x816]==2 && bus.regs[0x814]==2);
        assert(bus.regs[0x811]==(0x80 | ((trigger&3) ? 2 : 0)));
        assert(bus.regs[0x812]==(0x80 | ((trigger&1 || trigger==4) ? 2 : 0)));
        assert(bus.regs[0x813]==(0x80 | ((trigger&2 || trigger==8) ? 2 : 0)));
        assert(hw.unmask(irq) && bus.regs[0x815]==2);
    }
    for (IRQ bad: {IRQ{16,8,0,3},IRQ{0,256,0,3},IRQ{0,8,8,3},IRQ{0,8,0,5}}) {
        auto calls=bus.calls; assert(!hw.configureIRQ(bad) && calls==bus.calls);
    }
    Button power{0x30,0,0,0,15}, up{0xe9,1,0,6,15}, down{0xea,0,0,1,15};
    assert(interrupt(up).peripheral==0xc5 && interrupt(down).bit==1);
    bus.regs[0x805]=4; bus.regs[0x870]=0x80; bus.regs[0x871]=0x9f; bus.regs[0x85a]=7;
    assert(hw.prepareButton(power) && hw.prepareButton(down));
    assert(bus.regs[0x870]==0x83 && bus.regs[0x871]==0x9f && bus.regs[0x85a]==7);
    bool pressed=false; bus.regs[0x810]=3;
    assert(hw.readButton(power,pressed) && pressed && hw.readButton(down,pressed) && pressed);
    bus.regs[0x810]=0; assert(hw.readButton(power,pressed) && !pressed);
    for (unsigned subtype:{1U,5U,9U,13U,0x10U,0x11U,0x12U,0x13U}) {
        bus.regs[0xc504]=0x10; bus.regs[0xc505]=subtype; bus.regs[0xc541]=2;
        assert(hw.prepareButton(up));
        assert(bus.regs[0xc546]&0x80); assert(bus.regs[0xc543]==0 && bus.regs[0xc541]==2);
        bus.regs[0xc510]=0; assert(hw.readButton(up,pressed) && pressed);
        bus.regs[0xc510]=1; assert(hw.readButton(up,pressed) && !pressed);
        assert(hw.configureGPIO(0,6,true,true,5));
        bool value=false; assert(hw.readGPIO(0,6,value) && value);
        assert(hw.configureGPIO(0,6,true,false,5) && hw.readGPIO(0,6,value) && !value);
    }
    bus.regs[0xc505]=0xff; assert(!hw.prepareButton(up));
    for (unsigned fail=1;fail<=4;++fail) {
        Bus bad; bad.fail=fail; Hardware<Bus> broken(bad);
        assert(!broken.configureIRQ({0,8,0,3}));
    }
    MMIO mmio;
    mmio.regs[0]=0x30000000;
    mmio.regs[0x800+4*7]=0x00800; // SID0/PON8 -> APID7
    mmio.regs[(4ULL<<32)+0x700+4*7]=0;
    mmio.regs[0x800+4*8]=0x0c500; // SID0/GPIO6 -> APID8, another owner
    mmio.regs[(4ULL<<32)+0x700+4*8]=1;
    QcomPower::SPMIArbiter arb;
    assert(arb.initialize({&mmio,MMIO::read,MMIO::write,MMIO::wait,MMIO::kick},0)==QcomPower::OK);
    uint16_t apid=0;
    assert(arb.irqChannel(0,8,apid) && apid==7);
    assert(!arb.irqChannel(0,0xc5,apid) && !arb.irqChannel(16,8,apid));
    arb.irqAccess(7,true); assert(mmio.regs[(3ULL<<32)+0x7000]==1);
    mmio.regs[(3ULL<<32)+0x7004]=0x103; assert(arb.irqStatus(7)==3);
    arb.irqClear(7,2); assert(mmio.regs[(3ULL<<32)+0x7008]==2);
    puts("PMIC: native PON polarity, GPIO6 active-low/LV/MV/legacy, IRQ RMW, fail injection and APID ownership passed");
}
