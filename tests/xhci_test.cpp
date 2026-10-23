// SPDX-License-Identifier: BSD-2-Clause
#include "XHCIRing.h"
#include "XHCICore.h"
#include "USBHostCurrent.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <deque>
#include <random>
#include <vector>
#include <utility>
#include <map>
#include <string>

struct Order {
    unsigned reads = 0, writes = 0;
    void dmaReadBarrier() { ++reads; }
    void dmaWriteBarrier() { ++writes; }
};

static void boundaries() {
    assert(XHCI::segment(0x10000, 4096, 32));
    assert(!XHCI::segment(0x10040, 4096, 32));
    assert(!XHCI::segment(0x10010, 16, 32));
    assert(!XHCI::segment(0x10000, 15, 32));
    assert(!XHCI::segment(1ull << 32, 16, 32));
    std::array<XHCI::TRB, 10> output{}; unsigned used = 0;
    assert(XHCI::appendData(output.data(), 10, used, 0x1fff0, 65560, 40) == USB::Result::Success);
    assert(used == 3 && output[0].parameter() == 0x1fff0 && output[0].status == 16);
    assert(output[1].parameter() == 0x20000 && output[1].status == 65536);
    assert(output[2].parameter() == 0x30000 && output[2].status == 8);
    assert(XHCI::appendData(output.data(), 10, used, 0x1000, 0, 40) == USB::Result::Invalid);
    unsigned old = used;
    assert(XHCI::appendData(output.data(), 4, used, 0x1fff0, 65560, 40) == USB::Result::Busy);
    assert(used == old);
    assert(XHCI::appendData(output.data(), 10, used, 0xfffffff0, 17, 32) == USB::Result::Invalid);
    assert(XHCI::appendData(output.data(), 10, used, UINT64_MAX - 2, 4, 64) == USB::Result::Invalid);
    assert(XHCI::appendData(output.data(), 10, used, 0x1000, 1, 40, XHCI::Link) == USB::Result::Invalid);
}

// An independent hardware-side consumer follows Links and producer cycles.
// Random TD sizes and completion delays repeatedly wrap the ring, fill it,
// and leave old DMA values in free entries.
static void producer() {
    alignas(64) std::array<XHCI::TRB, 16> memory{};
    Order order; XHCI::ProducerRing<Order> ring(order);
    assert(ring.available() == 0);
    assert(!ring.initialize(memory.data(), 0x1ffc0, 16, 40));
    assert(ring.initialize(memory.data(), 0x10000, 16, 40));
    assert(!ring.initialize(memory.data(), 0x10000, 16, 40));
    std::mt19937 random(0x835);
    struct Pending { std::vector<uint64_t> parameters; uint64_t last; };
    std::deque<Pending> pending;
    unsigned hardwareAt = 0; uint32_t hardwareCycle = 1; uint64_t serial = 0;
    uint64_t dummy = 0;
    XHCI::TRB bad = XHCI::trb(0, 0, XHCI::Link << 10);
    assert(ring.submit(&bad, 1, dummy) == USB::Result::Invalid);
    assert(ring.submit(memory.data(), 1, dummy) == USB::Result::Invalid);
    for (unsigned step = 0; step < 50000; ++step) {
        unsigned count = 1 + random() % 7;
        if (ring.available() >= count && (pending.empty() || random() % 3)) {
            std::array<XHCI::TRB, 7> td{}; Pending request;
            for (unsigned i = 0; i < count; ++i) {
                uint64_t parameter = ++serial;
                td[i] = XHCI::trb(parameter, i, XHCI::Normal << 10 |
                    (i + 1 == count ? XHCI::IOC : XHCI::Chain));
                request.parameters.push_back(parameter);
            }
            assert(ring.submit(td.data(), count, request.last) == USB::Result::Success);
            pending.push_back(request);
        } else if (!pending.empty()) {
            auto &request = pending.front();
            for (uint64_t expected : request.parameters) {
                if (memory[hardwareAt].type() == XHCI::Link) {
                    auto link = memory[hardwareAt];
                    assert((link.control & 1) == hardwareCycle);
                    assert(link.parameter() == ring.startAddress());
                    assert(link.control & XHCI::ToggleCycle);
                    hardwareAt = 0; hardwareCycle ^= 1;
                }
                auto v = memory[hardwareAt];
                assert(v.type() == XHCI::Normal && (v.control & 1) == hardwareCycle);
                assert(v.parameter() == expected); ++hardwareAt;
            }
            assert(!ring.retire(unsigned(request.parameters.size()), request.last + 16));
            assert(ring.retire(unsigned(request.parameters.size()), request.last));
            pending.pop_front();
        }
        unsigned queued = 0;
        for (auto &p : pending) queued += unsigned(p.parameters.size());
        assert(ring.pending() == queued && ring.available() + queued == 14);
        assert(!ring.retire(queued + 1, 0));
    }
    assert(serial > 10000 && order.writes > 10000);
    // Failed admission does not mutate the producer index or ring memory.
    // Ensure there is at least one outstanding TD before requesting capacity.
    if (!ring.pending()) {
        XHCI::TRB one = XHCI::trb(1, 0, XHCI::CommandNoop << 10);
        assert(ring.submit(&one, 1, dummy) == USB::Result::Success);
    }
    std::array<XHCI::TRB, 14> td{};
    for (auto &v : td) v.control = XHCI::CommandNoop << 10;
    auto copy = memory; auto before = ring.nextAddress();
    assert(ring.submit(td.data(), 14, dummy) == USB::Result::Busy);
    assert(before == ring.nextAddress());
    for (unsigned i = 0; i < 16; ++i) assert(copy[i].control == memory[i].control && copy[i].parameter() == memory[i].parameter());
}

static void events() {
    alignas(64) std::array<XHCI::TRB, 16> memory{};
    Order order; XHCI::EventRing<Order> ring(order);
    assert(ring.initialize(memory.data(), 0x100000, 16, 40));
    XHCI::TRB event{};
    assert(!ring.peek(event));
    uint32_t cycle = 1;
    for (unsigned round = 0; round < 1000; ++round) {
        for (unsigned i = 0; i < 16; ++i) {
            uint64_t parameter = uint64_t(round) << 32 | i;
            memory[i] = XHCI::trb(parameter, 1u << 24, XHCI::CommandEvent << 10 | cycle);
        }
        for (unsigned i = 0; i < 16; ++i) {
            assert(ring.dequeueAddress() == 0x100000 + i * 16);
            assert(ring.peek(event) && event.type() == XHCI::CommandEvent);
            assert(event.parameter() == (uint64_t(round) << 32 | i));
            ring.consume();
        }
        assert(!ring.peek(event)); cycle ^= 1;
    }
    assert(order.reads == 16000);
}

struct HostIO {
    std::array<uint32_t, 0xc100 / 4> registers{};
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    unsigned elapsed = 0, barriers = 0;
    bool stuck = false;
    HostIO() {
        registers[0] = 0x01000040;
        registers[1] = 2u << 24 | 1u << 8 | 32;
        registers[16 / 4] = 5; // 64-bit DMA, 64-byte contexts
        registers[20 / 4] = 0x1000; registers[24 / 4] = 0x2000;
        registers[0x44 / 4] = 1; registers[0x48 / 4] = 1;
    }
    uint32_t read(uint32_t r) { assert(r < 0xc100 && !(r & 3)); return registers[r / 4]; }
    void write(uint32_t r, uint32_t v) {
        assert(r < 0xc100 && !(r & 3)); writes.emplace_back(r, v);
        if (r == 0x44) { registers[r / 4] &= ~v; return; }
        registers[r / 4] = v;
        if (r == 0x40 && !stuck) {
            if (v & 1) registers[0x44 / 4] &= ~1u;
            else registers[0x44 / 4] |= 1;
        }
    }
    void delay(unsigned us) { elapsed += us; }
    void dmaWriteBarrier() { ++barriers; }
};

static void controller() {
    using USB::Result;
    HostIO mmio; XHCI::Core<HostIO> core(mmio);
    assert(core.halt() == Result::NotReady);
    assert(core.probe(0xc100, 40) == Result::Success);
    assert(core.currentState() == XHCI::State::Halted && core.maxSlots() == 32);
    assert(core.maxPorts() == 2 && core.contextStride() == 64 && core.dmaBits() == 40);
    assert(core.beginReset(100) == Result::Success);
    assert(core.advanceReset(110) == Result::Busy && mmio.elapsed == 0);
    assert(core.advanceReset(109) == Result::Invalid);
    mmio.registers[0x40 / 4] &= ~2u;
    mmio.registers[0x44 / 4] |= 1u << 11;
    assert(core.advanceReset(120) == Result::Busy);
    mmio.registers[0x44 / 4] &= ~(1u << 11);
    assert(core.advanceReset(130) == Result::Success);
    alignas(64) XHCI::SegmentTable table;
    XHCI::MemoryLayout layout{0x100000, 0x110000, 0x120000, 0x130000, 0, &table, 256, 0, 32};
    auto invalid = layout; invalid.eventEntries = 4097;
    auto before = mmio.writes.size();
    assert(core.configure(invalid) == Result::Invalid && mmio.writes.size() == before);
    invalid = layout; invalid.command |= 16;
    assert(core.configure(invalid) == Result::Invalid);
    invalid = layout; invalid.dcbaa = 0x10000000000;
    assert(core.configure(invalid) == Result::Invalid);
    assert(core.configure(layout) == Result::Success);
    assert(table.address == layout.events && table.entries == 256 && !table.reserved);
    assert(mmio.registers[0x58 / 4] == 0x100001 && mmio.registers[0x70 / 4] == 0x110000);
    assert(mmio.registers[0x2028 / 4] == 1 && mmio.registers[0x2030 / 4] == 0x120000);
    assert(core.run() == Result::Success);
    assert(core.doorbell(0, 0) == Result::Success && mmio.registers[0x1000 / 4] == 0);
    assert(core.doorbell(2, 3) == Result::Success && mmio.registers[0x1008 / 4] == 3);
    assert(core.doorbell(33, 1) == Result::Invalid);
    assert(core.doorbell(1, 0) == Result::Invalid && core.doorbell(0, 1) == Result::Invalid);
    assert(core.doorbell(1, 1, 1) == Result::Invalid);
    assert(core.acknowledgeEvents(layout.events + 16) == Result::Success);
    assert(mmio.registers[0x2038 / 4] == 0x130018);
    assert(core.acknowledgeEvents(layout.events + 1) == Result::Invalid);
    assert(core.acknowledgeEvents(layout.events - 16) == Result::Invalid);
    assert(core.acknowledgeEvents(layout.events + 4096) == Result::Invalid);
    uint32_t status = 0;
    assert(core.readPort(0, status) == Result::Invalid && core.readPort(3, status) == Result::Invalid);
    mmio.registers[0x440 / 4] = 0x1203;
    assert(core.readPort(1, status) == Result::Success && status == 0x1203);
    mmio.stuck = true;
    assert(core.halt() == Result::Timeout && core.currentState() == XHCI::State::Fault);
    assert(core.doorbell(0, 0) == Result::NotReady);
    mmio.stuck = false;
    assert(core.halt() == Result::Success && core.beginReset(200) == Result::Success);
    assert(core.advanceReset(250199) == Result::Busy);
    assert(core.advanceReset(250200) == Result::Timeout);

    HostIO unsupported; unsupported.registers[24 / 4] = 0x40;
    XHCI::Core<HostIO> bad(unsupported);
    assert(bad.probe(0xc100, 40) == Result::Unsupported && unsupported.writes.empty());
    assert(bad.halt() == Result::NotReady && unsupported.writes.empty());
    for (unsigned value=0;value<16;++value) {
        HostIO capabilities;capabilities.registers[8/4]=value;
        if (value&1) capabilities.registers[16/4]|=1u<<11;
        XHCI::Core<HostIO> measured(capabilities);
        assert(measured.probe(0xc100,40)==Result::Success);
        assert(measured.isochronousSchedulingThreshold()==(value&7)*((value&8) ? 8 : 1));
        assert(measured.contiguousFrameIDSupported()==bool(value&1));
    }
}

struct PortIO : HostIO {
    bool ignoreCompletionClear=false,failedRead=false;
    PortIO() {
        registers[4]|=8|(0x800u/4)<<16;
        registers[0x800/4]=3u<<24|4u<<8|2;registers[0x804/4]=0x20425355;
        registers[0x808/4]=1|1u<<8;registers[0x80c/4]=1;
        registers[0x810/4]=2u<<24|2;registers[0x814/4]=0x20425355;
        registers[0x818/4]=2|1u<<8;registers[0x81c/4]=2;
        registers[0x440/4]=1|2|1u<<9|4u<<10;
        registers[0x450/4]=1|2|1u<<9|3u<<10;
    }
    uint32_t read(uint32_t r) { return failedRead && r>=0x440 && r<=0x450 ? UINT32_MAX : HostIO::read(r); }
    void write(uint32_t r,uint32_t value) {
        using namespace XHCI::PortSC;
        if (r!=0x440 && r!=0x450) { HostIO::write(r,value);return; }
        writes.emplace_back(r,value);
        auto &raw=registers[r/4];const auto oldLink=raw&LinkMask;
        raw=(raw&~Persistent)|(value&Persistent);
        if (!(value&LinkWrite)) raw=(raw&~LinkMask)|oldLink;
        uint32_t clear=value&(Changes|Enabled);
        if (ignoreCompletionClear) clear&=~(ResetChange|WarmResetChange);
        raw&=~clear;
        if (value&(Reset|WarmReset)) { raw|=value&(Reset|WarmReset);raw&=~Enabled; }
    }
    void completed(unsigned port,bool warm=false) {
        using namespace XHCI::PortSC;
        auto &raw=registers[(0x440+(port-1)*16)/4];
        raw&=~(Reset|WarmReset|LinkMask);raw|=Enabled|(warm ? WarmResetChange : ResetChange);
    }
};
struct PortMachine {
    PortIO io;
    XHCI::Core<PortIO> core{io};
    XHCI::PortProtocols protocols;
    XHCI::Port<XHCI::Core<PortIO>> usb2{core},usb3{core};
    alignas(64) XHCI::SegmentTable table{};
    PortMachine() {
        using USB::Result;
        assert(protocols.initialize(io,2)==Result::Success);
        assert(core.probe(0xc100,40)==Result::Success && core.beginReset(0)==Result::Success);
        io.registers[0x40/4]&=~2u;assert(core.advanceReset(1)==Result::Success);
        XHCI::MemoryLayout layout{0x100000,0x110000,0x120000,0x130000,0,&table,256,0,32};
        assert(core.configure(layout)==Result::Success && core.run()==Result::Success);
        assert(usb2.initialize(2,protocols)==Result::Success && usb3.initialize(1,protocols)==Result::Success);
    }
};
static void protocolCapabilities() {
    using USB::Result;using USB::Speed;
    PortIO io;XHCI::PortProtocols protocols;
    assert(!protocols.get(1) && protocols.initialize(io,0)==Result::Invalid);
    assert(protocols.initialize(io,2)==Result::Success && protocols.initialize(io,2)==Result::Busy);
    assert(protocols.get(1)->major==3 && protocols.get(2)->major==2);
    assert(protocols.get(1)->slotType==1 && protocols.get(2)->speeds[2]==Speed::Low);
    assert(!protocols.get(0) && !protocols.get(3));
    for (unsigned bad=0;bad<8;++bad) {
        PortIO peer;XHCI::PortProtocols map;
        switch (bad) {
        case 0:peer.registers[0x818/4]=1|1u<<8;break; // overlaps USB3 range
        case 1:peer.registers[0x818/4]=2;break;
        case 2:peer.registers[0x818/4]=3|1u<<8;break;
        case 3:peer.registers[0x808/4]|=1u<<28;break; // PSI extends into next cap
        case 4:peer.registers[0x804/4]=0;break;
        case 5:peer.registers[0x800/4]=1;break; // legacy ownership unsupported
        case 6:peer.registers[4]=0;break;
        case 7:peer.registers[4]=0xffff0000;break;
        }
        assert(map.initialize(peer,2)!=Result::Success && !map.get(1));
    }
    PortIO custom;custom.registers[0x818/4]|=3u<<28;
    custom.registers[0x820/4]=1500u<<16|1u<<4|6;
    custom.registers[0x824/4]=12u<<16|2u<<4|7;
    custom.registers[0x828/4]=480u<<16|2u<<4|8;
    XHCI::PortProtocols speeds;assert(speeds.initialize(custom,2)==Result::Success);
    assert(speeds.get(2)->speeds[6]==Speed::Low && speeds.get(2)->speeds[7]==Speed::Full);
    assert(speeds.get(2)->speeds[8]==Speed::High && speeds.get(2)->speeds[1]==Speed::Unknown);
    for (uint32_t bad:{1500u<<16|1u<<4|6,12u<<16|2u<<4|1u<<6|7,100u<<16|2u<<4|7}) {
        auto peer=custom;peer.registers[0x824/4]=bad;XHCI::PortProtocols map;
        assert(map.initialize(peer,2)==Result::Unsupported);
    }
}

static PortIO companionCapabilities(bool reversed,unsigned variant) {
    PortIO io;
    const unsigned ss=reversed ? 2:1,hs=reversed ? 1:2;
    io.registers[0x808/4]=ss|1u<<8;io.registers[0x818/4]=hs|1u<<8;
    if (variant) {
        // Non-default PSI IDs and an extended first capability. Pairing is
        // independent of both protocol order and hardware speed-ID values.
        for (unsigned n=0;n<4;++n) io.registers[0x820/4+n]=io.registers[0x810/4+n];
        io.registers[0x800/4]=3u<<24|8u<<8|2;
        io.registers[0x808/4]|=1u<<28;
        io.registers[0x810/4]=5000u<<16|2u<<4|9;
        const unsigned entries=variant==1 ? 3:1;
        io.registers[0x828/4]=hs|1u<<8|entries<<28;
        if (variant==1) {
            io.registers[0x830/4]=1500u<<16|1u<<4|6;
            io.registers[0x834/4]=12u<<16|2u<<4|7;
            io.registers[0x838/4]=480u<<16|2u<<4|8;
        } else io.registers[0x830/4]=480u<<16|2u<<4|8;
    }
    return io;
}

// External Family cache model. The controller supplies borrowed table entries;
// the inherited Native port getter retains each of its six speed caches.
// Stop drops factory references and Native handleStopPhase2 clears the caches.
struct NativeCompanionCache {
    struct Port { unsigned references=1; } ports[2];
    Port *table[2]{&ports[0],&ports[1]},*cache[2][6]{};
    const XHCI::ConnectorPorts &policy;
    bool complete=true,controllerGate=false,sourceGate=false,stopping=false,stopPending=false,reverse=false;
    unsigned sequence=0,retainedAt=0,factoryDroppedAt=0;
    explicit NativeCompanionCache(const XHCI::ConnectorPorts &p,bool backwards) : policy(p),reverse(backwards) {}
    unsigned references() const { return ports[0].references+ports[1].references; }
    void dropFactory() {
        if (controllerGate) { stopPending=true;return; }
        stopping=true;factoryDroppedAt=++sequence;
        for (auto *&p:table) if (p) { assert(p->references);--p->references;p=nullptr; }
    }
    void drain() {
        assert(!sourceGate && !controllerGate);
        for (unsigned index=0;index<2;++index) {
            auto &row=cache[reverse ? 1-index:index];
            for (auto *&p:row) if (p) { assert(p->references);--p->references;p=nullptr; }
        }
    }
    bool query(unsigned source,unsigned speed,bool stopInBorrowedWindow=false) {
        if (!complete || stopping || !source || source>2 || !table[source-1] || speed>5) return false;
        auto &caller=ports[source-1];++caller.references;sourceGate=true;controllerGate=true;
        auto *&entry=cache[source-1][speed];
        if (!entry) {
            const auto index=policy.companion(source,speed);
            auto *borrowed=index ? table[index-1]:nullptr;
            if (stopInBorrowedWindow) {
                assert(borrowed && borrowed->references);dropFactory();
                assert(stopPending && table[0] && table[1] && !factoryDroppedAt);
            }
            if (borrowed) {
                assert(borrowed->references);++borrowed->references;entry=borrowed;retainedAt=++sequence;
            }
        }
        const bool found=entry!=nullptr;
        controllerGate=false;
        if (stopPending) { stopPending=false;dropFactory();assert(retainedAt<factoryDroppedAt); }
        // The Native caller still holds its source port gate while it uses
        // the cached peer after the controller gate opens.
        if (found) assert(entry->references);
        sourceGate=false;assert(caller.references);--caller.references;
        if (stopping) drain();
        return found;
    }
};

static void nativeCompanions() {
    using USB::Result;
    unsigned queries=0,lifecycles=0,invalid=0;
    XHCI::ConnectorPorts empty;
    assert(!empty.routed(0) && !empty.routed(1));
    for (unsigned speed:{0u,1u,2u,3u,4u,5u,UINT32_MAX}) { assert(!empty.companion(1,speed));++queries; }
    for (bool reversed:{false,true}) for (unsigned variant:{0,1,2}) for (unsigned ceiling:{3,4}) {
        auto io=companionCapabilities(reversed,variant);XHCI::PortProtocols protocols;
        assert(protocols.initialize(io,2)==Result::Success);
        XHCI::ConnectorPorts ports;assert(ports.initialize(protocols,2,ceiling)==Result::Success);
        assert(ports.initialize(protocols,2,ceiling)==Result::Busy);
        const unsigned hs=reversed ? 1:2,ss=reversed ? 2:1;
        assert(ports.routed(hs) && ports.routed(ss)==(ceiling==4) && !ports.routed(0) && !ports.routed(3));
        for (unsigned source=0;source<=3;++source) for (unsigned speed:{0u,1u,2u,3u,4u,5u,UINT32_MAX}) {
            unsigned expected=0;
            if (ceiling==4) {
                if (source==hs && speed==3) expected=ss;
                if (source==ss && speed<3 && (variant!=2 || speed==2)) expected=hs;
            }
            assert(ports.companion(source,speed)==expected);++queries;
        }
    }
    PortIO io;XHCI::PortProtocols protocols,notReady;assert(protocols.initialize(io,2)==Result::Success);
    for (auto inputs:{std::pair<unsigned,unsigned>{0,3},{3,4},{2,2}}) {
        XHCI::ConnectorPorts ports;assert(ports.initialize(protocols,inputs.first,inputs.second)==Result::Unsupported);++invalid;
    }
    XHCI::ConnectorPorts pending;assert(pending.initialize(notReady,2,4)==Result::NotReady);++invalid;
    assert(pending.initialize(protocols,2,4)==Result::Success); // failure did not publish a partial map
    for (unsigned major:{2,3}) {
        PortIO bad;bad.registers[0x800/4]=major<<24|2;bad.registers[0x808/4]=1|2u<<8;
        XHCI::PortProtocols map;assert(map.initialize(bad,2)==Result::Success);
        XHCI::ConnectorPorts ports;assert(ports.initialize(map,2,major==2 ? 3:4)==Result::Unsupported);++invalid;
    }
    for (unsigned major:{2,3}) {
        PortIO single;single.registers[0x800/4]=major<<24|2;single.registers[0x808/4]=1|1u<<8;
        XHCI::PortProtocols map;assert(map.initialize(single,1)==Result::Success);
        XHCI::ConnectorPorts ports;assert(ports.initialize(map,1,major==2 ? 4:3)==Result::Unsupported);++invalid;
        if (major==2) {
            assert(ports.initialize(map,1,3)==Result::Success);
            for (unsigned speed=0;speed<6;++speed) assert(!ports.companion(1,speed));
        }
    }
    for (unsigned epoch=0;epoch<4;++epoch) for (bool backwards:{false,true}) for (unsigned source:{1,2}) {
        XHCI::ConnectorPorts ports;assert(ports.initialize(protocols,2,4)==Result::Success);
        NativeCompanionCache family(ports,backwards);
        family.complete=false;
        for (unsigned speed=0;speed<6;++speed) assert(!family.query(source,speed));
        assert(family.references()==2);family.complete=true;
        // In this fixture USB3 is port1 and USB2 is port2. Fill the other
        // speed caches first, exercising repeated queries without new holds.
        assert(family.query(2,3));assert(family.query(1,0));assert(family.query(1,1));
        const auto retained=family.references();
        assert(family.query(2,3) && family.query(1,0) && family.references()==retained);
        // Keep the final cache empty so stop is injected after a borrowed
        // lookup but before Native's first retain for that cache entry.
        if (source==2) {
            family.drain();assert(family.references()==2);
            assert(family.query(1,0) && family.query(1,1) && family.query(1,2));
        }
        assert(family.query(source,source==2 ? 3:2,true));
        assert(family.stopping && !family.references() && !family.table[0] && !family.table[1]);
        assert(!family.query(source,source==2 ? 3:2) && !family.query(3,3));
        for (auto &row:family.cache) for (auto *entry:row) assert(!entry);
        ++lifecycles;
    }
    assert(queries==343 && invalid==8 && lifecycles==16);
    std::printf("Native companions: %u topology/speed queries, %u invalid topologies and %u cache/stop/cold-PM lifecycles passed\n",
        queries,invalid,lifecycles);
}


// External Native table consumer: the actual Family reset/stop loops skip null
// entries while preserving the table's hardware port-number range.
struct NativeRoutedTable {
    struct Port {
        unsigned references=0,messages=0,resets=0;
        std::map<std::string,unsigned> properties;
    } ports[2];
    Port *entries[2]{};
    bool complete=false,fault=false;
    unsigned propertyCalls=0,failAt=0;
    bool property(unsigned number,const char *key,unsigned value) {
        if (++propertyCalls==failAt) return false;
        ports[number-1].properties[key]=value;return true;
    }
    void publish(unsigned number) { auto &p=ports[number-1];assert(!p.references);p.references=1;entries[number-1]=&p; }
    void message() { for (auto *p:entries) if (p) ++p->messages; }
    void reset() { for (auto *p:entries) if (p) ++p->resets; }
    bool admits(unsigned number,const XHCI::ConnectorPorts &policy) const {
        return complete && !fault && number && number<=2 && entries[number-1] && policy.routed(number);
    }
    void stop() {
        complete=false;
        for (auto *&p:entries) if (p) { assert(p->references==1);--p->references;p=nullptr; }
    }
};
static void routedNativePorts() {
    using USB::Result;
    unsigned lifecycles=0,failures=0;
    for (bool reversed:{false,true}) for (unsigned ceiling:{3,4}) {
        auto io=companionCapabilities(reversed,0);XHCI::PortProtocols protocols;XHCI::ConnectorPorts policy;
        assert(protocols.initialize(io,2)==Result::Success && policy.initialize(protocols,2,ceiling)==Result::Success);
        USB::PlatformContract resources{2,31,120000000,19200000,40,ceiling,ceiling==4 ? 1000000u:500000u,0};
        USB::HostCurrent current;assert(current.configure(resources)==Result::Success);
        const unsigned hs=reversed ? 1:2,ss=reversed ? 2:1,expected=ceiling==4 ? 2:1;
        for (unsigned epoch=0;epoch<4;++epoch) {
            NativeRoutedTable family;
            for (unsigned n=1;n<=2;++n) {
                assert(!family.admits(n,policy)); // no creator before PortsCreated
                if (!policy.routed(n)) continue;
                const bool super=protocols.get(n)->major==3;
                assert(current.port(super,[&](const char *key,unsigned value) { return family.property(n,key,value); }));
                family.publish(n);
            }
            assert(family.propertyCalls==expected*3);
            family.complete=true;family.message();family.reset();
            assert(family.admits(hs,policy) && family.admits(ss,policy)==(ceiling==4));
            assert(family.ports[hs-1].properties.at("UsbConnector")==unsigned(ceiling==4 ? 9:8));
            assert(family.ports[hs-1].messages==1 && family.ports[hs-1].resets==1);
            if (ceiling==3) {
                assert(!family.entries[ss-1] && !family.ports[ss-1].references);
                assert(family.ports[ss-1].properties.empty() && !family.ports[ss-1].messages && !family.ports[ss-1].resets);
                assert(!policy.companion(hs,3) && !policy.routed(ss));
            } else {
                assert(family.ports[ss-1].properties.at("UsbConnector")==9);
                assert(policy.companion(hs,3)==ss && policy.companion(ss,2)==hs);
            }
            family.stop();
            assert(!family.admits(hs,policy) && !family.admits(ss,policy));
            for (const auto &port:family.ports) assert(!port.references);
            ++lifecycles;
        }
        for (unsigned position=1;position<=expected*3;++position) {
            NativeRoutedTable family;family.failAt=position;
            for (unsigned n=1;n<=2;++n) {
                if (!policy.routed(n)) continue;
                if (!current.port(protocols.get(n)->major==3,[&](const char *key,unsigned value) {
                    return family.property(n,key,value);
                })) { family.fault=true;break; }
                family.publish(n);
            }
            assert(family.fault && !family.complete && family.propertyCalls==position);
            assert(!family.admits(1,policy) && !family.admits(2,policy));
            // Partially started ports keep their factory reference until
            // Native stop; untouched and unrouted entries remain null.
            family.stop();for (const auto &port:family.ports) assert(!port.references);
            ++failures;
        }
    }
    assert(lifecycles==16 && failures==18);
    std::printf("Native routed ports: %u publication/current/cold-PM lifecycles and %u property failure positions passed\n",lifecycles,failures);
}

static void portLifecycle() {
    using USB::Result;using namespace XHCI::PortSC;
    PortMachine machine;auto &p=machine.usb2;auto &raw=machine.io.registers[0x450/4];
    assert(p.initialize(2,machine.protocols)==Result::Busy);
    uint32_t value=0;assert(p.status(value)==Result::Success && value==0x1b00);
    assert(machine.usb3.status(value)==Result::Success && value==0x1c00);
    raw=(raw&~(15u<<10))|(1u<<10);assert(p.status(value)==Result::Success && value==0x1900);
    raw=(raw&~(15u<<10))|(2u<<10);assert(p.status(value)==Result::Success && value==0x1a00);
    raw=(raw&~(15u<<10))|(3u<<10);
    raw|=ResetChange|ConnectChange|(1u<<18)|(7u<<25)|(2u<<14);
    const auto preserve=raw&~(Enabled|ResetChange);
    assert(p.reset(100)==Result::Pending && (raw&~Reset)==preserve);
    assert(p.suspend(101)==Result::Busy && p.advance(99)==Result::Invalid);
    assert(p.advance(101)==Result::Pending);
    machine.io.completed(2);
    assert(p.advance(1000)==Result::Pending && p.currentPhase()==XHCI::PortPhase::Recovery);
    assert(p.advance(10999)==Result::Pending && p.advance(11000)==Result::Success);
    assert(!(raw&ResetChange) && (raw&ConnectChange) && (raw&(1u<<18)) && (raw&(7u<<25))==(7u<<25));
    assert(p.reset(12000,true)==Result::Unsupported);
    assert(p.suspend(12000)==Result::Pending && p.advance(12001)==Result::Success);
    assert((raw&LinkMask)==(3u<<5));
    assert(p.resume(13000)==Result::Pending && (raw&LinkMask)==(15u<<5));
    assert(p.advance(32999)==Result::Pending && (raw&LinkMask)==(15u<<5));
    assert(p.advance(33000)==Result::Pending && !(raw&LinkMask));
    assert(p.advance(33001)==Result::Pending && p.advance(43000)==Result::Pending && p.advance(43001)==Result::Success);
    auto &ss=machine.usb3;auto &ssRaw=machine.io.registers[0x440/4];
    assert(ss.reset(50000,true)==Result::Pending && (ssRaw&WarmReset));machine.io.completed(1,true);
    assert(ss.advance(51000)==Result::Pending && ss.advance(61000)==Result::Success);
    assert(ss.suspend(62000)==Result::Pending && ss.advance(62001)==Result::Success);
    assert(ss.resume(63000)==Result::Pending && !(ssRaw&LinkMask) && ss.advance(63001)==Result::Success);
    // PORTSC controls logical port power, independently of PMIC sourcing.
    raw|=OverCurrent;
    assert(p.power(true,70000)==Result::HardwareError);
    assert(p.power(false,70000)==Result::Pending && !(raw&Power));
    assert(p.advance(70001)==Result::Success && (raw&OverCurrent));
    raw&=~OverCurrent;
    assert(p.power(true,71000)==Result::Pending && p.advance(71001)==Result::Success);
    assert(p.advance(72000)==Result::NotReady);
    const auto before=machine.io.writes.size();
    assert(machine.core.writePort(2,Enabled,0,0)==Result::Invalid);
    assert(machine.core.writePort(2,LinkMask,0,0)==Result::Invalid);
    assert(machine.core.writePort(2,0,0,LinkWrite)==Result::Invalid);
    assert(machine.core.writePort(2,0,0,Reset|WarmReset)==Result::Invalid);
    assert(machine.io.writes.size()==before);
}
static void portFailures() {
    using USB::Result;using namespace XHCI::PortSC;
    {
        PortMachine m;auto &raw=m.io.registers[0x450/4];raw|=ResetChange;m.io.ignoreCompletionClear=true;
        assert(m.usb2.reset(1)==Result::HardwareError && !(raw&Reset));
        assert(m.usb2.currentPhase()==XHCI::PortPhase::Fault);
    }
    {
        PortMachine m;assert(m.usb2.reset(10)==Result::Pending);
        const auto before=m.io.writes.size();
        assert(m.usb2.advance(300010)==Result::Timeout && m.io.writes.size()==before);
        assert(m.usb2.reset(300011)==Result::HardwareError && m.usb2.advance(300011)==Result::HardwareError);
    }
    {
        PortMachine m;assert(m.usb2.reset(10)==Result::Pending);auto &raw=m.io.registers[0x450/4];
        raw&=~Connected;assert(m.usb2.advance(11)==Result::Aborted);
    }
    {
        PortMachine m;assert(m.usb2.reset(10)==Result::Pending);m.io.completed(2);
        assert(m.usb2.advance(11)==Result::Pending);m.io.registers[0x450/4]&=~Enabled;
        assert(m.usb2.advance(10011)==Result::HardwareError);
    }
    {
        PortMachine m;assert(m.usb2.suspend(10)==Result::Pending);m.io.failedRead=true;
        assert(m.usb2.advance(11)==Result::HardwareError && m.core.currentState()==XHCI::State::Fault);
    }
    {
        PortMachine m;assert(m.usb2.reset(UINT64_MAX)==Result::Invalid);
        auto &raw=m.io.registers[0x450/4];raw=(raw&~(15u<<10))|(15u<<10);
        uint32_t status=0;assert(m.usb2.status(status)==Result::Unsupported);
        raw&=~(15u<<10);assert(m.usb2.status(status)==Result::Pending && m.usb2.currentPhase()==XHCI::PortPhase::Idle);
    }
}
void xhciHostContextsAndCommands();
void xhciHostTransfers();
void xhciEventTests();
void xhciSlotTests();
void xhciPipeTests();
void xhciRequestTests();
void xhciIsochronousTests();
void xhciPowerTests();
void xhciHubTests();
static void portChangeOwnership() {
    using USB::Result;using namespace XHCI::PortSC;
    PortMachine m;auto &raw=m.io.registers[0x450/4];
    raw|=ConnectChange|LinkChange|ResetChange;
    assert(m.usb2.reset(10)==Result::Pending && !(raw&ResetChange));
    m.io.completed(2);uint32_t observed=0;
    assert(m.usb2.acknowledgeChanges(observed)==Result::Success);
    assert((observed&(ConnectChange|LinkChange|ResetChange))==(ConnectChange|LinkChange|ResetChange));
    assert(!(raw&(ConnectChange|LinkChange)) && (raw&ResetChange));
    assert(m.usb2.advance(11)==Result::Pending);
    assert(m.usb2.acknowledgeChanges(observed)==Result::Success && (raw&ResetChange));
    assert(m.usb2.advance(10011)==Result::Success && !(raw&ResetChange));
    raw|=ConnectChange|WarmResetChange;
    assert(m.usb2.acknowledgeChanges(observed)==Result::Success && !(raw&Changes));
    const auto writes=m.io.writes.size();
    assert(m.usb2.acknowledgeChanges(observed)==Result::Success && !observed && m.io.writes.size()==writes);
}
int main() {
    boundaries(); producer(); events(); controller();protocolCapabilities();nativeCompanions();routedNativePorts();portLifecycle();portFailures();
    xhciHostContextsAndCommands();
    xhciHostTransfers();
    xhciEventTests();
    xhciSlotTests();
    xhciPipeTests();
    xhciRequestTests();
    xhciIsochronousTests();
    xhciPowerTests();
    xhciHubTests();
    portChangeOwnership();
    std::puts("xHCI: rings/ports, contexts, commands, transfer TDs/ED, recovery, bounded event dispatch and interrupt acknowledgement passed");
}
