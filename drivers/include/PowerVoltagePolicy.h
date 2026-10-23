// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <QcomPowerHardware.h>
namespace QcomPower {
// One slot per in-kernel consumer, with independent active/sleep sets. A vote
// is committed only after the device acknowledges it. Unknown completion
// quarantines the resource: retrying a timed-out lowering operation is unsafe.
class VoltageVotes {
    uint32_t requested[2][16] = {};
    uint32_t applied[2] = {};
    uint32_t retainedMinimum[2] = {};
    bool known[2] = {}, fault[2] = {};
public:
    uint32_t aggregate(unsigned set, unsigned client, uint32_t value) const {
        uint32_t maximum = value;
        for (unsigned i = 0; i < 16; ++i) if (i != client && requested[set][i] > maximum) maximum = requested[set][i];
        return maximum;
    }
    template <typename Send> Status vote(unsigned set, unsigned client, uint32_t value, Send send) {
        if (set > 1 || client > 15) return Invalid;
        if (fault[set]) return Stale;
        uint32_t maximum = aggregate(set, client, value);
        if (retainedMinimum[set] > maximum) maximum = retainedMinimum[set];
        if (!known[set] || maximum != applied[set]) {
            Status result = send(set, maximum);
            if (result != OK) {
                if (result == TimedOut || result == Stale || result == Failed) fault[set] = true;
                return result;
            }
            applied[set] = maximum; known[set] = true;
        }
        requested[set][client] = value; return OK;
    }
    Status get(unsigned set, uint32_t &value) const {
        if (set > 1) return Invalid;
        if (fault[set]) return Stale;
        if (!known[set]) return NotReady;
        value = applied[set]; return OK;
    }
    Status retainMinimum(unsigned set) {
        uint32_t value=0; const auto status=get(set,value);
        if (status!=OK) return status;
        if (!value) return NotReady;
        if (retainedMinimum[set]) return Busy;
        retainedMinimum[set]=value; return OK;
    }
    Status releaseMinimum(unsigned set) {
        uint32_t value=0; const auto status=get(set,value);
        if (status!=OK) return status;
        if (!retainedMinimum[set] || value<retainedMinimum[set]) return Stale;
        // Do not lower hardware here. The next normal transaction recomputes
        // the aggregate from live consumers after the GPU has been isolated.
        retainedMinimum[set]=0; return OK;
    }
    void adopt(unsigned set, uint32_t value, unsigned client = 0) {
        if (set > 1 || client > 15) return;
        applied[set] = value; requested[set][client] = value; known[set] = true;
    }
};
}
