// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace USB {
// Cold controller PM. Native children drain before their parent changes
// state. All callbacks run on a sleepable thread, outside workloop gates.
// A failed resource teardown has unknown power ownership and is terminal
// until stop/recovery; it must never be followed by another MMIO operation.
template<class Backend> class ControllerPower {
    enum class Phase { Active,Suspending,Suspended,Resuming,Fault } phase=Phase::Active;
    Backend &backend;
    Result fail(Result result) { phase=Phase::Fault;backend.powerFault();return result; }
public:
    explicit ControllerPower(Backend &owner):backend(owner) {}
    bool suspended() const { return phase==Phase::Suspended; }
    bool failed() const { return phase==Phase::Fault; }
    Result suspend() {
        if (suspended()) return Result::Success;
        if (phase!=Phase::Active) return Result::NotReady;
        phase=Phase::Suspending;
        backend.closePowerAdmission();backend.drainRoleWork();
        auto result=backend.parkRole();
        if (result!=Result::Success) return fail(result);
        result=backend.suspendResources();
        if (result!=Result::Success) return fail(result);
        phase=Phase::Suspended;return Result::Success;
    }
    Result resume() {
        if (phase==Phase::Active) return Result::Success;
        if (!suspended()) return Result::NotReady;
        phase=Phase::Resuming;
        auto result=backend.resumeResources();
        if (result==Result::NotReady || result==Result::Busy) { phase=Phase::Suspended;return result; }
        if (result!=Result::Success) return fail(result);
        result=backend.restartRole();
        if (result!=Result::Success) return fail(result);
        phase=Phase::Active;return Result::Success;
    }
};
}
