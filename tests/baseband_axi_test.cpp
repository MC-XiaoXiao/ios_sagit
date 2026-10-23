// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandAXI.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <utility>
using namespace Baseband;
struct Bus {
    bool asserted = false, freezeClock = false, ignoreReset = false, dropClear = false;
    int blockedAck = -1, blockedIdle = -1;
    unsigned reads = 0, resets = 0, writeCount = 0, failWrite = 0, delays = 0;
    uint32_t invalidRegister = UINT32_MAX;
    uint64_t time = 0, ready[3]{100, 300, 500};
    uint32_t requests[3]{};
    Result resetResult = Result::OK, queryResult = Result::OK;
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    int port(uint32_t offset) {
        for (unsigned i=0; i<3; ++i) if (offset>=AXI::Ports[i] && offset<=AXI::Ports[i]+8) return int(i);
        assert(false); return -1;
    }
    bool read(uint32_t offset, uint32_t &value) {
        assert(AXI::registerOffset(offset, false)); ++reads;
        if (offset==invalidRegister) { value=UINT32_MAX; return true; }
        const auto i=port(offset);
        const auto reg=offset-AXI::Ports[i];
        if (reg==AXI::Request) value=requests[i];
        else if (reg==AXI::Acknowledge) value=requests[i] && blockedAck!=i && time>=ready[i];
        else value=blockedIdle!=i && (asserted || (requests[i] && time>=ready[i]+100));
        return true;
    }
    bool write(uint32_t offset, uint32_t value) {
        assert(AXI::registerOffset(offset, true) && value<=1);
        if (!value) assert(asserted);
        writes.emplace_back(offset,value);
        if (value || !dropClear) requests[port(offset)]=value;
        return ++writeCount!=failWrite;
    }
    Result reset(bool value) {
        assert(value && !asserted); ++resets;
        // No reset or halt release until every port has ACK and IDLE.
        for (unsigned i=0; i<3; ++i) {
            assert(requests[i] && blockedAck!=int(i) && blockedIdle!=int(i) && time>=ready[i]+100);
        }
        if (!ignoreReset) asserted=true;
        return resetResult;
    }
    Result resetState(bool &state) { state=asserted; return queryResult; }
    uint64_t nowUS() { return time; }
    void delayUS(unsigned us) { assert(us==100); ++delays; if (!freezeClock) time+=us; }
};
int main() {
    Bus io; AXI::Controller<Bus> bus(io);
    assert(bus.cold()==Result::NotReady && io.writes.empty());
    assert(bus.quiesceAndReset()==Result::OK && io.asserted && io.resets==1 && io.writes.size()==6);
    for (unsigned i=0; i<3; ++i) {
        assert(io.writes[i]==std::make_pair(AXI::Ports[i],1u));
        assert(io.writes[i+3]==std::make_pair(AXI::Ports[i],0u));
    }
    assert(bus.cold()==Result::OK && !bus.quarantined());
    Bus cold; cold.asserted=true; AXI::Controller<Bus> coldBus(cold);
    assert(coldBus.cold()==Result::OK && cold.writes.empty());
    assert(coldBus.quiesceAndReset()==Result::OK && cold.resets==0 && cold.writes.size()==3);
    for (int port=0; port<3; ++port) {
        Bus stuck; stuck.blockedAck=port; AXI::Controller<Bus> s(stuck);
        assert(s.quiesceAndReset()==Result::Quarantined && s.lastFailure()==Result::Timeout);
        assert(!stuck.resets && stuck.writes.size()==3 && stuck.time<=101000);
        for (auto req:stuck.requests) assert(req==1);
        const auto readCount=stuck.reads;
        assert(s.cold()==Result::Quarantined && s.quiesceAndReset()==Result::Quarantined && stuck.reads==readCount);
        Bus active; active.blockedIdle=port; AXI::Controller<Bus> a(active);
        assert(a.quiesceAndReset()==Result::Quarantined && !active.resets && active.writes.size()==3);
    }
    Bus timer; timer.freezeClock=true; timer.blockedAck=0; AXI::Controller<Bus> t(timer);
    assert(t.quiesceAndReset()==Result::Quarantined && timer.delays==1000);
    for (unsigned step=1; step<=6; ++step) {
        Bus broken; broken.failWrite=step; AXI::Controller<Bus> b(broken);
        assert(b.quiesceAndReset()==Result::Quarantined && b.lastFailure()==Result::IO);
        assert(broken.writes.size()==step);
    }
    Bus noReset; noReset.ignoreReset=true; AXI::Controller<Bus> n(noReset);
    assert(n.quiesceAndReset()==Result::Quarantined && noReset.writes.size()==3);
    Bus ambiguous; ambiguous.resetResult=Result::Timeout; AXI::Controller<Bus> u(ambiguous);
    assert(u.quiesceAndReset()==Result::Quarantined && ambiguous.asserted && ambiguous.writes.size()==3);
    Bus stale; stale.dropClear=true; AXI::Controller<Bus> z(stale);
    assert(z.quiesceAndReset()==Result::Quarantined && stale.asserted && stale.writes.size()==4);
    Bus invalid; invalid.invalidRegister=AXI::Ports[1]+8; AXI::Controller<Bus> v(invalid);
    assert(v.quiesceAndReset()==Result::Quarantined && v.lastFailure()==Result::Invalid && !invalid.resets);
    Bus stillActive; stillActive.asserted=true; stillActive.blockedIdle=2; AXI::Controller<Bus> c(stillActive);
    assert(c.cold()==Result::NotReady && stillActive.writes.empty());
    assert(c.quiesceAndReset()==Result::Quarantined && stillActive.writes.empty());
    assert(!AXI::registerOffset(0x3004,true) && !AXI::registerOffset(0x6000,false));
    std::puts("baseband three-port AXI drain/reset/ambiguous cleanup checks passed");
}
