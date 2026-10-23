// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandSignals.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
using namespace Baseband;
struct Item {
    uint8_t bytes[344]{};
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    unsigned notifications = 0, barriers = 0, changes = 0;
    bool failNotify = false, failStore = false;
    bool read32(uint32_t at, uint32_t &v) {
        assert(!(at & 3) && range(at, 4, sizeof(bytes)));
        v = Baseband::read32(bytes + at);
        if (at == 16 && changes) { --changes; write32(bytes + at, v ^ 1); }
        return true;
    }
    bool store32(uint32_t at, uint32_t v) {
        assert(!(at & 3) && range(at, 4, sizeof(bytes)));
        writes.emplace_back(at, v); write32(bytes + at, v); return !failStore;
    }
    void release() { ++barriers; }
    void acquire() { ++barriers; }
    bool notify() { ++notifications; assert(barriers); return !failNotify; }
};
int main() {
    SMP2P::ReadyGate gate;SMP2P::Observation ready{};ready.state=SMP2P::Ready;
    gate.baseline(true,ready);
    assert(gate.observe(ready)==Result::OK && !gate.ready()); // Retained READY is not a new boot.
    SMP2P::Observation cleared{};
    assert(gate.observe(cleared)==Result::OK && !gate.ready());
    assert(gate.observe(ready)==Result::OK && gate.ready());
    gate.baseline(true,ready);gate.invalidated();
    assert(gate.observe(ready)==Result::OK && gate.ready());
    gate.baseline(true,ready);ready.restart=true;
    assert(gate.observe(ready)==Result::OK && gate.ready());
    ready.restart=false;ready.rising=SMP2P::Fatal;
    assert(gate.observe(ready)==Result::IO);
    ready.rising=0;ready.state|=SMP2P::Fatal;
    assert(gate.observe(ready)==Result::OK && !gate.ready());
    Item out;
    assert(SMP2P::publish(out) == Result::OK);
    assert(out.writes.front() == std::make_pair(4u, 0u));
    assert(out.writes.back() == std::make_pair(4u, 0x101u));
    assert(out.notifications == 1 && read16(out.bytes + 14) == 1);
    assert(!memcmp(out.bytes + 20, "master-kernel", 14));
    write32(out.bytes + 16, 0x80); // Unrelated header flags survive SSR ACK updates.
    assert(SMP2P::acknowledge(out, 2) == Result::OK && read32(out.bytes + 16) == 0x82);
    const auto notifications = out.notifications;
    assert(SMP2P::acknowledge(out, 2) == Result::OK && out.notifications == notifications);
    assert(SMP2P::acknowledge(out, 1) == Result::Invalid);
    assert(SMP2P::stop(out, true) == Result::OK && read32(out.bytes + 36) == 1);
    assert(SMP2P::stop(out, false) == Result::OK && !read32(out.bytes + 36));
    Item in; memcpy(in.bytes, out.bytes, 340);
    write16(in.bytes + 8, 1); write16(in.bytes + 10, 0);
    memset(in.bytes + 20, 0, 16); memcpy(in.bytes + 20, "slave-kernel", 12);
    write32(in.bytes + 36, SMP2P::Ready);
    uint8_t snapshot[340]{};
    assert(SMP2P::snapshot(in, snapshot, sizeof(snapshot)) == Result::OK);
    SMP2P::Monitor monitor; SMP2P::Observation observation;
    assert(monitor.sample(snapshot, sizeof(snapshot), observation) == Result::OK && observation.rising == SMP2P::Ready);
    in.changes = 16;
    assert(SMP2P::snapshot(in, snapshot, sizeof(snapshot)) == Result::Busy);
    in.changes = 1;
    assert(SMP2P::snapshot(in, snapshot, sizeof(snapshot)) == Result::OK); // Stable retry after an SSR flag update.
    write32(in.bytes + 4, 0);
    assert(SMP2P::snapshot(in, snapshot, sizeof(snapshot)) == Result::NotReady);
    Item failed; failed.failNotify = true;
    assert(SMP2P::publish(failed) == Result::Quarantined);
    Item partial; partial.failStore = true;
    assert(SMP2P::publish(partial) == Result::Quarantined && partial.notifications == 0);
    out.failNotify = true;
    assert(SMP2P::stop(out, true) == Result::Quarantined);
    std::puts("baseband SMP2P publication/atomic snapshot/IRQ notification checks passed");
}
