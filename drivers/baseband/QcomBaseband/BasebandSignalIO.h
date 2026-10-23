// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandSignals.h"
#include "BasebandLink.h"
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>

namespace Baseband {
// A cold-boot Board calls bind while holding the proven reset/resource lease.
// This object owns the AP SMP2P item and AP GLINK cursors, never peer cursors.
// It keeps QcomSMEM open while pointers/IRQs can refer to the shared mapping.
// All callbacks and methods run on the supplied controller's work loop.
class NativeSignals {
    struct Context;
    Context *context = nullptr;
    uint64_t lastEpoch = 0;
public:
    NativeSignals() = default;
    NativeSignals(const NativeSignals &) = delete;
    NativeSignals &operator=(const NativeSignals &) = delete;
    ~NativeSignals();
    Result bind(IOService *, IOWorkLoop *, uint64_t,
                IOInterruptEventSource::Action smp, IOInterruptEventSource::Action glink,
                IOInterruptEventSource::Action watchdog,
                IOFilterInterruptEventSource::Filter watchdogFilter = nullptr);
    Result sample(uint64_t, SMP2P::Observation &, bool negotiate = true);
    Result stopRequest(uint64_t, bool);
    Result bindRings(uint64_t, GLINK::MemoryRing &, GLINK::MemoryRing &);
    bool notifyGLINK();
    void enable();
    void disable();
    void close();
};
} // namespace Baseband
