// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandResources.h"
#include <IOKit/IOService.h>

namespace Baseband {
class NativeResources {
    struct Context;
    Context *context = nullptr;
    Result result(Result);
public:
    NativeResources() = default;
    NativeResources(const NativeResources &) = delete;
    NativeResources &operator=(const NativeResources &) = delete;
    ~NativeResources();
    Result bind(IOService *, const ResourcePolicy &);
    Result cold();
    Result acquire();
    Result validate();
    Result deassertReset();
    Result quiesceAndReset();
    Result release();
    bool close();
    bool quarantined() const;
};
} // namespace Baseband
