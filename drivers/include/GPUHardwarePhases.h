// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "GPUDiagnostics.h"

namespace GPU { namespace HardwarePhases {
constexpr uint32_t Version=1,ReferenceHz=19200000;
constexpr const char *Property="GPUHardwarePhases";
// The existing flush/progress allocation reserves its first128 bytes. Keep
// these two diagnostic clock samples separate from flush/progress cells0/8/16.
constexpr uint32_t ScratchOffset=64,ScratchBytes=16;
struct Group {
    Diagnostics::Timing enter,commands,leave;
    uint64_t skipped=0,invalid=0;
    bool sample(uint64_t begin,uint64_t entered,uint64_t drained,uint64_t end) {
        if(!begin || !entered || !drained || begin>entered || entered>drained || drained>end) {
            Diagnostics::add(invalid);return false;
        }
        enter.sample(begin,entered);commands.sample(entered,drained);leave.sample(drained,end);return true;
    }
};
struct Snapshot {
    uint32_t version=Version,bytes=440;
    uint64_t sequence=0;
    uint32_t referenceHz=ReferenceHz,enabled=0;
    uint64_t reserved=0;
    Group native,compositor,compositorRender;
    void sample(bool isCompositor,bool isRender,uint64_t begin,uint64_t entered,uint64_t drained,uint64_t end) {
        native.sample(begin,entered,drained,end);
        if(isCompositor) {
            compositor.sample(begin,entered,drained,end);
            if(isRender)compositorRender.sample(begin,entered,drained,end);
        }
    }
    void skip(bool isCompositor,bool isRender) {
        Diagnostics::add(native.skipped);
        if(isCompositor) {
            Diagnostics::add(compositor.skipped);
            if(isRender)Diagnostics::add(compositorRender.skipped);
        }
    }
};
static_assert(sizeof(Group)==136 && sizeof(Snapshot)==440,"optional GPU phase property ABI");
} }
