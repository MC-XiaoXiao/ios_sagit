// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

// MSM8998 v2 APCS mux and Agera PLL, decoded from the stock ClockDxe HAL.
// Read-only configuration evidence, not a cycle-counter frequency measurement.
namespace QcomOSMClock {
constexpr uint64_t base(unsigned cluster) { return cluster ? 0x17810000 : 0x17910000; }
constexpr uint32_t BootHz[2] = {1555200000, 1728000000};
constexpr uint32_t Offsets[] = {0x1050, 0x1054, 0x6000, 0x6004, 0x6008, 0x600c, 0x602c};
struct State {
    uint32_t words[7] = {};
    bool stable = false;
    uint32_t localPLLHz() const {
        const auto command = words[0], config = words[1], user = words[5];
        const unsigned source = (config >> 8) & 7, hid = config & 31;
        // Only the integer, divide-by-one local PLL paths used by the stock
        // boot rates are admitted. Unknown/fractional configurations return 0.
        if (!stable || (command & 0x800000f1U) || (config & 0x3000) || hid > 1 ||
            (source != 3 && source != 5) || (words[2] & 7) != 7 ||
            !(words[6] & 0x10000) || words[4] || (user & 0x03001000) ||
            (source == 3 && ((user & 0x300) || !(user & 1))) ||
            (source == 5 && !(user & 8)) || !words[3] || words[3] > 145)
            return 0;
        return words[3] * 19200000U;
    }
};
template <class Read> State observe(Read read) {
    State s{}; s.stable = true;
    for (unsigned i = 0; i < 7; ++i) s.words[i] = read(Offsets[i]);
    for (unsigned i = 0; i < 7; ++i)
        if (s.words[i] != read(Offsets[i])) s.stable = false;
    return s;
}
// The caller keeps a core online in the cluster throughout this check.
// Require two separated observations; accepting the desired-index register
// would let a stalled sequencer on the auxiliary clock pass this handoff.
template <class Read, class Delay>
bool awaitBootRate(unsigned cluster, Read read, Delay delay, State &last) {
    if (cluster > 1) return false;
    unsigned matched = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        last = observe(read);
        matched = last.localPLLHz() == BootHz[cluster] ? matched + 1 : 0;
        if (matched == 2) return true;
        delay(5);
    }
    return false;
}
}
