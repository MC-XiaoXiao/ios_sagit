// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

namespace BridgeOSMPeer {
constexpr uint64_t Affinity = 0x100;
struct Result {
    int32_t status = 0;
    uint64_t observedAffinity = 0;
    bool owned = false, ready = false, stopped = false;
};
// A temporary core keeps the otherwise powered-down performance cluster
// available during ACD local transfers. Firmware owns CPU power sequencing.
// It must be OFF again before XNU owns the machine, including a four-core boot.
template<class IO> bool start(IO &io, uint64_t entry, uint64_t mailbox, Result &out) {
    out = {};
    if (!entry || (entry & 3) || entry >= (1ULL << 40) || !mailbox ||
        (mailbox & 4095) || mailbox >= (1ULL << 40)) return false;
    out.status = io.psci(0x84000000, 0, 0, 0);
    if (out.status < 2) return false;
    out.status = io.psci(0xc4000004, Affinity, 0, 0);
    if (out.status != 1) return false; // Never take ownership of a firmware AP.
    out.status = io.psci(0xc4000003, Affinity, entry, mailbox);
    if (out.status) return false;
    out.owned = true;
    for (unsigned i = 0; i < 1000; ++i) {
        const uint64_t state = io.read(64);
        if (state == 1) {
            out.observedAffinity = io.read(128);
            out.ready = (out.observedAffinity & 0xff00ffffffULL) == Affinity;
            return out.ready;
        }
        if (state) return false;
        io.delay(100);
    }
    return false;
}
template<class IO> bool stop(IO &io, Result &out) {
    if (!out.owned) return true;
    io.stop();
    for (unsigned i = 0; i < 1000; ++i) {
        out.status = io.psci(0xc4000004, Affinity, 0, 0);
        if (out.status == 1) {
            out.owned = false; out.ready = false; out.stopped = true;
            return true;
        }
        if (out.status < 0 || io.read(64) == 2) return false;
        io.delay(100);
    }
    return false;
}
}
