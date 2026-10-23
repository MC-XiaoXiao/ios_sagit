// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
namespace GPU { namespace Native {
// One Family stamp channel per command queue. A540 ring execution may interleave
// queues, while each queue's events still advance in its own FIFO order.
// Native event values reserve the low byte for the channel. Rebase before the
// Family's quarter-range restart threshold, and only after every old fence and
// job has drained. Hardware sequence resets do not reset this timeline.
class Timeline {
    uint32_t fNext = 0x100, fCompleted = 0;
    uint64_t fEpoch = 0;
public:
    static constexpr uint32_t Step = 0x100, RebaseAt = 0x3ffffe00;
    uint32_t next() const { return fNext; }
    uint32_t completed() const { return fCompleted; }
    uint64_t epoch() const { return fEpoch; }
    bool needsRebase() const { return fNext >= RebaseAt; }
    bool reserve(uint32_t &stamp) {
        if (needsRebase()) return false;
        stamp=fNext;fNext+=Step;return true;
    }
    bool retire(uint32_t stamp) {
        if (!stamp || (stamp&(Step-1)) || stamp<=fCompleted || stamp>=fNext) return false;
        // Within this queue, no later event may cover an unfinished gap.
        if (stamp!=fCompleted+Step) return false;
        fCompleted=stamp;return true;
    }
    bool rebase(bool hardwareIdle,bool fencesIdle,bool callsIdle) {
        if (!needsRebase() || !hardwareIdle || !fencesIdle || !callsIdle ||
            fCompleted+Step!=fNext || fEpoch==UINT64_MAX) return false;
        ++fEpoch;fNext=Step;fCompleted=0;return true;
    }
};
} }
