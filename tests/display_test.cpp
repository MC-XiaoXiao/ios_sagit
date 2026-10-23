/* SPDX-License-Identifier: BSD-2-Clause */
#include "DisplayProtocol.h"
#include "../drivers/display/QcomDisplay/DisplaySMMU.h"
#include "../drivers/display/QcomDisplay/DisplayDMAHalt.h"
#include "../drivers/display/QcomDisplay/DisplayCommandTransfer.h"
#include "../drivers/display/QcomDisplay/DisplayPrepareSlot.h"
#include "DisplayTimeline.h"
#include "../drivers/display/QcomDisplay/DisplayIdleMappings.h"
#include "../drivers/display/QcomDisplay/DisplayInterruptDispatch.h"
#include "PMI8998WLEDHardware.h"
#include <assert.h>
#include <array>
#include <map>
#include <vector>
struct Registers {
    std::array<uint32_t, 32> values{};
    uint32_t cleared = 0;
    uint32_t read(uint32_t r) { return values[r / 4]; }
    void write(uint32_t r, uint32_t v) {
        if (r == Display::InterruptClear) {
            cleared |= v; values[Display::InterruptStatus / 4] &= ~v;
        } else values[r / 4] = v;
    }
};
struct MDPRegisters {
    std::map<uint32_t, uint32_t> values;
    uint32_t read(uint32_t r) { return values[r]; }
    void write(uint32_t r, uint32_t v) { values[r] = v; }
};
static void testInterruptTiming() {
    constexpr uint32_t vbl=1u<<12,done=1u<<8;
    constexpr uint64_t epoch=7000000000ULL,period=16666667;
    uint64_t now=epoch,last=epoch-period,count=0;
    unsigned completed=0,notified=0,active=1;
    std::vector<unsigned> order;
    // Alternating cold/warm retirement costs must not alternate the VBL
    // interval. The native completion callback can also admit the next swap.
    for(uint64_t delay:{9000000ULL,0ULL,3500000ULL,0ULL}) {
        order.clear();const auto observedAt=now;
        Display::dispatchInterrupt(vbl|done,vbl,done,[&]() {
            order.push_back(1);
            Display::VBLObservation timing{++count,now,now-last};last=now;return timing;
        },[&]() {
            order.push_back(2);++completed;now+=delay;
            ++active; // Reentrant native completion starts another request.
        },[&]() {order.push_back(4);},[&](const Display::VBLObservation &timing) {
            order.push_back(3);++notified;
            assert(timing.count==count && timing.time==observedAt && timing.delta==period);
            assert(completed==notified && active==completed+1);
        });
        assert((order==std::vector<unsigned>{1,2,4,3}));
        now=observedAt+period;
    }
    // A completion-only IRQ never invents a VBL. A VBL-only IRQ never retires
    // the active transfer (including one whose latch/read is still pending).
    const auto beforeCount=count;const auto beforeComplete=completed;
    Display::dispatchInterrupt(done,vbl,done,[&]() -> Display::VBLObservation {
        assert(false);return {};
    },[&](){++completed;},[](){},[&](const auto &){assert(false);});
    assert(count==beforeCount && completed==beforeComplete+1);
    Display::dispatchInterrupt(vbl,vbl,done,[&]() {
        return Display::VBLObservation{++count,now,period};
    },[&](){assert(false);},[](){assert(false);},[&](const auto &timing){assert(timing.time==now && timing.delta==period);});
    for(auto status:{0u,1u<<4,done}) {
        // Video mode has no command-done bit. Unowned and disabled sources
        // must not invoke either completion or VBL callbacks.
        Display::dispatchInterrupt(status,vbl,0,[&]() -> Display::VBLObservation {
            assert(false);return {};
        },[&](){assert(false);},[](){assert(false);},[&](const auto &){assert(false);});
    }
}
static void testCommandQueueDrain() {
    constexpr uint32_t vbl=1u<<12,done=1u<<8;
    // Replay completion before TE: native completion may leave a ready head,
    // reenter and start it itself, or reject an undrained old transfer.
    for(bool combined:{false,true})for(unsigned outcome=0;outcome<3;++outcome) {
        unsigned active=1,queued=2,retired=0,starts=0,vbls=0;
        bool idle=false;std::vector<unsigned> order;
        Display::dispatchInterrupt(done|(combined?vbl:0),vbl,done,[&]() {
            order.push_back(1);return Display::VBLObservation{7,1000,100};
        },[&]() {
            order.push_back(2);
            if(outcome==2)return; // Old DMA still reading: no retirement.
            assert(active==1);retired=active;active=0;idle=true;
            if(outcome==1){active=queued;queued=0;idle=false;++starts;}
        },[&]() {
            order.push_back(3);
            if(idle && !active && queued) {
                assert(retired==1);active=queued;queued=0;idle=false;++starts;
            }
        },[&](const Display::VBLObservation &timing) {
            order.push_back(4);++vbls;assert(timing.count==7 && timing.time==1000);
            assert(active==(outcome==2?1u:2u));
        });
        assert(starts==(outcome==2?0u:1u) && vbls==unsigned(combined));
        assert(retired==(outcome==2?0u:1u)); // Captured old IRQ never retires #2.
        assert((order==(combined?std::vector<unsigned>{1,2,3,4}:std::vector<unsigned>{2,3})));
    }
}
static void testCommandTransfer() {
    constexpr uint32_t pp=0x70000;
    struct CommandRegisters : MDPRegisters {
        std::vector<std::pair<uint32_t,uint32_t>> writes;
        bool ignoreStop=false;
        void write(uint32_t reg,uint32_t value) {
            writes.push_back({reg,value});
            if (!ignoreStop || reg!=pp+0x30) values[reg]=value;
        }
    };
    // Actual firmware starts with auto-refresh and synthetic TE, tearcheck=0.
    // A delayed transfer must drain before tearcheck is enabled. No source,
    // trigger, interrupt, period, threshold or external TE source may be changed.
    for (uint32_t external : {0u,1u<<20}) {
        CommandRegisters r;r.values[pp+4]=0x800a6|external;
        r.values[pp+8]=1927;r.values[pp+0x18]=0x50004;r.values[pp+0x1c]=5;r.values[pp+0x20]=1921;
        r.values[pp+0x30]=0x80000001;r.values[pp+0x2c]=700;
        r.values[0x101c]=1;
        Display::CommandTransfer<CommandRegisters> transfer(r,0,1920);
        assert(!transfer.idle());
        unsigned delays=0;
        assert(transfer.adopt([&](unsigned us) {
            assert(us==100);++delays;
            assert(r.values[pp]==0 && !r.values[pp+0x30] && !(r.values[pp+4]&(1u<<20)));
            if(delays==3) {r.values[pp+0x2c]=1920;r.values[0x101c]=0;}
        }));
        assert(delays==3 && r.values[pp]==1 && r.values[pp+4]==(0x800a6|external));
        assert(r.writes.size()==9 && r.writes[0].first==pp+4 && r.writes[1].first==pp+0x30);
        assert(r.values[pp+0x10]==1920 && r.values[pp+0x1c]==1920 && r.values[pp+0xc]==1925 && r.values[pp+0x20]==1921);
        assert(r.values[pp+8]==1927 && r.values[pp+0x18]==0x50004);
        assert(r.writes[3]==std::make_pair(pp,0u) && r.writes.back()==std::make_pair(pp,1u));
        assert(transfer.idle());
        r.values[0x101c]=1;assert(!transfer.idle());
        r.values[0x101c]=0;r.values[pp+0x2c]=1;assert(!transfer.idle());
        r.values[pp+0x2c]=0;assert(transfer.idle());
        r.values[pp+0x30]=0x80000001;assert(!transfer.idle());
    }
    for (bool ignored : {false,true}) {
        CommandRegisters r;r.values[pp+4]=0x1800a6;r.values[pp+0x30]=0x80000001;
        r.values[pp+0x2c]=500;r.ignoreStop=ignored;
        Display::CommandTransfer<CommandRegisters> transfer(r,0,1920);
        unsigned elapsed=0;
        assert(!transfer.adopt([&](unsigned us){elapsed+=us;}));
        assert(elapsed==100000 && !r.values[pp] && r.values[pp+4]==0x1800a6);
        assert(!transfer.idle()); // Neither a timeout nor ignored MMIO proves DMA stopped.
    }
    CommandRegisters r;Display::CommandTransfer<CommandRegisters> transfer(r,0,1920);
    assert(!transfer.adopt([](unsigned){assert(false);}) && r.writes.empty());
}
struct WLEDBus {
    std::map<uint32_t, uint8_t> values;
    struct Write { uint16_t address; unsigned bytes; uint8_t first; };
    std::vector<Write> writes;
    std::vector<unsigned> delays;
    std::array<uint32_t, 4> displayedBrightness{{0x123, 0x123, 0, 0}};
    unsigned calls = 0, failAt = 0;
    bool owned = true, ignoredWrite = false;
    WLEDBus() {
        values[0x20104] = 0x51; values[0x20105] = 0x15;
        values[0x3d846] = 0xb5; values[0x3d946] = 0x30;
        values[0x3d947] = 0x80;
        for (unsigned i = 0; i < 2; ++i) {
            values[0x3d950 + 16*i] = 0x80;
            values[0x3d957 + 16*i] = 0x23;
            values[0x3d958 + 16*i] = 0xb1;
        }
    }
    bool irqChannel(uint8_t sid, uint8_t peripheral, uint16_t &apid) const {
        apid = peripheral; return owned && sid == 3 && (peripheral == 0xd8 || peripheral == 0xd9);
    }
    QcomPower::Status transfer(uint8_t sid, uint16_t address, uint8_t *p, unsigned n, bool write) {
        assert(n && n <= 8 && (address & 255) + n <= 256);
        ++calls;
        if (write) {
            assert(sid == 3);
            assert((address == 0xd846 && n == 1) || (address == 0xd947 && n == 1) ||
                   ((address == 0xd957 || address == 0xd967) && n == 2));
            writes.push_back({address, n, p[0]});
        }
        // Simulate an ACK lost after the write may have reached the device.
        if (write && !ignoredWrite) {
            uint8_t oldSync = values[0x3d947];
            for (unsigned i = 0; i < n; ++i) values[(uint32_t(sid)<<16) | (address+i)] = p[i];
            // Brightness shadow readback alone cannot prove a latch occurred.
            if (address == 0xd947) for (unsigned i = 0; i < 4; ++i)
                if ((~oldSync & p[0]) & (1u << i))
                    displayedBrightness[i] = values[0x3d957 + 16*i] |
                        uint32_t(values[0x3d958 + 16*i] & 15) << 8;
        }
        if (calls == failAt) return QcomPower::TimedOut;
        if (!write) for (unsigned i = 0; i < n; ++i) p[i] = values[(uint32_t(sid)<<16) | (address+i)];
        return QcomPower::OK;
    }
    void delay(unsigned us) { delays.push_back(us); }
    void resetTrace() { calls = 0; failAt = 0; writes.clear(); delays.clear(); }
};
static void testPMI8998WLED() {
    using namespace QcomPower;
    using namespace PMI8998Backlight;
    Config config{1, 3, 0x101, 3, 0xd800, 0xd900, 3, 1000};
    WLEDBus bus;
    Hardware hardware;
    DisplayBacklight::Request state;
    assert(hardware.initialize(bus, config) == OK && bus.writes.empty());
    hardware.state(state);
    assert(state.valid && state.power && state.brightness == 0x123 && state.appliedBrightness == 0x123);
    auto inherited = hardware;
    auto baseline = bus;
    // Real firmware left SYNC=3, sink enable=0x30 and dim mode=0x4c.
    // Adoption must stay read-only; subsequent updates need a real edge.
    // Other initial latch combinations also preserve unowned/reserved bits.
    for (uint8_t sync : {0x00, 0x01, 0x02, 0x03, 0x80, 0x83, 0x8f}) {
        auto b = baseline; Hardware h;
        b.values[0x3d947] = sync; b.values[0x3d94a] = 0x4c;
        b.values[0x3d846] = 0x80;
        assert(h.initialize(b, config) == OK && b.writes.empty());
        b.resetTrace();
        assert(h.setBrightness(b, 0x456) == OK);
        assert(b.displayedBrightness[0] == 0x456 && b.displayedBrightness[1] == 0x456);
        assert(b.displayedBrightness[2] == 0 && b.displayedBrightness[3] == 0);
        assert(b.writes[2].address == 0xd947 && b.writes[2].first == (sync & ~3u));
        assert(b.writes[3].address == 0xd947 && b.writes[3].first == (sync | 3u));
        assert(h.setPower(b, false) == OK);
        b.resetTrace();
        assert(h.setBrightness(b, 0x789) == OK && b.writes.empty());
        assert(b.displayedBrightness[0] == 0x456 && b.displayedBrightness[1] == 0x456);
        assert(h.setPower(b, true) == OK);
        assert(b.displayedBrightness[0] == 0x789 && b.displayedBrightness[1] == 0x789);
    }
    // Reject wrong PMIC, ownership, handoff, string mask, resolution and
    // nonuniform inherited brightness before any write can occur.
    for (unsigned variant = 0; variant < 10; ++variant) {
        auto b = baseline; auto c = config; Hardware h;
        if (variant == 0) c.version = 0;
        if (variant == 1) c.flags = 1;
        if (variant == 2) c.control = 0xd700;
        if (variant == 3) c.sid = 2;
        if (variant == 4) b.owned = false;
        if (variant == 5) b.values[0x20105] = 0x0a;
        if (variant == 6) b.values[0x3d946] = 0x70;
        if (variant == 7) b.values[0x3d94a] = 2;
        if (variant == 8) b.values[0x3d967] = 0x24;
        if (variant == 9) b.values[0x3d810] = 4;
        assert(h.initialize(b, c) != OK && b.writes.empty());
    }
    bus.resetTrace();
    assert(hardware.setPower(bus, false) == OK);
    hardware.state(state);
    assert(state.valid && !state.power && !state.appliedBrightness && state.brightness == 0x123);
    assert(bus.writes.size() == 1 && bus.values[0x3d846] == 0x35);
    bus.resetTrace();
    assert(hardware.setPower(bus, false) == OK && bus.writes.empty());
    assert(hardware.setBrightness(bus, 0x987) == OK && bus.writes.empty());
    assert(bus.values[0x3d957] == 0x23); // Deferred, still dark.
    auto dark = hardware; auto darkBus = bus;
    bus.resetTrace();
    assert(hardware.setPower(bus, true) == OK);
    hardware.state(state);
    assert(state.power && state.brightness == 0x987 && state.appliedBrightness == 0x987);
    assert(bus.values[0x3d846] == 0xb5 && bus.values[0x3d947] == 0x83);
    assert(bus.values[0x3d957] == 0x87 && bus.values[0x3d958] == 0xb9);
    assert(bus.values[0x3d967] == 0x87 && bus.values[0x3d968] == 0xb9);
    assert(bus.delays == std::vector<unsigned>({1000, 10000}));
    // A wake must latch brightness before switching on the boost module.
    assert(bus.writes.size() == 5 && bus.writes[2].address == 0xd947 &&
        bus.writes[2].first == 0x80 && bus.writes[3].first == 0x83 && bus.writes.back().address == 0xd846);
    for (uint32_t value : {0u, 1u, 16u, 1023u, 2048u, 4095u}) {
        assert(hardware.setBrightness(bus, value) == OK);
        hardware.state(state);
        assert(state.brightness == value && state.appliedBrightness == value && state.power);
        assert(bool(bus.values[0x3d846] & 128) == (value != 0));
    }
    bus.resetTrace();
    assert(hardware.setBrightness(bus, 4096) == Invalid && !bus.calls);
    assert(hardware.setBrightness(bus, 0) == OK);
    assert(hardware.setPower(bus, false) == OK && hardware.setPower(bus, true) == OK);
    hardware.state(state); assert(state.power && !state.appliedBrightness && !state.brightness);
    // Every transport failure in off, wake and brightness update must leave
    // the requested software state uncommitted and block further mutations.
    for (unsigned op = 0; op < 3; ++op) {
        auto run = [op](Hardware &h, WLEDBus &b) {
            return op == 0 ? h.setPower(b, false) : op == 1 ? h.setPower(b, true) : h.setBrightness(b, 0x456);
        };
        auto sourceHardware = op == 1 ? dark : inherited;
        auto sourceBus = op == 1 ? darkBus : baseline;
        auto goodH = sourceHardware; auto goodB = sourceBus; goodB.resetTrace();
        assert(run(goodH, goodB) == OK);
        for (unsigned fail = 1; fail <= goodB.calls; ++fail) {
            auto h = sourceHardware; auto b = sourceBus; b.resetTrace(); b.failAt = fail;
            assert(run(h, b) != OK);
            h.state(state); assert(!state.valid);
            assert(state.brightness == (op == 1 ? 0x987u : 0x123u));
            assert(state.power == (op != 1));
            auto calls = b.calls;
            assert(h.setPower(b, true) == Stale && b.calls == calls);
        }
    }
    auto mismatchH = inherited; auto mismatchB = baseline;
    mismatchB.ignoredWrite = true;
    assert(mismatchH.setPower(mismatchB, false) == Failed);
    mismatchH.state(state); assert(!state.valid && state.power);
}
static void testPreparedSwapSlot() {
    struct Lease {unsigned refs=1;void retain(){++refs;}void release(){assert(refs);--refs;}} lease,other;
    struct Mapping {unsigned surface;bool mapped=true;void retire(){assert(mapped);mapped=false;}} oldFront{42},next{42},replacement{43};
    Display::PreparedSwapSlot<Mapping,Lease> slot;
    Display::PreparedSwapKey key{100,42,reinterpret_cast<uintptr_t>(&lease),7};
    assert(slot.empty() && slot.adopt(key,&next,&lease) && lease.refs==2);
    assert(!slot.adopt(key,&replacement,&other) && other.refs==1);
    for(unsigned i=0;i<64;++i) {
        auto changed=key;
        switch(i%4){case 0:changed.request+=i+1;break;case 1:changed.surface+=i+1;break;
            case 2:changed.token+=i+1;break;default:changed.fence=reinterpret_cast<uintptr_t>(&other);break;}
        assert(!slot.take(changed,true,false) && lease.refs==2 && next.mapped);
    }
    assert(!slot.take(key,false,false) && !slot.take(key,true,true));
    // A ping-pong surface can already be the old front. Its old DMA graph
    // retires at PP_DONE; the fresh, unexposed graph must remain independent.
    oldFront.retire();assert(next.mapped);
    assert(slot.take(key,true,false)==&next && slot.empty() && lease.refs==1);
    next.retire();
    assert(slot.adopt(key,&replacement,&lease));
    // Cancellation does not recycle the fence identity while the slot owns it.
    lease.release();assert(lease.refs==1);
    auto recycled=key;recycled.fence=reinterpret_cast<uintptr_t>(&other);
    assert(!slot.take(recycled,true,false));
    unsigned retired=0;
    assert(slot.discard([&](Mapping *m){assert(slot.empty());m->retire();++retired;return true;}));
    assert(retired==1 && lease.refs==0 && slot.empty());
    assert(slot.discard([&](Mapping *){assert(false);return false;}));
    Mapping failed{44};Display::PreparedSwapKey failure{200,44,reinterpret_cast<uintptr_t>(&other),0};
    assert(slot.adopt(failure,&failed,&other));bool quarantined=false;
    assert(!slot.discard([&](Mapping *m){assert(slot.empty() && m==&failed);quarantined=true;return false;}));
    assert(quarantined && failed.mapped && other.refs==1 && slot.empty());
}
static void testDisplayTimeline() {
    auto *ring=new Display::TimelineRing{};
    Display::TimelineSample read{};
    assert(!ring->read(0,read));
    for(uint64_t n=0;n<Display::TimelineCapacity+77;++n) {
        auto s=Display::TimelineSample::make(n*319845,n,Display::TimelineKind::IRQ,uint32_t(n),0x1100,uint32_t(n),1,0x12340781,1920,0xf0004000,n+3);
        assert(ring->append(s) && ring->read(n,read));
        for(unsigned i=0;i<7;++i)assert(read.words[i]==s.words[i]);
    }
    assert(ring->total()==Display::TimelineCapacity+77 && !ring->losses());
    assert(!ring->read(0,read) && !ring->read(76,read) && ring->read(77,read));
    // An interrupted writer is unavailable, never a partially updated event.
    const auto n=ring->total();auto &slot=ring->slots[n%Display::TimelineCapacity];
    slot.busy=1;
    assert(!ring->append(read) && ring->losses()==1 && !ring->read(n,read));
    slot.busy=0;slot.serial=0;slot.words[0]=0xfeed;
    assert(!ring->read(n,read));
    delete ring;
}
static void testIdleMappings() {
    struct Surface{} a,b,c;
    struct Map {Surface *s;unsigned identity;Surface *surface() const{return s;}} first{&a,1},second{&b,2},third{&c,3},alias{&a,4};
    Display::IdleMappings<Map> idle;
    assert(!idle.count() && !idle.take(&a));
    assert(!idle.put(&first) && !idle.put(&second) && idle.count()==2);
    assert(idle.take(&a)==&first && idle.count()==1 && !idle.take(&a));
    // The checked-out owner is absent during DMA. A separate preparation of
    // the same surface has a distinct lifetime; lookup never aliases it.
    assert(!idle.put(&alias) && idle.count()==2);
    assert(idle.take(&a)==&alias && !idle.take(&a));
    assert(!idle.put(&first));assert(idle.put(&third)==&first);
    assert(idle.count()==2 && idle.take(&b)==&second && idle.pop()==&third && !idle.pop());
}
int main() {
    testInterruptTiming();
    testCommandQueueDrain();
    testIdleMappings();
    testDisplayTimeline();
    testPreparedSwapSlot();
    testCommandTransfer();
    testPMI8998WLED();
    {
        struct Raster {
            std::map<uint32_t,uint32_t> values{{0,0x30000001},{0x5000,1080u|(1920u<<16)},
                {0x500c,1080u|(1920u<<16)},{0x5014,0x9d400000},{0x5024,4320},
                {0x2000,0x1000005},{0x2014,0x20020},{0x6b800,1},{0xb0200,0x20},{0xb0204,0x20}};
            bool stalled=false,clockStatusChanges=false;
            QcomPower::Status accessible(){return QcomPower::OK;}
            uint32_t read(uint32_t offset){return values[offset];}
            void write(uint32_t offset,uint32_t value){values[offset]=value;if(offset==0xb0200 && !stalled)values[0xb0204]=value;
                if(offset==0x12ac && clockStatusChanges)values[offset]^=1u<<31;}
            void delay(unsigned){}
        } raster;
        Display::MMU::DMAHalt<Raster> halt(raster);
        assert(halt.prepare(0x9d400000,1080,1920) && halt.isolated()==QcomPower::Busy);
        assert(halt.halt() && halt.isolated()==QcomPower::OK && raster.values[0xb0200]==0x21);
        raster.values[0xb0204]=UINT32_MAX;assert(halt.isolated()==QcomPower::Busy);raster.values[0xb0204]=0x21;
        assert(halt.resume() && raster.values[0xb0200]==0x20 && raster.values[0x12ac]==0);
        auto clocks=raster;clocks.clockStatusChanges=true;Display::MMU::DMAHalt<Raster> changingStatus(clocks);
        assert(changingStatus.prepare(0x9d400000,1080,1920) && changingStatus.halt() && changingStatus.isolated()==QcomPower::OK && changingStatus.resume());
        auto timeout=raster;timeout.stalled=true;Display::MMU::DMAHalt<Raster> missingAck(timeout);
        assert(missingAck.prepare(0x9d400000,1080,1920) && !missingAck.halt() && missingAck.isolated()==QcomPower::Busy);
        assert(missingAck.resume());
        auto other=raster;other.values[0x7000]=1;Display::MMU::DMAHalt<Raster> foreign(other);
        assert(!foreign.prepare(0x9d400000,1080,1920));
    }
    {
        struct ReadOnlySMMU {
            std::map<uint32_t,uint64_t> values;
            uint32_t read(uint32_t offset){return uint32_t(values[offset]);}
            uint64_t read64(uint32_t offset){return values[offset];}
        } smmu;
        smmu.values={{0x20,(1u<<30)|(1u<<27)|(15u<<9)|1},{0x24,0x40000014},{0x28,0x1222},
            {0x800,1u<<31},{0xc00,2},{0x1008,1u<<16},{0x1808,1},{0x22000,0x1067},
            {0x22030,(1u<<23)|(2u<<12)|25},{0x22010,(7u<<15)|16|2},
            {0x22038,0x44},{0x22020,(7ULL<<48)|0x900000}};
        SMMU::ContextHandoff context;
        assert(Display::MMU::capture(smmu,context)==QcomPower::OK && context.bank==2 && context.inputBits==39);
        Display::MMU::Handoff h;h.context=context;h.flags=31;h.tableCount=1;
        h.first=0xf0000000;h.end=0x100000000ULL;h.framebuffer=h.deviceAddress=0x90000000;
        h.framebufferBytes=0x800000;h.cachePolicy=1;h.tables[0].physical=0x900000;
        assert(h.valid());h.tables[0].physical=0x90001000;assert(!h.valid());
        smmu.values[0xc00]=0x10002;assert(Display::MMU::capture(smmu,context)==QcomPower::Unsupported);
        smmu.values[0xc00]=2;smmu.values[0x20000]=1;
        assert(Display::MMU::capture(smmu,context)==QcomPower::Stale);
        // The real MMSS profile reports 32-bit VA and 36-bit PA, with a
        // disabled stage-1 bank. Detection remains read-only and exclusive.
        smmu.values={{0,0x200626},{0x20,0x48017836},{0x24,0x40008011},{0x28,0x7011},
            {0x800,1u<<31},{0x1000,0x10003},{0x1800,1}};
        assert(Display::MMU::captureDisabled(smmu,context)==QcomPower::OK && context.outputBits==36 && context.bank==0);
        smmu.values[0x21000]=1;assert(Display::MMU::captureDisabled(smmu,context)==QcomPower::Stale);
    }
    using namespace Display;
    Mode mode{1080, 1920, 4352, 60, 0x90000000, 4352 * 1920};
    assert(mode.valid());
    auto bad = mode; bad.width = UINT32_MAX; assert(!bad.valid());
    bad = mode; bad.framebufferSize--; assert(!bad.valid());
    bad = mode; bad.framebufferAddress = (1ULL << 40) - 4096; assert(!bad.valid());
    bad = mode; bad.rowBytes = 4321; assert(!bad.valid());
    Surface surface{1080, 1920, 4352, 4352 * 1920, 4352 * 1920, BGRA};
    assert(surface.valid(mode));
    surface.tiled = true; assert(!surface.valid(mode));
    surface.tiled = false; surface.tileFormat = 1; assert(!surface.valid(mode));
    surface.tileFormat = 0; assert(surface.valid(mode));
    surface.format = RGBA; assert(!surface.valid(mode));
    surface.format = BGRA; surface.descriptorSize--; assert(!surface.valid(mode));
    // Run50's native compositor updates one physical surface and clears two
    // layers. Color metadata is not a fifth plane. Retained extra planes,
    // metadata-only updates and unknown flags must still be rejected.
    assert(singleSurfaceUpdate(1, 1));
    assert(singleSurfaceUpdate(0x80000007, 1));
    assert(singleSurfaceUpdate(0xf, 1));
    assert(!singleSurfaceUpdate(0x80000007, 3));
    assert(!singleSurfaceUpdate(1, 9));
    assert(!singleSurfaceUpdate(0x80000000, 1));
    assert(!singleSurfaceUpdate(0x80000007, 0));
    assert(!singleSurfaceUpdate(0x80000017, 1));
    // Native cached default/empty transactions use an all-fields sentinel.
    // Keep it out of normal surface submissions and reject incomplete clears.
    assert(nativeSwapContent(6, UINT32_MAX, 1, 0) == SwapContent::Surface);
    assert(nativeSwapContent(7, UINT32_MAX, 0, 0) == SwapContent::Black);
    assert(nativeSwapContent(7, 0x8000000f, 0, 0) == SwapContent::Black);
    assert(nativeSwapContent(0, UINT32_MAX, 1, 0) == SwapContent::Unsupported);
    assert(nativeSwapContent(6, UINT32_MAX, 0, 0) == SwapContent::Unsupported);
    assert(nativeSwapContent(7, 0x80000007, 0, 0) == SwapContent::Unsupported);
    assert(nativeSwapContent(7, UINT32_MAX, 1, 0) == SwapContent::Unsupported);
    assert(nativeSwapContent(7, UINT32_MAX, 0, 0xff0000) == SwapContent::Unsupported);
    assert(nativeSwapContent(8, UINT32_MAX, 0, 0) == SwapContent::Unsupported);
    PlatformContract c{1, 31, 1, 0, 0, 27};
    assert(c.valid()); c.interface = 2; assert(!c.valid());
    c.interruptBit = 29; assert(c.valid()); c.flags = 15; assert(!c.valid());
    c.flags = 63; assert(c.valid()); c.flags = 127; assert(!c.valid());
    c.flags = 95; c.interface = 1; c.interruptBit = 12; assert(c.valid());
    assert(c.ownedInterrupts() == ((1u << 12) | (1u << 8)));
    c.pingPong = 1; assert(!c.valid()); c.interruptBit = 13; assert(c.valid());
    assert(bufferSpan(mode) >= uint64_t(mode.rowBytes) * mode.height && !(bufferSpan(mode) & 4095));
    assert(linearBGRA(0x236ff, 0x03020001, 0));
    assert(linearBGRA(0x237ff, 0x03020001, 0));
    assert(!linearBGRA(0x800236ff, 0x03020001, 0));
    assert(!linearBGRA(0x236ff, 0x03020001, 1));
    assert(!linearBGRA(0x236ff, 0x03000002, 0));
    // Native queue references must move once, without retaining an active
    // node in the pending list or damaging the following node's back link.
    int firstRequest = 1, secondRequest = 2;
    void *head = &firstRequest;
    NativeSwapLinks first{&secondRequest, &head}, second{nullptr, &first.next};
    void **tail = &second.next;
    auto originalTail = tail;
    assert(!detachNativeSwapHead(head, tail, &secondRequest, second, nullptr));
    assert(head == &firstRequest && tail == originalTail && second.previous == &first.next);
    assert(detachNativeSwapHead(head, tail, &firstRequest, first, &second));
    assert(head == &secondRequest && tail == &second.next && second.previous == &head);
    assert(!detachNativeSwapHead(head, tail, &firstRequest, first, &second));
    auto savedBackLink = second.previous;
    second.previous = nullptr;
    assert(!detachNativeSwapHead(head, tail, &secondRequest, second, nullptr));
    assert(head == &secondRequest && tail == &second.next);
    second.previous = savedBackLink;
    assert(detachNativeSwapHead(head, tail, &secondRequest, second, nullptr));
    assert(!head && tail == &head);
    assert(!detachNativeSwapHead(head, tail, nullptr, second, nullptr));
    MDPRegisters mdp;
    Scanout<MDPRegisters> scanout(mdp, 8);
    assert(!scanout.stage(0x100000000ULL));
    assert(scanout.stage(0x90800000));
    assert(mdp.values[0x24014] == 0x90800000 && mdp.values[0x1018] == (1u << 11));
    assert(!scanout.latched(0x90800000));
    assert(!scanout.stage(0x90000000)); // Don't overwrite a pending latch.
    mdp.values[0x1018] = 0;
    assert(scanout.latched(0x90800000));
    assert(!scanout.latched(0x90000000));
    assert(mappedRaster(mode, 0x90800000, 4352, 4352 * 1920));
    assert(!mappedRaster(mode, 0xffc00000, 4352, 4352 * 1920));
    assert(!mappedRaster(mode, 0x90800001, 4352, 4352 * 1920));
    assert(!mappedRaster(mode, 0x90800000, 65536, 65536 * 1920));
    mdp.values[0x24024] = 0xa5000000u | 4320;
    assert(scanout.stage(0x91000000, 4352));
    assert(mdp.values[0x24024] == (0xa5000000u | 4352));
    mdp.values[0x1018] = 0;
    assert(scanout.latched(0x91000000, 4352) && !scanout.latched(0x91000000, 4320));
    mdp.values[0x1018] = 0x60000000; // unrelated firmware timing flush bits
    assert(scanout.startCommand());
    assert(mdp.values[0x101c] == 1 && mdp.values[0x1018] == ((1u << 11) | (1u << 17)));
    assert(!scanout.startCommand());
    Registers regs;
    regs.values[InterruptEnable / 4] = 1u << 4;
    Interrupts<Registers> irq(regs, 27);
    irq.enable(); assert(regs.values[InterruptEnable / 4] == ((1u << 4) | (1u << 27)));
    regs.values[InterruptStatus / 4] = (1u << 4) | (1u << 27);
    assert(irq.acknowledge());
    assert(regs.values[InterruptStatus / 4] == (1u << 4));
    assert(!irq.acknowledge());
    regs.values[InterruptEnable / 4] |= 1u << 5;
    irq.disable(); assert(regs.values[InterruptEnable / 4] == ((1u << 4) | (1u << 5)));
}
