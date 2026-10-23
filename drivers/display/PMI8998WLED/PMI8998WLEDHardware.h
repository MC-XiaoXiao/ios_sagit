// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerHardware.h>
#include <DisplayBacklight.h>

namespace PMI8998Backlight {
using namespace QcomPower;
// PMI8998 WLED4, retaining firmware current limits, boost, CABC and strings.
// Only module enable, owned brightness registers and their latch are writable.
struct Config {
    uint32_t version = 0, flags = 0, identifier = 0, sid = 0;
    uint32_t control = 0, sink = 0, strings = 0, syncDelayUS = 0;
    bool valid() const {
        return version == 1 && flags == 3 && identifier && sid <= 15 && (sid & 1) &&
            control == 0xd800 && sink == 0xd900 && strings && !(strings & ~15u) &&
            syncDelayUS >= 1000 && syncDelayUS <= 10000;
    }
};
class Hardware {
    Config config{};
    uint32_t level = 0;
    bool ready = false, fault = false, powered = false, enabled = false;
    template <class Bus> Status read(Bus &b, uint16_t address, uint8_t *p, unsigned n = 1) {
        auto s = b.transfer(config.sid, address, p, n, false);
        if (ready && s != OK) fault = true;
        return s;
    }
    template <class Bus> Status write(Bus &b, uint16_t address, uint8_t *p, unsigned n = 1) {
        auto s = b.transfer(config.sid, address, p, n, true);
        if (s != OK) fault = true; // A failed write may already have reached the PMIC.
        return s;
    }
    template <class Bus> Status check(Bus &b, bool lighting) {
        uint8_t sinks = 0, dim = 0, state = 0;
        auto s = read(b, config.sink + 0x46, &sinks);
        if (s == OK) s = read(b, config.sink + 0x4a, &dim);
        if (s == OK && lighting) s = read(b, config.control + 0x10, &state);
        if (s != OK) return s;
        // The 9-bit mode and live OVP/short faults need their own recovery.
        // SYNC is a software-controlled brightness latch, not a busy bit.
        // Firmware may leave the enabled strings high (Mi 6: 0xd947=3).
        if ((sinks >> 4) != config.strings || (dim & 2) || (state & 6)) {
            if (ready) fault = true;
            return NotReady;
        }
        for (unsigned i = 0; i < 4; ++i) if (config.strings & (1u << i)) {
            uint8_t mod = 0;
            s = read(b, config.sink + 0x50 + 16 * i, &mod);
            if (s != OK) return s;
            if (!(mod & 128)) { if (ready) fault = true; return NotReady; }
        }
        return OK;
    }
    template <class Bus> Status module(Bus &b, bool on) {
        uint8_t old = 0, actual = 0;
        auto s = read(b, config.control + 0x46, &old);
        if (s != OK) return s;
        uint8_t value = (old & 127) | (on ? 128 : 0);
        if (value != old) {
            s = write(b, config.control + 0x46, &value);
            if (s != OK) return s;
            if (on) b.delay(10000); // WLED4 soft start, without changing protection.
        }
        s = read(b, config.control + 0x46, &actual);
        if (s != OK || actual != value) { fault = true; return s != OK ? s : Failed; }
        enabled = on;
        return OK;
    }
    template <class Bus> Status program(Bus &b, uint32_t target) {
        // Read all values before the first write; preserve reserved MSB bits.
        uint8_t bytes[4][2] = {}, sync = 0;
        auto s = read(b, config.sink + 0x47, &sync);
        if (s != OK) return s;
        for (unsigned i = 0; i < 4; ++i) if (config.strings & (1u << i)) {
            s = read(b, config.sink + 0x57 + 16 * i, bytes[i], 2);
            if (s != OK) return s;
            bytes[i][0] = uint8_t(target);
            bytes[i][1] = (bytes[i][1] & 0xf0) | uint8_t(target >> 8);
        }
        for (unsigned i = 0; i < 4; ++i) if (config.strings & (1u << i)) {
            s = write(b, config.sink + 0x57 + 16 * i, bytes[i], 2);
            if (s != OK) return s;
        }
        // Always create a low-to-high edge, including when firmware left
        // SYNC asserted. Preserve reserved bits and other strings' latches.
        uint8_t clear = sync & ~config.strings;
        uint8_t latch = clear | config.strings;
        s = write(b, config.sink + 0x47, &clear);
        if (s != OK) return s;
        b.delay(config.syncDelayUS);
        s = write(b, config.sink + 0x47, &latch);
        if (s != OK) return s;
        uint8_t actual = 0;
        s = read(b, config.sink + 0x47, &actual);
        if (s != OK || actual != latch) { fault = true; return s != OK ? s : Failed; }
        for (unsigned i = 0; i < 4; ++i) if (config.strings & (1u << i)) {
            uint8_t value[2] = {};
            s = read(b, config.sink + 0x57 + 16 * i, value, 2);
            if (s != OK || value[0] != bytes[i][0] || value[1] != bytes[i][1]) {
                fault = true; return s != OK ? s : Failed;
            }
        }
        return OK;
    }
public:
    bool available() const { return ready && !fault; }
    void state(DisplayBacklight::Request &r) const {
        r.brightness = level; r.appliedBrightness = enabled ? level : 0;
        r.power = powered; r.valid = available();
    }
    template <class Bus> Status initialize(Bus &b, const Config &c) {
        if (ready || fault) return Stale;
        if (!c.valid()) return Invalid;
        config = c;
        uint16_t apid;
        if (!b.irqChannel(c.sid, c.control >> 8, apid) || !b.irqChannel(c.sid, c.sink >> 8, apid)) return Denied;
        uint8_t identity[2] = {};
        auto s = b.transfer(c.sid & ~1u, 0x104, identity, 2, false);
        if (s != OK) return s;
        if (identity[0] != 0x51 || identity[1] != 0x15) return Unsupported;
        s = check(b, true);
        if (s != OK) return s;
        uint32_t inherited = 0;
        bool first = true;
        for (unsigned i = 0; i < 4; ++i) if (c.strings & (1u << i)) {
            uint8_t value[2] = {};
            s = read(b, c.sink + 0x57 + 16 * i, value, 2);
            if (s != OK) return s;
            uint32_t v = value[0] | uint32_t(value[1] & 15) << 8;
            if (!first && v != inherited) return Invalid;
            inherited = v; first = false;
        }
        uint8_t value = 0;
        s = read(b, c.control + 0x46, &value);
        if (s != OK) return s;
        level = inherited; powered = enabled = value & 128; ready = true;
        return OK; // Adoption is read-only: never flash a default brightness.
    }
    template <class Bus> Status setPower(Bus &b, bool on) {
        if (!available()) return fault ? Stale : NotReady;
        auto s = check(b, on && level);
        if (s != OK) return s;
        bool programmed = on && !powered && level;
        if (programmed) {
            s = program(b, level);
            if (s != OK) return s;
        }
        s = module(b, on && level);
        if (s != OK && programmed) fault = true;
        if (s == OK) powered = on;
        return s;
    }
    template <class Bus> Status setBrightness(Bus &b, uint32_t target) {
        if (target > 4095) return Invalid;
        if (!available()) return fault ? Stale : NotReady;
        if (!powered) { level = target; return OK; } // No accidental wake while dark.
        auto s = check(b, target != 0);
        if (s != OK) return s;
        if (target) {
            s = program(b, target);
            if (s != OK) return s;
        }
        s = module(b, target != 0);
        if (s != OK && target) fault = true;
        if (s == OK) level = target;
        return s;
    }
};
}
