// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandBoot.h"
#include <IOKit/IOService.h>

namespace Baseband {
// Native, serialized SCM binding. It owns its physically contiguous command
// storage, and never accepts arbitrary regions outside the admitted layout.
// Bind/probe/assign are for the boot coordinator, not the diagnostic client.
class NativeAssignment {
    struct Context;
    Context *context = nullptr;
public:
    NativeAssignment() = default;
    NativeAssignment(const NativeAssignment &) = delete;
    NativeAssignment &operator=(const NativeAssignment &) = delete;
    ~NativeAssignment();
    Result bind(IOService *owner, IOService *scm, const BootLayout &admitted, bool rmtfs = false);
    Result probe();
    Result confirmHost(const Region &);
    Result assign(const Region &, Ownership from, Ownership to);
    bool close();
    bool quarantined() const;
};
} // namespace Baseband
