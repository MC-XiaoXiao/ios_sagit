// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerHardware.h>
#include <RetainedGPUState.h>
namespace QcomPower {
constexpr int NoParent = -100, XO = -1, GPLL = -2, GPLLDiv = -3;
struct CCRate { uint32_t hz, source, divider, m, n; };
struct CCRoot { uint32_t id; const char *name; uint32_t reg, width; int parents[8]; const CCRate *rates; unsigned count; };
struct CCPll { uint32_t id, outputID; const char *name; uint32_t reg, voteReg, voteMask; bool fixed; };
struct CCBranch { uint32_t id; const char *name; uint32_t reg, mask, root; int domain; bool skip, critical; uint32_t hwcgMask; };
struct CCReset { uint32_t id, reg; };
struct CCDomain { uint32_t id, reg, status, clamp, reset; int parent; uint32_t flags; const uint32_t *memories; unsigned count; };
enum { Votable = 1, HardwareControl = 2, Clamp = 4, SoftwareReset = 8, AONReset = 16, NoRetainPeripheral = 32 };
struct CCDescription {
    const CCRoot *roots; unsigned rootCount;
    const CCPll *plls; unsigned pllCount;
    const CCBranch *branches; unsigned branchCount;
    const CCReset *resets; unsigned resetCount;
    const CCDomain *domains; unsigned domainCount;
    bool gpu;
};
struct SupplyPoint { uint32_t root, hz, controller, resource, value; };
struct SupplyIO {
    void *context;
    Status (*set)(void *, uint32_t controller, uint32_t resource, uint32_t value);
    Status (*parent)(void *, int source);
};

class ClockHardware : public Registers {
    const CCDescription *desc = nullptr;
    struct Gate { uint32_t refs = 0; bool allowed = false, pinned = false; } gates[160];
    bool resetAllowed[64] = {}, domainAllowed[12] = {};
    uint32_t domainRefs[12] = {}, maximum[64] = {}, desired[64] = {};
    bool domainPinned[12] = {};
    SupplyPoint supplies[192] = {}; unsigned supplyCount = 0;
    SupplyIO voltage{};
    bool ready = false, fault = false, unknownDemand = false;
    GPUPlatform::RetainedState gpuState;
    int rootIndex(uint32_t id) const { for (unsigned i = 0; i < desc->rootCount; ++i) if (desc->roots[i].id == id) return i; return -1; }
    int branchIndex(uint32_t id) const { for (unsigned i = 0; i < desc->branchCount; ++i) if (desc->branches[i].id == id) return i; return -1; }
    int domainIndex(uint32_t id) const { for (unsigned i = 0; i < desc->domainCount; ++i) if (desc->domains[i].id == id) return i; return -1; }
    bool rootBusy(uint32_t id) const {
        for (unsigned i = 0; i < desc->branchCount; ++i)
            if (desc->branches[i].root == id && (gates[i].refs || (rd(desc->branches[i].reg)&desc->branches[i].mask))) return true;
        return false;
    }
    uint64_t pllRate(unsigned index, bool divided = true) const {
        const auto &p = desc->plls[index];
        uint64_t hz = uint64_t(19200000)*rd(p.reg+4) + ((uint64_t(19200000)*(rd(p.reg+0x38)&0xffff))>>16);
        if (divided) {
            uint32_t div = (rd(p.reg+0xc)>>8)&15;
            if (div != 0 && div != 1 && div != 3 && div != 7) return 0;
            hz /= div+1;
        }
        return hz;
    }
    uint64_t parentRate(int parent) const {
        if (parent == XO) return 19200000;
        if (parent == GPLL || parent == GPLLDiv) {
            if ((rd(0,1)&0x80000001U) != 0x80000001U) return 0;
            uint64_t rate = uint64_t(19200000)*rd(4,1)+((uint64_t(19200000)*(rd(0x38,1)&0xffff))>>16);
            return parent == GPLLDiv ? rate/2 : rate;
        }
        return parent >= 0 && unsigned(parent) < desc->pllCount ? pllRate(parent) : 0;
    }
    Status parentOn(int parent) {
        if (parent == XO) {
            if (desc->gpu) { bits(0x1020,1,1); if (!poll(0x1020,0x80000000U,0,200)) return TimedOut; }
            return OK;
        }
        if (parent == GPLL || parent == GPLLDiv) {
            if (!voltage.parent) return NotReady;
            Status s = voltage.parent(voltage.context,parent);
            return s != OK ? s : parentRate(parent) ? OK : NotReady;
        }
        if (parent < 0 || unsigned(parent) >= desc->pllCount) return Unsupported;
        const auto &p = desc->plls[parent];
        if (!rd(p.reg+4) || !(rd(p.reg)&4)) return NotReady; // no invented analog/PLL reset configuration.
        if (p.voteMask) bits(p.voteReg,p.voteMask,p.voteMask);
        else {
            if (!(rd(p.reg)&1)) {
                bits(p.reg,1,0); wr(p.reg+0x2c,0);
                bits(p.reg,4,4); wr(p.reg+0x2c,1);
            }
        }
        if (!poll(p.reg,0x80000000U,0x80000000U,500)) return TimedOut;
        if (!p.voteMask) { bits(p.reg+0xc,7,7); bits(p.reg,1,1); }
        return OK;
    }
    Status latchPLL(const CCPll &pll) {
        uint32_t mode = rd(pll.reg);
        bits(pll.reg,1U<<22,1U<<22); wait(1);
        if (mode & (1U<<23)) {
            if (!poll(pll.reg,1U<<29,1U<<29,500)) return TimedOut;
            bits(pll.reg,1U<<22,0);
        } else if (!poll(pll.reg,1U<<22,0,500)) return TimedOut;
        if (!poll(pll.reg,1U<<29,0,500)) return TimedOut;
        wait(10); return OK;
    }
    // Compute one controller vote per rail from every root's demand. A new
    // low-rate request cannot overwrite another root's higher voltage demand.
    Status applySupplies(int changed, uint32_t hz, bool lowering, int extraDomain = -1) {
        if (!voltage.set || unknownDemand) return NotReady;
        bool covered = false;
        for (unsigned i = 0; i < supplyCount; ++i) {
            const auto &s = supplies[i];
            bool first = true;
            for (unsigned j = 0; j < i; ++j) if (supplies[j].controller == s.controller && supplies[j].resource == s.resource) first = false;
            if (!first) continue;
            uint32_t oldVote = 0, newVote = 0;
            for (unsigned root = 0; root < desc->rootCount; ++root) {
                uint32_t before = desired[root], after = int(root) == changed ? hz : before;
                uint32_t oldPointHz = UINT32_MAX, newPointHz = UINT32_MAX, oldValue = 0, newValue = 0;
                bool hasMapping = false;
                for (unsigned j = 0; j < supplyCount; ++j) {
                    const auto &point = supplies[j];
                    if (point.root != desc->roots[root].id || point.controller != s.controller || point.resource != s.resource) continue;
                    hasMapping = true;
                    if (before && point.hz >= before && point.hz < oldPointHz) { oldPointHz = point.hz; oldValue = point.value; }
                    if (after && point.hz >= after && point.hz < newPointHz) { newPointHz = point.hz; newValue = point.value; }
                    if (int(root) == changed && point.hz >= hz) covered = true;
                }
                if (hasMapping && ((before && oldPointHz == UINT32_MAX) || (after && newPointHz == UINT32_MAX))) return Unsupported;
                if (oldValue > oldVote) oldVote = oldValue;
                if (newValue > newVote) newVote = newValue;
            }
            for (unsigned j = 0; j < supplyCount; ++j) {
                const auto &point = supplies[j];
                if (!(point.root&0x80000000U) || point.controller != s.controller || point.resource != s.resource) continue;
                int domain = domainIndex(point.root&0x7fffffffU);
                if (domain < 0) return Invalid;
                if (domainRefs[domain] && point.value > oldVote) oldVote = point.value;
                if ((domainRefs[domain] || domain == extraDomain) && point.value > newVote) newVote = point.value;
                if (domain == extraDomain) covered = true;
            }
            if ((!lowering && newVote >= oldVote) || (lowering && newVote < oldVote)) {
                Status status = voltage.set(voltage.context,s.controller,s.resource,newVote);
                if (status != OK) return status;
            }
        }
        return covered ? OK : Unsupported;
    }
    bool waitGate(const CCBranch &b, bool on) {
        if (b.skip || (b.hwcgMask && (rd(b.reg)&b.hwcgMask))) return true;
        for (unsigned n = 0; n <= 200; ++n) {
            uint32_t state = rd(b.reg);
            if (on ? (!(state&0x80000000U) || ((state>>28)&7)==2) : bool(state&0x80000000U)) return true;
            if (n != 200) wait(1);
        }
        return false;
    }
public:
    bool configure(const CCDescription &description, IO access, SupplyIO supply) {
        if (ready || description.rootCount > 64 || description.pllCount > 8 || description.branchCount > 160 ||
            description.resetCount > 64 || description.domainCount > 12) return false;
        desc = &description; setIO(access); voltage = supply; return true;
    }
    bool allowRoot(uint32_t id, uint32_t ceiling) {
        int i = rootIndex(id); if (ready || i < 0 || maximum[i] || !ceiling) return false;
        maximum[i] = ceiling; return true;
    }
    bool allowClock(uint32_t id) {
        int i = branchIndex(id); if (ready || i < 0 || gates[i].allowed) return false;
        gates[i].allowed = true; return true;
    }
    bool allowDomain(uint32_t id) { int i = domainIndex(id); if (ready || i < 0 || domainAllowed[i]) return false; domainAllowed[i] = true; return true; }
    bool allowReset(uint32_t id) {
        for (unsigned i = 0; i < desc->resetCount; ++i) if (desc->resets[i].id == id) {
            if (ready || resetAllowed[i]) return false;
            resetAllowed[i] = true; return true;
        }
        return false;
    }
    bool addSupply(SupplyPoint point) {
        bool domain = point.root&0x80000000U;
        if (ready || supplyCount == 192 || (domain ? domainIndex(point.root&0x7fffffffU) < 0 : rootIndex(point.root) < 0) ||
            (domain ? point.hz != 0 : !point.hz) || !point.value ||
            (point.controller != 2 && point.controller != 3)) return false;
        for (unsigned i = 0; i < supplyCount; ++i) {
            const auto &p = supplies[i];
            if (p.root == point.root && p.controller == point.controller && p.resource == point.resource) {
                if (p.hz == point.hz || (p.hz < point.hz ? p.value > point.value : p.value < point.value)) return false;
            }
        }
        supplies[supplyCount++] = point; return true;
    }
    bool initialize() {
        if (ready || !desc) return false;
        for (unsigned i = 0; i < desc->branchCount; ++i) gates[i].pinned = desc->branches[i].critical || !gates[i].allowed;
        for (unsigned i = 0; i < desc->domainCount; ++i) domainPinned[i] = !domainAllowed[i];
        ready = true;
        for (unsigned i = 0; i < desc->rootCount; ++i) if (rootBusy(desc->roots[i].id)) {
            uint64_t hz = rate(desc->roots[i].id); if (hz > UINT32_MAX) return false; desired[i] = hz; if (!hz) unknownDemand = true;
        }
        return true;
    }
    const char *name(uint32_t id) const {
        int r = rootIndex(id), b = branchIndex(id);
        if (r >= 0) return desc->roots[r].name;
        if (b >= 0) return desc->branches[b].name;
        for (unsigned i = 0; i < desc->pllCount; ++i) if (desc->plls[i].id == id || desc->plls[i].outputID == id) return desc->plls[i].name;
        return nullptr;
    }
    uint64_t rate(uint32_t id) const {
        if (!ready || fault) return 0;
        if (desc->gpu && id == 10) return 19200000;
        int b = branchIndex(id); if (b >= 0) id = desc->branches[b].root;
        int index = rootIndex(id);
        if (index < 0) {
            for (unsigned i = 0; i < desc->pllCount; ++i) if (desc->plls[i].id == id || desc->plls[i].outputID == id)
                return pllRate(i, desc->plls[i].outputID == id);
            return desc->gpu && id == 10 ? 19200000 : 0;
        }
        const auto &r = desc->roots[index];
        uint32_t cfg = rd(r.reg+4); if (rd(r.reg)&1) return 0;
        uint64_t hz = parentRate(r.parents[(cfg>>8)&7]);
        uint32_t div = cfg&31, mode = (cfg>>12)&3;
        if (div) hz = hz*2/(div+1);
        if (mode) {
            if (mode != 2 || !r.width) return 0;
            uint32_t rawN = rd(r.reg+12);
            unsigned width = r.width == 32 ? (rawN >> 8 ? 16 : 8) : r.width;
            uint32_t mask = (1U<<width)-1, m = rd(r.reg+8)&mask, n = ((~rawN)&mask)+m;
            if (!m || n < m || n > mask) return 0;
            hz = hz*m/n;
        }
        // MSM8998's gfx core has a fixed /2 after this RCGR. The vendor
        // F_SLEW table pairs nominal596MHz with a1192MHz parent and unity
        // HID, not another /2 in the RCGR. Report the nominal core rate.
        if(desc->gpu && r.id==3)hz/=2;
        return hz;
    }
    Status setRate(uint32_t id, uint64_t hz) {
        if (!ready) return NotReady;
        if (gpuState.held()) return Busy;
        if (fault) return Stale;
        int b = branchIndex(id); if (b >= 0) id = desc->branches[b].root;
        int index = rootIndex(id);
        if (index < 0 || !maximum[index]) return Unsupported;
        if (!hz || hz > maximum[index] || hz > UINT32_MAX) return Invalid;
        const auto &r = desc->roots[index];
        if (rootBusy(id) || (rd(r.reg)&3)) return Busy;
        const CCRate *point = nullptr; CCRate dynamic{};
        for (unsigned i = 0; i < r.count; ++i) if (r.rates[i].hz == hz) point = &r.rates[i];
        if (desc->gpu && id == 3) { dynamic = {uint32_t(hz),1,1,0,0}; point = &dynamic; }
        if (!point || (point->n && !r.width)) return Unsupported;
        int parent = r.parents[point->source];
        if (parent == NoParent) return Unsupported;
        // A GPU PLL retune is only safe with all sibling roots/branches idle.
        bool retune = desc->gpu && id == 3;
        if (retune) for (unsigned i = 0; i < desc->rootCount; ++i)
            for (int p : desc->roots[i].parents) if (p == parent && (rootBusy(desc->roots[i].id) || (rd(desc->roots[i].reg)&3))) return Busy;
        Status s = applySupplies(index,uint32_t(hz),false); if (s != OK) return s;
        uint32_t savedPLL[5] = {}; const CCPll *pll = nullptr;
        if (retune) {
            if (parent < 0 || unsigned(parent) >= desc->pllCount) return Unsupported;
            pll = &desc->plls[parent];
            const uint32_t offsets[] = {0,4,0xc,0x2c,0x38};
            for (unsigned i = 0; i < 5; ++i) savedPLL[i] = rd(pll->reg+offsets[i]);
            if (!(savedPLL[0]&4) || !savedPLL[1]) return NotReady; // analog calibration is secure-boot state.
            if (savedPLL[0]&(1U<<20)) return Unsupported;
            uint64_t pllHz = hz*2; if (pllHz < 249600000 || pllHz > 2000000000) return Invalid;
            bits(pll->reg,1,0); wr(pll->reg+0x2c,0);
            wr(pll->reg+4,pllHz/19200000);
            wr(pll->reg+0x38,((pllHz%19200000)*65536+19199999)/19200000);
            bits(pll->reg+0xc,0x300f00,0); // VCO0, even output /1.
        }
        s = pll ? latchPLL(*pll) : OK;
        if (s == OK) s = parentOn(parent);
        uint32_t saved[4] = {rd(r.reg+4),0,0,0};
        if (r.width) for (unsigned i = 1; i < 4; ++i) saved[i] = rd(r.reg+4+4*i);
        if (s == OK) {
            uint64_t actual = parentRate(parent)*2/(point->divider+1);
            if (point->n) actual = actual*point->m/point->n;
            if(retune)actual/=2;
            uint64_t error = actual > hz ? actual-hz : hz-actual;
            if (error > hz/10000+1) s = NotReady;
        }
        if (s == OK) {
            uint32_t mask = r.width == 32 ? UINT32_MAX : r.width ? (1U<<r.width)-1 : 0;
            if (r.width && point->n) {
                uint32_t d = point->n, maximumD = 2*(point->n-point->m);
                if (d > maximumD) d = maximumD;
                if (d < point->m) d = point->m;
                bits(r.reg+8,mask,point->m); bits(r.reg+12,mask,~(point->n-point->m)); bits(r.reg+16,mask,~d);
            }
            bits(r.reg+4,0x10371f,point->divider|(point->source<<8)|
                (r.width && point->n && point->m != point->n ? 0x2000 : 0));
            bits(r.reg,1,1);
            if (!poll(r.reg,1,0,500)) s = TimedOut;
            else { uint64_t actual = rate(id), error = actual > hz ? actual-hz : hz-actual; if (error > hz/10000+1) s = Failed; }
        }
        if (s != OK) {
            // Restore the clock before considering voltage release. Retain
            // raised votes on failure; their cost is preferable to undervolting.
            wr(r.reg+4,saved[0]);
            if (r.width) for (unsigned i = 1; i < 4; ++i) wr(r.reg+4+4*i,saved[i]);
            bits(r.reg,1,1); bool restored = poll(r.reg,1,0,500);
            if (pll) {
                bits(pll->reg,1,0); wr(pll->reg+0x2c,0);
                wr(pll->reg+4,savedPLL[1]); wr(pll->reg+0xc,savedPLL[2]); wr(pll->reg+0x38,savedPLL[4]);
                restored &= latchPLL(*pll) == OK;
                wr(pll->reg+0x2c,savedPLL[3]);
                if (savedPLL[0]&1) { restored &= poll(pll->reg,0x80000000U,0x80000000U,500); bits(pll->reg,1,1); }
            }
            restored &= rd(r.reg+4) == saved[0];
            if (r.width) for (unsigned i = 1; i < 4; ++i) restored &= rd(r.reg+4+4*i) == saved[i];
            if (!restored) { fault = true; return Stale; }
            return s;
        }
        // Do not lower a rail until the lower clock setting is acknowledged.
        s = applySupplies(index,uint32_t(hz),true);
        desired[index] = uint32_t(hz);
        if (s != OK) fault = true;
        return s;
    }
    Status domain(uint32_t id, bool on) {
        if (!ready) return NotReady;
        if (gpuState.held()) return Busy;
        if (fault) return Stale;
        int index = domainIndex(id); if (index < 0 || !domainAllowed[index]) return Unsupported;
        auto &refs = domainRefs[index]; const auto &d = desc->domains[index];
        if (on && refs) { if (refs == UINT32_MAX) return Invalid; ++refs; return OK; }
        if (!on && !refs) return Invalid;
        if (!on && refs > 1) { --refs; return OK; }
        if (!on) {
            for (unsigned i = 0; i < desc->domainCount; ++i) if (desc->domains[i].parent == int(id) && domainRefs[i]) return Busy;
            for (unsigned i = 0; i < desc->branchCount; ++i) if (desc->branches[i].domain == int(id) &&
                (gates[i].refs || (!desc->branches[i].critical && (rd(desc->branches[i].reg)&desc->branches[i].mask)))) return Busy;
        }
        if (on && d.parent >= 0) { Status s = domain(d.parent,true); if (s != OK) return s; }
        if (on) {
            Status s = applySupplies(-1,0,false,index);
            if (s != OK) {
                if (d.parent >= 0) { Status released = domain(d.parent,false); if (released != OK) fault = true; }
                return s;
            }
            if (desc->gpu) { Status s = parentOn(XO); if (s != OK) { fault = true; return s; } }
        }
        uint32_t gxRootVote=0;
        if(on && desc->gpu && id==2) {
            // MSM8998 GX's downstream GDSC requests core_root_clk for the
            // reset/power sequence (force-enable-root-clk). The branch is
            // still gated; release only our temporary RCG vote after ACK.
            gxRootVote=rd(0x1070)&2;
            bits(0x1070,2,2);
            if(!poll(0x1070,0x80000000U,0,500)) {fault=true;return TimedOut;}
        }
        // Own the state-machine wait fields too, instead of relying on the
        // bootloader's GDSC timing. Values are the Qualcomm reference defaults.
        if (on) bits(d.reg,0x00fff006,0x00282000);
        // GX supports retention: gdsc_init() forces its SRAM retention before
        // the first OFF->ON transition. NO_RET_PERIPH excludes bit 13.
        if (on && desc->gpu && id == 2) bits(0x1098,1U<<14,1U<<14);
        if (d.flags&HardwareControl) bits(d.reg,2,0);
        if (on && (d.flags&SoftwareReset)) { bits(d.reset,1,1); wait(1); bits(d.reset,1,0); }
        if (on && (d.flags&Clamp)) {
            if (d.flags&AONReset) { bits(d.clamp,16,16); wait(1); bits(d.clamp,16,0); }
            bits(d.clamp,1,0);
        }
        if (on || !domainPinned[index]) bits(d.reg,1,on ? 0 : 1);
        if (d.status) wait(1); // wait >8 XO cycles before reading HW controller status.
        bool confirmed = true;
        if (!on && (d.flags&Votable)) wait(500);
        else confirmed = poll(d.status ? d.status : d.reg,0x80000000U,on ? 0x80000000U : 0,1500);
        if (!confirmed) { fault = true; return TimedOut; }
        for (unsigned i = 0; i < d.count; ++i) {
            uint32_t mask = 1U<<14; if (!(d.flags&NoRetainPeripheral)) mask |= 1U<<13;
            bits(d.memories[i],mask,on ? mask : 0);
        }
        if (!on && (d.flags&Clamp)) bits(d.clamp,1,1);
        wait(1); refs = on ? 1 : 0;
        if(on && desc->gpu && id==2)bits(0x1070,2,gxRootVote);
        if (!on && d.parent >= 0) return domain(d.parent,false);
        return OK;
    }
    uint32_t domainState(uint32_t id) const { int i = domainIndex(id); return !ready || fault || i < 0 ? UINT32_MAX : bool(rd(desc->domains[i].status ? desc->domains[i].status : desc->domains[i].reg)&0x80000000U); }
    Status clock(uint32_t id, bool on) {
        if (!ready) return NotReady;
        if (gpuState.held()) return Busy;
        if (fault) return Stale;
        int index = branchIndex(id); if (index < 0 || !gates[index].allowed) return Unsupported;
        auto &g = gates[index]; const auto &b = desc->branches[index];
        if (on && g.refs) { if (g.refs == UINT32_MAX) return Invalid; ++g.refs; return OK; }
        if (!on && !g.refs) return Invalid;
        if (!on && g.refs > 1) { --g.refs; return OK; }
        int r = rootIndex(b.root);
        if (on && r >= 0) {
            uint64_t hz = rate(b.root); if (!hz || hz > UINT32_MAX) return NotReady;
            Status s = applySupplies(r,uint32_t(hz),false); if (s != OK) return s;
            desired[r] = hz;
            const auto &root = desc->roots[r]; s = parentOn(root.parents[(rd(root.reg+4)>>8)&7]); if (s != OK) return s;
        }
        if (on && b.domain >= 0) { Status s = domain(b.domain,true); if (s != OK) return s; }
        uint32_t before = rd(b.reg)&b.mask;
        if (on || !g.pinned) bits(b.reg,b.mask,on ? b.mask : 0);
        if (!waitGate(b,on || g.pinned)) {
            bits(b.reg,b.mask,before);
            if (!waitGate(b,before != 0)) fault = true;
            if (on && b.domain >= 0 && !fault) { Status s = domain(b.domain,false); if (s != OK) fault = true; }
            return fault ? Stale : TimedOut;
        }
        g.refs = on ? 1 : 0;
        if (!on && b.domain >= 0) {
            Status s = domain(b.domain,false);
            if (s != OK) { bits(b.reg,b.mask,before); g.refs = 1; if (!waitGate(b,before != 0)) fault = true; return s; }
        }
        return OK;
    }
    Status reset(uint32_t id, bool asserted) {
        if (!ready) return NotReady;
        if (gpuState.held()) return Busy;
        if (fault) return Stale;
        for (unsigned i = 0; i < desc->resetCount; ++i) if (desc->resets[i].id == id) {
            if (!resetAllowed[i]) return Unsupported;
            bits(desc->resets[i].reg,1,asserted ? 1 : 0); wait(1);
            return (rd(desc->resets[i].reg)&1) == unsigned(asserted) ? OK : Failed;
        }
        return Unsupported;
    }
    uint32_t maxRate(uint32_t id) const { int i = rootIndex(id); return i >= 0 ? maximum[i] : 0; }
    uint32_t stateFlags() const {return unsigned(ready)|(unsigned(fault)<<1)|(unsigned(unknownDemand)<<2);}
    bool rootState(uint32_t id, uint32_t &reg, uint32_t &command, uint32_t &configuration,
                   int &parent, uint64_t &parentHz) const {
        const int index=rootIndex(id); if(index<0)return false;
        const auto &root=desc->roots[index];
        reg=root.reg; command=rd(reg); configuration=rd(reg+4);
        parent=root.parents[(configuration>>8)&7]; parentHz=parentRate(parent);
        return true;
    }
    Status retainGPUState(uint64_t &token) {
        if (token) return Invalid;
        if (!ready || fault || unknownDemand) return NotReady;
        if (!desc->gpu) return Unsupported;
        if (!gpuState.begin()) return Busy;
        auto watch=[&](uint32_t reg,uint32_t mask=UINT32_MAX,unsigned bank=0) {
            return gpuState.watch(bank,reg,mask,rd(reg,bank));
        };
        // CX/GX must already be on under software control. Ordinary
        // domain(true) runs the GX reset/clamp sequence, so never call it here.
        for (unsigned i=0;i<desc->domainCount;++i) {
            const auto &d=desc->domains[i];
            if ((rd(d.reg)&7) || domainState(d.id)!=1 || !watch(d.reg,0x00fff007) ||
                !watch(d.status ? d.status : d.reg,0x80000000U)) return NotReady;
            if ((d.flags&Clamp) && ((rd(d.clamp)&17) || !watch(d.clamp,17))) return NotReady;
            for (unsigned j=0;j<d.count;++j) if (!watch(d.memories[j],(1U<<13)|(1U<<14))) return NotReady;
        }
        for (unsigned i=0;i<desc->resetCount;++i) {
            const auto &r=desc->resets[i];
            if ((r.id<3 && (rd(r.reg)&1)) || !watch(r.reg,1)) return NotReady;
        }
        if(!watch(0x10a0,1U<<23) || !watch(0x10a4))return NotReady;
        for (unsigned i=0;i<desc->branchCount;++i) {
            const auto &b=desc->branches[i];
            if ((b.id==6 || b.id==7 || b.id==8 || b.id==10) && !(rd(b.reg)&b.mask)) return NotReady;
            if (!watch(b.reg,b.mask|b.hwcgMask)) return NotReady;
        }
        for (unsigned i=0;i<desc->rootCount;++i) {
            const auto &r=desc->roots[i];
            if ((rd(r.reg)&1) || !rate(r.id) || !watch(r.reg,3) || !watch(r.reg+4)) return NotReady;
            if (r.width) for (unsigned j=2;j<5;++j) if (!watch(r.reg+4*j)) return NotReady;
        }
        for (unsigned i=0;i<desc->pllCount;++i) {
            const auto &p=desc->plls[i];
            const uint32_t mode=rd(p.reg);
            if ((mode&0x80000005U)!=0x80000005U || rd(p.reg+0x2c)!=1 || !(rd(p.reg+0xc)&2) ||
                (mode&((1U<<22)|(1U<<29))) ||
                !watch(p.reg,0x81f00007U)) return NotReady;
            const uint32_t offsets[] = {4U, 0xcU, 0x2cU, 0x38U};
            for (uint32_t offset : offsets) if (!watch(p.reg+offset)) return NotReady;
        }
        return gpuState.acquire(token,[&](unsigned bank,uint32_t reg) {return rd(reg,bank);}) ? OK : NotReady;
    }
    Status validateGPUState(uint64_t token, bool release=false) {
        if (!ready || fault || !desc->gpu) return NotReady;
        auto read=[&](unsigned bank,uint32_t reg) {return rd(reg,bank);};
        return (release ? gpuState.release(token,read) : gpuState.validate(token,read)) ? OK : Stale;
    }
};
}
