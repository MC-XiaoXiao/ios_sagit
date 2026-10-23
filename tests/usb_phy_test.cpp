// SPDX-License-Identifier: BSD-2-Clause
#include "USBPhy.h"
#include "USBPhyResources.h"
#include "USBPhyProtocol.h"
#include <PowerVoltagePolicy.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

using USB::Result;
namespace CC = ClockController;
struct Peer {
    struct Action { CC::Operation op; uint32_t id; uint64_t value; unsigned consumer; };
    std::vector<Action> calls;
    std::map<uint32_t, unsigned> clocks;
    std::map<uint32_t, bool> resets{{18,true}};
    std::map<uint32_t, bool> firmwarePins{{119,true},{120,true},{166,true},{170,true},{121,true}};
    std::map<uint32_t, QcomPower::VoltageVotes> votes;
    std::map<uint32_t, uint32_t> physicalVoltage;
    uint64_t auxHz = 1200000, pipeHz = 125000000;
    unsigned failAt = 0;
    CC::Operation failOperation = CC::GetName;
    uint32_t failID = UINT32_MAX;
    Result error = Result::NotReady;
    bool failAlways = false, appliedWithoutACK = false;
    bool unclamped=false,failClampOn=false,failClampOff=false;
    std::vector<bool> clampWrites;
    Result request(unsigned consumer, CC::Controller controller, CC::Operation op, uint32_t id, uint64_t &value) {
        calls.push_back({op, id, value, consumer});
        const bool fail = (failAt && calls.size() == failAt) ||
            (op == failOperation && id == failID);
        if (fail && !failAlways) { failAt = 0; failOperation = CC::GetName; failID = UINT32_MAX; }
        if (fail && !appliedWithoutACK) return error;
        if (op == CC::SetVoltage) {
            if (!value) assert(consumer == CC::USB2PHYConsumer ? resets[106] : resets[70] && resets[69]);
            if (consumer==CC::USB2PHYConsumer) assert(unclamped==bool(value));
            const auto status = votes[id].vote(0, consumer, uint32_t(value), [&](unsigned, uint32_t maximum) {
                physicalVoltage[id] = maximum;
                return fail ? QcomPower::TimedOut : QcomPower::OK;
            });
            if (status != QcomPower::OK) return status == QcomPower::TimedOut ? Result::Timeout : Result::HardwareError;
            uint32_t applied = 0; assert(votes[id].get(0, applied) == QcomPower::OK);
            value = applied; return Result::Success;
        }
        if (op == CC::GetCapabilities) value = controller == CC::GCC ? CC::Clocks | CC::Resets | CC::ClockHandoff : CC::Voltage;
        else if (op == CC::GetResetState) { assert(id==18);value=resets[id]; }
        else if (op == CC::AdoptClock) {
            assert(resets[18]);
            assert(consumer == CC::USB2PHYConsumer ? id==170 && resets[106] :
                (id==119 || id==120 || id==166) && resets[69] && resets[70]);
            firmwarePins[id]=false;
        }
        else if (op == CC::GetRate) value = id == 165 ? auxHz : pipeHz;
        else if (op == CC::SetRate) { assert(id == 165 && value == 1200000); auxHz = value; }
        else if (op == CC::AssertReset || op == CC::DeassertReset) resets[id] = op == CC::AssertReset;
        else if (op == CC::RetainClock) ++clocks[id];
        else if (op == CC::ReleaseClock) {
            assert(clocks[id]);
            assert(consumer == CC::USB2PHYConsumer ? resets[106] : resets[70] && resets[69]);
            --clocks[id];
        } else assert(false);
        return fail ? error : Result::Success;
    }
    uint32_t voltage(uint32_t id) { return physicalVoltage[id]; }
    bool idle() {
        for (auto c : clocks) if (c.second) return false;
        for (auto v : physicalVoltage) if (v.second) return false;
        return true;
    }
};
struct Backend {
    Peer &peer;
    unsigned consumer;
    Result request(CC::Controller controller, CC::Operation op, uint32_t id, uint64_t &value) {
        return peer.request(consumer, controller, op, id, value);
    }
    Result clamp(bool enabled) {
        assert(consumer==CC::USB2PHYConsumer);
        peer.clampWrites.push_back(enabled);
        if (enabled ? peer.failClampOn : peer.failClampOff) return Result::HardwareError;
        peer.unclamped=enabled;return Result::Success;
    }
};
struct Registers {
    Peer &peer;
    std::array<uint32_t, 0x1000 / 4> words{};
    bool usb3, noLock = false;
    uint32_t badRead = UINT32_MAX, badWrite = UINT32_MAX;
    unsigned elapsed = 0, readyAt = 0;
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    Registers(Peer &bus, bool ss) : peer(bus), usb3(ss) {
        if (ss) { words[0xc00 / 4] = 1; words[0xd74 / 4] = 0x40; words[0xa00 / 4] = 0x20; }
        else words[0x210 / 4] = 1;
    }
    uint32_t read(uint32_t offset) {
        assert(!(offset & 3) && offset < (usb3 ? 0x1000u : 0x2a8u));
        assert(peer.clocks[121]);
        return offset == badRead ? UINT32_MAX : words[offset / 4];
    }
    void write(uint32_t offset, uint32_t value) {
        assert(!(offset & 3) && offset < (usb3 ? 0x1000u : 0x2a8u));
        assert(peer.clocks[121]);
        writes.emplace_back(offset, value);
        if (offset == badWrite) return;
        words[offset / 4] = value;
        if ((usb3 && offset == 0xc08 && (value & 3) == 3) ||
            (!usb3 && offset == 0x210 && !(value & 1))) readyAt = elapsed + 400;
    }
    void delay(unsigned microseconds) {
        elapsed += microseconds;
        if (readyAt && elapsed >= readyAt && !noLock) {
            if (usb3) words[0xd74 / 4] &= ~0x40u;
            else words[0x1a0 / 4] |= 1;
        }
    }
};
struct Fixture {
    Peer peer;
    Backend backend;
    USBPHY::Resources<Backend> resources{backend};
    Registers io;
    USBPHY::USB2<Registers, decltype(resources)> usb2{io, resources};
    USBPHY::USB3<Registers, decltype(resources)> usb3{io, resources};
    Fixture(bool ss, bool differential = false) :
        backend{peer, ss ? CC::USB3PHYConsumer : CC::USB2PHYConsumer}, io(peer, ss) {
        const USBPHY::Supply rails[] = {{257, 880000}, {268, 1800000}, {280, 3300000}};
        assert(resources.configure(ss, differential, rails, ss ? 2 : 3));
    }
    void release() { assert(resources.release() == Result::Success && resources.empty() && peer.idle()); }
};

static void usb2Calibration() {
    {
        Fixture f(false);
        assert(f.usb2.start(0) == Result::NotReady && f.io.writes.empty() && f.peer.calls.empty());
    }
    for (unsigned trim : {0u, 1u, 12u, 15u}) {
        Fixture f(false);
        assert(f.resources.acquire() == Result::Success);
        assert(!f.peer.clocks[170]); // single-ended reference does not acquire this gate
        assert(f.usb2.start(trim) == Result::Success && f.usb2.ready());
        assert(f.io.words[0x23c / 4] == (trim ? trim << 4 | 5 : 0xa5));
        assert(f.io.words[0x240 / 4] == 9 && !(f.io.words[0x210 / 4] & 1));
        assert(f.io.elapsed >= 500 && f.usb2.start(trim) == Result::Busy);
        assert(f.usb2.stop() == Result::Success && !f.usb2.ready());
        assert((f.io.words[0x210 / 4] & 1) && f.peer.resets[106]);
        f.release();
    }
    Fixture f(false, true);
    assert(f.resources.acquire() == Result::Success && f.peer.clocks[170] == 1);
    assert(f.usb2.start(16) == Result::Invalid && f.io.writes.empty());
    f.release();
}

static void usb3Lanes() {
    {
        Fixture f(true);
        assert(f.usb3.start(USBPHY::Orientation::Normal) == Result::NotReady && f.io.writes.empty() && f.peer.calls.empty());
    }
    for (auto orientation : {USBPHY::Orientation::Normal, USBPHY::Orientation::Reverse}) {
        Fixture f(true);
        f.peer.auxHz = 19200000;
        assert(f.resources.acquire() == Result::Success && f.peer.auxHz == 1200000);
        assert(f.usb3.start(orientation) == Result::Success && f.usb3.ready());
        assert(f.io.words[0xa00 / 4] == (orientation == USBPHY::Orientation::Normal ? 2u : 3u));
        assert(f.io.words[0x034 / 4] == 4); // MSM8998 value, not generic QMP-v3's 8
        assert(f.io.words[0x28c / 4] == 0x16 && f.io.words[0x68c / 4] == 0x16);
        assert(f.io.words[0x504 / 4] == 0x43 && f.io.words[0x904 / 4] == 0x43);
        assert(f.io.words[0xdd8 / 4] == 0x8a && f.peer.clocks[120] == 1);
        assert((f.io.words[0xc04 / 4] & 1) && (f.io.words[0xc08 / 4] & 3) == 3);
        assert(f.usb3.stop() == Result::Success && !f.usb3.ready());
        assert((f.io.words[0xc00 / 4] & 1) && !(f.io.words[0xc04 / 4] & 1) && !(f.io.words[0xc08 / 4] & 3));
        assert(!f.peer.clocks[120]); f.release();
    }
}

static void missingPeersAndPartialAcquire() {
    for (bool ss : {false, true}) {
        Fixture golden(ss); assert(golden.resources.acquire() == Result::Success);
        const auto count = golden.peer.calls.size(); golden.release();
        // Inject one deterministic failure at each peer query, rail vote and
        // clock acquire, exercising the actual production rollback code.
        for (unsigned failure = 1; failure <= count; ++failure) {
            Fixture f(ss); f.peer.failAt = failure;
            const auto acquired = f.resources.acquire();
            if (golden.peer.calls[failure-1].op == CC::GetRate) assert(acquired == Result::Success);
            else {
                assert(acquired != Result::Success && !f.resources.powered());
                const auto started = ss ? f.usb3.start(USBPHY::Orientation::Normal) : f.usb2.start(0);
                assert(started == Result::NotReady && f.io.writes.empty());
            }
            f.release();
        }
    }
    Fixture f(false);
    f.peer.failAt = 1;
    assert(f.resources.acquire() == Result::NotReady);
    assert(f.resources.release() == Result::Success && f.peer.calls.size() == 1);
    assert(f.resources.acquire() == Result::Success); f.release(); // startup peer can appear later
}

static void stopProofAndRetry() {
    Fixture f(true);
    assert(f.resources.acquire() == Result::Success && f.usb3.start(USBPHY::Orientation::Normal) == Result::Success);
    f.peer.failOperation = CC::AssertReset; f.peer.failID = 70; f.peer.failAlways = true;
    assert(f.usb3.stop() == Result::NotReady && f.usb3.currentState() == USBPHY::State::Fault);
    assert(f.peer.clocks[120] && f.peer.clocks[121] && f.peer.voltage(257));
    assert(f.resources.release() == Result::NotReady && !f.resources.empty());
    assert(f.peer.resets[69]); // the second reset is attempted despite the first failure
    f.peer.failAlways = false; f.peer.failID = UINT32_MAX;
    assert(f.usb3.stop() == Result::Success);
    f.peer.failOperation = CC::ReleaseClock; f.peer.failID = 119;
    assert(f.resources.release() == Result::NotReady);
    assert(!f.peer.clocks[166] && f.peer.clocks[119] && f.peer.clocks[121] && f.peer.voltage(257));
    f.release();
    Fixture rail(false);
    assert(rail.resources.acquire() == Result::Success && rail.usb2.start(0) == Result::Success);
    assert(rail.usb2.stop() == Result::Success);
    rail.peer.failOperation = CC::SetVoltage; rail.peer.failID = 268;
    assert(rail.resources.release() == Result::NotReady);
    assert(!rail.peer.clocks[121] && !rail.peer.voltage(280) && rail.peer.voltage(268) && rail.peer.voltage(257));
    rail.release(); // already released clocks/rails are not decremented twice
}

static void hardwareFaults() {
    for (bool ss : {false, true}) for (unsigned fault = 0; fault < 3; ++fault) {
        Fixture f(ss); assert(f.resources.acquire() == Result::Success);
        if (!fault) f.io.noLock = true;
        if (fault == 1) f.io.badRead = ss ? 0xd74 : 0x1a0;
        if (fault == 2) f.io.badWrite = ss ? 0xc08 : 0x210;
        const auto r = ss ? f.usb3.start(USBPHY::Orientation::Normal) : f.usb2.start(0);
        assert(r == (!fault ? Result::Timeout : Result::HardwareError));
        assert((ss ? f.usb3.currentState() : f.usb2.currentState()) == USBPHY::State::Off);
        assert(f.io.elapsed <= 10350); f.release();
    }
    Fixture f(true); f.peer.pipeHz = 19200000;
    assert(f.resources.acquire() == Result::Success);
    assert(f.usb3.start(USBPHY::Orientation::Normal) == Result::NotReady);
    assert(!f.peer.clocks[120]); f.release();
}

static void voltageACKAndSharedVotes() {
    Fixture lost(false); lost.peer.error = Result::Timeout; lost.peer.appliedWithoutACK = true;
    lost.peer.failOperation = CC::SetVoltage; lost.peer.failID = 257;
    assert(lost.resources.acquire() == Result::Timeout && lost.peer.voltage(257));
    // Use the production shared VoltageVotes policy: RPM quarantines an
    // unknown acknowledgement. Reissuing zero cannot pretend to recover it.
    assert(lost.resources.release() == Result::HardwareError && !lost.resources.empty());
    assert(lost.peer.voltage(257));
    Fixture badClock(true); badClock.peer.error = Result::HardwareError;
    badClock.peer.failOperation = CC::RetainClock; badClock.peer.failID = 119;
    assert(badClock.resources.acquire() == Result::HardwareError);
    assert(badClock.resources.unknownClockState());
    assert(badClock.resources.release() == Result::HardwareError);
    assert(badClock.peer.clocks[121] && badClock.peer.voltage(257)); // retained for explicit clock recovery
    Peer shared;
    Backend a{shared, CC::USB2PHYConsumer}, b{shared, CC::USB3PHYConsumer};
    USBPHY::Resources<Backend> usb2(a), usb3(b);
    const USBPHY::Supply rails[] = {{257,880000},{268,1800000},{280,3300000}};
    assert(usb2.configure(false, false, rails, 3) && usb3.configure(true, false, rails, 2));
    assert(usb2.acquire() == Result::Success && usb3.acquire() == Result::Success);
    assert(shared.clocks[121] == 2);
    assert(usb2.release() == Result::Success);
    assert(shared.clocks[121] == 1 && shared.voltage(257) == 880000 && shared.voltage(268) == 1800000);
    assert(usb3.release() == Result::Success && shared.idle());
}

static void firmwareHandoff() {
    for (bool ss : {false,true}) {
        Fixture f(ss);
        f.peer.resets[18]=false;
        assert(f.resources.acquire()==Result::NotReady && f.resources.empty());
        assert(f.peer.firmwarePins[170] && f.peer.firmwarePins[119] && f.peer.firmwarePins[120]);
        for (const auto &call:f.peer.calls) assert(call.op==CC::GetCapabilities || call.op==CC::GetResetState);
        f.peer.resets[18]=true;
        assert(f.resources.acquire()==Result::Success);
        assert(ss ? !f.peer.firmwarePins[119] && !f.peer.firmwarePins[120] && !f.peer.firmwarePins[166] : !f.peer.firmwarePins[170]);
        assert(f.peer.firmwarePins[121]); // AHB2PHY is shared with other PHY users
        f.release();
        // Later orientation/data-role changes use the transferred lease.
        f.peer.resets[18]=false;
        const auto before=f.peer.calls.size();
        assert(f.resources.acquire()==Result::Success);
        for (size_t i=before;i<f.peer.calls.size();++i) assert(f.peer.calls[i].op!=CC::AdoptClock);
        f.release();
    }
    Fixture f(false);
    const USBPHY::Supply boardRails[]={{257,880000},{258,1200000},{268,1800000},{280,3088000}};
    Backend boardBackend{f.peer,CC::USB2PHYConsumer};
    USBPHY::Resources<Backend> board(boardBackend);
    assert(board.configure(false,true,boardRails,4));
    assert(board.acquire()==Result::Success && f.peer.voltage(258)==1200000);
    assert(board.release()==Result::Success && f.peer.idle());
    Fixture clamp(false);clamp.peer.failClampOn=true;
    assert(clamp.resources.acquire()==Result::HardwareError && !clamp.resources.empty());
    assert(clamp.peer.voltage(257)==0);
    clamp.peer.failClampOn=false;clamp.release();
    assert(!clamp.peer.unclamped && clamp.peer.clampWrites.size()==2);
    assert(clamp.resources.acquire()==Result::Success && clamp.peer.unclamped);
    clamp.peer.failClampOff=true;
    assert(clamp.resources.release()==Result::HardwareError && !clamp.resources.empty());
    assert(clamp.peer.clocks[121] && clamp.peer.voltage(257));
    clamp.peer.failClampOff=false;clamp.release();
}

static void boardRoleTuning() {
    // Pinned official board tables, converted to offset/value pairs. The
    // driver's profile selector is generic and carries no board identifier.
    const USBPHY::USB2Tuning::Entry deviceValues[]={{0,0x80},{4,0x13},{0x18c,0x7c},{0x2c,0x80},
        {0x184,0x0a},{0x23c,0xf0},{0x240,0x0d}};
    const USBPHY::USB2Tuning::Entry hostValues[]={{0x210,0x63},{4,0x13},{0x18c,0x7c},{0x2c,0x80},
        {0x184,0x0a},{0x21c,0x8c},{0x23c,5},{0x240,3},{0x218,0xff},{0x210,0x62}};
    USBPHY::USB2Tuning device{},host{};
    device.count=sizeof(deviceValues)/sizeof(deviceValues[0]);
    host.count=sizeof(hostValues)/sizeof(hostValues[0]);
    std::copy(std::begin(deviceValues),std::end(deviceValues),device.settings);
    std::copy(std::begin(hostValues),std::end(hostValues),host.settings);
    assert(device.valid(false) && host.valid(true) && !host.valid(false));
    for (unsigned trim : {0u,11u}) {
        Fixture f(false);
        assert(f.usb2.configure(device,host) && f.resources.acquire()==Result::Success);
        assert(f.usb2.start(trim,USB::Role::Device)==Result::Success);
        assert(f.io.words[0x23c/4]==(trim ? 0xb0 : 0xf0) && f.io.words[0x240/4]==0x0d);
        assert(!f.io.words[0xb4/4] && f.io.words[0]==0x80); // no unrelated default analog values
        assert(!f.usb2.configure(device,host));
        assert(f.usb2.stop()==Result::Success);
        f.io.writes.clear();f.io.readyAt=0;f.io.words[0x1a0/4]=0;
        assert(f.usb2.start(trim,USB::Role::Host)==Result::Success);
        assert(f.io.writes.size()==host.count);
        for (unsigned i=0;i<host.count;++i) assert(f.io.writes[i]==std::make_pair(host.settings[i].offset,host.settings[i].value));
        assert(f.io.words[0x23c/4]==5 && f.io.words[0x240/4]==3 && f.io.words[0x210/4]==0x62);
        assert(f.usb2.stop()==Result::Success);f.release();
    }
    Fixture f(false);
    auto bad=host;bad.settings[bad.count-1].value|=1;
    assert(!f.usb2.configure(device,bad));
    bad=host;bad.settings[2].offset=0x210;
    assert(!f.usb2.configure(device,bad));
    bad=device;bad.settings[0].offset=0x22c; // interrupt control is not a tuning register
    assert(!f.usb2.configure(bad,host));
    bad=device;bad.settings[0].value=256;
    assert(!f.usb2.configure(bad,host));
    bad.count=17;assert(!f.usb2.configure(bad,host));
    assert(f.usb2.start(0,USB::Role::None)==Result::Invalid && f.peer.calls.empty() && f.io.writes.empty());
}

int main() {
    USBPHY::Request request; assert(!USBPHY::valid(request));
    request.operation = USBPHY::Operation::Enable; assert(USBPHY::valid(request));
    request.role=unsigned(USB::Role::None);assert(!USBPHY::valid(request));
    request.role=unsigned(USB::Role::Host);assert(USBPHY::valid(request));
    request.version=1;assert(!USBPHY::valid(request));request.version=USBPHY::Version;
    request.orientation = 3; assert(!USBPHY::valid(request));
    usb2Calibration(); usb3Lanes(); missingPeersAndPartialAcquire(); stopProofAndRetry(); hardwareFaults(); voltageACKAndSharedVotes(); firmwareHandoff(); boardRoleTuning();
    std::puts("USB PHY: QFPROM trim, QUSB2/QMP startup, both Type-C lanes, clocks/rails, reset proof, rollback, timeout and shared votes passed");
}
