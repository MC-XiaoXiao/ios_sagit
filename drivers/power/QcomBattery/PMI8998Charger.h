// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "PMI8998Battery.h"

// Register semantics: Xiaomi sagit-n-oss smb-reg.h and
// smblib_chg_disable_vote_callback (see local/reference/qcom/msm8998/battery).
// Own CHARGING_ENABLE_CMD_BIT and CHGR_CFG2's enable source/polarity bits.
// Source selection follows qpnp-smb2.c's software-control initialization.
// Limits, JEITA, watchdog, input
// suspend, Type-C/OTG and FG profile remain owned by their existing backends.
namespace PMICBattery {
enum class ControlResult : uint32_t { Success, NotReady, Unsupported, IOFailure,
                                     VerifyFailed, Fault };
enum Inhibit : uint32_t { Manual = 1, System = 2, Predictive = 4, Capacity = 8,
                         Telemetry = 16, Protection = 32, Transport = 64 };
struct ChargePolicy {
    bool manualEnabled = true, systemEnabled = true, predictive = false;
    uint32_t limit = 100;
    bool atLimit = false;
    void setLimit(uint32_t value) { if (limit != value) atLimit = false; limit = value; }
    uint32_t reasons(const Sample *s) {
        if (limit == 100) atLimit = false;
        else if (s && s->installed) {
            if (s->percent >= limit) atLimit = true;
            else if (s->percent + 2 <= limit) atLimit = false;
        }
        uint32_t value = (!manualEnabled ? Manual : 0) | (!systemEnabled ? System : 0) |
                         (predictive ? Predictive : 0) | (atLimit ? Capacity : 0);
        if (!s) return value | Telemetry;
        if (!s->installed || !s->external || !s->temperatureValid ||
            s->centiCelsius < 0 || s->centiCelsius > 4500 || s->chargerFaults)
            value |= Protection;
        return value;
    }
};

template<class Bus> ControlResult chargeEnable(Bus &bus, const Config &c, bool enable) {
    if (!c.valid()) return ControlResult::Unsupported;
    uint8_t id[3]{}, mode = 0, before = 0, after = 0;
    if (!bus.read(c.sid, 0x103, id, 3) || !bus.read(c.sid, 0x1051, &mode, 1) ||
        !bus.read(c.sid, 0x1042, &before, 1)) return ControlResult::IOFailure;
    if (id[1] != 0x51 || id[2] != 0x15 || id[0] < 1 || id[0] > 2)
        return ControlResult::Unsupported;
    if (mode & 0xc0) {
        // Stage a verified OFF command before handing the enable pin to
        // software. A failed handoff must never reach the enable write.
        const uint8_t off = before & ~1u;
        if (off != before && !bus.write(c.sid, 0x1042, &off, 1)) return ControlResult::IOFailure;
        if (!bus.read(c.sid, 0x1042, &after, 1)) return ControlResult::IOFailure;
        if (after & 1) return ControlResult::VerifyFailed;
        const uint8_t software = mode & ~0xc0u;
        if (!bus.write(c.sid, 0x1051, &software, 1) ||
            !bus.read(c.sid, 0x1051, &mode, 1) ||
            !bus.read(c.sid, 0x1042, &before, 1)) return ControlResult::IOFailure;
        if (mode != software || (before & 1)) return ControlResult::VerifyFailed;
    }
    const uint8_t next = (before & ~1u) | uint8_t(enable);
    if (next != before && !bus.write(c.sid, 0x1042, &next, 1)) return ControlResult::IOFailure;
    if (!bus.read(c.sid, 0x1042, &after, 1)) return ControlResult::IOFailure;
    return (after & 1) == unsigned(enable) ? ControlResult::Success : ControlResult::VerifyFailed;
}

class ChargeControl {
public:
    ChargePolicy policy{};
    bool active = false, fault = false, commandKnown = false, commandEnabled = false;
    uint32_t inhibit = 0;
    ControlResult result = ControlResult::NotReady;
    template<class Bus> ControlResult synchronize(Bus &bus, const Config &c, const Sample *s) {
        if (!active) return result = ControlResult::NotReady; // Retain boot firmware policy.
        inhibit = policy.reasons(s) | (fault ? Transport : 0);
        const bool enable = !inhibit;
        result = chargeEnable(bus, c, enable);
        commandKnown = result == ControlResult::Success;
        if (commandKnown) commandEnabled = enable;
        else {
            fault = true; inhibit |= Transport;
            // A lost write ACK may have enabled charging. Prove it off, but
            // preserve the original failure and require explicit recovery.
            if (enable && chargeEnable(bus, c, false) == ControlResult::Success) {
                commandKnown = true; commandEnabled = false;
            }
        }
        if (result == ControlResult::Success && fault) result = ControlResult::Fault;
        return result;
    }
    template<class Bus> ControlResult recover(Bus &bus, const Config &c) {
        const auto r = chargeEnable(bus, c, false);
        if (r == ControlResult::Success) { fault = false; commandKnown = true; commandEnabled = false; }
        else { fault = true; commandKnown = false; }
        result = r;
        return r;
    }
};
}
