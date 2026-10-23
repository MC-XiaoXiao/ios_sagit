// SPDX-License-Identifier: GPL-2.0-only
#include <PMI8998HapticsHardware.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace PMI8998Haptic;
constexpr uint64_t MS = 1000000;
struct Bus {
    std::array<uint8_t, 65536> regs{};
    struct Write { uint16_t address; uint8_t value; };
    std::vector<Write> writes;
    unsigned reads = 0, failRead = 0, failWrite = 0;
    bool offline = false, unlocked = false, stuckPlay = false, stuckEnable = false, failedWriteApplies = false;
    Bus() {
        regs[0x103] = 2; regs[0x104] = 0x51; regs[0x105] = 0x15;
        // Reserved bits exercise masked updates and high RATE_CFG2 preservation.
        for (auto a : {0xc046, 0xc070, 0xc04c, 0xc04d, 0xc04e, 0xc04f,
                       0xc051, 0xc052, 0xc053, 0xc055, 0xc056, 0xc058, 0xc048}) regs[a] = 0x40;
    }
    bool read(uint8_t sid, uint16_t a, uint8_t *p, unsigned n) {
        assert((sid == 2 && a == 0x103 && n == 3) || (sid == 3 && a >= 0xc000 && a <= 0xc0ff && n == 1));
        if (offline || ++reads == failRead) return false;
        for (unsigned i = 0; i < n; ++i) p[i] = regs[a+i];
        return true;
    }
    bool write(uint8_t sid, uint16_t a, uint8_t *p, unsigned n) {
        assert(sid == 3 && a >= 0xc000 && a <= 0xc0ff && n == 1);
        const bool authorized = a == 0xc0d0 || a == 0xc0e3 || a == 0xc046 || a == 0xc070 ||
            a == 0xc04c || a == 0xc04d || a == 0xc04e || a == 0xc04f ||
            (a >= 0xc051 && a <= 0xc056) || a == 0xc058 || a == 0xc048 || a == 0xc05c;
        assert(authorized); // No charger, rail, MISC, SRAM, IRQ, or SC-clear writes.
        writes.push_back({a, *p});
        const bool failed = writes.size() == failWrite;
        if (offline || (failed && !failedWriteApplies)) return false;
        if (a == 0xc0d0) { assert(*p == 0xa5); unlocked = true; }
        else if (a == 0xc0e3) { assert(unlocked && writes[writes.size()-2].address == 0xc0d0); unlocked = false; }
        else unlocked = false;
        if ((a == 0xc070 && stuckPlay) || (a == 0xc046 && stuckEnable)) return true;
        regs[a] = *p;
        return !failed;
    }
    bool off() const { return !(regs[0xc046] & 0x80); }
};
static Config config() { Config c; c.flags = 1; return c; }
static void ready(Hardware &h, Bus &b) { assert(h.initialize(b, config(), 100*MS) == Result::Success); }
static void basic() {
    Hardware h; Bus b; ready(h,b);
    assert(h.currentState() == State::Idle && h.knownOff());
    assert(b.regs[0xc054] == 0xe8 && b.regs[0xc055] == 0x43);
    assert(b.regs[0xc04f] == 0x24 && b.regs[0xc04e] == 0x40);
    assert(b.regs[0xc051] == 0x78 && b.regs[0xc052] == 0x41);
    assert(b.regs[0xc053] == 0x41 && b.regs[0xc056] == 0x41 && b.regs[0xc058] == 0x41);
    assert(b.regs[0xc048] == 0x41 && b.regs[0xc05c] == 0x0b);
    assert(h.play(b, 100*MS, 100, 500) == Result::Success);
    assert(!b.off() && h.appliedMV() == 1624 && h.remainingMS(150*MS) == 50);
    assert(h.play(b, 101*MS, 5, 1000) == Result::Busy);
    assert(h.advance(b, 119*MS) == Result::Success && !(b.regs[0xc0e3] & 0x80));
    assert(h.advance(b, 120*MS) == Result::Success && (b.regs[0xc0e3] & 0x80));
    assert(h.advance(b, 200*MS) == Result::Success && h.currentState() == State::Braking);
    assert(!(b.regs[0xc070] & 0x80) && !h.knownOff());
    assert(h.advance(b, 224*MS) == Result::Success && !b.off());
    assert(h.advance(b, 225*MS) == Result::Success && b.off() && h.knownOff());
    assert(h.play(b, 226*MS, 5, 1) == Result::Success && h.appliedMV() == 116);
    assert(h.advance(b, 240*MS) == Result::Success && !(b.regs[0xc0e3] & 0x80));
    assert(h.advance(b, 265*MS) == Result::Success && b.off());
    assert(h.play(b, 300*MS, 15000, 1000) == Result::Success && h.appliedMV() == 3248);
    assert(h.quiesce(b, true) == Result::Success && h.currentState() == State::Suspended && b.off());
    assert(h.play(b, 301*MS, 5, 1000) == Result::NotReady);
    assert(h.initialize(b, config(), 302*MS) == Result::Success);
    assert(h.play(b, 303*MS, 10, 1000) == Result::Success);
    assert(h.stop(b, 304*MS) == Result::Success && h.currentState() == State::Braking);
    assert(h.quiesce(b, false) == Result::Success && h.currentState() == State::Idle && b.off());
}
static void invalidAndOwnership() {
    for (unsigned field = 0; field < 8; ++field) {
        auto c = config();
        switch (field) {
        case 0:c.version=2;break;case 1:c.flags=2;break;case 2:c.sid=2;break;
        case 3:c.base=0xd800;break;case 4:c.periodUS=6667;break;
        case 5:c.maximumMV=3596;break;case 6:c.currentMA=1000;break;case 7:c.maximumMS=15001;break;
        }
        Hardware h;Bus b;assert(h.initialize(b,c,0)==Result::Invalid && !b.reads && b.writes.empty());
    }
    Hardware disabled;Bus bus;Config c;
    assert(disabled.initialize(bus,c,0)==Result::Invalid && bus.writes.empty() && !bus.reads);
    for (auto address : {0x103,0x104,0x105}) {
        Hardware h;Bus b;b.regs[address]=0;
        assert(h.initialize(b,config(),0)==Result::WrongChip && b.writes.empty());
    }
    for (auto address : {0xc046,0xc070,0xc00a}) {
        Hardware h;Bus b;b.regs[address]=address==0xc00a?2:0x80;
        assert(h.initialize(b,config(),0)==Result::Busy && b.writes.empty());
    }
    Hardware h;Bus b;ready(h,b);const auto writes=b.writes.size();
    for (auto d : {0u,4u,15001u,UINT32_MAX}) assert(h.play(b,100*MS,d,1000)==Result::Invalid);
    for (auto s : {0u,1001u,UINT32_MAX}) assert(h.play(b,100*MS,10,s)==Result::Invalid);
    assert(h.play(b,99*MS,10,1000)==Result::Invalid);
    assert(h.play(b,UINT64_MAX,10,1000)==Result::Invalid);
    assert(b.writes.size()==writes);
    c=config();c.maximumMV=2320;c.currentMA=400;c.maximumMS=50;
    assert(h.initialize(b,c,100*MS)==Result::Success);
    assert(!(b.regs[0xc052]&1));
    assert(h.play(b,100*MS,51,1000)==Result::Invalid);
    assert(h.play(b,100*MS,50,1000)==Result::Success && h.appliedMV()==2320);
}
static void faults() {
    for (auto fault : {uint8_t(8),uint8_t(0x10)}) {
        Hardware h;Bus b;ready(h,b);assert(h.play(b,100*MS,100,1000)==Result::Success);
        assert(h.advance(b,120*MS)==Result::Success);
        b.regs[0xc00a]=fault;
        assert(h.advance(b,125*MS)==(fault==8?Result::ShortCircuit:Result::ResonanceError));
        assert(h.currentState()==State::Fault && b.off() && h.knownOff());
        b.regs[0xc00a]=0;
        assert(h.play(b,130*MS,5,1000)==Result::Faulted);
        assert(h.initialize(b,config(),130*MS)==Result::Faulted);
        h.quiesce(b,false);assert(h.currentState()==State::Fault);
    }
    Hardware h;Bus b;ready(h,b);assert(h.play(b,100*MS,100,1000)==Result::Success);
    assert(h.advance(b,99*MS)==Result::Invalid && b.off());
    Hardware t;Bus bt;ready(t,bt);assert(t.play(bt,100*MS,100,1000)==Result::Success);
    assert(t.timerFailure(bt)==Result::IOFailure && bt.off());
    Hardware u;Bus bu;ready(u,bu);assert(u.play(bu,100*MS,100,1000)==Result::Success);
    bu.offline=true;u.advance(bu,101*MS);
    assert(u.currentState()==State::Fault && !u.knownOff());
    bu.offline=false;u.quiesce(bu,false);assert(u.currentState()==State::Fault && bu.off() && u.knownOff());
    Hardware sticky;Bus bs;ready(sticky,bs);bs.stuckPlay=true;
    assert(sticky.play(bs,100*MS,100,1000)==Result::IOFailure && bs.off());
}
static void failedTransactions() {
    Hardware sample;Bus reference;ready(sample,reference);const auto count=reference.writes.size();
    // Fail every initialization write, including the secure unlock/TEST2 pair.
    for (bool uncertain : {false,true}) for (size_t i=1;i<=count;++i) {
        Hardware h;Bus b;b.failWrite=unsigned(i);
        b.failedWriteApplies=uncertain;
        assert(h.initialize(b,config(),100*MS)==Result::IOFailure);
        assert(h.currentState()==State::Fault && b.off());
    }
    for (unsigned i=1;i<=reference.reads;++i) {
        Hardware h;Bus b;b.failRead=i;
        assert(h.initialize(b,config(),0)==Result::IOFailure);
        if (i<=4) assert(b.writes.empty());
        else assert(h.currentState()==State::Fault && b.off());
    }
    Hardware hp;Bus bp;ready(hp,bp);const auto initial=bp.writes.size();
    hp.play(bp,100*MS,100,1000);const auto playWrites=bp.writes.size()-initial;
    for (bool uncertain : {false,true}) for (size_t i=1;i<=playWrites;++i) {
        Hardware h;Bus b;ready(h,b);b.failWrite=unsigned(b.writes.size()+i);
        b.failedWriteApplies=uncertain;
        assert(h.play(b,100*MS,100,1000)==Result::IOFailure);
        assert(h.currentState()==State::Fault && b.off());
    }
    // Loss of PLAY clear still attempts MODULE_EN; both a write error and a
    // successful transport with failed readback are covered.
    for (bool stuck : {false,true}) {
        Hardware h;Bus b;ready(h,b);h.play(b,100*MS,100,1000);
        b.failWrite=stuck?0:unsigned(b.writes.size()+1);b.stuckPlay=stuck;
        assert(h.quiesce(b,false)==Result::IOFailure);
        assert(h.currentState()==State::Fault && b.off());
    }
    Hardware late;Bus b;ready(late,b);late.play(b,100*MS,30,1000);
    late.advance(b,200*MS); // late tick must clear PLAY, never enable auto-res
    assert(!(b.regs[0xc070]&0x80) && !(b.regs[0xc0e3]&0x80));
    Hardware stuck;Bus bs;ready(stuck,bs);stuck.play(bs,100*MS,100,1000);
    bs.stuckEnable=true;
    assert(stuck.quiesce(bs,false)==Result::IOFailure && !stuck.knownOff() && !bs.off());
    bs.stuckEnable=false;stuck.quiesce(bs,false);
    assert(stuck.knownOff() && bs.off() && stuck.currentState()==State::Fault);
}
static void protocol() {
    uint64_t p[]={5,1000};
    assert(Haptics::validCall(Haptics::GetInfo,0,Haptics::InfoScalars,nullptr));
    assert(Haptics::validCall(Haptics::GetStatus,0,Haptics::StatusScalars,nullptr));
    assert(Haptics::validCall(Haptics::Play,2,0,p));
    assert(!Haptics::validCall(Haptics::Play,2,0,nullptr));
    for (unsigned inputs=0;inputs<4;++inputs) for(unsigned outputs=0;outputs<14;++outputs) {
        if(inputs!=2 || outputs!=0)assert(!Haptics::validCall(Haptics::Play,inputs,outputs,p));
    }
    p[0]=UINT64_MAX;assert(!Haptics::validCall(Haptics::Play,2,0,p));
    p[0]=5;p[1]=UINT64_MAX;assert(!Haptics::validCall(Haptics::Play,2,0,p));
    assert(!Haptics::validCall(Haptics::SelectorCount,0,0,nullptr));
}
int main() {
    basic();invalidAndOwnership();faults();failedTransactions();protocol();
    puts("PMI8998 haptics: configuration, ownership, QWD timing, braking, deadlines, fault latching, I/O failure injection and diagnostic ABI passed");
}
