// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandQ6.h"
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>

namespace Baseband {
// Mapping alone does not authorize a boot. The resource coordinator must
// retain clocks, memory assignments and reset ownership before using Core.
class RegisterIO {
    IOMemoryMap *maps[2]{};
public:
    RegisterIO() = default;
    RegisterIO(const RegisterIO &) = delete;
    RegisterIO &operator=(const RegisterIO &) = delete;
    ~RegisterIO();
    bool bind(IOService *);
    void unbind();
    bool read(unsigned, uint32_t, uint32_t &);
    bool write(unsigned, uint32_t, uint32_t);
    uint64_t nowUS();
    void delayUS(unsigned);
};
} // namespace Baseband
