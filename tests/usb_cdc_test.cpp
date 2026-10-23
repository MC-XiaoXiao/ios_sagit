// SPDX-License-Identifier: BSD-2-Clause
#include "USBCDCACM.h"
#include "DWC3Control.h"
#include "DWC3DeviceReset.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

using USB::CDCACM;
using USB::Result;
using USB::SetupPacket;
using USB::Speed;
using Action = CDCACM::Action;
static constexpr SetupPacket Address{0, 5, 7, 0, 0}, Configure{0, 9, 1, 0, 0};
static constexpr SetupPacket DTR{0x21, 0x22, 3, 0, 0};
static void configure(CDCACM &acm) { acm.commit(Address); acm.commit(Configure); }

static void descriptors() {
    CDCACM acm(0x1234, 0x5678); // synthetic IDs are test data only
    std::array<uint8_t, CDCACM::BufferBytes + 16> bytes{};
    auto *b = bytes.data();
    auto r = acm.setup({0x80, 6, 0x100, 0, 255}, Speed::High, b);
    assert(r.action == Action::Reply && r.length == 18);
    assert(b[0] == 18 && b[1] == 1 && b[4] == 2 && b[7] == 64);
    assert(b[8] == 0x34 && b[9] == 0x12 && b[10] == 0x78 && b[11] == 0x56);
    assert(b[16] == 0 && b[17] == 1); // no invented unique serial number
    for (auto speed : {Speed::Full, Speed::High}) for (unsigned type : {2u, 7u}) {
        bytes.fill(0xcc);
        r = acm.setup({0x80, 6, uint16_t(type << 8), 0, 65535}, speed, b);
        assert(r.action == Action::Reply && r.length == 67);
        assert(b[1] == type && b[2] == r.length && b[4] == 2);
        unsigned offset = 0, interfaces = 0, endpoints = 0, functional = 0;
        bool comm = false, data = false, unite = false;
        while (offset < r.length) {
            const auto *p = b + offset;
            assert(p[0] >= 2 && offset + p[0] <= r.length);
            if (p[1] == 4) {
                ++interfaces; assert(p[3] == 0);
                if (p[2] == 0) { assert(p[4] == 1 && p[5] == 2 && p[6] == 2); comm = true; }
                if (p[2] == 1) { assert(p[4] == 2 && p[5] == 10); data = true; }
            }
            if (p[1] == 5) {
                ++endpoints;
                if (p[3] == 3) { assert(p[2] == 0x82 && p[4] == 16); }
                else {
                    assert(p[2] == 1 || p[2] == 0x81);
                    const unsigned packet = p[4] | unsigned(p[5]) << 8;
                    assert(packet == (((speed == Speed::High) != (type == 7)) ? 512u : 64u));
                }
            }
            if (p[1] == 0x24) {
                ++functional;
                if (p[2] == 6) { assert(p[3] == 0 && p[4] == 1); unite = true; }
                if (p[2] == 2) assert(p[3] == 2); // no unsupported SEND_BREAK claim
            }
            offset += p[0];
        }
        assert(interfaces == 2 && endpoints == 3 && functional == 4 && comm && data && unite);
        for (unsigned n = 67; n < bytes.size(); ++n) assert(b[n] == 0xcc);
    }
    r = acm.setup({0x80, 6, 0x200, 0, 9}, Speed::High, b);
    assert(r.length == 9);
    assert(acm.setup({0x80, 6, 0x600, 0, 255}, Speed::High, b).length == 10);
    assert(acm.setup({0x80, 6, 0x300, 0, 255}, Speed::High, b).length == 4);
    r = acm.setup({0x80, 6, 0x302, 0x409, 255}, Speed::High, b);
    assert(r.action == Action::Reply && b[1] == 3 && b[3] == 0 && r.length == b[0]);
    assert(acm.setup({0x80, 6, 0x302, 0x410, 255}, Speed::High, b).action == Action::Stall);
    assert(acm.setup({0x80, 6, 0xf00, 0, 255}, Speed::High, b).action == Action::Stall);
    assert(acm.setup({0x80, 6, 0x201, 0, 255}, Speed::High, b).action == Action::Stall);
    assert(acm.setup({0x80, 6, 0x200, 1, 255}, Speed::High, b).action == Action::Stall);
    assert(acm.setup({0x80, 6, 0x100, 0, 255}, Speed::Super, b).action == Action::Stall);
    assert(!CDCACM::validIdentity(0, 1) && !CDCACM::validIdentity(0xffff, 1));
    assert(!CDCACM::validIdentity(0x1234, 0) && !CDCACM::validIdentity(0x1234, 65536));
    assert(CDCACM::validIdentity(0x1234, 0x5678));
}

static void requests() {
    CDCACM acm(1, 1); uint8_t b[CDCACM::BufferBytes]{};
    assert(acm.setup(Configure, Speed::High, b).action == Action::Stall);
    assert(acm.setup(Address, Speed::High, b).action == Action::Address);
    acm.commit(Address);
    assert(acm.setup(Configure, Speed::High, b).action == Action::Configuration);
    assert(!acm.configured()); // hardware action alone must not commit
    acm.commit(Configure);
    assert(acm.configured() && !acm.terminalOpen());
    assert(acm.setup(Address, Speed::High, b).action == Action::Stall);
    assert(acm.setup(DTR, Speed::High, b).action == Action::Status);
    assert(!acm.terminalOpen()); acm.commit(DTR); assert(acm.terminalOpen());
    assert(acm.needsNotification()); acm.serialState(b); assert(b[0] == 0xa1 && b[1] == 0x20 && b[6] == 2 && b[8] == 3);
    acm.notified(); assert(!acm.needsNotification());
    acm.requestNotification(); acm.serialState(b, true); assert(b[8] == 0x43);
    acm.commit({0x21, 0x22, 0, 0, 0}); assert(!acm.terminalOpen());
    const uint8_t coding[7] = {0x80, 0x25, 0, 0, 2, 2, 7};
    assert(acm.setup({0x21, 0x20, 0, 0, 7}, Speed::High, b).action == Action::ReceiveLineCoding);
    assert(acm.stageLineCoding(coding, 7));
    assert(acm.setup({0xa1, 0x21, 0, 0, 7}, Speed::High, b).length == 7 && std::memcmp(coding, b, 7));
    acm.commit({0x21, 0x20, 0, 0, 7});
    assert(acm.setup({0xa1, 0x21, 0, 0, 7}, Speed::High, b).length == 7 && !std::memcmp(coding, b, 7));
    assert(!acm.stageLineCoding(coding, 6));
    b[6] = 9; assert(!acm.stageLineCoding(b, 7));
    std::memcpy(b, coding, 7); b[4] = 3; assert(!acm.stageLineCoding(b, 7));
    std::memcpy(b, coding, 7); b[5] = 5; assert(!acm.stageLineCoding(b, 7));
    std::memcpy(b, coding, 7); b[0] = 1; assert(acm.stageLineCoding(b, 7)); acm.abortControl();
    acm.commit({0x21, 0x20, 0, 0, 7});
    assert(acm.setup({0xa1, 0x21, 0, 0, 7}, Speed::High, b).length == 7 && !std::memcmp(coding, b, 7));
    for (auto bad : std::array<SetupPacket, 11>{{
        {0x21, 0x20, 0, 1, 7}, {0x21, 0x20, 1, 0, 7}, {0x21, 0x20, 0, 0, 65535},
        {0x21, 0x22, 4, 0, 0}, {0x21, 0x22, 3, 0, 1}, {0x21, 0x23, 0, 0, 0},
        {0xa1, 0x21, 0, 1, 7}, {0x81, 10, 0, 2, 1}, {1, 11, 1, 1, 0},
        {2, 3, 0, 0x8f, 0}, {0, 3, 1, 0, 0}
    }}) assert(acm.setup(bad, Speed::High, b).action == Action::Stall);
    SetupPacket halt{2, 3, 0, 0x81, 0}, clear{2, 1, 0, 0x81, 0};
    assert(acm.setup(halt, Speed::High, b).endpoint == CDCACM::In);
    assert(!acm.stalled(CDCACM::In)); acm.commit(halt); assert(acm.stalled(CDCACM::In));
    assert(acm.setup({0x82, 0, 0, 0x81, 2}, Speed::High, b).length == 2 && b[0] == 1);
    acm.commit(clear); assert(!acm.stalled(CDCACM::In));
    acm.commit(halt); acm.commit({1, 11, 0, 1, 0}); assert(!acm.stalled(CDCACM::In));
    acm.commit({0, 9, 0, 0, 0}); assert(!acm.configured());
    assert(acm.setup(DTR, Speed::High, b).action == Action::Stall);
    acm.commit(Configure); acm.commit(DTR); acm.reset();
    assert(!acm.configured() && !acm.terminalOpen() && !acm.needsNotification());
}

// Exercise the real EP0 transport with CDC responses, including the rounded
// OUT packet that must never overwrite its seven-byte line-coding value.
struct Controller {
    bool halted = false;
    unsigned address = 7, configurations = 0, modifications = 0, starts = 0;
    unsigned endpoints = 6;
    unsigned endpointCount() const { return endpoints; }
    unsigned dmaBits() const { return 40; }
    bool stopped() const { return halted; }
    Result startTransfer(unsigned ep, uint64_t, DWC3::TRBType, uint32_t &reply) {
        ++starts; reply = ep ? 3u << 16 : 0; return Result::Success;
    }
    Result configureEndpoint(const DWC3::EndpointConfiguration &cfg, bool modify) {
        assert(cfg.valid() && cfg.physicalEndpoint < 2 && modify); ++modifications; return Result::Success;
    }
    Result beginEndpointConfiguration(unsigned resource) {
        assert(resource == 2 && !halted); ++configurations; return Result::Success;
    }
    Result setAddress(unsigned value) { address = value; return Result::Success; }
    Result command(unsigned, DWC3::Command cmd, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t &) {
        return cmd == DWC3::Command::End ? Result::Pending : Result::Success;
    }
};
struct Order { void dmaReadBarrier() {} void dmaWriteBarrier() {} };
struct Transport {
    Controller hw; Order order; CDCACM acm{1, 1};
    alignas(16) std::array<DWC3::TRB, 4> out{}, in{};
    alignas(8) std::array<uint8_t, 8> raw{};
    uint8_t data[512]{};
    DWC3::Control<Controller, Order> control{hw, order};
    DWC3::ControlNotification notice{};
    Transport() {
        assert(control.initialize({out.data(), in.data(), 0x10000, 0x11000, 4, raw.data(), 0x12000}, 64, 1) == Result::Success);
        assert(control.start() == Result::Success); configure(acm);
    }
    void complete(unsigned ep, unsigned remaining = 0) {
        auto &trbs = ep ? in : out; trbs[0].control &= ~DWC3::Owned; trbs[0].size = remaining;
        assert(control.handle({ep << 1 | 1u << 6}, 1, notice) == Result::Success);
    }
    uint64_t setup(const SetupPacket &s) {
        raw = {s.requestType, s.request, uint8_t(s.value), uint8_t(s.value >> 8),
            uint8_t(s.index), uint8_t(s.index >> 8), uint8_t(s.length), uint8_t(s.length >> 8)};
        complete(0); assert(notice.kind == DWC3::ControlNotificationKind::Setup); return notice.transaction;
    }
    void status(uint64_t token, unsigned ep) {
        assert(control.allowStatus(token) == Result::Pending);
        assert(control.handle({ep << 1 | 3u << 6 | 2u << 12}, 1, notice) == Result::Success);
        complete(ep); assert(notice.kind == DWC3::ControlNotificationKind::Status);
    }
};
static void transport() {
    Transport t;
    SetupPacket s{0x80, 6, 0x200, 0, 255};
    auto token = t.setup(s); auto r = t.acm.setup(s, Speed::High, t.data);
    DWC3::DMASegment segment{0x20000, r.length};
    assert(t.control.supplyData(token, &segment, 1, r.length) == Result::Success);
    t.complete(1); assert(t.notice.kind == DWC3::ControlNotificationKind::Data && t.notice.bytes == 67);
    t.status(token, 0);
    s = {0x21, 0x20, 0, 0, 7}; token = t.setup(s);
    assert(t.acm.setup(s, Speed::High, t.data).action == Action::ReceiveLineCoding);
    segment.length = 7;
    assert(t.control.supplyData(token, &segment, 1, 7) == Result::Invalid);
    segment.length = 64;
    assert(t.control.supplyData(token, &segment, 1, 7) == Result::Success);
    t.complete(0, 57); assert(t.notice.bytes == 7);
    uint8_t coding[7] = {0, 0xc2, 1, 0, 0, 0, 8};
    assert(t.acm.stageLineCoding(coding, t.notice.bytes)); t.status(token, 1); t.acm.commit(s);
    token = t.setup(DTR); assert(!t.acm.terminalOpen());
    t.status(token, 1); t.acm.commit(DTR); assert(t.acm.terminalOpen());
    // A role reset with outstanding SETUP cannot recycle descriptors first.
    assert(t.control.resetAfterHalt(t.notice) == Result::NotReady);
    t.hw.halted = true; assert(t.control.resetAfterHalt(t.notice) == Result::Success);
    t.acm.reset(); assert(!t.acm.terminalOpen());
}
static void receiveBackpressure() {
    const uint8_t input[] = {1, 2, 3, 4, 5}; uint8_t output[5]{};
    unsigned offset = 0, used = 0, capacity = 0;
    auto sink = [&](const uint8_t *bytes, unsigned count) {
        const unsigned accepted = count < capacity ? count : capacity;
        std::memcpy(output + used, bytes, accepted); used += accepted; return accepted;
    };
    assert(CDCACM::drainReceive(input, 5, offset, sink) == Result::Pending && offset == 0);
    capacity = 2;
    assert(CDCACM::drainReceive(input, 5, offset, sink) == Result::Pending && offset == 2);
    assert(CDCACM::drainReceive(input, 5, offset, sink) == Result::Pending && offset == 4);
    assert(CDCACM::drainReceive(input, 5, offset, sink) == Result::Success && offset == 5);
    assert(used == 5 && !std::memcmp(input, output, 5));
    assert(CDCACM::drainReceive(input, 4, offset, sink) == Result::Invalid);
    offset = 0;
    assert(CDCACM::drainReceive(input, 5, offset, [](const uint8_t *, unsigned n) { return n + 1; }) == Result::Invalid);
    assert(CDCACM::drainReceive(input, 513, offset, sink) == Result::Invalid);
}
static void busResetWithoutDetach() {
    Transport t;
    using TD = DWC3::Transfer<Controller, Order>;
    alignas(16) DWC3::TRB outTRBs[4]{}, inTRBs[4]{}, notifyTRBs[4]{};
    TD out(t.hw, t.order), in(t.hw, t.order), notify(t.hw, t.order);
    assert(out.initialize(2, outTRBs, 0x30000, 4, 1) == Result::Success);
    assert(in.initialize(3, inTRBs, 0x31000, 4, 1) == Result::Success);
    assert(notify.initialize(5, notifyTRBs, 0x32000, 4, 1) == Result::Success);
    DWC3::DeviceReset<Controller, decltype(t.control), TD> reset(t.hw, t.control, out, in, notify);
    // These are the actual adapter's reset backend and EP0/TD classes.
    // Same snapshot: USBRST -> CONNECT_DONE -> SETUP. Reset preserves SETUP
    // ownership; the connection's speed is applied after the snapshot and
    // the already-delivered request is still serviced on the original token.
    const unsigned starts = t.hw.starts;
    assert(reset.begin(1000, 0) == Result::Pending && !reset.stoppingControl());
    assert(t.control.currentState() == DWC3::ControlState::Setup && t.control.ownsBuffers());
    const Speed connected = Speed::Full;
    SetupPacket descriptor{0x80, 6, 0x100, 0, 18};
    auto token = t.setup(descriptor);
    assert(reset.finish(1001, connected) == Result::Success);
    assert(!reset.active() && !t.hw.halted && t.hw.address == 0 && t.hw.starts == starts);
    assert(t.control.currentState() == DWC3::ControlState::Dispatch && t.control.currentTransaction() == token);
    auto response = t.acm.setup(descriptor, connected, t.data);
    DWC3::DMASegment controlBytes{0x20000, response.length};
    assert(t.control.supplyData(token, &controlBytes, 1, response.length) == Result::Success);
    t.complete(1); t.status(token, 0);
    // A later reset interrupts DATA and both bulk directions. END acceptance
    // alone is not a reuse proof; racing XferComplete is also insufficient.
    DWC3::DMASegment outBytes{0x40000, 512}, inBytes{0x41000, 64};
    assert(out.submit(&outBytes, 1, 512, 64, DWC3::TRBType::Normal, 1) == Result::Success);
    assert(in.submit(&inBytes, 1, 64, 64, DWC3::TRBType::Normal, 2, true) == Result::Success);
    token = t.setup({0x21, 0x20, 0, 0, 7});
    controlBytes.length = 64;
    assert(t.control.supplyData(token, &controlBytes, 1, 7) == Result::Success);
    assert(reset.begin(2000, 1u << 3) == Result::Pending && reset.stoppingControl());
    assert(reset.finish(2001, Speed::High) == Result::Pending && t.control.ownsBuffers());
    DWC3::TransferCompletion done;
    assert(out.handle({2u << 1 | 1u << 6}, 1, done) == Result::Pending && out.ownsBuffers());
    assert(out.handle({2u << 1 | 7u << 6 | 8u << 24}, 1, done) == Result::Success);
    assert(in.handle({3u << 1 | 7u << 6 | 8u << 24}, 1, done) == Result::Success);
    assert(reset.finish(2002, Speed::High) == Result::Pending); // EP0 still owns DATA
    assert(t.control.handle({7u << 6 | 8u << 24}, 1, t.notice) == Result::Success);
    assert(t.control.currentState() == DWC3::ControlState::Stopped);
    assert(reset.finish(2003, Speed::High) == Result::Success);
    assert(!t.hw.halted && t.control.currentState() == DWC3::ControlState::Setup);
    assert(t.hw.configurations == 2 && t.hw.modifications == 4 && t.hw.address == 0);
    // A lost END cannot silently retire DMA. The caller may now halt and
    // isolate on this timeout, but the bus-reset backend never detaches.
    assert(out.submit(&outBytes, 1, 512, 64, DWC3::TRBType::Normal, 3) == Result::Success);
    assert(reset.begin(3000, 0) == Result::Pending);
    assert(reset.finish(12999, Speed::High) == Result::Pending);
    assert(reset.finish(13000, Speed::High) == Result::Timeout);
    assert(out.ownsBuffers() && !t.hw.halted && reset.active());
}
static void nativeEndpointSetReset() {
    Transport t;
    t.hw.endpoints = 32;
    using TD = DWC3::Transfer<Controller, Order>;
    alignas(16) DWC3::TRB rings[30][4]{};
    std::unique_ptr<TD> endpoints[30];
    DWC3::DeviceReset<Controller, decltype(t.control), TD> reset(t.hw, t.control);
    TD invalid(t.hw, t.order);
    assert(!reset.add(invalid)); // EP0 belongs to the control state machine.
    for (unsigned n = 0; n < 30; ++n) {
        endpoints[n].reset(new TD(t.hw, t.order));
        auto &td = *endpoints[n];
        assert(td.initialize(n + 2, rings[n], 0x30000 + n * 0x1000, 4, 1) == Result::Success);
        assert(reset.add(td) && !reset.add(td));
        DWC3::DMASegment bytes{0x80000 + n * 0x1000, 64};
        assert(td.submit(&bytes, 1, 64, 64, DWC3::TRBType::Normal, n + 1) == Result::Success);
    }
    const unsigned starts = t.hw.starts;
    assert(reset.begin(1000, 1u << 31) == Result::Pending);
    assert(!reset.add(*endpoints[0]) && !reset.stoppingControl());
    DWC3::TransferCompletion done;
    for (unsigned n = 0; n < 29; ++n) {
        assert(endpoints[n]->currentState() == DWC3::TransferState::Stopping);
        assert(endpoints[n]->handle({(n + 2) << 1 | 7u << 6 | 8u << 24}, 1, done) == Result::Success);
    }
    // The last physical endpoint is outside the CDC set. It must hold the
    // reset barrier until its real END arrives, even if XferComplete races.
    auto &last = *endpoints[29];
    assert(reset.finish(1001, Speed::High) == Result::Pending && last.ownsBuffers());
    assert(last.handle({31u << 1 | 1u << 6}, 1, done) == Result::Pending && last.ownsBuffers());
    assert(reset.finish(1002, Speed::High) == Result::Pending);
    assert(last.handle({31u << 1 | 7u << 6 | 8u << 24}, 1, done) == Result::Success);
    assert(reset.finish(1003, Speed::High) == Result::Success);
    assert(!reset.active() && !t.hw.halted && t.hw.address == 0 && t.hw.starts == starts);
    assert(t.control.currentState() == DWC3::ControlState::Setup && t.control.ownsBuffers());
}
int main() {
    descriptors(); requests(); transport(); receiveBackpressure(); busResetWithoutDetach(); nativeEndpointSetReset();
    std::puts("USB CDC ACM: descriptors, control policy, EP0 and reset tests passed");
}
