// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <Uefi.h>
#include <stdint.h>

namespace BridgeOSMMMIO {
constexpr uint64_t PageBytes = 4096, PhysicalMask = 0x0000fffffffff000ULL;
constexpr uint64_t Attributes = EFI_MEMORY_UC | EFI_MEMORY_XP;
struct IO {
    void *context;
    EFI_STATUS (*attributes)(void *, uint64_t, uint64_t, uint64_t);
    uint64_t (*translate)(void *, uint64_t, bool);
};
struct Evidence {
    uint64_t address = 0, read = 0, write = 0;
    EFI_STATUS status = EFI_SUCCESS;
    unsigned calls = 0;
    bool retry = false;
};
inline bool validRange(uint64_t address, uint64_t bytes) {
    return address && bytes && !((address | bytes) & (PageBytes - 1)) &&
        address < (1ULL << 48) && bytes <= (1ULL << 48) - address;
}
inline bool deviceIdentity(uint64_t address, uint64_t par) {
    // PAR_EL1's successful ATTR field has the MAIR encoding. Only the four
    // Device types are accepted; Normal non-cacheable is not MMIO-safe.
    return !(par & 1) && (par & PhysicalMask) == address && !((par >> 56) & ~0xcULL);
}
inline bool inspect(const IO &io, uint64_t address, uint64_t bytes, Evidence &out) {
    if (!io.translate || !validRange(address, bytes)) return false;
    for (uint64_t at = address; at < address + bytes; at += PageBytes) {
        out.address = at;
        out.read = io.translate(io.context, at, false);
        out.write = io.translate(io.context, at, true);
        if (!deviceIdentity(at, out.read) || !deviceIdentity(at, out.write)) return false;
    }
    return true;
}
inline bool prepare(const IO &io, uint64_t address, uint64_t bytes, Evidence &out) {
    out = {};
    if (!io.attributes || !io.translate || !validRange(address, bytes)) return false;
    out.status = io.attributes(io.context, address, bytes, Attributes); ++out.calls;
    if (EFI_ERROR(out.status)) return false;
    if (inspect(io, address, bytes, out)) return true;
    // Some older CpuDxe implementations return success for an unmapped
    // aperture when their region scan mistakes a hole for matching attributes.
    // Force an attribute transition without ever making MMIO cacheable. There
    // are no register accesses between these calls; verify actual translations
    // again before permitting any access. Never trust API success alone.
    out.retry = true;
    out.status = io.attributes(io.context, address, bytes, Attributes | EFI_MEMORY_RO); ++out.calls;
    if (EFI_ERROR(out.status)) return false;
    out.status = io.attributes(io.context, address, bytes, Attributes); ++out.calls;
    return !EFI_ERROR(out.status) && inspect(io, address, bytes, out);
}
}
