// SPDX-License-Identifier: GPL-2.0-only
#include "PMICUSB.h"
#include "USBConnectorProtocol.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

using USB::Result;
namespace R = PMICUSB::Reg;
struct Bus {
    std::array<uint8_t, 65536> bytes{};
    std::vector<std::pair<uint16_t, uint8_t>> writes;
    std::function<void()> mutation;
    uint64_t edge = 0, faultSerial = 0, timeOffset = 0;
    unsigned calls = 0, failAt = 0, mutateAt = 0, elapsed = 0, externalMV = 0;
    uint16_t unlocked = 0;
    bool failApplied = false, failAll = false, noADC = false, noBoost = false, stuckSource = false;
    bool incoherentADC = false, alternate = false, ignoreCC = false, overCurrentOnEnable = false;
    bool stuckVCONN = false, vconnOCOnEnable = false;
    explicit Bus(bool host = true) {
        bytes[R::Type] = 0x51; bytes[R::Subtype] = 0x15;
        bytes[0x0103] = 1;
        bytes[R::TypeCConfig] = 0x80; bytes[R::OTGConfig] = 0xc7;
        bytes[R::USBSuspend] = 0xa2; bytes[R::SoftStart] = 0xb0;
        bytes[0x4552] = 0x40; bytes[0x4591] = 2;
        attach(host);
    }
    void attach(bool host, bool reverse = false) {
        bytes[R::TypeCStatus] = host ? 0 : 0x80;
        bytes[R::TypeCStatus + 1] = host ? 8 : 0;
        bytes[R::TypeCStatus + 3] = 0x11 | (host ? 0x80 : 0x40) | (reverse ? 2 : 0);
        externalMV = host ? 0 : 5000;
        bytes[R::USBIRQ] = host ? 0 : 0x10;
    }
    bool failure() {
        ++calls;
        if (mutateAt == calls && mutation) mutation();
        return failAll || (failAt && calls == failAt);
    }
    Result read(uint16_t address, uint8_t *out, unsigned count) {
        assert(count && count <= 8 && (address & 255) + count <= 256);
        if (failure()) return Result::Timeout;
        if (address == 0x4592) bytes[address] = !noADC && (bytes[0x4552] & 8) && (bytes[0x4591] & 0x80) ? 3 : 0;
        if (address == 0x4594) {
            const unsigned mv = (bytes[R::OTGStatus] & 15) == 10 ? 5000 : externalMV;
            unsigned raw = mv * 1024 / 20000;
            if (incoherentADC && (alternate = !alternate)) ++raw;
            bytes[address] = uint8_t(raw); bytes[address + 1] = uint8_t(raw >> 8);
        }
        for (unsigned i = 0; i < count; ++i) out[i] = bytes[address + i];
        return Result::Success;
    }
    Result write(uint16_t address, const uint8_t *data, unsigned count) {
        assert(count == 1);
        const bool fail = failure();
        if (fail && !failApplied) return Result::Timeout;
        writes.emplace_back(address, data[0]);
        if ((address & 255) == 0xd0) {
            assert(data[0] == 0xa5); unlocked = address & 0xff00;
        } else if ((address & 255) >= 0xa0) {
            assert(unlocked == (address & 0xff00)); unlocked = 0;
        }
        const auto before=bytes[address];
        bytes[address] = data[0];
        if (address==0x1368) {
            if (stuckVCONN && (before&8) && !(data[0]&8)) bytes[address]|=8;
            if (vconnOCOnEnable && !(before&8) && (data[0]&8)) {
                bytes[R::TypeCStatus+3]|=4;++edge;++faultSerial;
            }
        }
        if (address == R::OTGCommand) {
            assert(!(data[0] & 1) || (bytes[R::USBSuspend] & 1));
            if (data[0] & 1) {
                bytes[R::OTGStatus] = noBoost ? 1 : 10;
                if (!noBoost) bytes[R::TypeCStatus + 3] |= 0x40;
                if (overCurrentOnEnable) {
                    bytes[R::OTGStatus] = 0; bytes[R::OTGIRQ] |= 2;
                    bytes[R::TypeCStatus + 3] &= ~0x40;
                    ++edge; ++faultSerial;
                }
            } else if (!stuckSource) {
                bytes[R::OTGStatus] = 0;
                if (externalMV < 3600) bytes[R::TypeCStatus + 3] &= ~0x40;
            }
        }
        return fail ? Result::Timeout : Result::Success;
    }
    void delay(unsigned ms) { elapsed += ms; }
    uint64_t edges() const { return edge; }
    uint64_t faults() const { return faultSerial; }
    uint64_t nowMS() const { return timeOffset + elapsed; }
    void suppressCC(bool value) { if (value && !ignoreCC) ++edge; ignoreCC = value; }
    void inject(unsigned call, std::function<void()> action) { mutateAt = calls + call; mutation = action; }
};
const PMICUSB::Config config{2, 0x4500, 500000};

static void protocolAndDecode() {
    assert(config.valid());
    auto c = config; c.sourceMicroamps = 300000; assert(!c.valid());
    c = config; c.adcBase = 0x4501; assert(!c.valid());
    c = config; c.sid = 16; assert(!c.valid());
    c = config; c.vconn = 2; assert(!c.valid());
    USBConnector::Request r{};
    r.operation = USBConnector::Operation::Acquire; assert(USBConnector::valid(r));
    r.version=1;assert(!USBConnector::valid(r));r.version=USBConnector::Version;
    r.size=48;assert(!USBConnector::valid(r));r.size=sizeof(r);
    for (unsigned budget : {0, 500000, 750000, 1000000, 1250000, 1500000}) {
        assert(USBConnector::validBudget(budget));
        r.sourceMicroamps=budget;assert(USBConnector::valid(r)==!budget);
    }
    r.sourceMicroamps=0;r.budgetReserved=1;assert(!USBConnector::valid(r));r.budgetReserved=0;
    for (unsigned budget : {1u, 499999u, 900000u, 1500001u, UINT32_MAX}) assert(!USBConnector::validBudget(budget));
    r.lease = 1; assert(!USBConnector::valid(r));
    r.operation = USBConnector::Operation::Sample; assert(USBConnector::valid(r));
    r.size = 24; assert(!USBConnector::valid(r));
    Bus bus; PMICUSB::Hardware<Bus> hw(bus); assert(hw.configure(config) == Result::Success);
    assert(bus.bytes[R::OTGConfig] == 0xe0 && bus.bytes[R::TypeCInterrupts] == 4);
    USB::ConnectorState s{}; assert(hw.sample(s) == Result::Success);
    assert(s.host && s.power == USB::PowerRole::Source && s.orientation == USB::Orientation::Normal && s.connection);
    r = {}; r.lease = 1; r.operation = USBConnector::Operation::Source;
    r.source = true; r.next = USB::Role::Host; r.connector = s; assert(USBConnector::valid(r));
    r.connector.power = USB::PowerRole::Sink; assert(!USBConnector::valid(r));
    for (unsigned mode : {1, 2, 4}) {
        bus.bytes[R::TypeCStatus + 1] = mode;
        assert(hw.sample(s) == Result::Unsupported);
    }
    bus.bytes[R::TypeCStatus + 1] = 8;
    bus.bytes[R::TypeCStatus + 3] &= ~0x10;
    assert(hw.sample(s) == Result::NotReady);
    bus.attach(false, true); assert(hw.sample(s) == Result::Success);
    assert(!s.host && s.vbus && s.power == USB::PowerRole::Sink && s.orientation == USB::Orientation::Reverse);
    for (unsigned current : {0x80, 0x40, 0x20}) {
        bus.bytes[R::TypeCStatus] = current; assert(hw.sample(s) == Result::Success);
    }
    bus.bytes[R::TypeCStatus] = 0xc0; assert(hw.sample(s) == Result::Unsupported);
    assert(hw.release() == Result::Success && !hw.holds());
    bus.bytes[R::Subtype] = 0x1b; assert(hw.configure(config) == Result::Unsupported);
    bus.bytes[R::Subtype] = 0x15; bus.bytes[R::TypeCConfig] |= 1;
    assert(hw.configure(config) == Result::Unsupported);
    bus.bytes[R::TypeCConfig] &= ~1; bus.bytes[R::OTGCommand] = 1;
    assert(hw.configure(config) == Result::Success && !(bus.bytes[R::OTGCommand]&1));
    assert(hw.release() == Result::Success);
    bus.bytes[R::OTGCommand] = 0; bus.bytes[R::OTGStatus] = 10;
    assert(hw.configure(config) == Result::Busy && !hw.holds());
}

static void dormantFirmwareRequest() {
    for (unsigned command : {1u, 0x81u}) {
        Bus bus(false);bus.bytes[R::OTGCommand]=command;
        PMICUSB::Hardware<Bus> hw(bus);
        assert(hw.configure(config)==Result::Success);
        assert(bus.writes.front()==std::make_pair(R::OTGCommand,uint8_t(command&~1u)));
        assert(bus.bytes[R::OTGCommand]==(command&~1u) && bus.externalMV==5000);
        assert(hw.release()==Result::Success);
    }
    for (unsigned status=1;status<8;++status) {
        Bus bus(false);bus.bytes[R::OTGCommand]=1;bus.bytes[R::OTGStatus]=status;
        PMICUSB::Hardware<Bus> hw(bus);
        assert(hw.configure(config)==Result::Busy && !hw.holds() && bus.writes.empty());
    }
    // Every failed initial read leaves the dormant firmware request untouched.
    for (unsigned call=1;call<=6;++call) {
        Bus bus(false);bus.bytes[R::OTGCommand]=1;bus.failAt=call;
        PMICUSB::Hardware<Bus> hw(bus);
        assert(hw.configure(config)==Result::Timeout);
        assert((bus.bytes[R::OTGCommand]&1) && bus.writes.empty());
        if (hw.holds()) assert(hw.release()==Result::Success);
    }
    // Lost ACK, command readback failure, and status recheck failure all
    // retain the lease, including when the source-off write reached hardware.
    for (unsigned call : {7u,8u,9u}) for (bool applied : {false,true}) {
        Bus bus(false);bus.bytes[R::OTGCommand]=1;bus.failAt=call;bus.failApplied=applied;
        PMICUSB::Hardware<Bus> hw(bus);
        assert(hw.configure(config)==Result::Timeout && hw.holds() && hw.failed());
        assert(hw.release()==Result::Success && !hw.holds() && !(bus.bytes[R::OTGCommand]&1));
    }
    Bus raced(false);raced.bytes[R::OTGCommand]=1;
    raced.inject(9,[&] {raced.bytes[R::OTGStatus]=2;});
    PMICUSB::Hardware<Bus> hw(raced);
    assert(hw.configure(config)==Result::HardwareError && hw.holds() && hw.failed());
    assert(hw.release()==Result::Timeout && hw.holds());
    raced.bytes[R::OTGStatus]=0;
    assert(hw.release()==Result::Success && !(raced.bytes[R::OTGCommand]&1));
}

static void generation() {
    Bus bus(false); PMICUSB::Hardware<Bus> hw(bus); assert(hw.configure(config) == Result::Success);
    USB::ConnectorState a{}, b{}; assert(hw.sample(a) == Result::Success);
    assert(hw.sample(b) == Result::Success && b.sameRoute(a));
    bus.edge += 4; // detach, attach and VBUS edges coalesced before one Sample
    assert(hw.sample(b) == Result::Success && b.connection == a.connection + 4);
    a = b; bus.bytes[R::TypeCStatus + 3] &= ~0x40;
    assert(hw.sample(b) == Result::Success && b.connection == a.connection + 1 && b.desired() == USB::Role::None);
    a = b; bus.bytes[R::TypeCStatus + 3] |= 0x40;
    assert(hw.sample(b) == Result::Success && b.connection == a.connection);
    a = b; bus.failAt = bus.calls + 1;
    assert(hw.sample(b) == Result::Timeout && !b.connection);
    assert(hw.sample(b) == Result::Success && b.connection == a.connection + 1);
    a = b; bus.inject(2, [&] { ++bus.edge; });
    assert(hw.sample(b) == Result::Busy);
    assert(hw.sample(b) == Result::Success && b.connection == a.connection + 1);
    a = b; bus.attach(false, true);
    assert(hw.sample(b) == Result::Success && b.connection == a.connection + 1);
    bus.edge = UINT64_MAX;
    assert(hw.sample(b) == Result::Overflow && !b.connection);
}

static void power() {
    Bus bus; PMICUSB::Hardware<Bus> hw(bus); assert(hw.configure(config) == Result::Success);
    USB::ConnectorState s{}, powered{}; assert(hw.sample(s) == Result::Success);
    assert(hw.safePower(USB::Role::Host, s) == Result::Success);
    assert(hw.source(s) == Result::Success && hw.sourcing());
    assert(bus.bytes[R::OTGLimit] == 1 && bus.bytes[R::USBSuspend] == 0xa3);
    assert(bus.bytes[R::SoftStart] == 0xb3 && bus.bytes[R::OTGEngineering] == 1);
    assert(bus.bytes[0x4552] == 0x40 && bus.bytes[0x4591] == 2 && !(bus.bytes[0x4553] & 1));
    assert(hw.sample(powered) == Result::Success && powered.vbus && powered.sameRoute(s));
    assert(hw.source(s) == Result::Success); // our VBUS rise does not replace route
    assert(hw.stop() == Result::Success && !hw.sourcing());
    assert(bus.bytes[R::USBSuspend] == 0xa2 && !(bus.bytes[R::OTGEngineering] & 1));
    assert(hw.release() == Result::Success && !hw.holds());
    // Permission is per acquisition; the kernel service currently passes 0.
    auto noSource = config; noSource.sourceMicroamps = 0;
    assert(hw.configure(noSource) == Result::Success);
    const auto writes = bus.writes.size();
    assert(hw.source(s) == Result::Unsupported && bus.writes.size() == writes);
    assert(hw.release() == Result::Success);
    Bus inherited;
    inherited.bytes[R::OTGEngineering] = 0xc1;
    PMICUSB::Hardware<Bus> firmware(inherited);
    assert(firmware.configure(config) == Result::Success && firmware.sample(s) == Result::Success);
    assert(firmware.stop() == Result::Success && inherited.bytes[R::OTGEngineering] == 0xc1);
    assert(firmware.source(s) == Result::Success && firmware.release() == Result::Success);
    assert(inherited.bytes[R::OTGEngineering] == 0xc1);
    // External VBUS is never driven/discharged, even below the digital valid threshold.
    for (unsigned mv : {820, 2500, 5000, 9000}) {
        Bus upstream; upstream.externalMV = mv;
        PMICUSB::Hardware<Bus> charger(upstream); assert(charger.configure(config) == Result::Success);
        assert(charger.sample(s) == Result::Success);
        assert(charger.source(s) == Result::Busy && !(upstream.bytes[R::OTGCommand] & 1));
        assert(charger.release() == Result::Success && upstream.externalMV == mv);
    }
    Bus suspended; suspended.bytes[R::USBSuspend] |= 1;
    PMICUSB::Hardware<Bus> existing(suspended); assert(existing.configure(config) == Result::Success);
    assert(existing.sample(s) == Result::Success && existing.source(s) == Result::Success);
    assert(existing.release() == Result::Success && suspended.bytes[R::USBSuspend] == 0xa3);
}

static void faults() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        Bus bus; PMICUSB::Hardware<Bus> hw(bus); assert(hw.configure(config) == Result::Success);
        USB::ConnectorState s{}; assert(hw.sample(s) == Result::Success);
        const auto started = bus.elapsed;
        bus.noADC = mode == 0; bus.noBoost = mode == 1; bus.incoherentADC = mode == 2;
        assert(hw.source(s) == (mode == 2 ? Result::Busy : Result::Timeout));
        assert(!(bus.bytes[R::OTGCommand] & 1) && !hw.sourcing());
        assert(bus.bytes[R::USBSuspend] == 0xa2 && bus.bytes[0x4552] == 0x40 && bus.bytes[0x4591] == 2);
        assert(bus.elapsed - started <= 250);
        assert(hw.release() == Result::Success);
    }
    Bus bus; PMICUSB::Hardware<Bus> hw(bus); assert(hw.configure(config) == Result::Success);
    USB::ConnectorState s{}, now{}; assert(hw.sample(s) == Result::Success);
    bus.bytes[R::TypeCStatus + 3] &= ~0x10;
    assert(hw.safePower(USB::Role::Host, s) == Result::Aborted && hw.source(s) == Result::Aborted);
    bus.attach(true); assert(hw.sample(s) == Result::Success);
    ++bus.edge; assert(hw.source(s) == Result::Aborted && !hw.sourcing());
    assert(hw.sample(s) == Result::Success && hw.source(s) == Result::Success);
    bus.stuckSource = true;
    bus.bytes[R::OTGIRQ] = 2; ++bus.edge;
    assert(hw.sample(now) == Result::HardwareError && hw.failed() && !now.connection);
    assert(hw.release() == Result::Timeout && hw.holds() && hw.sourcing());
    assert(bus.bytes[R::USBSuspend] == 0xa3); // input stays isolated until boost stops
    bus.stuckSource = false; bus.bytes[R::OTGStatus] = 0;
    assert(hw.release() == Result::Success && !hw.holds() && bus.bytes[R::USBSuspend] == 0xa2);
}

static void transactionFailures() {
    // Inject a lost ACK at every bus operation in a complete source sequence,
    // both before and after a write takes effect. No successful enable may
    // escape with an unverified electrical state; cleanup is retryable.
    Bus baseline; PMICUSB::Hardware<Bus> good(baseline); USB::ConnectorState target{};
    assert(good.configure(config) == Result::Success && good.sample(target) == Result::Success);
    const unsigned begin = baseline.calls;
    assert(good.source(target) == Result::Success);
    const unsigned length = baseline.calls - begin;
    for (bool applied : {false, true}) for (unsigned n = 1; n <= length; ++n) {
        Bus bus; PMICUSB::Hardware<Bus> hw(bus); USB::ConnectorState s{};
        assert(hw.configure(config) == Result::Success && hw.sample(s) == Result::Success);
        bus.failAt = bus.calls + n; bus.failApplied = applied;
        assert(hw.source(s) != Result::Success);
        assert(hw.release() == Result::Success && !hw.holds());
        assert(!(bus.bytes[R::OTGCommand] & 1) && bus.bytes[R::USBSuspend] == 0xa2);
        assert(bus.bytes[0x4552] == 0x40 && bus.bytes[0x4591] == 2 && !(bus.bytes[0x4553] & 1));
    }
    // A route edge anywhere during activation prevents publication, including
    // the ADC conversion after the boost became electrically active.
    for (unsigned n = 1; n <= length; ++n) {
        Bus bus; PMICUSB::Hardware<Bus> hw(bus); USB::ConnectorState s{};
        assert(hw.configure(config) == Result::Success && hw.sample(s) == Result::Success);
        bus.inject(n, [&] { ++bus.edge; });
        const auto r = hw.source(s);
        assert(r == Result::Aborted || r == Result::Busy);
        assert(hw.release() == Result::Success && !(bus.bytes[R::OTGCommand] & 1));
    }
    // Persistent SPMI loss retains the lease's electrical hold for recovery.
    Bus bus; PMICUSB::Hardware<Bus> hw(bus);
    assert(hw.configure(config) == Result::Success && hw.sample(target) == Result::Success);
    assert(hw.source(target) == Result::Success); bus.failAll = true;
    assert(hw.release() != Result::Success && hw.holds() && hw.sourcing());
    bus.failAll = false; assert(hw.release() == Result::Success && !hw.holds());
    // Partial initialization and teardown retain enough state to finish later.
    Bus initialized; PMICUSB::Hardware<Bus> init(initialized);
    assert(init.configure(config) == Result::Success);
    const unsigned initLength = initialized.calls;
    for (bool applied : {false, true}) for (unsigned n = 1; n <= initLength; ++n) {
        Bus failed; PMICUSB::Hardware<Bus> device(failed);
        failed.failAt = n; failed.failApplied = applied;
        assert(device.configure(config) != Result::Success);
        assert(device.release() == Result::Success && !device.holds());
    }
    Bus stopping; PMICUSB::Hardware<Bus> stopped(stopping);
    assert(stopped.configure(config) == Result::Success && stopped.sample(target) == Result::Success);
    assert(stopped.source(target) == Result::Success);
    const unsigned stopBegin = stopping.calls;
    assert(stopped.release() == Result::Success);
    const unsigned stopLength = stopping.calls - stopBegin;
    for (bool applied : {false, true}) for (unsigned n = 1; n <= stopLength; ++n) {
        Bus failed; PMICUSB::Hardware<Bus> device(failed); USB::ConnectorState s{};
        assert(device.configure(config) == Result::Success && device.sample(s) == Result::Success);
        assert(device.source(s) == Result::Success);
        failed.failAt = failed.calls + n; failed.failApplied = applied;
        assert(device.release() != Result::Success && device.holds());
        assert(device.release() == Result::Success && !device.holds());
        assert(failed.bytes[R::USBSuspend] == 0xa2 && !(failed.bytes[R::OTGCommand] & 1));
    }
}

static void typeCInitialization() {
    for (unsigned budget : {0u, 500000u, 750000u, 1000000u, 1250000u, 1500000u}) {
        Bus bus(budget != 0); bus.bytes[0x1359] = 0xfe;
        bus.bytes[0x135a] = 0xfb; bus.bytes[0x1364] = 0xc1; bus.bytes[0x1368] = 0xff;
        PMICUSB::Hardware<Bus> hw(bus); auto c = config; c.sourceMicroamps = budget;
        assert(hw.configure(c) == Result::Success && bus.elapsed == 200);
        assert((bus.bytes[0x1359] & 15) == (budget == 1500000 ? 1 : 0));
        assert((bus.bytes[0x1359] & 0xf0) == 0xf0); // preserve calibration/analog fields
        assert((bus.bytes[0x135a] & 0x64) == (budget ? 4 : 0));
        assert(!(bus.bytes[0x1358] & 0x22) && !(bus.bytes[0x1364] & 0x80));
        assert((bus.bytes[0x1368] & 0x9f) == (budget ? 0x10 : 0x94));
        assert(bus.bytes[0x16e9] == (budget ? 0xa5 : 0));
        assert(hw.sourceAuthorized() == bool(budget));
        assert(hw.release() == Result::Success && !hw.sourceAuthorized());
        assert((bus.bytes[0x1368] & 0x9f) == 0x94 && !bus.bytes[0x16e9]);
    }
    // The v2 CC2 erratum must not run on unaffected revisions or on CC1.
    for (unsigned revision : {1, 2, 3}) {
        Bus bus(false); bus.bytes[0x0103] = revision;
        PMICUSB::Hardware<Bus> hw(bus); USB::ConnectorState s{};
        assert(hw.configure(config) == Result::Success && hw.sample(s) == Result::Success);
        bus.bytes[R::USBIRQ] = 0; ++bus.edge; // digital Type-C VBUS can remain stale-high
        assert(hw.sample(s) == Result::Success && !s.vbus && s.desired() == USB::Role::None && !bus.ignoreCC);
        assert(hw.release() == Result::Success);
    }
}

static void cc2Recovery() {
    const uint16_t addresses[5] = {0x1359, 0x135a, 0x1364, 0x1652, 0x1368};
    const uint8_t masks[5] = {0x0a, 4, 0x80, 0x20, 0x47};
    Bus bus(false); bus.attach(false, true); bus.bytes[0x0103] = 2;
    PMICUSB::Hardware<Bus> hw(bus); USB::ConnectorState s{}, lost{};
    assert(hw.configure(config) == Result::Success && hw.sample(s) == Result::Success);
    std::array<uint8_t, 5> before{};
    for (unsigned i = 0; i < 5; ++i) before[i] = bus.bytes[addresses[i]];
    const auto beginTime = bus.elapsed;
    bus.bytes[R::USBIRQ] = 0; ++bus.edge;
    assert(hw.sample(lost) == Result::NotReady && !lost.connection && bus.ignoreCC && !hw.sourceAuthorized());
    assert((bus.bytes[0x1368] & 6) == 6 && bus.elapsed == beginTime);
    assert((bus.bytes[0x1359] & 0x0a) == 2 && !(bus.bytes[0x135a] & 4));
    assert((bus.bytes[0x1364] & 0x80) && (bus.bytes[0x1652] & 0x20));
    assert(hw.safePower(USB::Role::Device, s) == Result::Aborted);
    assert(hw.stop() == Result::Success && bus.ignoreCC); // source-off must not cancel detection
    bus.elapsed += 9; assert(hw.sample(lost) == Result::NotReady && (bus.bytes[0x1368] & 6) == 6);
    bus.elapsed += 1; assert(hw.sample(lost) == Result::NotReady && (bus.bytes[0x1368] & 6) == 4);
    bus.elapsed += 29; assert(hw.sample(lost) == Result::NotReady && (bus.bytes[0x1368] & 6) == 4);
    bus.elapsed += 1; assert(hw.sample(lost) == Result::NotReady && (bus.bytes[0x1368] & 6) == 6); // still debounced
    bus.bytes[R::TypeCStatus + 3] &= ~0x10; bus.bytes[R::TypeCStatus + 4] |= 0x10;
    bus.elapsed += 10; assert(hw.sample(lost) == Result::NotReady && (bus.bytes[0x1368] & 6) == 4);
    bus.elapsed += 30; assert(hw.sample(lost) == Result::NotReady && (bus.bytes[0x1368] & 6) == 6); // timer stage remains
    bus.bytes[R::TypeCStatus + 3] = 0; bus.bytes[R::TypeCStatus + 4] = 0;
    bus.elapsed += 10; assert(hw.sample(lost) == Result::NotReady);
    bus.bytes[0x1364] |= 1; // unrelated field changes survive restoration
    bus.elapsed += 30; assert(hw.sample(lost) == Result::NotReady && !bus.ignoreCC);
    for (unsigned i = 0; i < 5; ++i) assert((bus.bytes[addresses[i]] & masks[i]) == (before[i] & masks[i]));
    assert(bus.bytes[0x1364] & 1);
    assert(hw.sample(lost) == Result::Success && !lost.attached && lost.connection > s.connection);
    assert(hw.release() == Result::Success);
    // A new upstream supply cancels the test and restores the original DRP
    // role; no stale pre-restoration sample is allowed to reopen admission.
    Bus replug(false); replug.attach(false, true); replug.bytes[0x0103] = 2;
    PMICUSB::Hardware<Bus> plugged(replug);
    assert(plugged.configure(config) == Result::Success && plugged.sample(s) == Result::Success);
    replug.bytes[R::USBIRQ] = 0; ++replug.edge;
    assert(plugged.sample(lost) == Result::NotReady && replug.ignoreCC);
    replug.bytes[R::USBIRQ] = 0x10; ++replug.edge;
    assert(plugged.sample(lost) == Result::NotReady && !replug.ignoreCC && !(replug.bytes[0x1368] & 7));
    assert(plugged.sample(lost) == Result::Success && lost.vbus && lost.connection > s.connection);
    assert(plugged.release() == Result::Success);
}

static void cc2FailureRecovery() {
    // Fail every operation of the snapshot/programming phase, both before and
    // after write side effects, then prove release can drain the partial test.
    Bus baseline(false); baseline.attach(false, true); baseline.bytes[0x0103] = 2;
    PMICUSB::Hardware<Bus> good(baseline); USB::ConnectorState s{};
    assert(good.configure(config) == Result::Success && good.sample(s) == Result::Success);
    baseline.bytes[R::USBIRQ] = 0;
    const auto begin = baseline.calls;
    assert(good.sample(s) == Result::NotReady && baseline.ignoreCC);
    const auto count = baseline.calls - begin;
    for (bool applied : {false, true}) for (unsigned n = 1; n <= count; ++n) {
        Bus bus(false); bus.attach(false, true); bus.bytes[0x0103] = 2;
        PMICUSB::Hardware<Bus> hw(bus);
        assert(hw.configure(config) == Result::Success && hw.sample(s) == Result::Success);
        bus.bytes[R::USBIRQ] = 0; bus.failAt = bus.calls + n; bus.failApplied = applied;
        assert(hw.sample(s) != Result::Success && !s.connection);
        assert(hw.release() == Result::Success && !bus.ignoreCC && !hw.holds());
        assert((bus.bytes[0x1368] & 0x9f) == 0x94);
    }
    Bus bus(false); bus.attach(false, true); bus.bytes[0x0103] = 2;
    PMICUSB::Hardware<Bus> hw(bus);
    assert(hw.configure(config) == Result::Success && hw.sample(s) == Result::Success);
    bus.bytes[R::USBIRQ] = 0;
    assert(hw.sample(s) == Result::NotReady && bus.ignoreCC);
    bus.failAll = true;
    assert(hw.release() != Result::Success && hw.holds() && hw.failed() && bus.ignoreCC);
    bus.failAll = false;
    assert(hw.release() == Result::Success && !hw.holds() && !bus.ignoreCC);
    // Both the timed role transition and the restoration phase must retain
    // their saved fields after an ambiguous transaction.
    for (bool restoring : {false, true}) {
        auto prepare = [&](Bus &b, PMICUSB::Hardware<Bus> &device) {
            b.attach(false, true); b.bytes[0x0103] = 2;
            assert(device.configure(config) == Result::Success && device.sample(s) == Result::Success);
            b.bytes[R::USBIRQ] = 0;
            assert(device.sample(s) == Result::NotReady && b.ignoreCC);
            b.elapsed += 10;
            if (restoring) {
                assert(device.sample(s) == Result::NotReady);
                b.bytes[R::TypeCStatus + 3] = 0; b.bytes[R::TypeCStatus + 4] = 0; b.elapsed += 30;
            }
        };
        Bus normal(false); PMICUSB::Hardware<Bus> reference(normal); prepare(normal, reference);
        const auto begin = normal.calls;
        assert(reference.sample(s) == Result::NotReady);
        const auto steps = normal.calls - begin;
        for (bool applied : {false, true}) for (unsigned n = 1; n <= steps; ++n) {
            Bus broken(false); PMICUSB::Hardware<Bus> device(broken); prepare(broken, device);
            broken.failAt = broken.calls + n; broken.failApplied = applied;
            assert(device.sample(s) != Result::Success && device.holds());
            assert(device.release() == Result::Success && !device.holds() && !broken.ignoreCC);
        }
    }
    // Backward time and counter exhaustion retain the electrical ownership.
    Bus clock(false); PMICUSB::Hardware<Bus> timed(clock);
    assert(timed.configure(config) == Result::Success && timed.sample(s) == Result::Success);
    --clock.elapsed; assert(timed.sample(s) == Result::Overflow && timed.failed());
    assert(timed.release() == Result::Success);
}

static void latchedFaultAndRouteLoss() {
    Bus bus; PMICUSB::Hardware<Bus> hw(bus); USB::ConnectorState s{}, now{};
    assert(hw.configure(config) == Result::Success && hw.sample(s) == Result::Success && hw.source(s) == Result::Success);
    // An OCP IRQ remains authoritative after the real-time bit has fallen.
    ++bus.edge; ++bus.faultSerial;
    assert(hw.sample(now) == Result::Success && now.overCurrent && !hw.sourcing());
    assert(!hw.sourceAuthorized() && now.desired() == USB::Role::None && !(bus.bytes[R::OTGCommand] & 1));
    assert(hw.release() == Result::Success && hw.configure(config) == Result::Success);
    assert(hw.sample(now) == Result::Success && now.overCurrent && !hw.sourceAuthorized()); // same cable, new lease
    bus.bytes[R::TypeCStatus + 3] = 0; ++bus.edge;
    assert(hw.sample(now) == Result::Success && !now.attached && !now.overCurrent);
    bus.attach(true); ++bus.edge;
    assert(hw.sample(s) == Result::Success && !s.overCurrent && hw.source(s) == Result::Success);
    // Same-role coalesced replug must cut boost, even with identical CC bytes.
    ++bus.edge;
    assert(hw.sample(now) == Result::Success && !hw.sourcing() && now.connection > s.connection);
    assert(!(bus.bytes[R::OTGCommand] & 1) && bus.bytes[R::USBSuspend] == 0xa2);
    assert(hw.source(s) == Result::Aborted);
    assert(hw.source(now) == Result::Success);
    // A data/power role change also cuts our old source before returning it.
    bus.attach(false); ++bus.edge;
    assert(hw.sample(now) == Result::Success && now.power == USB::PowerRole::Sink && !hw.sourcing());
    assert(!(bus.bytes[R::OTGCommand] & 1) && bus.externalMV == 5000);
    assert(hw.release() == Result::Success);
    Bus overload; PMICUSB::Hardware<Bus> limited(overload);
    assert(limited.configure(config) == Result::Success && limited.sample(s) == Result::Success);
    overload.overCurrentOnEnable = true;
    assert(limited.source(s) == Result::Aborted && !limited.sourcing());
    overload.bytes[R::OTGIRQ] = 0;
    assert(limited.sample(now) == Result::Success && now.overCurrent && !limited.sourceAuthorized());
    assert(limited.release() == Result::Success);
    Bus exhausted; PMICUSB::Hardware<Bus> counters(exhausted);
    assert(counters.configure(config) == Result::Success && counters.sample(s) == Result::Success);
    assert(counters.source(s) == Result::Success); exhausted.faultSerial = UINT64_MAX;
    assert(counters.sample(now) == Result::Overflow && !counters.sourcing() && !now.connection);
    assert(counters.release() == Result::Success);
}

static void powerLifecycle() {
    Bus bus; PMICUSB::Hardware<Bus> hw(bus); USB::ConnectorState before{}, after{};
    assert(hw.configure(config) == Result::Success && hw.sample(before) == Result::Success);
    assert(hw.source(before) == Result::Success && hw.resume() == Result::Success && hw.sourcing());
    assert(hw.suspend() == Result::Success && !hw.sourcing() && !hw.sourceAuthorized() && hw.holds());
    assert(!(bus.bytes[R::OTGCommand] & 1) && (bus.bytes[0x1368] & 0x9f) == 0x94);
    const auto calls = bus.calls;
    bus.failAll = true; // parent bus may be asleep after the child acknowledged
    assert(hw.stop() == Result::Success && hw.sample(after) == Result::NotReady);
    assert(hw.source(before) == Result::NotReady && bus.calls == calls);
    assert(hw.resume() != Result::Success && !hw.sourceAuthorized());
    bus.failAll = false; bus.elapsed += 100;
    assert(hw.resume() == Result::Success && hw.sourceAuthorized());
    assert(hw.sample(after) == Result::Success && after.connection > before.connection);
    assert(hw.source(before) == Result::Aborted && hw.source(after) == Result::Success);
    assert(hw.suspend() == Result::Success);
    bus.failAll = true;
    assert(hw.release() == Result::Success && !hw.holds()); // off proof survives bus suspension

    // Failure in either PM direction cannot open Source or drop ownership.
    for (bool down : {false, true}) {
        Bus baseline; PMICUSB::Hardware<Bus> reference(baseline);
        assert(reference.configure(config) == Result::Success && reference.sample(before) == Result::Success);
        assert(reference.source(before) == Result::Success);
        if (!down) assert(reference.suspend() == Result::Success);
        const auto begin = baseline.calls;
        assert((down ? reference.suspend() : reference.resume()) == Result::Success);
        const auto count = baseline.calls - begin;
        for (bool applied : {false, true}) for (unsigned n = 1; n <= count; ++n) {
            Bus broken; PMICUSB::Hardware<Bus> device(broken);
            assert(device.configure(config) == Result::Success && device.sample(before) == Result::Success);
            assert(device.source(before) == Result::Success);
            if (!down) assert(device.suspend() == Result::Success);
            broken.failAt = broken.calls + n; broken.failApplied = applied;
            assert((down ? device.suspend() : device.resume()) != Result::Success);
            assert(device.holds() && !device.sourceAuthorized());
            assert(device.release() == Result::Success && !device.holds());
            assert(!(broken.bytes[R::OTGCommand] & 1));
        }
    }
}

static void vconnLifecycle() {
    auto c=config;c.vconn=1;
    for (bool reverse:{false,true}) {
        Bus bus;bus.attach(true,reverse);bus.bytes[R::TypeCStatus+1]=4;
        PMICUSB::Hardware<Bus> hw(bus);USB::ConnectorState route{},after{};
        assert(hw.configure(c)==Result::Success && hw.sample(route)==Result::Success);
        assert((bus.bytes[0x1368]&0x38)==0x30 && (bus.bytes[0x135a]&0x30)==0x30);
        assert(!hw.vconnSourcing() && hw.source(route)==Result::Success && hw.vconnSourcing());
        assert((bus.bytes[0x1368]&0x58)==(reverse ? 0x18 : 0x58));
        assert(hw.source(route)==Result::Success);
        assert(hw.suspend()==Result::Success && !hw.sourcing() && !hw.vconnSourcing());
        assert(!(bus.bytes[0x1368]&8) && !(bus.bytes[R::OTGCommand]&1));
        assert(hw.resume()==Result::Success && hw.sample(after)==Result::Success && after.connection>route.connection);
        assert(hw.source(route)==Result::Aborted && hw.source(after)==Result::Success);
        // A powered-to-passive cable change is a new incarnation even if
        // the PMIC edge IRQ is coalesced and orientation remains unchanged.
        bus.bytes[R::TypeCStatus+1]=8;
        assert(hw.sample(route)==Result::Success && route.connection>after.connection);
        assert(!hw.sourcing() && !hw.vconnSourcing() && !(bus.bytes[0x1368]&8));
        assert(hw.release()==Result::Success && !hw.holds());
    }
    for (unsigned mode:{1u,2u}) {
        Bus bus;bus.bytes[R::TypeCStatus+1]=mode;
        PMICUSB::Hardware<Bus> hw(bus);USB::ConnectorState s{};
        assert(hw.configure(c)==Result::Success && hw.sample(s)==Result::Unsupported);
        assert(hw.release()==Result::Success && !(bus.bytes[0x1368]&8));
    }
    Bus bus;bus.bytes[R::TypeCStatus+1]=4;
    PMICUSB::Hardware<Bus> hw(bus);USB::ConnectorState s{};
    assert(hw.configure(c)==Result::Success && hw.sample(s)==Result::Success && hw.source(s)==Result::Success);
    bus.stuckVCONN=true;
    assert(hw.release()==Result::HardwareError && hw.holds() && hw.vconnSourcing());
    assert(!hw.sourcing() && !(bus.bytes[R::OTGCommand]&1)); // still stop boost when VCONN off fails
    bus.stuckVCONN=false;
    assert(hw.release()==Result::Success && !hw.holds() && !(bus.bytes[0x1368]&8));
    Bus oc;oc.bytes[R::TypeCStatus+1]=4;oc.vconnOCOnEnable=true;
    PMICUSB::Hardware<Bus> faulty(oc);
    assert(faulty.configure(c)==Result::Success && faulty.sample(s)==Result::Success);
    assert(faulty.source(s)==Result::Aborted && !faulty.sourcing() && !faulty.vconnSourcing());
    assert(faulty.sample(s)==Result::Success && s.overCurrent && s.desired()==USB::Role::None);
    assert(faulty.release()==Result::Success);
}

static void vconnAcknowledgements() {
    auto c=config;c.vconn=1;
    for (bool shutdown:{false,true}) {
        Bus baseline;baseline.bytes[R::TypeCStatus+1]=4;
        PMICUSB::Hardware<Bus> reference(baseline);USB::ConnectorState target{};
        assert(reference.configure(c)==Result::Success && reference.sample(target)==Result::Success);
        if (shutdown) assert(reference.source(target)==Result::Success);
        const auto begin=baseline.calls;
        assert((shutdown ? reference.release() : reference.source(target))==Result::Success);
        const auto count=baseline.calls-begin;
        for (bool applied:{false,true}) for (unsigned n=1;n<=count;++n) {
            Bus broken;broken.bytes[R::TypeCStatus+1]=4;
            PMICUSB::Hardware<Bus> device(broken);USB::ConnectorState s{};
            assert(device.configure(c)==Result::Success && device.sample(s)==Result::Success);
            if (shutdown) assert(device.source(s)==Result::Success);
            broken.failAt=broken.calls+n;broken.failApplied=applied;
            assert((shutdown ? device.release() : device.source(s))!=Result::Success);
            assert(device.release()==Result::Success && !device.holds());
            assert(!(broken.bytes[0x1368]&8) && !(broken.bytes[R::OTGCommand]&1));
        }
    }
}

int main() {
    dormantFirmwareRequest();
    protocolAndDecode(); generation(); power(); faults(); transactionFailures();
    typeCInitialization(); cc2Recovery(); cc2FailureRecovery(); latchedFaultAndRouteLoss();
    powerLifecycle();vconnLifecycle();vconnAcknowledgements();
    std::puts("USB connector: Type-C/Rp init, CC2 recovery, VBUS/VCONN direction and fault latches, PM transitions and lost-ACK recovery passed");
}
