/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

namespace Display {
struct VBLObservation {
    uint64_t count=0,time=0,delta=0;
};

// Sample native timing before completion can unmap a surface or admit another
// swap. Deliver the captured transfer-done before publishing VBL: that native
// notification can start a new transfer, which must not consume the old IRQ.
// Drain a ready queued command immediately after retirement. Waiting for the
// next read-pointer VBL starts after TE and loses an entire refresh period.
// All callbacks run on the display workloop, never in the primary IRQ filter.
template<class Observe,class Complete,class Drain,class Notify>
void dispatchInterrupt(uint32_t status,uint32_t vblMask,uint32_t doneMask,
        const Observe &observe,const Complete &complete,const Drain &drain,const Notify &notify) {
    const bool vbl=(status&vblMask)!=0;
    const auto timing=vbl?observe():VBLObservation{};
    if(status&doneMask){complete();drain();}
    if(vbl)notify(timing);
}
}
