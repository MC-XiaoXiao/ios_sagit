// SPDX-License-Identifier: GPL-2.0-only
// MSM8998 v2/v2.1 CPRh controller/thread/corner initialization, derived from
// cpr3_regulator_init_ctrl/init_thread/init_cprh in the pinned Xiaomi source.
#pragma once
#include "CPRhCalibration.h"

namespace QcomCPRh {
static constexpr uint32_t kControllerBase[2]={0x179c8000,0x179c4000};
struct RegisterIO {
    void *context;
    bool (*read32)(void *,unsigned cluster,uint32_t offset,uint32_t &value);
    bool (*write32)(void *,unsigned cluster,uint32_t offset,uint32_t value);
    void (*barrier)(void *);
    void (*delayUS)(void *,uint32_t) = nullptr;
};
// A trusted caller may transfer firmware CPR ownership only after these
// observations. These are observations of this boot, never device-tree claims.
struct Ownership {
    bool bootServicesExited, bothOSMBanksDisabled, tzBootParametersMatched, clockVerified;
};
namespace InitDetail {
inline bool valid(const Plan &p) {
    if (!supported(p.identity) || p.cluster>1 || p.speedBin>3 || p.cprRevision>7 ||
        p.count!=p.activeCount+2 || p.count>40 ||
        p.activeCount!=unsigned((p.cluster ? Data::cluster1 : Data::cluster0).
                                  counts.values[p.speedBin*8+p.cprRevision]))
        return false;
    uint32_t mask=0xffff, base[16]={};
    for (unsigned i=0;i<p.activeCount;++i) {
        const Corner &c=p.corners[i];
        if (!c.frequencyHz || c.ro>15 || !c.targetQuotient || c.targetQuotient>4095)
            return false;
        mask &= ~(1u<<c.ro);
        if (!base[c.ro] || c.targetQuotient<base[c.ro]) base[c.ro]=c.targetQuotient;
    }
    if (p.roMask!=mask) return false;
    for (unsigned r=0;r<16;++r) if (base[r]!=p.baseQuotient[r]) return false;
    for (unsigned i=0;i<p.count;++i) {
        const Corner &c=p.corners[i];
        if (c.ro>15 || c.floorUV<kBaseUV || c.floorUV>c.openLoopUV ||
            c.openLoopUV>c.ceilingUV || c.ceilingUV>1372000 ||
            c.openLoopUV%kStepUV || c.floorUV%kStepUV) return false;
        uint32_t delta=i<p.activeCount ? (c.targetQuotient-base[c.ro]+3)/4 : 0;
        uint32_t v=(c.openLoopUV-kBaseUV)/kStepUV, fl=(c.floorUV-kBaseUV)/kStepUV;
        if (delta>511 || v>255 || fl>255) return false;
        uint32_t word=v|(fl<<8)|(delta<<16)|(c.ro<<25);
        if (i>=p.activeCount) {
            const uint32_t uv=i==p.activeCount ? 880000 : 852000;
            if (c.frequencyHz || c.ro || c.targetQuotient ||
                c.openLoopUV!=uv || c.floorUV!=uv || c.ceilingUV!=uv) return false;
            word|=1u<<29;
        }
        if (c.word!=word) return false;
    }
    return true;
}
// Only the documented fields are touched. Any failure is propagated to the
// bridge; writes are read back before the CPR loop or OSM may be enabled.
inline bool reg(const RegisterIO &io,unsigned c,uint32_t offset,uint32_t mask,
                uint32_t value,bool write) {
    uint32_t old=0,read=0;
    if (!io.read32(io.context,c,offset,old)) return false;
    if (!write) return (old&mask)==(value&mask);
    if (!io.write32(io.context,c,offset,(old&~mask)|(value&mask))) return false;
    io.barrier(io.context);
    return io.read32(io.context,c,offset,read) && (read&mask)==(value&mask);
}
inline bool program(const Plan &p,const RegisterIO &io,bool write) {
    unsigned c=p.cluster;
    // ceil(19.2 MHz * 1000 ns)-1 = 19, only for ROs actually used.
    for (unsigned ro=0;ro<16;++ro)
        if (!(p.roMask&(1u<<ro)) &&
            !reg(io,c,0xa0+4*ro,0xffffffff,19,write)) return false;
    // 5 ms continuous-loop delay, 15 idle cycles, count mode0, repeat1.
    if (!reg(io,c,0x00c,0xffffffff,96000,write) ||
        !reg(io,c,0x004,0xfffffefe,0x21e,write) ||
        !reg(io,c,0x014,0xffffffff,c ? 9|(14u<<6) : 11|(12u<<6),write)) return false;
    for (unsigned sensor=0;sensor<(c ? 9u : 6u);++sensor)
        if (!reg(io,c,0x200+4*sensor,0xffffffff,0,write)) return false;
    // Thread0: consecutive down2/up0, thresholds down2/up2.
    if (!reg(io,c,0x808,0xffffffff,0x4202,write) ||
        !reg(io,c,0x80c,0xffffffff,p.roMask,write)) return false;
    for (unsigned ro=0;ro<16;++ro)
        if (!reg(io,c,0x840+4*ro,0xffffffff,p.baseQuotient[ro],write)) return false;
    for (unsigned i=0;i<p.count;++i)
        if (!reg(io,c,0x3a00+4*i,0xffffffff,p.corners[i].word,write)) return false;
    // Sagit disables aging and temperature/core-count/boost margins. Set the
    // baseline explicitly so stale optional policy cannot alter calibrated UV.
    // HW closed loop, dynamic per-RO step quotient, PMIC step=4 mV.
    if (!reg(io,c,0x7f8,0xfc01f197,0x4090,write) ||
        !reg(io,c,0x7a4,0x3ff,0x21,write) ||
        // floor(19.2 MHz * 1760 ns)=33 voltage-settling reference clocks.
        !reg(io,c,0x7a8,0x1ffc0000,33u<<18,write) ||
        // base 352mV/4=88, multiplier4mV, switch delay floor(20)/4=5.
        !reg(io,c,0x3aa0,0x1ffe07ff,(4u<<25)|(5u<<17)|(88u<<1)|1u,write))
        return false;
    return true;
}
} // namespace InitDetail

inline bool verify(const Plan &p,const RegisterIO &io) {
    if (!InitDetail::valid(p) || !io.read32 || !io.barrier) return false;
    return InitDetail::program(p,io,false) &&
        InitDetail::reg(io,p.cluster,0x004,1,1,false);
}
// Read-only preflight. Before this call the bridge must have enabled and
// verified the 19.2MHz gcc_hmss_rbcpr clock and proved both OSM banks disabled.
// Never reprogram an already running CPR loop unless it exactly matches this
// device's calibrated plan, in which case apply() is an idempotent no-op.
inline bool inspect(const Plan &p,const RegisterIO &io) {
    if (!InitDetail::valid(p) || !io.read32 || !io.write32 || !io.barrier) return false;
    uint32_t ctl=0,status=0,hctl=0;
    if (!io.read32(io.context,p.cluster,0x004,ctl) || ctl==0xffffffff ||
        !io.read32(io.context,p.cluster,0x008,status) || status==0xffffffff ||
        !io.read32(io.context,p.cluster,0x3aa0,hctl) || hctl==0xffffffff)
        return false;
    if (ctl&1) return verify(p,io);
    return !(status&1); // A disabled controller must have no measurement in flight.
}
inline bool ownershipValid(const Ownership &ownership) {
    return ownership.bootServicesExited && ownership.bothOSMBanksDisabled &&
        ownership.tzBootParametersMatched && ownership.clockVerified;
}
inline bool inspect(const Plan &p,const RegisterIO &io,const Ownership &ownership) {
    if (!ownershipValid(ownership) || !InitDetail::valid(p) || !io.read32 ||
        !io.write32 || !io.barrier || !io.delayUS) return false;
    uint32_t ctl=0,status=0,hctl=0;
    // A firmware loop may still be running here. inspect is strictly read-only;
    // apply will quiesce it after both clusters have passed preflight.
    return io.read32(io.context,p.cluster,0x004,ctl) && ctl!=0xffffffff &&
        io.read32(io.context,p.cluster,0x008,status) && status!=0xffffffff &&
        io.read32(io.context,p.cluster,0x3aa0,hctl) && hctl!=0xffffffff;
}
inline bool disable(const Plan &p,const RegisterIO &io) {
    if (p.cluster>1 || !io.read32 || !io.write32 || !io.barrier) return false;
    // OSM must be disabled before detaching its CPR interface.
    bool loop=InitDetail::reg(io,p.cluster,0x004,1,0,true);
    bool osm=InitDetail::reg(io,p.cluster,0x3aa0,1,0,true);
    return loop && osm;
}
inline bool apply(const Plan &p,const RegisterIO &io) {
    if (!inspect(p,io)) return false;
    uint32_t ctl=0;
    if (!io.read32(io.context,p.cluster,0x004,ctl)) return false;
    if (ctl&1) return true; // inspect verified the complete existing state.
    // The caller inspects both clusters before either apply. Initial loop=0
    // is preserved until every controller, thread and corner field verifies.
    if (!InitDetail::program(p,io,true) ||
        !InitDetail::reg(io,p.cluster,0x004,1,1,true) || !verify(p,io)) {
        disable(p,io);
        return false;
    }
    return true;
}
inline bool apply(const Plan &p,const RegisterIO &io,const Ownership &ownership) {
    if (!inspect(p,io,ownership)) return false;
    // Follow cpr3_regulator_init_ctrl/init_cprh: install sensor count settings
    // with LOOP_EN=0, configure controller/thread/corners, then enable last.
    // CPR_STATUS.BUSY can stay set with the loop disabled (observed on Sagit).
    // The vendor's wait_for_idle is for a triggered measurement, not this
    // initialization transition. It must not gate programming a disabled loop.
    // Ownership, LOOP_EN readback and every configuration readback still gate
    // publication; no desired frequency request is authorized by this alone.
    if (!InitDetail::reg(io,p.cluster,0x004,0xfffffeff,0x21e,true)) {
        disable(p,io); return false;
    }
    if (!InitDetail::program(p,io,true) ||
        !InitDetail::reg(io,p.cluster,0x004,1,1,true) || !verify(p,io)) {
        disable(p,io); return false;
    }
    return true;
}
} // namespace QcomCPRh
