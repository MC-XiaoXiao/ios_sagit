// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandMemory.h"
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>

namespace Baseband {
Result readMemoryHandoff(MemoryHandoff &);
// Reads only the EFI-owned immutable staging allocation, copies to OSData,
// verifies both hashes, then drops the temporary WB CPU mapping.
Result copyFirmware(const MemoryHandoff &, OSData *&mba, OSData *&modem);
class NativeMemory {
    struct Context;
    Context *context = nullptr;
public:
    NativeMemory() = default;
    NativeMemory(const NativeMemory &) = delete;
    NativeMemory &operator=(const NativeMemory &) = delete;
    ~NativeMemory();
    // Call after SCM confirmations, while the Board holds reset and resources.
    Result bindConfirmed(const BootLayout &);
    Result assigned(const Region &, Ownership from, Ownership to);
    void quarantine();
    bool copy(uint64_t, const uint8_t *, size_t);
    bool zero(uint64_t, size_t);
    bool publish(uint64_t, size_t);
    Result metadata(const FirmwareImage &, const Region &);
    bool close();
};
} // namespace Baseband
