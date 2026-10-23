// SPDX-License-Identifier: GPL-2.0-only
// RPM clock wire keys: Linux v6.12 clk-smd-rpm.c and MSM8998 handoff.
#pragma once
#include "RPMClockHandoff.h"
#include <PowerVoltagePolicy.h>

namespace QcomPower {
constexpr uint32_t ModemXO = 0x3000, ModemQDSS = 0x3001;
// XO is an enable bit; QDSS is the firmware STATE enum (0..4), never Hz.
// Independent active/sleep votes preserve the complete AP boot baseline.
// The common RPM gate serializes this object with voltage and bus owners.
class RPMModemClocks {
    VoltageVotes votes[2];
    bool bound = false;
    static int index(uint32_t id) { return id == ModemXO ? 0 : id == ModemQDSS ? 1 : -1; }
    static const RPMHandoffClock &clock(unsigned i) { return rpmHandoffClocks[i ? 19 : 6]; }
    Status bind(const RPMClockHandoff &handoff) {
        if (!handoff.ready()) return bound ? Stale : NotReady;
        if (bound) return OK;
        for (unsigned i = 0; i < 2; ++i) for (unsigned set = 0; set < 2; ++set) {
            const auto &c = clock(i); uint32_t floor = 0;
            const auto r = handoff.floor(c.type, c.resource, c.key, set, floor);
            if (r != OK) return r;
            // These exact baseline requests were ACKed by RPMClockHandoff.
            votes[i].adopt(set, floor, 0);
        }
        bound = true; return OK;
    }
public:
    Status validate(uint32_t id, unsigned set, unsigned consumer, uint64_t value,
                    const RPMClockHandoff &handoff) const {
        const auto i = index(id);
        if (i < 0) return Unsupported;
        if (set > 1 || consumer != 15 || value > (i ? 4u : 1u)) return Invalid;
        if (!handoff.configured()) return Unsupported;
        uint32_t current = 0;
        if (votes[i].get(set, current) == Stale) return Stale;
        const auto &c = clock(unsigned(i)); uint32_t floor = 0, cap = 0;
        auto r = handoff.floor(c.type, c.resource, c.key, set, floor);
        if (r != OK) return r;
        r = handoff.limit(c.type, c.resource, set, cap);
        if (r != OK) return r;
        const auto total = votes[i].aggregate(set, consumer, uint32_t(value));
        return cap && (value > cap || floor > cap || total > cap) ? Invalid : OK;
    }
    Status get(uint32_t id, unsigned set, const RPMClockHandoff &handoff, uint32_t &value) {
        const auto i = index(id);
        if (i < 0) return Unsupported;
        if (set > 1) return Invalid;
        const auto r = bind(handoff); return r == OK ? votes[i].get(set, value) : r;
    }
    template<class Send> Status vote(uint32_t id, unsigned set, unsigned consumer, uint64_t value,
                                    const RPMClockHandoff &handoff, Send send) {
        auto r = validate(id, set, consumer, value, handoff);
        if (r != OK) return r;
        r = bind(handoff); if (r != OK) return r;
        const auto i = index(id); const auto &c = clock(unsigned(i));
        uint32_t cap = 0; r = handoff.limit(c.type, c.resource, set, cap);
        if (r != OK) return r;
        return votes[i].vote(set, consumer, uint32_t(value), [&](unsigned which, uint32_t aggregate) {
            uint8_t kvp[24]{};
            put32(kvp, c.key); put32(kvp + 4, 4); put32(kvp + 8, aggregate);
            // Transfer the same declared cap. An omitted retained LIMIT_MAX
            // could otherwise turn a successful ACK into an undersupply.
            put32(kvp + 12, 0x0078616d); put32(kvp + 16, 4); put32(kvp + 20, cap);
            return send(c.type, c.resource, which, kvp, sizeof(kvp));
        });
    }
};
} // namespace QcomPower
