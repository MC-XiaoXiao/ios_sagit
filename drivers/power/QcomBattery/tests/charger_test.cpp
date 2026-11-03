// SPDX-License-Identifier: GPL-2.0-only
#include "PMI8998Charger.h"
#include "PMI8998Input.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <functional>
#include <vector>
using namespace PMICBattery;
struct Bus {
    std::array<uint8_t, 65536> bytes{};
    unsigned calls = 0, failAt = 0;
    bool lostACK = false, ignoreWrite = false;
    uint16_t ignoreAddress = 0;
    std::vector<uint16_t> writes;
    std::function<void(uint16_t)> beforeRead;
    Bus() {
        bytes[0x103] = 2; bytes[0x104] = 0x51; bytes[0x105] = 0x15;
        bytes[0x4005] = 0x10; bytes[0x4105] = 0x11;
        bytes[0x4046] = 0x80; bytes[0x4010] = 2;
        bytes[0x4009] = bytes[0x400a] = 128;
        bytes[0x4106] = 1; bytes[0x4150] = 0xa8; bytes[0x4151] = 4;
        bytes[0x41a0] = bytes[0x41a6] = 0; bytes[0x41a1] = bytes[0x41a7] = 125;
        bytes[0x41a2] = bytes[0x41a8] = 0; bytes[0x41a3] = bytes[0x41a9] = 8;
        bytes[0x160b] = 0x11; bytes[0x1006] = 2; bytes[0x100d] = 0x20;
        bytes[0x1042] = 0xa1;
    }
    bool read(uint8_t sid, uint16_t address, uint8_t *out, unsigned n) {
        assert(sid == 2 && n && n <= 8 && unsigned(address) + n <= bytes.size());
        if (++calls == failAt) return false;
        if (beforeRead) beforeRead(address);
        for (unsigned i = 0; i < n; ++i) out[i] = bytes[address + i];
        return true;
    }
    bool write(uint8_t sid, uint16_t address, const uint8_t *p, unsigned n) {
        assert(sid == 2 && (address == 0x1042 || address == 0x1051) && n == 1);
        if (address == 0x1051) assert(!(bytes[0x1042] & 1));
        if (address == 0x1042 && (*p & 1)) assert(!(bytes[0x1051] & 0xc0));
        writes.push_back(address);
        if (++calls == failAt) return false;
        if (!ignoreWrite && address != ignoreAddress) bytes[address] = *p;
        if (lostACK) { lostACK = false; return false; }
        return true;
    }
};
static Sample good() {
    Sample s{}; s.installed = s.external = s.temperatureValid = true;
    s.percent = 60; s.centiCelsius = 2500; return s;
}
int main() {
    auto s = good();
    // Settled input current is a hardware observation, separate from the
    // selected SDP ceiling and the inactive high-current configuration.
    for (unsigned mode : {0u, 2u, 4u, 6u}) {
        Bus b; b.bytes[0x1307] = 1; b.bytes[0x1308] = 1; b.bytes[0x130b] = 0x80;
        b.bytes[0x1366] = mode; b.bytes[0x1370] = 40; b.bytes[0x1607] = 20;
        InputStatus input;
        assert(readInputStatus(b, Config{}, input) == Result::Success && input.sdp);
        assert(input.sdpMA == (mode & 2 ? (mode & 4 ? 900u : 500u) : (mode & 4 ? 150u : 100u)));
        assert(input.settledMA == 500 && input.highCurrentMA == 1000 && !input.highCurrent);
        assert(b.writes.empty());
        b.bytes[0x1308] = 8;
        assert(readInputStatus(b, Config{}, input) == Result::Success && !input.sdp);
    }
    { Bus baseline; InputStatus input;
      assert(readInputStatus(baseline, Config{}, input) == Result::Success);
      for (unsigned failure = 1; failure <= baseline.calls; ++failure) {
          Bus b; b.failAt = failure; input.settledMA = 77;
          assert(readInputStatus(b, Config{}, input) == Result::IOFailure && input.settledMA == 77);
          assert(b.writes.empty());
      }
      Bus b; b.bytes[0x1607] = 193; input.settledMA = 77;
      assert(readInputStatus(b, Config{}, input) == Result::OutOfRange && input.settledMA == 77);
      b.bytes[0x1607] = 20;
      b.beforeRead = [&](uint16_t address) { if (address == 0x1365) b.bytes[0x1366] ^= 2; };
      assert(readInputStatus(b, Config{}, input) == Result::Incoherent && input.settledMA == 77); }
    { Bus b; b.bytes[0x1051] = 0xc5; b.ignoreAddress = 0x1051;
      assert(chargeEnable(b, Config{}, true) == ControlResult::VerifyFailed);
      assert(!(b.bytes[0x1042] & 1));
      assert((b.writes == std::vector<uint16_t>{0x1042, 0x1051})); }
    { Bus b; b.bytes[0x1051] = 0x85;
      b.beforeRead = [&](uint16_t address) { if (address == 0x1042 && b.calls == 5) b.lostACK = true; };
      ChargeControl c; c.active = true;
      assert(c.synchronize(b, Config{}, &s) == ControlResult::IOFailure);
      assert(c.fault && c.commandKnown && !c.commandEnabled && !(b.bytes[0x1042] & 1)); }
    { Bus b; ChargeControl c;
      assert(c.synchronize(b, Config{}, &s) == ControlResult::NotReady);
      assert(b.calls == 0 && b.writes.empty()); }
    // Independent votes cannot cancel one another, regardless of update order.
    for (unsigned mask = 0; mask < 16; ++mask) {
        Bus b; ChargeControl c; c.active = true;
        c.policy.manualEnabled = !(mask & 1); c.policy.systemEnabled = !(mask & 2);
        c.policy.predictive = mask & 4; c.policy.setLimit(mask & 8 ? 60 : 100);
        assert(c.synchronize(b, Config{}, &s) == ControlResult::Success);
        assert(c.inhibit == mask);
        assert((b.bytes[0x1042] & 1) == !mask);
        assert((b.bytes[0x1042] & 0xfe) == 0xa0);
    }
    { Bus b; ChargeControl c; c.active = true; c.policy.setLimit(80);
      auto v = s;
      for (unsigned soc : {79, 80, 79, 78, 79, 80}) {
          v.percent = soc; assert(c.synchronize(b, Config{}, &v) == ControlResult::Success);
          assert(bool(b.bytes[0x1042] & 1) == (soc < 80 && (soc != 79 || !c.policy.atLimit)));
      }
      c.policy.setLimit(100); v.percent = 100;
      c.synchronize(b, Config{}, &v); assert(b.bytes[0x1042] & 1); }
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        Bus b; ChargeControl c; c.active = true; auto v = s;
        if (invalid == 0) v.installed = false;
        if (invalid == 1) v.external = false;
        if (invalid == 2) v.temperatureValid = false;
        if (invalid == 3) v.centiCelsius = -1;
        if (invalid == 4) v.centiCelsius = 4501;
        if (invalid == 5) v.chargerFaults = 1;
        if (invalid == 6) v.chargerFaults = 0x40;
        assert(c.synchronize(b, Config{}, invalid == 7 ? nullptr : &v) == ControlResult::Success);
        assert(!(b.bytes[0x1042] & 1));
        assert(c.inhibit & (invalid == 7 ? Telemetry : Protection));
        c.synchronize(b, Config{}, &s); assert(b.bytes[0x1042] & 1);
    }
    for (unsigned field : {0x103, 0x104, 0x105}) {
        Bus b; b.bytes[field] = 0xff;
        assert(chargeEnable(b, Config{}, false) == ControlResult::Unsupported);
        assert(b.writes.empty());
    }
    // Firmware pin control is handed off only after a verified OFF command.
    for (unsigned mode : {0x45, 0x85, 0xc5}) {
        Bus b; b.bytes[0x1051] = mode;
        assert(chargeEnable(b, Config{}, true) == ControlResult::Success);
        assert(b.bytes[0x1051] == 5 && b.bytes[0x1042] == 0xa1);
        assert((b.writes == std::vector<uint16_t>{0x1042, 0x1051, 0x1042}));
        Bus baseline; baseline.bytes[0x1051] = mode;
        assert(chargeEnable(baseline, Config{}, true) == ControlResult::Success);
        for (unsigned failure = 1; failure <= baseline.calls; ++failure) {
            Bus failed; failed.bytes[0x1051] = mode; failed.failAt = failure;
            ChargeControl c; c.active = true;
            assert(c.synchronize(failed, Config{}, &s) != ControlResult::Success && c.fault);
            failed.failAt = 0;
            assert(c.synchronize(failed, Config{}, &s) == ControlResult::Fault);
            assert(!(failed.bytes[0x1042] & 1));
        }
    }
    // Every read/write failure in an enable operation latches a fault; the
    // next successful poll can only disable. Native votes never clear it.
    for (unsigned failure = 1; failure <= 5; ++failure) {
        Bus b; b.bytes[0x1042] = 0xa0; b.failAt = failure;
        ChargeControl c; c.active = true;
        assert(c.synchronize(b, Config{}, &s) != ControlResult::Success && c.fault);
        b.failAt = 0;
        assert(c.synchronize(b, Config{}, &s) == ControlResult::Fault);
        assert(!(b.bytes[0x1042] & 1));
        assert(c.recover(b, Config{}) == ControlResult::Success);
        assert(c.synchronize(b, Config{}, &s) == ControlResult::Success && (b.bytes[0x1042] & 1));
    }
    { Bus b; b.bytes[0x1042] = 0xa0; b.lostACK = true;
      ChargeControl c; c.active = true;
      assert(c.synchronize(b, Config{}, &s) == ControlResult::IOFailure);
      assert(c.fault && c.commandKnown && !c.commandEnabled && !(b.bytes[0x1042] & 1)); }
    { Bus b; b.ignoreWrite = true;
      assert(chargeEnable(b, Config{}, false) == ControlResult::VerifyFailed); }
    // Telemetry distinguishes the enable request, charger phase and net flow.
    { Bus b; Sample v;
      assert(readSample(b, Config{}, v) == Result::Success && v.charging && v.milliamps < 0);
      b.bytes[0x1042] &= ~1;
      assert(readSample(b, Config{}, v) == Result::Success && !v.charging && !v.chargeRequested);
      b.bytes[0x1051] = 0x80;
      assert(readSample(b, Config{}, v) == Result::Success && v.charging && v.pinControlled && !v.chargeRequested);
      b.bytes[0x1051] = 0;
      b.bytes[0x1007] = 0x8c;
      assert(readSample(b, Config{}, v) == Result::Success && v.inputLimited && v.thermalLimited);
      b.beforeRead = [&](uint16_t address) { if (address == 0x1042) b.bytes[address] ^= 1; };
      v.percent = 77;
      assert(readSample(b, Config{}, v) == Result::Incoherent && v.percent == 77); }
    { Bus baseline; Sample out;
      assert(readSample(baseline, Config{}, out) == Result::Success);
      for (unsigned i = 1; i <= baseline.calls; ++i) {
          Bus b; b.failAt = i; Sample v; v.percent = 77;
          assert(readSample(b, Config{}, v) == Result::IOFailure && v.percent == 77);
      } }
    puts("PMI8998 charger policy, write faults and coherent telemetry passed");
}
