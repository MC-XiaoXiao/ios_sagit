// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <HapticsProtocol.h>

namespace PMI8998Haptic {
using Haptics::State;
using Haptics::Result;
struct Config {
    uint32_t version = 1, flags = 0, sid = 3, base = 0xc000;
    uint32_t periodUS = 5000, maximumMV = 3248, currentMA = 800, maximumMS = 15000;
    bool valid() const {
        // Only the audited PMI8998 direct LRA register layout is supported.
        return version == 1 && !(flags & ~1u) && sid == 3 && base == 0xc000 &&
               periodUS == 5000 && maximumMV >= 116 && maximumMV <= 3248 &&
               maximumMV % 116 == 0 && (currentMA == 400 || currentMA == 800) &&
               maximumMS >= 5 && maximumMS <= 15000;
    }
    bool owned() const { return valid() && flags == 1; }
};

// All times are monotonic nanoseconds. The IOKit adapter runs every operation
// on the SPMI workloop, including each read/modify/write and secure unlock pair.
class Hardware {
    Config config{};
    State state = State::Uninitialized;
    Result result = Result::NotReady;
    bool accepted = false, initialized = false, off = false, autoRes = false;
    uint8_t idlePlay = 0;
    uint32_t strength = 0, millivolts = 0;
    uint64_t endNS = 0, autoNS = 0, brakeNS = 0, previousNS = 0;

    template<class Bus> bool read(Bus &b, uint16_t offset, uint8_t &value) {
        return b.read(uint8_t(config.sid), uint16_t(config.base + offset), &value, 1);
    }
    template<class Bus> bool write(Bus &b, uint16_t offset, uint8_t value) {
        return b.write(uint8_t(config.sid), uint16_t(config.base + offset), &value, 1);
    }
    template<class Bus> bool update(Bus &b, uint16_t offset, uint8_t mask, uint8_t value) {
        uint8_t before = 0, after = 0;
        if (!read(b, offset, before)) return false;
        const auto next = uint8_t((before & ~mask) | (value & mask));
        if (!write(b, offset, next) || !read(b, offset, after)) return false;
        return (after & mask) == (next & mask);
    }
    template<class Bus> bool resonance(Bus &b, bool on) {
        uint8_t value = 0, after = 0;
        if (!read(b, 0xe3, value)) return false;
        value = uint8_t((value & 0x7f) | (on ? 0x80 : 0));
        // TEST2 is protected: no intervening SPMI client may break this pair.
        if (!write(b, 0xd0, 0xa5) || !write(b, 0xe3, value) ||
            !read(b, 0xe3, after) || ((after ^ value) & 0x80)) return false;
        autoRes = on;
        return true;
    }
    template<class Bus> bool cutOutput(Bus &b) {
        if (!accepted) return false;
        // Always try MODULE_EN even if clearing PLAY failed. Never short-circuit
        // the second attempt or report an unconfirmed output as stopped.
        uint8_t play = 0, enable = 0;
        const bool playWrite = write(b, 0x70, idlePlay);
        const bool playRead = read(b, 0x70, play);
        // Stock MODULE_EN uses a whole-register 0/0x80 write. In particular,
        // a failed read must not prevent an independent disable attempt.
        const bool moduleWrite = write(b, 0x46, 0);
        const bool moduleRead = read(b, 0x46, enable);
        off = moduleRead && !(enable & 0x80);
        strength = millivolts = 0;
        endNS = autoNS = brakeNS = 0;
        return playWrite && playRead && !(play & 0x80) && moduleWrite && off;
    }
    template<class Bus> Result fail(Bus &b, Result reason) {
        cutOutput(b);
        state = State::Fault;
        return result = reason;
    }
    template<class Bus> Result beginBrake(Bus &b, uint64_t now) {
        if (now > UINT64_MAX - uint64_t(config.periodUS) * 5 * 1000)
            return fail(b, Result::Invalid);
        if (!update(b, 0x70, 0x80, 0)) return fail(b, Result::IOFailure);
        strength = millivolts = 0;
        autoNS = endNS = 0;
        // Stock direct mode allows five LRA cycles before MODULE_EN is cut.
        brakeNS = now + uint64_t(config.periodUS) * 5 * 1000;
        state = State::Braking;
        return result = Result::Success;
    }
public:
    State currentState() const { return state; }
    Result lastResult() const { return result; }
    bool isInitialized() const { return initialized; }
    bool knownOff() const { return off; }
    bool pending() const { return state == State::Playing || state == State::Braking; }
    uint32_t requestedStrength() const { return strength; }
    uint32_t appliedMV() const { return millivolts; }
    uint64_t remainingMS(uint64_t now) const {
        return state == State::Playing && now < endNS ? (endNS - now + 999999) / 1000000 : 0;
    }
    template<class Bus> Result initialize(Bus &b, const Config &c, uint64_t now) {
        if (!c.owned()) return Result::Invalid;
        if (state == State::Fault) return Result::Faulted;
        if (pending()) return Result::Busy;
        config = c;
        uint8_t identity[3] = {}, enable = 0, play = 0, status = 0;
        if (!b.read(uint8_t(c.sid & ~1u), 0x103, identity, 3) ||
            !read(b, 0x46, enable) || !read(b, 0x70, play) || !read(b, 0x0a, status))
            return result = Result::IOFailure;
        if (identity[0] < 1 || identity[0] > 2 || identity[1] != 0x51 || identity[2] != 0x15)
            return result = Result::WrongChip;
        // A boot declaration does not authorize interrupting a live producer.
        if ((enable | play) & 0x80 || status & 2) return result = Result::Busy;
        accepted = true; off = true; idlePlay = play & 0x7f;
        if (status & 8) return fail(b, Result::ShortCircuit);
        const uint32_t rate = c.periodUS / 5;
        // Audited sagit configuration: sine, 200 Hz, QWD, high-Z opt1, four
        // calibration cycles, 505 kHz internal PWM, eight-cycle SC debounce.
        // Keep the static 03 02 00 00 brake; no 3596 mV overbrake/overdrive.
        if (!resonance(b, false) || !update(b, 0x4c, 1, 0) ||
            !update(b, 0x4e, 0x30, 0) || !update(b, 0x4f, 0x7f, 0x24) ||
            !update(b, 0x51, 0x3e, uint8_t((c.maximumMV / 116) << 1)) ||
            !update(b, 0x52, 1, uint8_t(c.currentMA == 800)) ||
            !update(b, 0x53, 7, 1) || !update(b, 0x56, 3, 1) ||
            !update(b, 0x58, 3, 1) || !update(b, 0x4d, 1, 0) ||
            !update(b, 0x54, 0xff, uint8_t(rate)) ||
            !update(b, 0x55, 0x0f, uint8_t(rate >> 8)) ||
            !update(b, 0x48, 1, 1) || !update(b, 0x5c, 0xff, 0x0b))
            return fail(b, Result::IOFailure);
        initialized = true; previousNS = now;
        state = State::Idle;
        return result = Result::Success;
    }
    template<class Bus> Result play(Bus &b, uint64_t now, uint32_t durationMS, uint32_t level) {
        if (durationMS < 5 || durationMS > config.maximumMS || level < 1 || level > 1000)
            return Result::Invalid;
        if (state == State::Fault) return Result::Faulted;
        if (!initialized || state == State::Suspended || state == State::Uninitialized)
            return Result::NotReady;
        if (pending()) return Result::Busy;
        if (now < previousNS || now > UINT64_MAX - uint64_t(durationMS + 25) * 1000000)
            return Result::Invalid;
        uint8_t status = 0, enable = 0, playValue = 0;
        if (!read(b, 0x0a, status) || !read(b, 0x46, enable) || !read(b, 0x70, playValue))
            return fail(b, Result::IOFailure);
        if (status & 8) return fail(b, Result::ShortCircuit);
        if ((status & 2) || ((enable | playValue) & 0x80)) return fail(b, Result::Busy);
        uint32_t code = (config.maximumMV / 116) * level / 1000;
        if (!code) code = 1;
        if (!resonance(b, false) || !update(b, 0x51, 0x3e, uint8_t(code << 1)))
            return fail(b, Result::IOFailure);
        off = false; // From this point a failed write can have enabled output.
        if (!update(b, 0x46, 0x80, 0x80) || !update(b, 0x70, 0x80, 0x80))
            return fail(b, Result::IOFailure);
        strength = level; millivolts = code * 116;
        previousNS = now; endNS = now + uint64_t(durationMS) * 1000000;
        autoNS = durationMS > 20 ? now + 20000000 : 0;
        state = State::Playing;
        return result = Result::Success;
    }
    template<class Bus> Result stop(Bus &b, uint64_t now) {
        if (state == State::Fault) {
            cutOutput(b);
            return result; // Stopping never clears a latched fault.
        }
        if (state == State::Playing) return beginBrake(b, now);
        return result = Result::Success;
    }
    template<class Bus> Result advance(Bus &b, uint64_t now) {
        if (!pending()) return result;
        if (now < previousNS) return fail(b, Result::Invalid);
        previousNS = now;
        uint8_t status = 0;
        if (!read(b, 0x0a, status)) return fail(b, Result::IOFailure);
        if (status & 8) return fail(b, Result::ShortCircuit);
        if (state == State::Braking) {
            if (now >= brakeNS) {
                if (!cutOutput(b)) return fail(b, Result::IOFailure);
                state = State::Idle;
            }
        } else if (now >= endNS) {
            // Deadline takes precedence over enabling resonance on a late tick.
            return beginBrake(b, now);
        } else if (autoNS && now >= autoNS) {
            if (!resonance(b, true)) return fail(b, Result::IOFailure);
            autoNS = 0;
        } else if (autoRes && (status & 0x10)) return fail(b, Result::ResonanceError);
        return result = Result::Success;
    }
    template<class Bus> Result quiesce(Bus &b, bool suspend) {
        const bool fault = state == State::Fault;
        if (accepted && !cutOutput(b)) return fail(b, Result::IOFailure);
        if (!fault) {
            state = suspend ? State::Suspended : (initialized ? State::Idle : State::Uninitialized);
            result = Result::Success;
        }
        return result;
    }
    template<class Bus> Result timerFailure(Bus &b) { return fail(b, Result::IOFailure); }
};
}
