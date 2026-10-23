// SPDX-License-Identifier: GPL-2.0-only
#include "GCCController.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <cstring>

struct MMIO {
    std::vector<uint32_t> words = std::vector<uint32_t>(0x94000/4);
    unsigned writes = 0, usec = 0, failUpdates = 0;
    bool failPower = false, failClock = false, keepVotedOn = false;
    MMIO() {
        words[0] = 0x80000001; words[4/4] = 31;
        words[0x38/4] = 0x4000; words[0xc/4] = 1; // GPLL0 = 600 MHz.
        for (const auto &b : GCC::branches) if (b.mode != 3) words[b.halt/4] |= 0x80000000U;
        for (const auto &d : GCC::domains) words[d.reg/4] = 1;
    }
    uint32_t &at(uint32_t reg) { assert(reg < 0x94000 && !(reg&3)); return words[reg/4]; }
    static uint32_t read(void *p, uint32_t reg) { return static_cast<MMIO *>(p)->at(reg); }
    static void write(void *p, uint32_t reg, uint32_t value) {
        auto &m = *static_cast<MMIO *>(p); ++m.writes;
        m.at(reg) = value;
        for (const auto &r : GCC::roots) if (reg == r.reg && (value & 1)) {
            if (m.failUpdates) --m.failUpdates;
            else m.at(reg) &= ~1U;
        }
        for (const auto &d : GCC::domains) if (reg == d.reg) {
            if (!(value & 1) && !m.failPower) m.at(reg) |= 0x80000000U;
            else if ((value & 1) && !m.keepVotedOn) m.at(reg) &= ~0x80000000U;
        }
        for (const auto &b : GCC::branches) if (reg == b.reg && b.mode != 3 && !m.failClock) {
            if (value & b.mask) m.at(b.halt) &= ~0x80000000U;
            else m.at(b.halt) |= 0x80000000U;
        }
    }
    static void delay(void *p, unsigned us) { static_cast<MMIO *>(p)->usec += us; }
    GCC::RegisterIO io() { return {this, read, write, delay}; }
};
static unsigned id(const char *name) {
    for (const auto &r : GCC::roots) if (!strcmp(name, r.name)) return r.id;
    for (const auto &b : GCC::branches) if (!strcmp(name, b.name)) return b.id;
    assert(false); return 0;
}
int main() {
    assert(GCC::sharedPLLEnabled(0xd0118000, 1)); // Live firmware FSM GPLL0.
    assert(!GCC::sharedPLLEnabled(0x90118000, 1)); // Lock without ACTIVE.
    assert(!GCC::sharedPLLEnabled(0xd0118000, 0)); // Main output disabled.
    assert(!GCC::sharedPLLEnabled(UINT32_MAX, 1));
    using namespace GCC;
    const auto axi = id("gcc_ufs_axi_clk"), src = id("ufs_axi_clk_src");
    const auto sibling = id("gcc_aggre1_ufs_axi_clk"), ahb = id("gcc_ufs_ahb_clk");
    {
        MMIO m; Controller c(m.io());
        assert(c.allowClock(36) && c.allowClock(171) && c.allowClock(172) &&
            c.allowClock(173) && c.allowClock(174) && c.allowClock(175) && c.allowReset(108));
        assert(c.initialize());
        m.at(0x52004) |= 1u << 17; // Existing BLSP vote shares the register.
        m.at(0x5200c) |= 1u << 3; // Existing GPU GPLL vote shares the register.
        for (unsigned clock : {36, 171, 172, 173, 174, 175}) {
            assert(c.clock(clock, true) == Success && c.clock(clock, false) == Success);
        }
        assert(m.at(0x52004) & (1u << 17));
        assert(m.at(0x5200c) & (1u << 3));
        assert(c.reset(108, true) == Success && (m.at(0x79000) & 1));
        assert(c.reset(108, false) == Success && !(m.at(0x79000) & 1));
        assert(c.clock(173, false) == BadArgument);
    }
    {
        MMIO m;Controller c(m.io());uint64_t token=0,other=0;
        for(auto clock:{36u,171u,172u,173u,174u,175u})assert(c.allowClock(clock));
        assert(c.allowReset(108) && c.initialize());
        const auto writes=m.writes;
        assert(c.retainModemState(token)==NotReady && !token && m.writes==writes);
        assert(c.reset(108,true)==Success && c.retainModemState(token)==Success && token);
        assert(c.modemStateHeld() && c.retainModemState(other)==Busy && !other);
        assert(c.validateModemState(token+1)==BadArgument && c.validateModemState(token)==Success);
        assert(c.clock(171,true)==Success && c.validateModemState(token,true)==Busy);
        assert(c.clock(171,false)==Success && c.reset(108,false)==Success);
        assert(c.validateModemState(token,true)==Busy && c.modemStateHeld());
        assert(c.reset(108,true)==Success && c.validateModemState(token,true)==Success && !c.modemStateHeld());
        assert(c.retainModemState(other)==Success && other>token);
        assert(c.validateModemState(token)==BadArgument);
        const auto pll=m.at(4);m.at(4)=32;
        assert(c.validateModemState(other)==IOFailure && c.modemStateHeld());
        m.at(4)=pll;assert(c.validateModemState(other,true)==Success);
        // A live software voter cannot be silently adopted into the lease.
        assert(c.clock(175,true)==Success && c.retainModemState(token=0)==Busy && !token);
        assert(c.clock(175,false)==Success);
        m.at(0)=0;
        assert(c.retainModemState(token)==NotReady && !token);
    }
    {
        MMIO m;m.at(0x79000)=1;Controller c(m.io());uint64_t token=0;
        assert(c.allowReset(108) && c.initialize());
        assert(c.retainModemState(token)==Unsupported && !token && !m.writes);
    }
    {
        MMIO m;m.at(0x1e004)=1;m.at(0x1e00c)=2; // inherited UART3 leaf and force-enable
        Controller c(m.io());
        assert(c.allowClock(37) && c.allowClock(53) && c.allowRate(14,63157895));
        assert(c.initialize());
        uint64_t token=0,other=0;
        assert(c.adoptUARTClock(53,token)==BadArgument && c.adoptUARTClock(52,token)==Unsupported);
        assert(c.clock(37,true)==Success && c.clock(53,true)==Success);
        assert(c.setRate(14,7372800)==Busy); // never weaken the live sibling guard
        assert(c.clock(53,true)==Success && c.adoptUARTClock(53,token)==Busy);
        assert(c.clock(53,false)==Success && c.adoptUARTClock(53,token)==Success && token);
        assert(c.adoptUARTClock(53,other)==Busy && !other);
        assert(!(m.at(0x1e00c)&2));
        assert(c.restoreUARTClock(53,token)==Busy && token);
        assert(c.clock(53,false)==Success && !(m.at(0x1e004)&1));
        for(auto hz:{3686400U,7372800U,51200000U}) {
            assert(c.setRate(14,hz)==Success && c.rate(14)==hz);
            assert(c.clock(53,true)==Success && c.setRate(14,7372800)==Busy);
            assert(c.clock(53,false)==Success);
        }
        other=token+1;assert(c.restoreUARTClock(53,other)==BadArgument);
        assert(c.restoreUARTClock(53,token)==Success && !token && !c.uartStateHeld());
        assert(m.at(0x1e010)==0 && (m.at(0x1e00c)&2) && (m.at(0x1e004)&1));
        assert(c.clock(37,false)==Success);
        const auto writes=m.writes;
        assert(c.restoreUARTClock(53,token)==BadArgument && m.writes==writes);
    }
    {
        MMIO m;Controller c(m.io());uint64_t token=0;
        assert(c.allowClock(53) && c.allowRate(14,51200000) && c.initialize());
        assert(c.clock(53,true)==Success && c.adoptUARTClock(53,token)==Success);
        assert(c.clock(53,false)==Success && c.setRate(14,7372800)==Success);
        m.failUpdates=1;
        assert(c.restoreUARTClock(53,token)==Timeout && token && c.uartStateHeld());
        assert(c.restoreUARTClock(53,token)==IOFailure && token); // keep an uncertain restore quarantined
    }
    {
        MMIO m;Controller c(m.io());
        for(const auto &b:branches)assert(c.allowClock(b.id));
        for(const auto &d:domains)assert(c.allowDomain(d.id));
        assert(c.initialize());uint64_t token=0;
        assert(c.retainGPUState(token)==NotReady && !token && !m.writes);
        m.at(0x71004)=m.at(0x71010)=m.at(0x46040)=1;
        assert(c.retainGPUState(token)==Success && token && !m.writes);
        assert(c.clock(77,false)==Busy && c.clock(185,true)==Busy);
        assert(c.clock(32,true)==Success && c.clock(32,false)==Success); // unrelated USB clocks remain usable
        assert(c.validateGPUState(token)==Success && c.validateGPUState(token,true)==Success);
        assert(c.clock(77,true)==Success && c.clock(77,false)==Success && (m.at(0x71004)&1));
        token=0;assert(c.retainGPUState(token)==Success);const auto writes=m.writes;
        m.at(0x71004)=0;assert(c.validateGPUState(token)==IOFailure);
        m.at(0x71004)=1;assert(c.validateGPUState(token,true)==IOFailure && c.clock(77,false)==Busy && m.writes==writes);
    }
    {
        MMIO m; Controller c(m.io());
        for (const auto &r : roots) assert(c.allowRate(r.id, 0xffffffffU));
        for (const auto &b : branches) assert(c.allowClock(b.id));
        for (const auto &d : domains) assert(c.allowDomain(d.id));
        c.initialize();
        // Exercise every advertised table entry, including fractional HID,
        // 8-bit SPI/SDCC M/N and 16-bit UART/PCIe M/N, against readback.
        for (const auto &r : roots) for (unsigned i = 0; i < r.count; ++i) {
            assert(c.setRate(r.id, r.rates[i].hz) == Success);
            uint64_t actual = c.rate(r.id), expected = r.rates[i].hz;
            uint64_t error = actual > expected ? actual - expected : expected - actual;
            assert(error <= expected / 10000 + 1);
        }
        // Sweep independently owned branches: shared vote-register bit updates
        // must not corrupt another descriptor or underflow domain references.
        for (const auto &b : branches) {
            assert(c.clock(b.id, true) == Success);
            assert(c.clock(b.id, false) == Success);
        }
    }
    {
        MMIO m; Controller c(m.io());
        assert(!c.allowClock(axi, 100000000)); // An RCG child must not claim a fixed parent.
        assert(c.clock(axi, true) == NotReady);
        assert(c.initialize() && !c.initialize());
        assert(c.clock(axi, true) == Unsupported);
        assert(c.setRate(src, 100000000) == Unsupported);
        assert(c.domain(1, true) == Unsupported && c.reset(17, true) == Unsupported);
        assert(!c.allowClock(axi) && c.domain(99, true) == BadArgument);
        assert(m.writes == 0);
    }
    {
        MMIO m; Controller c(m.io());
        assert(c.allowClock(axi) && c.allowClock(ahb) && c.allowDomain(1));
        assert(c.allowRate(src, 240000000));
        assert(!c.allowClock(axi) && !c.allowRate(src, 240000000) && !c.allowDomain(1));
        assert(!c.allowClock(9999) && !c.allowReset(9999));
        assert(c.initialize() && m.writes == 0);
        m.at(0x7501c) = 0x80000000U; // Unrelated config bit must survive.
        assert(c.setRate(src, 240000000) == Success);
        assert((m.at(0x7501c) & 0x80000000U) && c.rate(axi) == 240000000);
        assert(c.setRate(src, 250000000) == BadArgument);
        assert(c.setRate(src, 123456) == Unsupported);
        assert(c.clock(axi, true) == Success && c.domainState(1) == 1);
        assert(c.clock(axi, true) == Success && c.clock(ahb, true) == Success);
        assert(c.setRate(src, 100000000) == Busy);
        assert(c.clock(axi, false) == Success && (m.at(0x75008) & 1));
        assert(c.clock(axi, false) == Success && !(m.at(0x75008) & 1));
        assert(c.domainState(1) == 1); // AHB still owns power.
        assert(c.clock(ahb, false) == Success && c.domainState(1) == 0);
        assert(c.clock(axi, false) == BadArgument && c.domain(1, false) == BadArgument);
        // An unowned physical sibling blocks a shared root change.
        const Branch *sb = nullptr;
        for (const auto &b : branches) if (b.id == sibling) sb = &b;
        assert(sb); m.at(sb->reg) |= sb->mask;
        unsigned before = m.writes;
        assert(c.setRate(src, 100000000) == Busy && before == m.writes);
    }
    {
        MMIO m; Controller c(m.io()); c.allowRate(src, 240000000); c.initialize();
        m.at(4) = 40; // Firmware PLL no longer equals the table's 600 MHz.
        assert(c.setRate(src, 100000000) == NotReady && m.writes == 0);
        m.at(4) = 31; m.at(0) &= ~0x80000000U;
        assert(c.setRate(src, 100000000) == NotReady && m.writes == 0);
    }
    {
        MMIO m; Controller c(m.io());
        auto spi = id("blsp1_qup1_spi_apps_clk_src"); c.allowRate(spi, 50000000); c.initialize();
        assert(c.setRate(spi, 960000) == Success && c.rate(spi) == 960000);
        assert((m.at(0x19014) & 255) == 1);
        assert((m.at(0x19018) & 255) == 254);
        assert((m.at(0x1901c) & 255) == 253);
        assert(c.setRate(spi, 19200000) == Success && c.rate(spi) == 19200000);
    }
    {
        MMIO m; Controller c(m.io()); c.allowRate(src, 240000000); c.initialize();
        uint32_t old = m.at(0x7501c); m.failUpdates = 1;
        assert(c.setRate(src, 200000000) == Timeout && m.at(0x7501c) == old);
        assert(m.usec == 500 && c.setRate(src, 100000000) == Success);
        m.failUpdates = 2;
        assert(c.setRate(src, 200000000) == IOFailure && c.rate(src) == 0);
        unsigned before = m.writes;
        assert(c.setRate(src, 100000000) == IOFailure && before == m.writes);
    }
    {
        MMIO m; Controller c(m.io()); c.allowDomain(1); c.initialize(); m.failPower = true;
        assert(c.domain(1, true) == Timeout && (m.at(0x75004) & 1));
        assert(m.usec == 2000 && c.domain(1, false) == BadArgument);
        m.failPower = false; assert(c.domain(1, true) == Success);
        m.keepVotedOn = true;
        assert(c.domain(1, false) == Success && c.domainState(1) == 1);
        assert(m.at(0x75004) & 1); // Another voter keeps it on: no false timeout.
    }
    {
        MMIO m; m.at(0x75008) = 1; m.at(0x75004) = 0x80000000;
        Controller c(m.io()); c.allowClock(axi); c.allowDomain(1); c.initialize();
        assert(c.clock(axi, true) == Success && c.clock(axi, false) == Success);
        assert((m.at(0x75008)&1) && !(m.at(0x75004)&1)); // Firmware votes are pinned.
        assert(c.setRate(src, 100000000) == Unsupported);
    }
    {
        MMIO m; Controller c(m.io()); c.allowDomain(2); c.allowDomain(4); c.initialize();
        assert(c.domain(2, true) == Success && c.domain(2, false) == Success);
        assert(!(m.at(0xf004)&1)); // USB retention never collapses.
        assert(c.domain(4, true) == Success && c.domain(4, false) == Success);
        assert(!(m.at(0x7d038)&1)); // LPASS core is always on.
    }
    {
        MMIO m; Controller c(m.io()); c.allowClock(axi); c.allowDomain(1); c.initialize();
        m.failClock = true;
        assert(c.clock(axi, true) == Timeout && !(m.at(0x75008)&1));
        assert(m.at(0x75004)&1); // Failed clock start unwinds the power vote.
        m.failClock = false;
        assert(c.clock(axi, true) == Success && c.clock(axi, false) == Success);
    }
    {
        MMIO m; Controller c(m.io()); c.allowClock(axi); c.allowDomain(1); c.initialize();
        assert(c.clock(axi, true) == Success);
        m.failClock = true;
        assert(c.clock(axi, false) == Timeout);
        assert((m.at(0x75008)&1) && c.domainState(1) == 1);
        m.failClock = false;
        assert(c.clock(axi, false) == Success && c.domainState(1) == 0);
    }
    {
        MMIO m; Controller c(m.io()); c.allowReset(17); c.initialize();
        m.at(0x75000) = 0xa0;
        assert(c.reset(17, true) == Success && m.at(0x75000) == 0xa1);
        assert(c.reset(17, false) == Success && m.at(0x75000) == 0xa0);
        assert(c.reset(18, true) == Unsupported);
    }
    {
        MMIO m; const unsigned usb[]={32,71,116,117,118};
        for (auto id:usb) for (const auto &b:branches) if (b.id==id) m.at(b.reg)|=b.mask;
        Controller c(m.io());
        c.allowDomain(2); c.allowRate(163,150000000); c.allowRate(164,19200000);
        for (auto id:usb) c.allowClock(id,id==118 ? 32768 : 0);
        c.initialize();
        assert(c.adoptUSBClock(31)==Unsupported && c.adoptUSBClock(116)==BadArgument);
        for (auto id:usb) assert(c.clock(id,true)==Success);
        assert(c.clock(116,true)==Success && c.adoptUSBClock(116)==Busy);
        assert(c.clock(116,false)==Success);
        for (auto id:usb) assert(c.adoptUSBClock(id)==Success);
        for (auto id:usb) assert(c.clock(id,false)==Success);
        for (auto id:usb) for (const auto &b:branches) if (b.id==id) assert(!(m.at(b.reg)&b.mask));
        assert(c.setRate(163,120000000)==Success && c.rate(116)==120000000);
        assert(c.setRate(164,19200000)==Success);
    }
    {
        MMIO m; m.at(0xf000)=1; // cold core held in reset, firmware leaf still on
        m.at(0xf008)=1;
        Controller c(m.io()); c.allowClock(116); c.allowReset(18); c.allowRate(163,150000000);
        c.initialize();
        assert(c.resetState(18)==1 && c.resetState(17)==UINT32_MAX);
        assert(c.adoptUSBClock(116)==Success && !(m.at(0xf008)&1));
        assert(c.setRate(163,120000000)==Success);
    }
    {
        MMIO m;
        const unsigned leaves[]={119,120,166,170};
        for (auto id:leaves) for (const auto &b:branches) if (b.id==id) m.at(b.reg)|=b.mask;
        m.at(0x6a004)|=1; // shared AHB2PHY firmware pin
        Controller c(m.io());
        for (auto id:leaves) assert(c.allowClock(id,id==120 ? 125000000 : id==170 ? 19200000 : 0));
        assert(c.allowClock(121) && c.allowRate(165,1200000) && c.allowDomain(2));
        for (auto reset:{18u,69u,70u,106u}) assert(c.allowReset(reset));
        assert(c.initialize());
        auto writes=m.writes;
        assert(c.adoptUSBPHYClock(119,true)==BadArgument && m.writes==writes);
        assert(c.adoptUSBPHYClock(121,true)==Unsupported && c.adoptUSBPHYClock(119,false)==Unsupported);
        assert(c.reset(18,true)==Success && c.adoptUSBPHYClock(119,true)==BadArgument);
        assert(c.reset(69,true)==Success && c.reset(70,true)==Success && c.reset(106,true)==Success);
        assert(c.clock(120,true)==Success && c.clock(120,true)==Success);
        assert(c.adoptUSBPHYClock(120,true)==Busy);
        assert(c.clock(120,false)==Success && c.adoptUSBPHYClock(120,true)==Success);
        assert(c.clock(120,false)==Success && !(m.at(0x50004)&1));
        assert(c.adoptUSBPHYClock(119,true)==Success && c.adoptUSBPHYClock(166,true)==Success);
        assert(c.adoptUSBPHYClock(170,false)==Success);
        assert(!(m.at(0x50000)&1) && !(m.at(0x88008)&1) && !(m.at(0x88014)&1));
        assert(m.at(0x6a004)&1);
        assert(c.setRate(165,1200000)==Success && c.clock(119,true)==Success);
        assert(c.clock(119,false)==Success && !(m.at(0x50000)&1));
    }
    std::puts("GCC: ownership, shared votes, UART adoption/retuning/exact restoration, USB controller/PHY adoption and fault quarantine passed");
}
