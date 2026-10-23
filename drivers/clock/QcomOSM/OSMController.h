// SPDX-License-Identifier: GPL-2.0-only
// MSM8998 OSM v1/v2 layout from Qualcomm clock-osm.c (pinned reference).
#pragma once
#include <QcomPowerHardware.h>
namespace QcomPower {
class OSMController : public Registers {
public:
    enum class InitializationFailure : uint32_t {
        None, MissingIO, Disabled, IndexMismatch, CoreCount, FrequencyRange,
        VoltageRange, FrequencyOrder, VoltageOrder, DuplicateVoltage, NoQuadCoreStates
    };
    static constexpr uint32_t NoLocation = UINT32_MAX;
    struct InitializationDiagnostic {
        InitializationFailure reason = InitializationFailure::None;
        uint32_t bank = NoLocation, row = NoLocation;
        uint32_t rawIndex = 0, rawFrequency = 0, rawVoltage = 0, rawEnable = 0;
    };
private:
    struct Entry { uint32_t hz = 0, uv = 0, index = 0, frequency = 0, voltage = 0; } table[2][40];
    struct Row {uint32_t frequency=0,voltage=0,state=0,overrideValue=0,spare=0;} rows[2][40];
    unsigned count[2] = {};
    uint32_t ceiling[2] = {};
    bool ready[2] = {}, fault[2] = {};
    InitializationDiagnostic diagnostic{};
    void clearState() {
        for (unsigned cluster = 0; cluster < 2; ++cluster) {
            count[cluster] = ceiling[cluster] = 0;
            ready[cluster] = fault[cluster] = false;
            for (unsigned row = 0; row < 40; ++row) {
                table[cluster][row] = {};
                rows[cluster][row] = {};
            }
        }
    }
    Status initializationFailed(InitializationFailure reason, Status result,
                                uint32_t bank = NoLocation, uint32_t row = NoLocation,
                                uint32_t index = 0, uint32_t frequency = 0,
                                uint32_t voltage = 0, uint32_t enable = 0) {
        clearState();
        diagnostic = {reason, bank, row, index, frequency, voltage, enable};
        return result;
    }
    static uint32_t base(unsigned cluster) { return cluster*0x2000; }
    uint32_t rowRate(unsigned cluster,uint32_t index) const {
        if (index>=40) return 0;
        const auto &row=rows[cluster][index];
        uint32_t offset=base(cluster)+0x1150+32*index;
        if (!row.state || rd(offset)!=index || rd(offset+4)!=row.frequency ||
            rd(offset+8)!=row.voltage || rd(offset+12)!=row.overrideValue ||
            rd(offset+20)!=row.spare) return 0;
        return table[cluster][row.state-1].hz;
    }
public:
    const InitializationDiagnostic &initializationDiagnostic() const { return diagnostic; }
    Status initialize(IO access) {
        // A fresh probe replaces the entire snapshot, including a prior fault.
        // Never append OPPs or leave a previously accepted bank writable when
        // the replacement table fails validation.
        clearState();
        diagnostic = {};
        setIO(access);
        if (!access.read || !access.write || !access.wait)
            return initializationFailed(InitializationFailure::MissingIO, Invalid);
        for (unsigned cluster = 0; cluster < 2; ++cluster) {
            const uint32_t enable = rd(base(cluster)+0x1004);
            if (!(enable&1)) continue;
            uint32_t lastHz = 0, lastUV = 0;
            for (unsigned i = 0; i < 40; ++i) {
                uint32_t offset = base(cluster)+0x1150+32*i;
                uint32_t f = rd(offset+4), v = rd(offset+8);
                const uint32_t overrideValue = rd(offset+12), spare = rd(offset+20);
                uint32_t index = rd(offset);
                const auto fail = [&](InitializationFailure reason) {
                    return initializationFailed(reason, Invalid, cluster, i, index, f, v, enable);
                };
                if (index != i) return fail(InitializationFailure::IndexMismatch);
                uint32_t cores = (f >> 16)&7, src = (f >> 26)&3, l = f&255;
                if (cores != 1 && cores != 4) return fail(InitializationFailure::CoreCount);
                uint64_t decodedHz = src ? 19200000ULL*l : 300000000ULL;
                uint32_t uv = (v&0xfff)*1000;
                if (!decodedHz || decodedHz > 2800000000ULL) return fail(InitializationFailure::FrequencyRange);
                if (uv < 300000 || uv > 1500000) return fail(InitializationFailure::VoltageRange);
                uint32_t hz = static_cast<uint32_t>(decodedHz);
                // Single-core turbo entries cannot be selected on an active
                // four-core cluster. Only publish/choose the quad-core table.
                if (cores != 4) continue;
                if (hz < lastHz) return fail(InitializationFailure::FrequencyOrder);
                if (uv < lastUV) return fail(InitializationFailure::VoltageOrder);
                if (hz == lastHz) {
                    if (uv != lastUV) return fail(InitializationFailure::DuplicateVoltage);
                    rows[cluster][i]={f,v,count[cluster],overrideValue,spare};continue;
                }
                table[cluster][count[cluster]++] = {hz,uv,i,f,v};
                rows[cluster][i]={f,v,count[cluster],overrideValue,spare};lastHz = hz; lastUV = uv;
            }
            if (!count[cluster])
                return initializationFailed(InitializationFailure::NoQuadCoreStates, Invalid,
                                            cluster, NoLocation, 0, 0, 0, enable);
            ceiling[cluster] = lastHz;
        }
        // OSM also needs HLOS LUT/CPRh/ACD initialization after secure-world
        // sequencer setup. The bridge owns that sequence; this runtime reader
        // never enables an uninitialized machine or guesses fuse voltages.
        if (!count[0] && !count[1])
            return initializationFailed(InitializationFailure::Disabled, NotReady);
        // Commit admission only after every enabled bank has been validated.
        ready[0] = count[0] != 0; ready[1] = count[1] != 0;
        return OK;
    }
    unsigned stateCount(unsigned cluster) const { return cluster < 2 && ready[cluster] && !fault[cluster] ? count[cluster] : 0; }
    uint32_t state(unsigned cluster, unsigned index, bool uv) const {
        return index < stateCount(cluster) ? (uv ? table[cluster][index].uv : table[cluster][index].hz) : 0;
    }
    uint32_t minimum(unsigned cluster) const { return cluster < 2 && ready[cluster] ? table[cluster][0].hz : 0; }
    uint32_t maximum(unsigned cluster) const { return cluster < 2 && ready[cluster] ? table[cluster][count[cluster]-1].hz : 0; }
    uint32_t limit(unsigned cluster) const { return cluster < 2 ? ceiling[cluster] : 0; }
    uint32_t rate(unsigned cluster) const {
        if (cluster > 1 || !ready[cluster] || fault[cluster] || !(rd(base(cluster)+0x1004)&1)) return 0;
        uint32_t index=rd(base(cluster)+0x1f10);
        // Firmware can leave the desired index on a duplicate LUT row. Keep
        // every validated quad-core row as an alias of the unique public OPP.
        return rowRate(cluster,index);
    }
    uint32_t voltage(unsigned cluster) const {
        uint32_t hz = rate(cluster);
        for (unsigned i = 0; cluster < 2 && i < count[cluster]; ++i) if (table[cluster][i].hz == hz) return table[cluster][i].uv;
        return 0;
    }
    Status setRate(unsigned cluster, uint64_t hz) {
        if (cluster > 1) return Invalid;
        if (!ready[cluster]) return NotReady;
        if (fault[cluster]) return Stale;
        if (hz > ceiling[cluster]) return Denied;
        const Entry *entry = nullptr;
        for (unsigned i = 0; i < count[cluster]; ++i) if (table[cluster][i].hz == hz) entry = &table[cluster][i];
        if (!entry) return Unsupported;
        uint32_t b = base(cluster);
        // Firmware/LMH changes to the table invalidate the previously checked
        // voltage policy. Read before every request, not only at driver start.
        if (!(rd(b+0x1004)&1) || rowRate(cluster,entry->index) != entry->hz) {
            fault[cluster] = true; return Stale;
        }
        uint32_t before = rd(b+0x1f10);
        uint32_t beforeHz = rowRate(cluster,before);
        // A request must have a known safe rollback state. In particular a
        // single-core turbo index is not valid for this quad-core policy.
        if (!beforeHz) { fault[cluster]=true;return Stale; }
        wr(b+0x1f10, entry->index);
        if (!poll(b+0x1f10, 0xffffffffU, entry->index, 1000)) {
            // The table or enable state can change during the failed poll.
            // Never write the cached index back into a changed voltage LUT.
            if (!(rd(b+0x1004)&1) || rowRate(cluster,before)!=beforeHz) {
                fault[cluster]=true;return Stale;
            }
            wr(b+0x1f10, before);
            fault[cluster] = !poll(b+0x1f10, 0xffffffffU, before, 1000) || rate(cluster)!=beforeHz;
            return fault[cluster] ? Stale : TimedOut;
        }
        // A matching desired-index readback does not protect against firmware
        // changing the enable state or any target LUT field during the poll.
        if (!(rd(b+0x1004)&1) || rowRate(cluster,entry->index) != entry->hz) {
            fault[cluster] = true; return Stale;
        }
        // OSM sequences voltage/PLL/APM and enforces CPRh/LMH constraints. This
        // is the accepted desired P-state, not proof of an instantaneous rate.
        return OK;
    }
    Status setLimit(unsigned cluster, uint64_t hz) {
        if (cluster > 1 || !ready[cluster]) return Invalid;
        uint32_t selected = 0;
        if (!hz) hz = maximum(cluster); // clear the policy ceiling.
        for (unsigned i = 0; i < count[cluster]; ++i) if (table[cluster][i].hz <= hz) selected = table[cluster][i].hz;
        if (!selected) return Invalid;
        uint32_t old = ceiling[cluster]; ceiling[cluster] = selected;
        uint32_t current = rate(cluster);
        if (!current) { ceiling[cluster] = old; return NotReady; }
        if (current > selected) {
            Status result = setRate(cluster, selected);
            if (result != OK) { ceiling[cluster] = old; return result; }
        }
        return OK;
    }
};
}
