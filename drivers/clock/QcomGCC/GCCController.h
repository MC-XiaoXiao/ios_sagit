// SPDX-License-Identifier: GPL-2.0-only
// Register protocols adapted from Linux v6.12 Qualcomm clk/gdsc drivers.
#pragma once
#include "GCCRegisters.h"
#include <RetainedGPUState.h>

namespace GCC {
// A shared PLL in vote/FSM mode reports ACTIVE rather than manual OUTCTRL.
// Observe both modes without changing the firmware's GPLL0 vote or rate.
constexpr bool sharedPLLEnabled(uint32_t mode, uint32_t user) {
    if (mode == UINT32_MAX || user == UINT32_MAX || !(user & 1)) return false;
    const uint32_t required = mode & (1u << 20) ? 0xc0000000u : 0x80000001u;
    return (mode & required) == required;
}
enum Status { Success, BadArgument, Unsupported, NotReady, Busy, Timeout, IOFailure, Overflow };
struct RegisterIO {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    void (*delay)(void *, unsigned);
};

// All calls are serialized by GenericClockController's command gate. No heap,
// kernel globals or IOKit dependency: the same state machine is exercised by
// the host MMIO simulator. Start is read-only; firmware votes remain pinned.
class Controller {
    struct ClockState { uint32_t refs = 0; bool allowed = false, pinned = false, fault = false; };
    struct RootState { uint32_t ceiling = 0; bool fault = false; };
    struct PowerState { uint32_t refs = 0; bool allowed = false, pinned = false, fault = false; };
    RegisterIO io;
    bool ready = false;
    ClockState clocks[BranchCount];
    RootState sources[RootCount];
    PowerState powers[DomainCount];
    bool resetAllowed[ResetCount] = {};
    uint32_t fixedRates[BranchCount] = {};
    GPUPlatform::RetainedState gpuState;
    struct UARTState { uint64_t token=0,serial=0; uint32_t configuration[4]{},force=0; bool pinned=false; } uartState;
    uint64_t modemToken=0,modemSerial=0,modemPLLRate=0;
    static bool gpuClock(uint32_t id) { return (id>=75 && id<=78) || id==176 || id==185 || id==186; }
    uint32_t read(uint32_t reg) const { return io.read(io.context, reg); }
    void write(uint32_t reg, uint32_t v) { io.write(io.context, reg, v); }
    void delay(unsigned us) { io.delay(io.context, us); }
    void update(uint32_t reg, uint32_t mask, uint32_t value) { write(reg, (read(reg) & ~mask) | (value & mask)); }
    bool poll(uint32_t reg, uint32_t mask, uint32_t value, unsigned us) {
        for (unsigned i = 0; i <= us; ++i) {
            if ((read(reg) & mask) == value) return true;
            if (i != us) delay(1);
        }
        return false;
    }
    static int rootIndex(uint32_t id) {
        for (unsigned i = 0; i < RootCount; ++i) if (roots[i].id == id) return i;
        return -1;
    }
    static int branchIndex(uint32_t id) {
        for (unsigned i = 0; i < BranchCount; ++i) if (branches[i].id == id) return i;
        return -1;
    }
    uint64_t parentRate(uint32_t source) const {
        if (source == 0) return 19200000;
        if (source != 1 && source != 6) return 0;
        // GPLL0 is shared with RPM/firmware: inspect the Fabia PLL, never
        // retune it or change another master's vote. Both selectors on this
        // GCC map to GPLL0's main output (see gcc_parent_data_0).
        if (!sharedPLLEnabled(read(0), read(0x0c))) return 0;
        uint32_t l = read(4), alpha = read(0x38) & 0xffff;
        if (!l || l > 104) return 0;
        return uint64_t(19200000) * l + ((uint64_t(19200000) * alpha) >> 16);
    }
    bool waitBranch(const Branch &b, bool on) {
        if (b.mode == 2) return true; // PHY symbol/pipe or simple reference gate.
        if (b.mode == 3 || (!on && b.mode == 1)) { delay(10); return true; }
        for (unsigned i = 0; i <= 200; ++i) {
            uint32_t val = read(b.halt);
            if (on ? (!(val & 0x80000000U) || ((val >> 28) & 7) == 2) : bool(val & 0x80000000U))
                return true;
            if (i != 200) delay(1);
        }
        return false;
    }
    bool domainBusy(unsigned id) const {
        for (unsigned i = 0; i < BranchCount; ++i)
            if (branches[i].domain == int(id) && (clocks[i].refs ||
                (read(branches[i].reg) & branches[i].mask))) return true;
        return false;
    }
public:
    static bool modemClock(uint32_t id) {return id==36 || (id>=171 && id<=175);}
    Controller() : io{} {}
    explicit Controller(RegisterIO access) : io(access) {}
    bool setIO(RegisterIO access) { if (ready) return false; io = access; return true; }
    // Ownership is a loader contract, not a claim inferred from compatible.
    // It includes parent/rail retention and voltage support up to each ceiling.
    bool allowClock(uint32_t id, uint32_t fixedHz = 0) {
        int i = branchIndex(id);
        if (ready || i < 0 || clocks[i].allowed || (branches[i].parent != NoClock && fixedHz)) return false;
        clocks[i].allowed = true; fixedRates[i] = fixedHz; return true;
    }
    bool allowRate(uint32_t id, uint32_t ceiling) {
        int i = rootIndex(id);
        if (ready || i < 0 || !ceiling || sources[i].ceiling) return false;
        bool supported = false;
        for (unsigned j = 0; j < roots[i].count; ++j) supported |= roots[i].rates[j].hz <= ceiling;
        if (!supported) return false;
        sources[i].ceiling = ceiling; return true;
    }
    bool allowDomain(uint32_t id) {
        if (ready || id >= DomainCount || powers[id].allowed) return false;
        powers[id].allowed = true; return true;
    }
    bool allowReset(uint32_t id) {
        if (ready) return false;
        for (unsigned i = 0; i < ResetCount; ++i) if (resets[i].id == id) {
            if (resetAllowed[i]) return false;
            resetAllowed[i] = true; return true;
        }
        return false;
    }
    bool initialize() {
        if (ready || !io.read || !io.write || !io.delay) return false;
        for (unsigned i = 0; i < BranchCount; ++i)
            clocks[i].pinned = branches[i].critical || (read(branches[i].reg) & branches[i].mask) != 0;
        for (unsigned i = 0; i < DomainCount; ++i)
            powers[i].pinned = !(read(domains[i].reg) & 1) || domains[i].retain || domains[i].alwaysOn;
        ready = true; return true;
    }
    const char *name(uint32_t id) const {
        int r = rootIndex(id), b = branchIndex(id);
        return r >= 0 ? roots[r].name : b >= 0 ? branches[b].name : nullptr;
    }
    uint64_t rate(uint32_t id) const {
        if (!ready) return 0;
        int b = branchIndex(id);
        if (b >= 0) {
            if (clocks[b].fault) return 0;
            if (branches[b].pllDivider) return parentRate(1)/branches[b].pllDivider;
            return branches[b].parent == NoClock ? fixedRates[b] : rate(branches[b].parent);
        }
        int r = rootIndex(id);
        if (r < 0 || sources[r].fault || (read(roots[r].reg) & 1)) return 0;
        const Root &root = roots[r]; uint32_t cfg = read(root.reg + 4);
        uint64_t hz = parentRate((cfg >> 8) & 7);
        uint32_t div = cfg & 31, mode = (cfg >> 12) & 3;
        if (div) hz = hz * 2 / (div + 1);
        if (mode) {
            if (mode != 2 || !root.width) return 0;
            uint32_t mask = (1U << root.width) - 1;
            uint32_t m = read(root.reg + 8) & mask;
            uint32_t n = ((~read(root.reg + 12)) & mask) + m;
            if (!m || n < m || n > mask) return 0;
            hz = hz * m / n;
        }
        return hz;
    }
    Status setRate(uint32_t id, uint64_t hz) {
        if (!ready) return NotReady;
        int b = branchIndex(id);
        if (b >= 0) id = branches[b].parent;
        int ri = rootIndex(id);
        if (ri < 0) return Unsupported;
        const Root &r = roots[ri]; RootState &state = sources[ri];
        if (state.fault) return IOFailure;
        if (!state.ceiling) return Unsupported;
        if (hz > state.ceiling) return BadArgument;
        const Rate *f = nullptr;
        for (unsigned i = 0; i < r.count; ++i) if (r.rates[i].hz == hz) { f = &r.rates[i]; break; }
        if (!f) return Unsupported;
        // Check every known sibling, including unowned firmware clients.
        for (unsigned i = 0; i < BranchCount; ++i)
            if (branches[i].parent == id && (clocks[i].refs || clocks[i].fault ||
                (read(branches[i].reg) & branches[i].mask))) return Busy;
        if (read(r.reg) & 3) return Busy;
        uint64_t parent = parentRate(f->source);
        if (!parent) return NotReady;
        uint64_t actual = parent * 2 / (f->divider + 1);
        if (f->n) actual = actual * f->m / f->n;
        // Tables use nominal rates. Do not program a nominal divisor against
        // a firmware PLL running at a different rate (including DVFS changes).
        uint64_t delta = actual > hz ? actual - hz : hz - actual;
        if (delta > hz / 10000 + 1) return NotReady;
        uint32_t old[4] = {read(r.reg+4), 0, 0, 0};
        if (r.width) for (unsigned i = 1; i < 4; ++i) old[i] = read(r.reg + 4 + 4*i);
        uint32_t mask = r.width ? (1U << r.width) - 1 : 0;
        if (r.width && f->n) {
            uint32_t d = f->n, max = 2 * (f->n - f->m);
            if (d < f->m) d = f->m;
            if (d > max) d = max;
            update(r.reg + 8, mask, f->m);
            update(r.reg + 12, mask, ~(f->n - f->m));
            update(r.reg + 16, mask, ~d);
        }
        update(r.reg + 4, 0x10371f, f->divider | (f->source << 8) |
            (r.width && f->n && f->m != f->n ? 0x2000 : 0));
        update(r.reg, 1, 1);
        bool acknowledged = poll(r.reg, 1, 0, 500);
        uint64_t measured = acknowledged ? rate(id) : 0;
        uint64_t error = measured > hz ? measured - hz : hz - measured;
        if (acknowledged && measured && error <= hz / 10000 + 1) return Success;
        // Restore only configuration registers, never overwrite command/status
        // bits with a stale word. Quarantine the source if rollback also fails.
        write(r.reg + 4, old[0]);
        if (r.width) for (unsigned i = 1; i < 4; ++i) write(r.reg + 4 + 4*i, old[i]);
        update(r.reg, 1, 1);
        state.fault = !poll(r.reg, 1, 0, 500) || read(r.reg + 4) != old[0];
        if (r.width) for (unsigned i = 1; i < 4; ++i)
            state.fault |= read(r.reg + 4 + 4*i) != old[i];
        return state.fault ? IOFailure : acknowledged ? IOFailure : Timeout;
    }
    Status domain(uint32_t id, bool on) {
        if (!ready) return NotReady;
        if (id >= DomainCount) return BadArgument;
        PowerState &s = powers[id]; const Domain &d = domains[id];
        if (!s.allowed) return Unsupported;
        if (s.fault) return IOFailure;
        if (on && s.refs == 0xffffffffU) return Overflow;
        if (!on && !s.refs) return BadArgument;
        if (on && s.refs) { ++s.refs; return Success; }
        if (!on && s.refs > 1) { --s.refs; return Success; }
        if (!on && !s.pinned && domainBusy(id)) return Busy;
        uint32_t old = read(d.reg);
        if (old & 6) return Unsupported; // HW control/SW override needs another protocol.
        if (on) {
            update(d.reg, 1, 0);
            if (!poll(d.reg, 0x80000000U, 0x80000000U, 1500)) {
                update(d.reg, 1, old);
                if (old & 1) delay(500); // Votable: global off cannot be required.
                else s.fault = !poll(d.reg, 0x80000000U, 0x80000000U, 1500);
                return s.fault ? IOFailure : Timeout;
            }
            delay(1); s.refs = 1;
        } else {
            // USB retention, always-on rails and inherited firmware votes stay
            // asserted. Releasing our vote doesn't promise global power-off.
            if (!s.pinned) { update(d.reg, 1, 1); delay(500); }
            s.refs = 0;
        }
        return Success;
    }
    uint32_t domainState(uint32_t id) const {
        if (!ready || id >= DomainCount || powers[id].fault) return 0xffffffffU;
        return read(domains[id].reg) & 0x80000000U ? 1 : 0;
    }
    Status clock(uint32_t id, bool on) {
        if (!ready) return NotReady;
        if (gpuState.held() && gpuClock(id)) return Busy;
        int i = branchIndex(id);
        if (i < 0) return Unsupported;
        ClockState &s = clocks[i]; const Branch &b = branches[i];
        if (!s.allowed) return Unsupported;
        if (s.fault) return IOFailure;
        if (on && s.refs == 0xffffffffU) return Overflow;
        if (!on && !s.refs) return BadArgument;
        if (on && s.refs) { ++s.refs; return Success; }
        if (!on && s.refs > 1) { --s.refs; return Success; }
        if (on && b.pllDivider) {
            if (!read(4)) return NotReady;
            update(0x52000,1,1); // Shared PLL0 vote; never retune its rate.
            if (!poll(0,0x80000000U,0x80000000U,500) || !parentRate(1)) return Timeout;
        }
        if (on && b.parent != NoClock && !rate(b.parent)) return NotReady;
        if (on && b.domain >= 0) {
            Status result = domain(b.domain, true);
            if (result != Success) return result;
        }
        uint32_t before = read(b.reg) & b.mask;
        if (on || !s.pinned) update(b.reg, b.mask, on ? b.mask : 0);
        if (!waitBranch(b, on || s.pinned)) {
            update(b.reg, b.mask, before);
            s.fault = !waitBranch(b, before != 0);
            if (on && b.domain >= 0 && !s.fault) {
                Status result = domain(b.domain, false);
                if (result != Success) { s.fault = true; return result; }
            }
            return s.fault ? IOFailure : Timeout;
        }
        s.refs = on ? 1 : 0;
        if (!on && b.domain >= 0) {
            // The clock has stopped. Remaining clocks carry their own domain
            // references; the final release validates all physical siblings.
            Status result = domain(b.domain, false);
            if (result != Success) {
                update(b.reg, b.mask, before);
                s.fault = !waitBranch(b, before != 0); s.refs = 1;
                return s.fault ? IOFailure : result;
            }
        }
        return Success;
    }
    // The dedicated USB platform owner has already halted/drained DWC3 and
    // retained these leaf clocks. Transfer the inherited pin into its live
    // reference so a later release can actually gate the leaf for retuning.
    // Other clock pins, PLL votes and retention domains are never adopted.
    Status adoptUSBClock(uint32_t id) {
        if (!ready) return NotReady;
        if (id!=32 && id!=71 && id!=116 && id!=117 && id!=118) return Unsupported;
        const int index=branchIndex(id);
        if (index<0 || !clocks[index].allowed) return Unsupported;
        auto &state=clocks[index];
        if (state.fault) return IOFailure;
        if (!state.refs) {
            // Cold handoff: a physically asserted USB BCR proves the core
            // cannot issue DMA even when no usable root clock exists yet.
            if (resetState(18)!=1) return BadArgument;
            const auto &branch=branches[index];
            const uint32_t before=read(branch.reg)&branch.mask;
            update(branch.reg,branch.mask,0);
            if (!waitBranch(branch,false)) {
                update(branch.reg,branch.mask,before);
                state.fault=!waitBranch(branch,before!=0);
                return state.fault ? IOFailure : Timeout;
            }
        }
        if (state.pinned && state.refs>1) return Busy;
        state.pinned=false; return Success;
    }
    // PHY handoff is scoped to dedicated leaves. The shared AHB2PHY clock
    // and oscillator/PLL votes keep their firmware pins. Initial adoption
    // requires the actual controller and PHY resets, including an unused
    // firmware PIPE/reference clock which has no kernel reference yet.
    Status adoptUSBPHYClock(uint32_t id, bool superSpeed) {
        if (!ready) return NotReady;
        if (superSpeed ? (id!=119 && id!=120 && id!=166) : id!=170) return Unsupported;
        const int index=branchIndex(id);
        if (index<0 || !clocks[index].allowed) return Unsupported;
        if (resetState(18)!=1 || (superSpeed ?
            (resetState(69)!=1 || resetState(70)!=1) : resetState(106)!=1)) return BadArgument;
        auto &state=clocks[index];
        if (state.fault) return IOFailure;
        if (state.refs>1) return Busy;
        if (!state.refs) {
            const auto &branch=branches[index];
            const uint32_t before=read(branch.reg)&branch.mask;
            update(branch.reg,branch.mask,0);
            if ((read(branch.reg)&branch.mask) || !waitBranch(branch,false)) {
                update(branch.reg,branch.mask,before);
                state.fault=(read(branch.reg)&branch.mask)!=before || !waitBranch(branch,before!=0);
                return state.fault ? IOFailure : Timeout;
            }
        }
        state.pinned=false; return Success;
    }
    // The dedicated UART owner has mapped and stopped its PIO engines and
    // rejected an active DMA handoff. Only UART3's leaf and explicitly owned
    // root may lose their firmware pin. Shared AHB/PLL pins remain inherited.
    bool uartStateHeld() const {return uartState.token!=0;}
    Status adoptUARTClock(uint32_t id,uint64_t &token) {
        if (!ready) return NotReady;
        if (id!=53) return Unsupported;
        if(token)return BadArgument;
        if(uartState.token)return Busy;
        if(uartState.serial==UINT64_MAX)return Overflow;
        const int bi=branchIndex(id),ri=rootIndex(14);
        if (bi<0 || ri<0 || !clocks[bi].allowed || !sources[ri].ceiling) return Unsupported;
        auto &leaf=clocks[bi];auto &root=sources[ri];
        if (leaf.fault || root.fault) return IOFailure;
        if (leaf.refs!=1) return leaf.refs?Busy:BadArgument;
        const auto reg=roots[ri].reg;
        if (read(reg)&1) return Busy;
        for(unsigned i=0;i<4;++i)uartState.configuration[i]=read(reg+4+4*i);
        uartState.force=read(reg)&2;uartState.pinned=leaf.pinned;
        uartState.token=++uartState.serial;token=uartState.token;
        // Drop only the force-enable vote while the retained leaf still runs.
        // Releasing that leaf then allows setRate's usual sibling checks.
        if (read(reg)&2) {
            update(reg,2,0);
            if (read(reg)&2) {root.fault=true;return IOFailure;}
        }
        leaf.pinned=false;return Success;
    }
    Status restoreUARTClock(uint32_t id,uint64_t &token) {
        if(!ready)return NotReady;
        if(id!=53)return Unsupported;
        if(!token || token!=uartState.token)return BadArgument;
        const int bi=branchIndex(53),ri=rootIndex(14);
        auto &leaf=clocks[bi];auto &root=sources[ri];
        if(leaf.fault || root.fault)return IOFailure;
        const auto &branch=branches[bi];const auto reg=roots[ri].reg;
        if(leaf.refs || (read(branch.reg)&branch.mask) || (read(reg)&1))return Busy;
        // Restore the exact inherited M/N/D/HID fields, including valid boot
        // divisors outside the driver's nominal rate table. UART is stopped.
        update(reg,2,0);
        for(unsigned i=0;i<4;++i)write(reg+4+4*i,uartState.configuration[i]);
        update(reg,1,1);
        if(!poll(reg,1,0,500)){root.fault=true;return Timeout;}
        for(unsigned i=0;i<4;++i)if(read(reg+4+4*i)!=uartState.configuration[i]) {
            root.fault=true;return IOFailure;
        }
        update(reg,2,uartState.force);
        if((read(reg)&2)!=uartState.force){root.fault=true;return IOFailure;}
        if(uartState.pinned) {
            update(branch.reg,branch.mask,branch.mask);
            if(!waitBranch(branch,true)){leaf.fault=true;return IOFailure;}
        }
        leaf.pinned=uartState.pinned;uartState.token=token=0;
        return Success;
    }
    uint32_t resetState(uint32_t id) const {
        if (!ready) return UINT32_MAX;
        for (unsigned i=0;i<ResetCount;++i) if (resets[i].id==id) {
            if (!resetAllowed[i]) return UINT32_MAX;
            const uint32_t value=read(resets[i].reg);
            return value==UINT32_MAX ? UINT32_MAX : value&1;
        }
        return UINT32_MAX;
    }
    bool modemStateHeld() const {return modemToken!=0;}
    Status retainModemState(uint64_t &token) {
        if(!ready)return NotReady;
        if(token)return BadArgument;
        if(modemToken)return Busy;
        if(modemSerial==UINT64_MAX)return Overflow;
        // No inferred grants or takeover of a running modem. Inherited
        // physical clock pins remain; existing software users cannot share
        // this reset/boot lease with a new Q6 owner.
        const auto reset=resetState(108);
        if(reset==UINT32_MAX)return Unsupported;
        if(reset!=1)return NotReady;
        for(unsigned i=0;i<BranchCount;++i)if(modemClock(branches[i].id)) {
            if(!clocks[i].allowed)return Unsupported;
            if(clocks[i].fault)return IOFailure;
            if(clocks[i].refs)return Busy;
        }
        // The MSS GPLL0 vote does not authorize changing the shared PLL.
        // Observe a live parent and retain its actual rate across the lease.
        modemPLLRate=parentRate(1);if(!modemPLLRate)return NotReady;
        token=modemToken=++modemSerial;return Success;
    }
    Status validateModemState(uint64_t token,bool release=false) {
        if(!ready)return NotReady;
        if(!token || token!=modemToken)return BadArgument;
        if(!modemPLLRate || parentRate(1)!=modemPLLRate)return IOFailure;
        const auto reset=resetState(108);
        if(reset==UINT32_MAX)return IOFailure;
        if(release && reset!=1)return Busy;
        for(unsigned i=0;i<BranchCount;++i)if(modemClock(branches[i].id)) {
            if(clocks[i].fault)return IOFailure;
            if(release && clocks[i].refs)return Busy;
        }
        if(release)modemToken=modemPLLRate=0;
        return Success;
    }
    Status reset(uint32_t id, bool asserted) {
        if (!ready) return NotReady;
        for (unsigned i = 0; i < ResetCount; ++i) if (resets[i].id == id) {
            if (!resetAllowed[i]) return Unsupported;
            update(resets[i].reg, 1, asserted ? 1 : 0);
            delay(1);
            return (read(resets[i].reg) & 1) == unsigned(asserted) ? Success : IOFailure;
        }
        return Unsupported;
    }
    Status retainGPUState(uint64_t &token) {
        if (!ready) return NotReady;
        if (token) return BadArgument;
        if (!gpuState.begin()) return Busy;
        for (unsigned i=0;i<BranchCount;++i) if (gpuClock(branches[i].id)) {
            const auto &b=branches[i]; const auto value=read(b.reg);
            if (clocks[i].fault || ((b.id==75 || b.id==77 || b.id==176) && !(value&b.mask)) ||
                !gpuState.watch(0,b.reg,b.mask,value)) return NotReady;
        }
        if (!parentRate(1)) return NotReady;
        // The shared GPLL0 is observed, never retuned or power cycled.
        if (!gpuState.watch(0,0,0xc0100007U,read(0))) return NotReady;
        const uint32_t offsets[]={4,0xc,0x38};
        for (uint32_t reg:offsets) if (!gpuState.watch(0,reg,UINT32_MAX,read(reg))) return NotReady;
        return gpuState.acquire(token,[&](unsigned,uint32_t reg) {return read(reg);}) ? Success : NotReady;
    }
    Status validateGPUState(uint64_t token, bool release=false) {
        if (!ready) return NotReady;
        auto access=[&](unsigned,uint32_t reg) {return read(reg);};
        return (release ? gpuState.release(token,access) : gpuState.validate(token,access)) ? Success : IOFailure;
    }
};
}
