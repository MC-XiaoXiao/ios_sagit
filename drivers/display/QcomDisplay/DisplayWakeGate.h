/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

namespace Display {
// Serialized by the native framebuffer gate. Submission and wake timestamps
// use the original framebuffer's continuous clock, not client swap IDs.
// A retained panel image, TE, or an old in-flight completion cannot unblank.
class WakeGate {
    uint64_t barrier=0;
    bool waiting=false, candidate=false;
public:
    void power(bool on,bool visible,uint64_t now) {
        if(!on) {waiting=false;candidate=false;}
        else if(!visible && !waiting) {
            barrier=now;waiting=true;candidate=false;
        }
    }
    bool pending() const {return waiting;}
    void submitted(uint64_t submittedAt) {
        candidate=waiting && submittedAt && submittedAt>=barrier;
    }
    bool completed(bool displayed) {
        const bool ready=waiting && candidate && displayed;
        candidate=false;
        return ready;
    }
    void lit() {waiting=false;candidate=false;}
};
}
