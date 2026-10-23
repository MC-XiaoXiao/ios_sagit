// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandMemoryIO.h"
#include "BasebandSignalIO.h"

namespace Baseband {
struct NativeActions {
    IOInterruptEventSource::Action smp = nullptr, glink = nullptr, watchdog = nullptr;
    IOFilterInterruptEventSource::Filter watchdogFilter = nullptr;
};
// The service's gate/timer/IRQs call this owner on one IOWorkLoop. Only the
// watchdog latch runs in primary interrupt context; it performs no allocation,
// registry access, clock/RPM/SCM call or MMIO.
class NativeController {
    struct Context;
    Context *context = nullptr;
public:
    NativeController() = default;
    NativeController(const NativeController &) = delete;
    NativeController &operator=(const NativeController &) = delete;
    ~NativeController();
    Result bind(IOService *, IOWorkLoop *, const MemoryHandoff &, OSData *, OSData *, const NativeActions &);
    Result start();
    Result poll();
    Result stop();
    void watchdog();
    bool close();
    bool quarantined() const;
    State state() const;
    uint64_t generation() const;
    uint32_t missing() const;
};
} // namespace Baseband
