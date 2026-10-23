// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include "CPRhCalibration.h"
#include "OSMTZContract.h"

// MSM8998 v2/v2.1 HLOS half of OSM initialization. Register sequences and
// policies follow the pinned Xiaomi sagit sources in docs/power/reference.
// The original TZ boot chain still owns sequencer microcode, PLL analog setup,
// and secure PMIC access. Accessible v2 boot sequencer parameters are checked;
// their presence is not cryptographic attestation of that firmware.
namespace QcomOSMInit {
enum Region : unsigned { PowerOSM, PerformanceOSM, PowerACD, PerformanceACD };
enum class Error : unsigned {
    None, InvalidInterface, UnsupportedIdentity, InvalidPlan, ClocksNotRetained,
    AlreadyEnabled, ReadFailed, WriteFailed, ReadbackFailed, TZNotReady,
    SecureWriteFailed, ACDTimeout, NotPrepared
};
enum class Stage : unsigned { Admission, Firmware, LUT, Policy, SecureParameters, ACD, Prepared, Enable, Complete };
struct IO {
    void *context;
    bool (*read)(void *, unsigned region, uint32_t offset, uint32_t &value);
    bool (*write)(void *, unsigned region, uint32_t offset, uint32_t value);
    bool (*secureRead)(void *, uint32_t physical, uint32_t &value);
    bool (*secureWrite)(void *, uint32_t physical, uint32_t value);
    void (*barrier)(void *);
    void (*delayUS)(void *, unsigned);
};
struct Clocks {
    uint32_t auxiliaryHz, osmHz, xoHz;
    // Set only after retaining all three clocks and checking their live
    // parent/divider configuration; these are not frequency-counter readings.
    bool retained;
};
struct Result {
    Error error = Error::None;
    Stage stage = Stage::Admission;
    unsigned cluster = 0;
    uint32_t offset = 0, expected = 0, observed = 0;
    uint32_t firmwareChecks[2] = {};
    unsigned writes = 0, enabledMask = 0;
};
constexpr uint32_t osmBase(unsigned cluster) { return 0x179c0000U + cluster * 0x2000U; }
constexpr uint32_t acdBase(unsigned cluster) { return cluster ? 0x17814800U : 0x17914800U; }
constexpr uint32_t seq(unsigned number) { return 0x300U + number * 4U; }

// Exported so the EFI SCM adapter can enforce the same physical-address
// allowlist. No sequencer instructions or secure PLL/PMIC addresses are written.
inline bool permittedSecureWrite(uint32_t physical) {
    return QcomOSMTZ::permitted(physical);
}
inline bool permittedSecureRead(uint32_t physical) {
    return QcomOSMTZ::permitted(physical);
}
inline bool allowedSecureAddress(uint32_t physical, bool write) {
    return write ? permittedSecureWrite(physical) : permittedSecureRead(physical);
}

class Engine {
    IO io{};
    Result *result = nullptr;
    bool prepared = false;
    unsigned current = 0;
    bool fail(Error error, uint32_t offset = 0, uint32_t expected = 0, uint32_t observed = 0) {
        result->error = error;
        result->cluster = current;
        result->offset = offset;
        result->expected = expected;
        result->observed = observed;
        prepared = false;
        return false;
    }
    bool read(unsigned region, uint32_t offset, uint32_t &value) {
        return io.read(io.context, region, offset, value) || fail(Error::ReadFailed, offset);
    }
    bool write(unsigned region, uint32_t offset, uint32_t value, bool verify = true) {
        if (!io.write(io.context, region, offset, value)) return fail(Error::WriteFailed, offset, value);
        ++result->writes;
        io.barrier(io.context);
        if (!verify) return true;
        uint32_t observed;
        if (!read(region, offset, observed)) return false;
        return observed == value || fail(Error::ReadbackFailed, offset, value, observed);
    }
    bool bits(uint32_t offset, uint32_t mask, uint32_t value) {
        uint32_t old;
        return read(current, offset, old) && write(current, offset, (old & ~mask) | (value & mask));
    }
    bool secureRead(uint32_t offset, uint32_t &value) {
        const uint32_t physical = osmBase(current) + offset;
        return (permittedSecureRead(physical) && io.secureRead(io.context, physical, value)) ||
            fail(Error::ReadFailed, offset);
    }
    bool secureWrite(unsigned number, uint32_t value) {
        const uint32_t offset = seq(number), physical = osmBase(current) + offset;
        if (!permittedSecureWrite(physical) || !io.secureWrite(io.context, physical, value))
            return fail(Error::SecureWriteFailed, offset, value);
        ++result->writes;
        io.barrier(io.context);
        uint32_t observed;
        if (!secureRead(offset, observed)) return false;
        return observed == value || fail(Error::ReadbackFailed, offset, value, observed);
    }
    bool firmware() {
        // Stock TZ selects different sequencer tables for v1 and v2. The old
        // common/no-TZ parameter addresses are neither v2 signatures nor
        // readable through this monitor's SCM IO interface. Match only the
        // eight accessible, unmodified boot values from the verified v2 table.
        const auto &required = QcomOSMTZ::BootParameters;
        for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
            uint32_t observed;
            if (!secureRead(seq(required[i].number), observed)) return false;
            if (observed != required[i].initial[current])
                return fail(Error::TZNotReady, seq(required[i].number), required[i].initial[current], observed);
            result->firmwareChecks[current] |= 1U << i;
        }
        // Do not probe protected instruction/branch/PLL/PMIC memory or assume
        // that a successful SCM status authorizes a register access.
        return true;
    }
    bool table(const QcomCPRh::Plan &plan) {
        for (unsigned i = 0; i < 40; ++i) {
            const auto &row = plan.osm[i];
            const uint32_t offset = 0x1150 + i * 32;
            if (!write(current, offset, i) || !write(current, offset + 4, row.frequency) ||
                !write(current, offset + 8, row.voltage) ||
                !write(current, offset + 12, row.overrideValue) ||
                !write(current, offset + 20, row.spare)) return false;
        }
        return true;
    }
    bool policy() {
        // Fixed 200 MHz OSM clock: cycles = ns / 5. Both LLM vote directions
        // use 327675 ns == 65535 cycles. Retention mapping follows the actual
        // reference implementation (set-ret-inactive writes zero).
        const struct { uint32_t offset, value; } registers[] = {
            {0x1f00, 1U | (9U << 1) | (1U << 8)}, // floor(OSM/XO)-1
            {0x101c, (200U << 16) | 200U}, {0x100c, 1},
            {0x1024, 0}, {0x1020, 0},
            {0x102c, 0xffffffffU}, {0x1030, 0xffffffffU}
        };
        for (const auto &reg : registers)
            if (!write(current, reg.offset, reg.value)) return false;
        // The pinned vendor driver ORs zero for Sagit's enabled LLM votes;
        // it does not clear firmware's interface-disable bits. Preserve that
        // boot policy until the LLM owner explicitly changes it.
        uint32_t llm;
        if (!read(current, 0x1034, llm) || !write(current, 0x1034, llm)) return false;
        // Sagit enables CC, DCVS, PS boost and PC, WFx, DCVS droop. VMIN
        // reduction is not enabled. Program all timers before enabling FSMs.
        const unsigned boostTimers[] = {0x1074U, 0x1084U, 0x1094U};
        for (unsigned base : boostTimers) {
            if (!write(current, base, (1000U << 16) | 2000U) ||
                !write(current, base + 4, (2000U << 16) | 2000U) ||
                !bits(base + 8, 0xffff, 200)) return false;
        }
        return write(current, 0x10a0, 5) &&
            write(current, 0x10ac, (100U << 16) | 100U) &&
            write(current, 0x10b0, (50U << 16) | 50U) &&
            write(current, 0x10bc, 1) &&
            write(current, 0x10a8, 1) &&
            write(current, 0x10b8, 100) &&
            bits(0x10a4, 0x807f007fU, 0x80020008U) &&
            bits(0x10c0, 1, 1) &&
            bits(0x1070, 0x3f, 0x3f);
    }
    bool secureParameters(const QcomCPRh::Plan &plan) {
        // Stock v2 exposes only the upper MEM-ACC threshold pair to HLOS.
        // A threshold at corner zero uses 0xff as the preceding-corner sentinel
        // (xiaomi-clock-osm.c); no unsigned decrement is performed at zero.
        const uint32_t mem = plan.memAccThresholdCorner;
        if (!secureWrite(88, plan.memAccCrossoverCorner) ||
            !secureWrite(57, mem ? mem - 1 : 0xff) ||
            !secureWrite(58, mem)) return false;
        // SEQ55/56 retain TZ's fixed values, 0x2c5ffd/0x2f5ffd.
        // The reference writes each matching row (last match wins if turbo
        // and four-core rows share a corner); preserve that behavior.
        for (unsigned i = 0; i < plan.osmCount; ++i)
            if ((plan.osm[i].voltage >> 16) == mem &&
                !secureWrite(32, plan.osm[i].frequency & 0xffU)) return false;
        const uint32_t apm = plan.apmThresholdCorner;
        return write(current, 0x1048, apm) &&
            secureWrite(72, plan.apmCrossoverCorner) &&
            secureWrite(15, apm) && secureWrite(31, apm ? apm - 1 : 0xff) &&
            secureWrite(76, 0x39U | (apm << 6));
    }
    bool acdPoll(uint32_t offset, uint32_t mask, unsigned attempts) {
        for (unsigned i = 0; i < attempts; ++i) {
            uint32_t value;
            if (!read(current + PowerACD, offset, value)) return false;
            if ((value & mask) == mask) return true;
            if (i + 1 != attempts) io.delayUS(io.context, 1);
        }
        return fail(Error::ACDTimeout, offset, mask);
    }
    bool acdLocal(uint32_t offset, uint32_t value) {
        const unsigned region = current + PowerACD;
        return write(region, offset, value) && write(region, 0x90, 0, false) &&
            write(region, 0x90, ((offset / 4) << 1) | 1, false) &&
            acdPoll(0x94, 1U << (offset / 4), 5);
    }
    bool acd() {
        const unsigned region = current + PowerACD;
        constexpr uint32_t initialMask = (1U << 2) | (1U << 1) | (1U << 10) | (1U << 12);
        return write(region, 0x08, 0x9611) &&
            write(region, 0x04, 0x002b5ffd) &&
            write(region, 0x28, 0x501) &&
            write(region, 0x30, 0x02cf9ae8) &&
            write(region, 0x88, 0x15) &&
            write(region, 0x80, initialMask) &&
            write(region, 0x84, 0, false) &&
            write(region, 0x84, 1, false) &&
            acdPoll(0x8c, 1, 20) &&
            acdLocal(0x3c, 1) && acdLocal(0x34, 1) && acdLocal(0x34, 0) &&
            (io.delayUS(io.context, 1), true) &&
            acdLocal(0x30, 0x02cf9afe) &&
            write(region, 0x80, initialMask | (1U << 15));
    }
public:
    static bool validIdentity(const QcomCPRh::Identity &id) {
        return id.chipID == 292 && (id.chipVersion == 0x20000 || id.chipVersion == 0x20001) &&
            id.boardID == 30;
    }
    static bool validPlan(const QcomCPRh::Plan &plan, unsigned cluster) {
        if (!validIdentity(plan.identity) || plan.cluster != cluster || plan.osmCount == 0 ||
            plan.osmCount > 40 || plan.count > 40 || plan.activeCount == 0 ||
            plan.activeCount + 2 != plan.count ||
            plan.apmCrossoverCorner != plan.activeCount ||
            plan.memAccCrossoverCorner != plan.activeCount + 1 ||
            plan.apmThresholdCorner > 39 || plan.memAccThresholdCorner > 39 ||
            plan.osm[0].frequencyHz != 300000000 || plan.osm[0].frequency != 0x0004000f ||
            (plan.osm[0].voltage >> 16) != 0 || plan.osm[0].spare != 1)
            return false;
        uint32_t previousCorner = 0, previousSpare = 1;
        unsigned transitions = 0;
        for (unsigned i = 0; i < 40; ++i) {
            const auto &row = plan.osm[i];
            const uint32_t corner = row.voltage >> 16, mv = row.voltage & 0xffff;
            const uint32_t cores = (row.frequency >> 16) & 7;
            const uint32_t hz = ((row.frequency >> 26) & 3) ?
                19200000U * (row.frequency & 0xff) : 300000000U;
            if (corner >= plan.activeCount || mv < 500 || mv > 1300 ||
                mv * 1000 != plan.corners[corner].ceilingUV ||
                (cores != 1 && cores != 4) || row.frequencyHz != hz ||
                row.spare < 1 || row.spare > 3 || corner < previousCorner ||
                (i && corner == previousCorner && row.spare != previousSpare)) return false;
            if (row.spare != previousSpare) {
                if (row.spare != previousSpare + 1) return false;
                ++transitions;
            }
            if (i >= plan.osmCount) {
                const auto &last = plan.osm[plan.osmCount - 1];
                if (row.frequencyHz != last.frequencyHz || row.frequency != last.frequency ||
                    row.voltage != last.voltage || row.overrideValue != last.overrideValue ||
                    row.spare != last.spare) return false;
            }
            previousCorner = corner;
            previousSpare = row.spare;
        }
        return transitions == 2;
    }
    // No writes: caller can perform this before initializing CPRh. Original
    // Linux resolves the CPRh regulators before programming OSM.
    bool inspect(IO access, const QcomCPRh::Identity &identity, const Clocks &clocks,
                 const QcomCPRh::Plan (&plans)[2], Result &out) {
        io = access; result = &out; out = Result{}; prepared = false; current = 0;
        if (!io.read || !io.write || !io.secureRead || !io.secureWrite || !io.barrier || !io.delayUS)
            return fail(Error::InvalidInterface);
        if (!validIdentity(identity)) return fail(Error::UnsupportedIdentity);
        if (!clocks.retained || clocks.auxiliaryHz != 300000000 ||
            clocks.osmHz != 200000000 || clocks.xoHz != 19200000)
            return fail(Error::ClocksNotRetained);
        for (current = 0; current < 2; ++current) {
            const auto &id = plans[current].identity;
            if (id.chipID != identity.chipID || id.chipVersion != identity.chipVersion ||
                id.boardID != identity.boardID || !validPlan(plans[current], current))
                return fail(Error::InvalidPlan);
            uint32_t enabled;
            if (!read(current, 0x1004, enabled)) return false;
            if (enabled & 1) return fail(Error::AlreadyEnabled, 0x1004, 0, enabled);
        }
        result->stage = Stage::Firmware;
        for (current = 0; current < 2; ++current) if (!firmware()) return false;
        return true;
    }
    // Call after successful CPRh initialization/readback, while the CPUs still
    // use firmware's boot clocks. All failures leave OSM disabled.
    bool prepare(IO access, const QcomCPRh::Identity &identity, const Clocks &clocks,
                 const QcomCPRh::Plan (&plans)[2], Result &out) {
        if (!inspect(access, identity, clocks, plans, out)) return false;
        for (current = 0; current < 2; ++current) {
            result->stage = Stage::LUT;
            if (!table(plans[current])) return false;
        }
        for (current = 0; current < 2; ++current) {
            result->stage = Stage::Policy;
            if (!policy()) return false;
        }
        for (current = 0; current < 2; ++current) {
            result->stage = Stage::SecureParameters;
            if (!secureParameters(plans[current])) return false;
        }
        for (current = 0; current < 2; ++current) {
            result->stage = Stage::ACD;
            if (!acd()) return false;
        }
        current = 0; result->stage = Stage::Prepared; prepared = true;
        return true;
    }
    bool enable() {
        if (!result) return false;
        if (!prepared) return fail(Error::NotPrepared);
        result->stage = Stage::Enable;
        for (current = 0; current < 2; ++current) {
            uint32_t value;
            if (!read(current, 0x1004, value)) return false;
            if (value & 1) return fail(Error::AlreadyEnabled, 0x1004, 0, value);
        }
        for (current = 0; current < 2; ++current)
            if (!write(current, 0x1f10, 0)) return false;
        for (current = 0; current < 2; ++current) {
            // Record a successful enable write even if the subsequent readback
            // fails. Never disable a running CPU clock controller on failure.
            if (!io.write(io.context, current, 0x1004, 1)) return fail(Error::WriteFailed, 0x1004, 1);
            ++result->writes; result->enabledMask |= 1U << current;
            io.barrier(io.context); io.delayUS(io.context, 5);
            uint32_t value;
            if (!read(current, 0x1004, value)) return false;
            if (value != 1) return fail(Error::ReadbackFailed, 0x1004, 1, value);
        }
        current = 0; prepared = false; result->stage = Stage::Complete;
        return true;
    }
};
} // namespace QcomOSMInit
