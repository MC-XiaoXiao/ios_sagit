// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "OSMTZContract.h"
#include "OSMClockState.h"

// An initialization/ownership record, not a secure-boot attestation. The EFI
// producer commits version LAST after CPRh, OSM and ACD readback succeeds.
// All cells are little endian in Apple DT; FDT conversion is explicit.
namespace QcomOSMHandoff {
constexpr uint32_t Version = 3;
constexpr uint32_t Calibrated = 1U << 0, CPRh = 1U << 1,
    Policies = 1U << 2, ACD = 1U << 3, Clocks = 1U << 4,
    TZBootParameters = 1U << 5, HLOSOwner = 1U << 6, BootClocks = 1U << 7;
constexpr uint32_t Required = Calibrated | CPRh | Policies | ACD | Clocks |
                              TZBootParameters | HLOSOwner | BootClocks;
// TZBootParameters covers the accessible table values only. Stock TZ can
// ignore a later initialization error; no proven HLOS-readable completion
// status is available. Protected post-init still depends on the boot chain.
struct Record {
    uint32_t version, bytes, chipID, chipVersion, boardID, flags;
    uint32_t auxHz, osmHz, xoHz;
    uint32_t lutDigest[2], initialDesired[2], enable[2];
    uint32_t firmwareProfile, firmwareChecks[2], reserved[2];
};
static_assert(sizeof(Record) == 80, "OSM handoff ABI");
static_assert(offsetof(Record, lutDigest) == 36, "OSM handoff LUT ABI");
static_assert(offsetof(Record, firmwareProfile) == 60, "OSM handoff TZ ABI");
enum class Status : uint32_t {
    Missing, Uncommitted, Invalid, Disabled, LUTChanged, DesiredChanged, Accepted,
    BootRateChanged, ClockNotReady
};
inline const char *name(Status s) {
    switch (s) {
    case Status::Missing: return "missing";
    case Status::Uncommitted: return "producer-not-complete";
    case Status::Invalid: return "invalid-contract";
    case Status::Disabled: return "osm-disabled";
    case Status::LUTChanged: return "lut-changed";
    case Status::DesiredChanged: return "desired-state-changed";
    case Status::Accepted: return "accepted";
    case Status::BootRateChanged: return "boot-rate-changed";
    case Status::ClockNotReady: return "cpu-clock-not-ready";
    }
    return "invalid-contract";
}
inline bool supported(uint32_t chip, uint32_t version, uint32_t board) {
    return chip == 292 && (version == 0x20000 || version == 0x20001) && board == 30;
}
inline uint32_t digestWord(uint32_t hash, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        hash = (hash ^ (value & 255)) * 16777619U;
        value >>= 8;
    }
    return hash;
}
// Include index, frequency, voltage, PLL override and spare data, including
// duplicate tail rows. The digest detects accidental changes; it is not a MAC.
template <class Read> uint32_t lutDigest(Read read, unsigned cluster) {
    uint32_t hash = 2166136261U;
    constexpr unsigned columns[] = {0, 4, 8, 12, 20};
    for (unsigned row = 0; row < 40; ++row)
        for (unsigned column : columns)
            hash = digestWord(hash, read(cluster, 0x1150 + row * 32 + column));
    return hash;
}
inline bool valid(const Record &r) {
    if (r.version != Version || r.bytes != sizeof(r) || r.flags != Required ||
        !supported(r.chipID, r.chipVersion, r.boardID) ||
        r.auxHz != 300000000 || r.osmHz != 200000000 || r.xoHz != 19200000 ||
        r.firmwareProfile != QcomOSMTZ::Profile)
        return false;
    for (uint32_t value : r.reserved) if (value) return false;
    for (unsigned c = 0; c < 2; ++c)
        if (r.initialDesired[c] >= 40 || r.enable[c] != 1 ||
            r.firmwareChecks[c] != QcomOSMTZ::CompleteChecks) return false;
    return true;
}
template <class Read> Status validate(const Record &r, Read read) {
    if (!r.version) return Status::Uncommitted;
    if (!valid(r)) return Status::Invalid;
    for (unsigned c = 0; c < 2; ++c) {
        if (read(c, 0x1004) != r.enable[c]) return Status::Disabled;
        if (lutDigest(read, c) != r.lutDigest[c]) return Status::LUTChanged;
        if (read(c, 0x1f10) != r.initialDesired[c]) return Status::DesiredChanged;
        const uint32_t frequency = read(c, 0x1154 + r.initialDesired[c] * 32);
        if (((frequency >> 16) & 7) != 4 || !((frequency >> 26) & 3) ||
            uint64_t(frequency & 255) * 19200000 != QcomOSMClock::BootHz[c])
            return Status::BootRateChanged;
    }
    return Status::Accepted;
}
}
