// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <Uefi.h>
#include <stddef.h>
#include <stdint.h>

// MSM8998 v2 / sagit only. These are configured-rate/readback evidence, not
// frequency-counter measurements or independent ownership of firmware clocks.
// No EFI Clock protocol is needed: the pinned revision 0x10005 has no public
// name for aux_clk or osm_clk. In particular, never call a 0x10009 suffix.
namespace BridgeOSMFirmware {
enum class Failure : uint32_t {
    None, Services, ChipProtocol, ChipRevision, ChipVersion, ChipID,
    PlatformProtocol, PlatformRevision, PlatformRead, BoardID, IO,
    InvalidRead, PLLInactive, PLLRate, RootBusy, RootUpdate, RootEnable,
    RootConfiguration, BranchEnable
};

// Prefixes from simple-init/include/{chipinfo,platforminfo}.h. ChipInfo's
// installed GUID is SimpleInit.dec's GUID, not the unused header macro.
struct ChipProtocol {
    UINT64 revision;
    EFI_STATUS (EFIAPI *getVersion)(ChipProtocol *, UINT32 *);
    void *getRawVersion;
    EFI_STATUS (EFIAPI *getID)(ChipProtocol *, UINT32 *);
};
struct PlatformInfo { UINT32 platform, version, subtype; BOOLEAN fusion; };
struct PlatformProtocol {
    UINT64 revision;
    EFI_STATUS (EFIAPI *getInfo)(PlatformProtocol *, PlatformInfo *);
};
static_assert(offsetof(ChipProtocol, getVersion) == 8, "ChipInfo version ABI");
static_assert(offsetof(ChipProtocol, getID) == 24, "ChipInfo ID ABI");
static_assert(sizeof(PlatformInfo) == 16, "PlatformInfo output ABI");
static_assert(offsetof(PlatformProtocol, getInfo) == 8, "PlatformInfo ABI");

struct Identity {
    uint32_t chipID = 0, chipVersion = 0, boardID = 0;
    uint32_t boardVersion = 0, boardSubtype = 0;
    uint64_t chipRevision = 0, platformRevision = 0;
    EFI_STATUS status = EFI_SUCCESS;
    Failure failure = Failure::None;
};

// Call before ExitBootServices; only read-only public EFI protocol methods.
// Board 30/subtype 0 is the sagit board matching contract in the pinned DTS.
// It is not a unique product identity or an attestation of secure firmware.
inline bool identity(EFI_BOOT_SERVICES *services, Identity &out) {
    out = {};
    if (!services || !services->LocateProtocol) {
        out.failure = Failure::Services; return false;
    }
    EFI_GUID chipGUID = {0xb0760469, 0x970c, 0x487a,
        {0xa4, 0xb5, 0x28, 0xdb, 0x7b, 0x45, 0xce, 0xf1}};
    EFI_GUID platformGUID = {0x157a5c45, 0x21b2, 0x43c5,
        {0xba, 0x7c, 0x82, 0x2f, 0xee, 0x5f, 0xe5, 0x99}};
    ChipProtocol *chip = nullptr;
    PlatformProtocol *platform = nullptr;
    out.status = services->LocateProtocol(&chipGUID, nullptr,
                                         reinterpret_cast<void **>(&chip));
    if (EFI_ERROR(out.status) || !chip) {
        out.failure = Failure::ChipProtocol; return false;
    }
    out.chipRevision = chip->revision;
    if (chip->revision < 0x10000 || chip->revision > 0x10002 ||
        !chip->getVersion || !chip->getID) {
        out.failure = Failure::ChipRevision; return false;
    }
    out.status = chip->getVersion(chip, &out.chipVersion);
    if (EFI_ERROR(out.status) ||
        (out.chipVersion != 0x20000 && out.chipVersion != 0x20001)) {
        out.failure = Failure::ChipVersion; return false;
    }
    out.status = chip->getID(chip, &out.chipID);
    if (EFI_ERROR(out.status) || out.chipID != 292) {
        out.failure = Failure::ChipID; return false;
    }
    out.status = services->LocateProtocol(&platformGUID, nullptr,
                                         reinterpret_cast<void **>(&platform));
    if (EFI_ERROR(out.status) || !platform) {
        out.failure = Failure::PlatformProtocol; return false;
    }
    out.platformRevision = platform->revision;
    if (platform->revision != 0x20000 || !platform->getInfo) {
        out.failure = Failure::PlatformRevision; return false;
    }
    PlatformInfo info{};
    out.status = platform->getInfo(platform, &info);
    if (EFI_ERROR(out.status)) { out.failure = Failure::PlatformRead; return false; }
    out.boardID = info.platform;
    out.boardVersion = info.version;
    out.boardSubtype = info.subtype;
    if (out.boardID != 30 || out.boardSubtype != 0 || info.fusion) {
        out.failure = Failure::BoardID; return false;
    }
    return true;
}

struct IO {
    void *ctx;
    uint32_t (*read32)(void *, uint64_t);
    void (*write32)(void *, uint64_t, uint32_t);
    void (*delayUS)(void *, uint32_t);
};
constexpr uint64_t GCCBase = 0x00100000;
constexpr uint64_t AuxCommand = GCCBase + 0x4805c;
constexpr uint64_t OSMCommand = 0x179d1000 + 0x12c;
constexpr uint64_t CPRCommand = GCCBase + 0x48044;
constexpr uint64_t CPRBranch = GCCBase + 0x48008;
constexpr uint32_t RootMask = 0x0000371f; // src, HID, MND mode
constexpr uint32_t AuxConfig = 0x103;      // GPLL0 main / 2
constexpr uint32_t OSMConfig = 0x102;      // fixed LMH 300 MHz / 1.5
constexpr uint32_t CPRConfig = 0x001;      // XO / 1
constexpr uint32_t ConfigDirtyMask = 0xf0;
constexpr uint32_t TimeoutUS = 500;

struct Evidence {
    uint32_t auxHz = 0, osmHz = 0, xoHz = 0, rbcprHz = 0;
    uint32_t pllMode = 0, pllUser = 0, pllL = 0, pllAlpha = 0;
    uint32_t auxCommand = 0, auxConfig = 0, osmCommand = 0, osmConfig = 0;
    uint32_t rbcprCommand = 0, rbcprConfig = 0, rbcprBranch = 0;
    Failure failure = Failure::None;
    uint64_t address = 0;
    uint32_t raw = 0, expected = 0;
    bool ready = false;
};

inline bool fail(Evidence &out, Failure why, uint64_t address = 0,
                 uint32_t raw = 0, uint32_t expected = 0) {
    out.ready = false;
    out.auxHz = out.osmHz = out.xoHz = out.rbcprHz = 0;
    out.failure = why; out.address = address; out.raw = raw; out.expected = expected;
    return false;
}

inline bool read(IO io, Evidence &out, uint64_t address, uint32_t &value) {
    value = io.read32(io.ctx, address);
    return value != UINT32_MAX || fail(out, Failure::InvalidRead, address, value);
}

// Fabia registers and clock rate formula: gcc-msm8998.c and clk-alpha-pll.c.
// Observe manual OUTCTRL and firmware vote/FSM mode. Never retune the shared
// GPLL0, clear another master's vote, or pretend to have an exclusive lease.
inline bool pllInspect(IO io, Evidence &out) {
    if (!read(io, out, GCCBase, out.pllMode) ||
        !read(io, out, GCCBase + 0x0c, out.pllUser) ||
        !read(io, out, GCCBase + 4, out.pllL) ||
        !read(io, out, GCCBase + 0x38, out.pllAlpha)) return false;
    const uint32_t required = out.pllMode & (1u << 20) ? 0xc0000000 : 0x80000001;
    if ((out.pllMode & required) != required || !(out.pllUser & 1))
        return fail(out, Failure::PLLInactive, GCCBase, out.pllMode, required);
    const uint64_t hz = uint64_t(19200000) * out.pllL +
        ((uint64_t(19200000) * (out.pllAlpha & 0xffff)) >> 16);
    if (hz != 600000000)
        return fail(out, Failure::PLLRate, GCCBase + 4, out.pllL, 31);
    return true;
}

inline bool rootInspect(IO io, Evidence &out, uint64_t command, uint32_t config,
                        uint32_t &cmdRaw, uint32_t &cfgRaw) {
    if (!read(io, out, command, cmdRaw) || !read(io, out, command + 4, cfgRaw))
        return false;
    if (cmdRaw & (1 | ConfigDirtyMask))
        return fail(out, Failure::RootBusy, command, cmdRaw);
    if ((cmdRaw & 0x80000002) != 2)
        return fail(out, Failure::RootEnable, command, cmdRaw, 2);
    if ((cfgRaw & RootMask) != config)
        return fail(out, Failure::RootConfiguration, command + 4, cfgRaw, config);
    return true;
}

// Read-only, callable after EBS; clears stale success evidence on every call.
// The fixed source contract comes from Xiaomi clock-osm.c's 200 MHz table
// (source selector 1, HID /1.5), not a cached EFI CalculateFrequency result.
// The XO evidence is the active CPR root selecting XO /1 plus branch readback.
// Frequencies are derived from the sagit/MSM8998 19.2 MHz crystal contract.
inline bool clocksInspect(IO io, Evidence &out) {
    out = {};
    if (!io.read32) return fail(out, Failure::IO);
    if (!pllInspect(io, out) ||
        !rootInspect(io, out, AuxCommand, AuxConfig, out.auxCommand, out.auxConfig) ||
        !rootInspect(io, out, OSMCommand, OSMConfig, out.osmCommand, out.osmConfig) ||
        !rootInspect(io, out, CPRCommand, CPRConfig, out.rbcprCommand, out.rbcprConfig) ||
        !read(io, out, CPRBranch, out.rbcprBranch)) return false;
    if ((out.rbcprBranch & 0x80000001) != 1)
        return fail(out, Failure::BranchEnable, CPRBranch, out.rbcprBranch, 1);
    // Detect a parent lost during the snapshot, not just at its beginning.
    if (!pllInspect(io, out)) return false;
    out.auxHz = 300000000; out.osmHz = 200000000;
    out.xoHz = out.rbcprHz = 19200000; out.ready = true;
    return true;
}

inline bool poll(IO io, Evidence &out, uint64_t address, uint32_t mask,
                 uint32_t value, Failure failure) {
    uint32_t raw = 0;
    for (uint32_t us = 0; us <= TimeoutUS; ++us) {
        if (!read(io, out, address, raw)) return false;
        if ((raw & mask) == value) return true;
        if (us != TimeoutUS) io.delayUS(io.ctx, 1);
    }
    return fail(out, failure, address, raw, value);
}

inline bool rootPrepare(IO io, Evidence &out, uint64_t command, uint32_t config) {
    uint32_t cmd = 0, cfg = 0;
    if (!read(io, out, command, cmd) || !read(io, out, command + 4, cfg)) return false;
    // An in-flight firmware update cannot be overwritten safely.
    if (cmd & 1) return fail(out, Failure::RootBusy, command, cmd);
    if ((cfg & RootMask) != config || (cmd & ConfigDirtyMask)) {
        io.write32(io.ctx, command + 4, (cfg & ~RootMask) | config);
        // clock-local2.c set_rate_hid: program CFG then latch UPDATE and wait.
        // These three roots have no non_local_children/control_timeout flag:
        // the original rcg_clk_set_rate does not force the old source on first.
        io.write32(io.ctx, command, cmd | 1);
        if (!poll(io, out, command, 1, 0, Failure::RootUpdate)) return false;
    }
    if (!read(io, out, command, cmd)) return false;
    if (!(cmd & 2)) io.write32(io.ctx, command, cmd | 2);
    if (!poll(io, out, command, 0x80000003, 2, Failure::RootEnable)) return false;
    uint32_t cmdRaw = 0, cfgRaw = 0;
    return rootInspect(io, out, command, config, cmdRaw, cfgRaw);
}

// Post-EBS only, after identity() has accepted this board. Retain the three
// roots and CPR branch for the entire OSM/CPRh lifetime; there is no release.
// GCC aux/CPR definitions: xiaomi-clock-gcc-8998.c:835,1043,1727.
// APCS OSM CMD offset: xiaomi-msm-clocks-hwio-8998.h APCS_COMMON_LMH_CMD_RCGR
// (0x12c); source/HID and init sequence: Xiaomi clock-osm.c:744,3300.
// RCG programming/poll semantics: xiaomi-clock-local2.c:109-218,350-409.
// Commit and SHA256 pins: docs/power/osm-firmware-sources.json.
// No rollback after a timeout: another clock consumer may already use a root.
inline bool clocksPrepare(IO io, Evidence &out) {
    out = {};
    if (!io.read32 || !io.write32 || !io.delayUS) return fail(out, Failure::IO);
    if (!pllInspect(io, out) ||
        !rootPrepare(io, out, AuxCommand, AuxConfig) || !pllInspect(io, out) ||
        !rootPrepare(io, out, OSMCommand, OSMConfig) ||
        !rootPrepare(io, out, CPRCommand, CPRConfig)) return false;
    uint32_t branch = 0;
    if (!read(io, out, CPRBranch, branch)) return false;
    if (!(branch & 1)) io.write32(io.ctx, CPRBranch, branch | 1);
    if (!poll(io, out, CPRBranch, 0x80000001, 1, Failure::BranchEnable)) return false;
    return clocksInspect(io, out);
}
}
