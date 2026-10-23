/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

namespace Display {
// MSM8998 command-mode PP/CTL0. The panel retains the transferred raster;
// keeping a mapping is not permission to fetch it again after its read fence
// has retired. Disable firmware auto-refresh once, before native swaps start.
template<class Registers> class CommandTransfer {
    Registers &r;
    uint32_t pp, height;
public:
    CommandTransfer(Registers &registers, unsigned pingPong, unsigned lines)
        : r(registers), pp(0x70000+0x800*pingPong), height(lines) {}
    bool idle() const {
        const auto line=r.read(pp+0x2c)&0xffff;
        return !r.read(pp+0x30) && !(r.read(0x101c)&1) && (!line || line>=height);
    }
    template<class Delay> bool adopt(const Delay &delay) {
        const auto sync=r.read(pp+4), tear=r.read(pp);
        if (!height || height>4096 || !(sync&(1u<<19)) || tear>1) return false;
        // Match the DPU disable-autorefresh ordering: disconnect external TE,
        // stop repeat triggers, drain the current transfer, reconnect TE.
        // Preserve the firmware counter and its synthetic/external TE choice.
        r.write(pp+4,sync&~(1u<<20));
        r.write(pp+0x30,0);
        bool drained=false;
        for (unsigned elapsed=0;elapsed<100000;elapsed+=100) {
            delay(100);
            if (idle()) {drained=true;break;}
        }
        r.write(pp+4,sync);
        if (!drained || r.read(pp+4)!=sync) return false;
        // Auto-refresh left start=5 and both initial counters zero on sagit.
        // Those are not the command-mode tearcheck parameters: they pace an
        // entire raster behind the read pointer and miss the next cycle.
        // Qualcomm's command setup places read/start at vdisplay and writes
        // start+threshold+1. Retain the working synthetic period/external-TE
        // choice and the panel's inherited start/continue thresholds.
        const auto threshold=r.read(pp+0x18)&0xffff;
        if((r.read(pp+8)&0xffff)<=height+threshold+1)return false;
        r.write(pp,0);
        r.write(pp+0x10,height);
        r.write(pp+0x20,height+1);
        r.write(pp+0x1c,height);
        r.write(pp+0x0c,height+threshold+1);
        r.write(pp,1);
        return r.read(pp)==1 && idle();
    }
};
}
