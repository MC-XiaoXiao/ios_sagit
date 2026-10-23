// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "SPMIArbiter.h"
#include <PowerVoltagePolicy.h>
#include <RetainedGPUState.h>
namespace QcomPower {
struct GPURegulatorHandoff {
    uint32_t version,id,microvolts,reserved;
    bool valid() const {return version==1 && id>=1 && id<=4 && !reserved &&
        microvolts>=524000 && microvolts<=1100000 && !(microvolts%4000);}
};
static_assert(sizeof(GPURegulatorHandoff)==16,"GPU regulator handoff ABI");
// PM8005 S1..S4 FTSMPS426 are AP-accessible through SPMI, unlike PM8998 RPM
// resources. The board supplies only topology and voltage limits, not MMIO state.
class SPMIRegulators {
    struct Rail {
        uint8_t sid = 0;
        uint16_t base = 0;
        uint32_t min = 0, max = 0, floor = 0, slew = 0;
        bool ready = false, enabled = false;
        VoltageVotes votes;
    } rails[4];
    SPMIArbiter arbiter;
    IO access{};
    GPUPlatform::RetainedState gpuState;
    uint32_t gpuID=0,gpuUV=0;
    uint32_t gpuRead(uint32_t address) {
        uint8_t value=0;
        return arbiter.transfer(rails[gpuID-1].sid,uint16_t(address),&value,1,false)==OK ? value : UINT32_MAX;
    }
public:
    SPMIArbiter &bus() { return arbiter; }
    const SPMIArbiter &bus() const { return arbiter; }
    Status initialize(IO io, unsigned ee) { access = io; return arbiter.initialize(io, ee); }
    bool configure(uint32_t id, uint32_t sid, uint32_t minimum, uint32_t maximum, uint32_t floor) {
        if (id < 1 || id > 4 || sid > 15 || rails[id-1].max || minimum < 320000 || maximum > 1352000 ||
            minimum > maximum || floor > maximum || (floor && (floor < minimum || floor % 4000))) return false;
        Rail &r = rails[id-1]; r.sid = sid; r.base = 0x1400+0x300*(id-1);
        r.min = minimum; r.max = maximum; r.floor = floor; return true;
    }
    Status prepare(unsigned id) {
        if (id < 1 || id > 4 || !rails[id-1].max) return Unsupported;
        Rail &r = rails[id-1]; if (r.ready) return OK;
        uint8_t identity[2] = {};
        Status s = arbiter.transfer(r.sid, r.base+4, identity, 2, false);
        // FTS type 0x1c, FTS426 control subtype 0x0a. Other PMIC layouts
        // need their own driver; never reinterpret legacy selectors as mV.
        if (s != OK) return s;
        if (identity[0] != 0x1c || identity[1] != 0x0a) return Unsupported;
        uint8_t voltage[2] = {}, enable = 0;
        s = arbiter.transfer(r.sid, r.base+0x40, voltage, 2, false); if (s != OK) return s;
        s = arbiter.transfer(r.sid, r.base+0x46, &enable, 1, false); if (s != OK) return s;
        uint32_t uv = (uint32_t(voltage[0]) | uint32_t(voltage[1])<<8)*1000;
        if ((enable&128) && (uv < r.min || uv > r.max)) return Invalid;
        uint8_t step = 0;
        s = arbiter.transfer(r.sid,r.base+0x61,&step,1,false); if (s != OK) return s;
        // Read the actual FTS426 step delay, using the reference clock and
        // 10/11 margin from the PMIC driver. Do not assume VREG_READY means
        // a voltage selector write has already settled at the output.
        r.slew = (4800*4000/(1000*(2U << (step&3))))*10/11;
        if (!r.slew) return Invalid;
        r.enabled = enable&128;
        // Retain the inherited vote in slot zero. This prevents a consumer
        // from removing an unrelated boot rail merely by releasing its own vote.
        r.votes.adopt(0, r.enabled ? uv : 0, 0); r.ready = true; return OK;
    }
    Status get(unsigned id, uint32_t &value) {
        Status s = prepare(id); if (s != OK) return s;
        Rail &r = rails[id-1]; uint8_t voltage[2] = {}, enable = 0;
        s = arbiter.transfer(r.sid, r.base+0x40, voltage, 2, false); if (s != OK) return s;
        s = arbiter.transfer(r.sid, r.base+0x46, &enable, 1, false); if (s != OK) return s;
        value = enable&128 ? (uint32_t(voltage[0]) | uint32_t(voltage[1])<<8)*1000 : 0; return OK;
    }
    Status set(unsigned id, unsigned client, uint32_t uv) {
        if(gpuState.held() && id==gpuID)return Busy;
        Status s = prepare(id); if (s != OK) return s;
        if (client == 0 || client > 15) return Denied;
        Rail &r = rails[id-1];
        if (uv && (uv < r.min || uv > r.max)) return Invalid;
        if (uv) uv = ((uv+3999)/4000)*4000;
        if (uv > r.max) return Invalid;
        if (uv < r.floor) uv = r.floor;
        return r.votes.vote(0, client, uv, [&](unsigned, uint32_t maximum) {
            uint8_t voltage[2] = {static_cast<uint8_t>((maximum/1000)&255),static_cast<uint8_t>(maximum/256000)};
            Status result;
            uint32_t previous = 0; result = get(id,previous); if (result != OK) return result;
            if (maximum) {
                uint8_t mode = 0;
                result = arbiter.transfer(r.sid,r.base+0x45,&mode,1,false); if (result != OK) return result;
                mode = (mode&~7U)|7; // FTS426 HPM, preserve unrelated control bits.
                result = arbiter.transfer(r.sid,r.base+0x45,&mode,1,true); if (result != OK) return Stale;
                result = arbiter.transfer(r.sid,r.base+0x45,&mode,1,false); if (result != OK || (mode&7) != 7) return Stale;
                result = arbiter.transfer(r.sid, r.base+0x40, voltage, 2, true);
                if (result != OK) return result;
            }
            uint8_t enable = 0;
            result = arbiter.transfer(r.sid, r.base+0x46, &enable, 1, false); if (result != OK) return maximum ? Stale : result;
            enable = (enable&127) | (maximum ? 128 : 0);
            result = arbiter.transfer(r.sid, r.base+0x46, &enable, 1, true); if (result != OK) return Stale;
            // Wait the computed ramp time before VREG_READY/readback. The
            // output may still be ramping while READY retains its old value.
            uint32_t delta = maximum > previous ? maximum-previous : previous-maximum;
            if (maximum && delta) accessWait((delta+r.slew-1)/r.slew);
            if (maximum) {
                for (unsigned n = 0; n <= 500; ++n) {
                    uint8_t status = 0;
                    result = arbiter.transfer(r.sid, r.base+8, &status, 1, false);
                    if (result != OK) return Stale;
                    if (status&128) { uint32_t actual; result = get(id, actual); return result != OK ? Stale : actual == maximum ? OK : Failed; }
                    if (n != 500) accessWait(1);
                }
                return TimedOut;
            }
            uint32_t actual = 1; result = get(id,actual);
            return result != OK ? Stale : actual == 0 ? OK : Failed;
        });
    }
    void accessWait(unsigned us) { access.wait(access.context, us); }
    Status configureGPU(const GPURegulatorHandoff &h) {
        if(gpuID || !h.valid() || !rails[h.id-1].max || h.microvolts<rails[h.id-1].floor ||
            h.microvolts<rails[h.id-1].min || h.microvolts>rails[h.id-1].max)return Invalid;
        uint32_t actual=0;const auto s=get(h.id,actual);
        if(s!=OK)return s;
        if(actual<h.microvolts || actual>rails[h.id-1].max)return NotReady;
        gpuID=h.id;gpuUV=h.microvolts;return OK;
    }
    bool rawWriteAllowed(uint8_t sid,uint16_t address) const {
        return !gpuState.held() || sid!=rails[gpuID-1].sid || (address&0xff00)!=rails[gpuID-1].base;
    }
    Status retainGPUState(uint64_t &token) {
        if(!gpuID)return Unsupported;
        if(token)return Invalid;
        if(!gpuState.begin())return Busy;
        uint32_t actual=0;auto s=get(gpuID,actual);if(s!=OK)return s;
        if(actual<gpuUV)return NotReady;
        const auto base=rails[gpuID-1].base;
        const uint32_t offsets[]={0x40,0x41,0x45,0x46,8};
        for(auto offset:offsets) {
            const auto v=gpuRead(base+offset);
            const auto mask=(offset==0x46 || offset==8)?128u:offset==0x45?7u:255u;
            if(v==UINT32_MAX || ((offset==0x46 || offset==8) && !(v&128)) ||
                !gpuState.watch(0,base+offset,mask,v))return NotReady;
        }
        return gpuState.acquire(token,[&](unsigned,uint32_t r){return gpuRead(r);})?OK:NotReady;
    }
    Status validateGPUState(uint64_t token,bool release=false) {
        if(!gpuID)return Unsupported;
        auto read=[&](unsigned,uint32_t r){return gpuRead(r);};
        return (release?gpuState.release(token,read):gpuState.validate(token,read))?OK:Stale;
    }
};
}
