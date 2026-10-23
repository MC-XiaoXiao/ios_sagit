// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandAXI.h"
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>

namespace Baseband {
// Maps only the modem's TCSR aperture. Bind is read-only; the eventual Board
// must retain this mapping and the full clock/power lease on quarantine.
class HaltIO {
    IOMemoryMap *map = nullptr;
    IOService *bus = nullptr;
    Result request(bool query, bool asserted, bool &state);
public:
    HaltIO() = default;
    HaltIO(const HaltIO &) = delete;
    HaltIO &operator=(const HaltIO &) = delete;
    ~HaltIO();
    bool bind(IOService *owner);
    void unbind();
    bool read(uint32_t, uint32_t &);
    bool write(uint32_t, uint32_t);
    Result resetState(bool &);
    Result reset(bool);
    uint64_t nowUS();
    void delayUS(unsigned);
};
} // namespace Baseband
