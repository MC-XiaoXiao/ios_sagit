// SPDX-License-Identifier: GPL-2.0-only
// MSM8998 v2/v2.1 sagit CPRh calibration. Derived from the pinned Xiaomi
// cprh-kbss-regulator.c and cpr3-util.c in docs/power/reference.
// No MMIO, allocation, voltage fallback or device-independent voltage table.
#pragma once
#include <stdint.h>
#include "CPRhSagitV2Tables.h"

namespace QcomCPRh {
static constexpr uint32_t kFuseBase = 0x00784000;
static constexpr uint32_t kFuseRowOffsets[] =
    {38*8, 39*8, 67*8, 68*8, 69*8, 70*8, 71*8, 100*8};
static constexpr uint32_t kBaseUV = 352000, kStepUV = 4000;
struct FuseRows {
    uint64_t row38, row39, row67, row68, row69, row70, row71, row100;
};
struct Identity { uint32_t chipID, chipVersion, boardID; };
struct Corner {
    uint32_t frequencyHz, openLoopUV, floorUV, ceilingUV, targetQuotient, ro, word;
};
struct OSMRow {
    uint32_t frequencyHz, frequency, voltage, overrideValue, spare;
};
struct Plan {
    Identity identity;
    unsigned cluster, activeCount, count, speedBin, cprRevision;
    Corner corners[40];
    uint32_t baseQuotient[16], roMask;
    // Threshold values are zero-based CPR virtual corner indices. Crossover
    // entries are inactive, open-loop-only corners appended after activeCount.
    unsigned apmThresholdCorner, apmCrossoverCorner;
    unsigned memAccThresholdCorner, memAccCrossoverCorner;
    // OSM LUT indices can differ from virtual corners (single-core entries).
    unsigned apmThresholdIndex, memAccThresholdIndex, memAccThresholdPreIndex;
    OSMRow osm[40];
    unsigned osmCount;
};
struct DecodedFuses {
    unsigned speedBin, revision, forceHighest;
    unsigned ro[4], voltage[4], target[4], offset[4];
};
inline unsigned bits(uint64_t value, unsigned shift, unsigned width) {
    return unsigned((value >> shift) & ((uint64_t(1) << width) - 1));
}
inline bool supported(const Identity &id) {
    return id.chipID == 292 && (id.chipVersion == 0x20000 || id.chipVersion == 0x20001)
        && id.boardID == 30;
}
inline bool decode(const FuseRows &r, unsigned cluster, DecodedFuses &f) {
    if (cluster > 1) return false;
    f = {};
    f.speedBin = bits(r.row38, 29, 3);
    f.revision = bits(r.row39, 51, 3);
    f.forceHighest = bits(r.row100, 45, 1);
    if (f.speedBin > 3) return false;
    if (cluster == 0) {
        for (unsigned i=0; i<4; ++i) {
            f.ro[i] = bits(r.row67, 12-4*i, 4);
            f.voltage[i] = bits(r.row67, 34-6*i, 6);
        }
        f.target[0]=bits(r.row68,18,12); f.target[1]=bits(r.row68,6,12);
        f.target[2]=bits(r.row67,58,6)|(bits(r.row68,0,6)<<6);
        f.target[3]=bits(r.row67,46,12);
        f.offset[1]=bits(r.row68,63,1)|(bits(r.row69,0,6)<<1);
        f.offset[2]=bits(r.row68,56,7); f.offset[3]=bits(r.row68,49,7);
    } else {
        for (unsigned i=0; i<4; ++i) {
            f.ro[i]=bits(r.row69,26-4*i,4);
            f.voltage[i]=bits(r.row69,48-6*i,6);
        }
        f.target[0]=bits(r.row70,32,12); f.target[1]=bits(r.row70,20,12);
        f.target[2]=bits(r.row70,8,12);
        f.target[3]=bits(r.row69,60,4)|(bits(r.row70,0,8)<<4);
        f.offset[1]=bits(r.row71,13,3)|(bits(r.row71,21,4)<<3);
        f.offset[2]=bits(r.row71,6,7);
        f.offset[3]=bits(r.row70,63,1)|(bits(r.row71,0,6)<<1);
    }
    // A zero fused target cannot select a working RO. Erased/unreadable banks
    // and arithmetic underflow are rejected instead of producing a low voltage.
    for (unsigned i=0; i<4; ++i) {
        if (!f.target[i] || f.target[i] == 4095) return false;
        if (i && f.ro[i] != f.ro[i-1] && f.target[i] <= f.offset[i]*5)
            return false;
    }
    return true;
}
namespace Detail {
inline const int64_t *tuple(const Data::Array &a, unsigned n, unsigned combo) {
    if (a.count == n) return a.values;
    if (a.count == n*32) return a.values+n*combo;
    if (a.count == n*4) return a.values+n*(combo/8);
    return nullptr;
}
inline const int64_t *corners(const Data::Array &a, const Data::Cluster &d,
                             unsigned combo) {
    unsigned total=0, offset=0, binTotal=0, binOffset=0;
    for (unsigned i=0; i<32; ++i) {
        if (i < combo) offset += unsigned(d.counts.values[i]);
        total += unsigned(d.counts.values[i]);
        if (i%8 == 0) {
            if (i/8 < combo/8) binOffset += unsigned(d.counts.values[i]);
            binTotal += unsigned(d.counts.values[i]);
        }
    }
    if (a.count == unsigned(d.counts.values[combo])) return a.values;
    if (a.count == total) return a.values+offset;
    if (a.count == binTotal) return a.values+binOffset;
    return nullptr;
}
inline int64_t max(int64_t a, int64_t b) { return a > b ? a : b; }
inline int64_t roundUV(int64_t a) { return ((a+kStepUV-1)/kStepUV)*kStepUV; }
inline int64_t interpolate(uint32_t x1, int64_t y1, uint32_t x2,
                           int64_t y2, uint32_t x) {
    return y2 - int64_t(uint64_t(x2-x)*uint64_t(y2-y1)/(x2-x1));
}
inline int64_t quotientAdjustment(int64_t scale, int64_t uv) {
    return scale*uv/1000000; // Signed division truncates toward zero, as Linux.
}
inline const Data::OSMSource *osmSource(unsigned cluster, unsigned bin, unsigned &n) {
    if (!cluster) { n=sizeof(Data::osm0_0)/sizeof(Data::osm0_0[0]); return Data::osm0_0; }
    switch (bin) {
    case 0: n=sizeof(Data::osm1_0)/sizeof(Data::osm1_0[0]); return Data::osm1_0;
    case 1: n=sizeof(Data::osm1_1)/sizeof(Data::osm1_1[0]); return Data::osm1_1;
    case 2: n=sizeof(Data::osm1_2)/sizeof(Data::osm1_2[0]); return Data::osm1_2;
    default: n=sizeof(Data::osm1_3)/sizeof(Data::osm1_3[0]); return Data::osm1_3;
    }
}
} // namespace Detail

// The caller must obtain rows and identity from this device's firmware/efuse
// aperture. Successful model calibration is not proof of secure OSM readiness.
// On failure output is cleared so a caller cannot reuse a previous device plan.
inline bool calibrate(const Identity &id, const FuseRows &rows, unsigned cluster, Plan &out) {
    out = {};
    DecodedFuses f;
    if (!supported(id) || !decode(rows,cluster,f)) return false;
    const Data::Cluster &d=cluster ? Data::cluster1 : Data::cluster0;
    unsigned combo=f.speedBin*8+f.revision;
    Plan p={};
    p.identity=id; p.cluster=cluster; p.speedBin=f.speedBin; p.cprRevision=f.revision;
    p.activeCount=unsigned(d.counts.values[combo]); p.count=p.activeCount+2;
    if (!p.activeCount || p.count>40) return false;
    const int64_t *fmax=Detail::tuple(d.fmax,4,combo);
    const int64_t *scale=Detail::tuple(d.roScale,64,combo);
    const int64_t *of=Detail::tuple(d.openFuse,4,combo);
    const int64_t *cf=Detail::tuple(d.closedFuse,4,combo);
    const int64_t *hz=Detail::corners(d.frequency,d,combo);
    const int64_t *floor=Detail::corners(d.floor,d,combo);
    const int64_t *ceiling=Detail::corners(d.ceiling,d,combo);
    const int64_t *range=Detail::corners(d.range,d,combo);
    const int64_t *oa=Detail::corners(d.openAdjust,d,combo);
    const int64_t *ca=Detail::corners(d.closedAdjust,d,combo);
    if (!fmax||!scale||!of||!cf||!hz||!floor||!ceiling||!range||!oa||!ca) return false;
    unsigned fm[4], fuseCorner[40]={};
    int64_t voltage[4], roScale[4], qlow[4], qhigh[4], open[40];
    static constexpr int ref[2][4]={{688000,756000,828000,1056000},
                                   {756000,756000,828000,1056000}};
    for (unsigned i=0; i<4; ++i) {
        if (fmax[i]<=0 || fmax[i]>p.activeCount || (i && fmax[i]<=fmax[i-1]))
            return false;
        fm[i]=unsigned(fmax[i]-1);
        int adjust=int(f.voltage[i]&31)*10000;
        voltage[i]=ref[cluster][i]+((f.voltage[i]&32) ? -adjust : adjust)+of[i];
        if (voltage[i] <= 0) return false;
        if (i) voltage[i]=Detail::max(voltage[i],voltage[i-1]);
        roScale[i]=scale[i*16+f.ro[i]];
        if (roScale[i]<=0) return false;
        for (unsigned j=i ? fm[i-1]+1 : 0; j<=fm[i]; ++j) fuseCorner[j]=i;
    }
    if (fm[3]+1 != p.activeCount) return false;
    for (unsigned i=0; i<p.activeCount; ++i) {
        if (hz[i]<=0 || hz[i]>UINT32_MAX || (i && hz[i]<=hz[i-1])) return false;
        if (floor[i]<kBaseUV || floor[i]>ceiling[i] || ceiling[i]>1372000 ||
            floor[i]%kStepUV || ceiling[i]%kStepUV) return false;
        unsigned fc=fuseCorner[i];
        open[i]=fc ? Detail::interpolate(uint32_t(hz[fm[fc-1]]),voltage[fc-1],
                             uint32_t(hz[fm[fc]]),voltage[fc],uint32_t(hz[i]))
                       : voltage[0];
        open[i]+=oa[i];
        // The board has no minimum-difference property; upstream still enforces
        // monotonically increasing open-loop voltage before clipping/rounding.
        if (i) open[i]=Detail::max(open[i],open[i-1]);
    }
    for (unsigned i=0; i<p.activeCount; ++i) {
        int64_t v=Detail::roundUV(open[i]), fl=floor[i], ce=ceiling[i];
        if (v<fl) v=fl; else if (v>ce) v=ce;
        const int64_t thresholds[2]={800000,852000};
        for (unsigned t=0;t<2;++t) if (fl<thresholds[t] && thresholds[t]<=ce) {
            if (v>=thresholds[t]) fl=thresholds[t];
            else ce=thresholds[t]-kStepUV;
        }
        ce=v; // qcom,cpr-scaled-open-loop-voltage-as-ceiling
        if (range[i]>=0) fl=Detail::max(fl,Detail::roundUV(ce-range[i]));
        v=Detail::max(v,fl);
        if (i && fl<p.corners[i-1].floorUV) {
            fl=p.corners[i-1].floorUV;
            v=Detail::max(v,fl); ce=Detail::max(ce,fl);
        }
        Corner &c=p.corners[i];
        c.frequencyHz=uint32_t(hz[i]); c.openLoopUV=uint32_t(v);
        c.floorUV=uint32_t(fl); c.ceilingUV=uint32_t(ce); c.ro=f.ro[fuseCorner[i]];
    }
    // Preserve the upstream two-pass quotient interpolation, including use of
    // the already-adjusted lowest quotient when adjacent fuse ROs match.
    qhigh[0]=qlow[0]=int64_t(f.target[0])+Detail::quotientAdjustment(roScale[0],cf[0]);
    for (unsigned i=1;i<4;++i) {
        qhigh[i]=f.target[i];
        qlow[i]=f.ro[i]==f.ro[i-1] ? qhigh[i-1] : qhigh[i]-f.offset[i]*5;
        qhigh[i]=Detail::max(qhigh[i],qlow[i]);
    }
    for (unsigned i=1;i<4;++i) {
        qhigh[i]+=Detail::quotientAdjustment(roScale[i],cf[i]);
        qlow[i]=f.ro[i]==f.ro[i-1] ? qhigh[i-1] :
            qlow[i]+Detail::quotientAdjustment(roScale[i],cf[i-1]);
        qhigh[i]=Detail::max(qhigh[i],qlow[i]);
    }
    for (unsigned i=0;i<4;++i)
        if (qlow[i]<=0 || qhigh[i]<=0 || qhigh[i]>4095) return false;
    for (unsigned i=0;i<p.activeCount;++i) {
        unsigned fc=fuseCorner[i];
        int64_t q=fc ? Detail::interpolate(uint32_t(hz[fm[fc-1]]),qlow[fc],
                       uint32_t(hz[fm[fc]]),qhigh[fc],uint32_t(hz[i])) : qhigh[0];
        q+=Detail::quotientAdjustment(roScale[fc],ca[i]);
        if (i && p.corners[i].ro==p.corners[i-1].ro)
            q=Detail::max(q,p.corners[i-1].targetQuotient);
        if (q<=0 || q>4095) return false;
        p.corners[i].targetQuotient=uint32_t(q);
    }
    if (f.forceHighest) for (unsigned i=0;i+1<p.activeCount;++i) {
        uint32_t frequency=p.corners[i].frequencyHz;
        p.corners[i]=p.corners[p.activeCount-1]; p.corners[i].frequencyHz=frequency;
    }
    p.apmCrossoverCorner=p.activeCount; p.memAccCrossoverCorner=p.activeCount+1;
    p.corners[p.activeCount]={0,880000,880000,880000,0,0,0};
    p.corners[p.activeCount+1]={0,852000,852000,852000,0,0,0};
    p.apmThresholdCorner=p.memAccThresholdCorner=0xff;
    p.roMask=0xffff;
    for (unsigned i=0;i<p.activeCount;++i) {
        const Corner &c=p.corners[i];
        p.roMask &= ~(1u<<c.ro);
        uint32_t &b=p.baseQuotient[c.ro];
        if (!b || c.targetQuotient<b) b=c.targetQuotient;
        if (p.apmThresholdCorner==0xff && c.ceilingUV>=800000) p.apmThresholdCorner=i;
        if (p.memAccThresholdCorner==0xff && c.ceilingUV>=852000) p.memAccThresholdCorner=i;
    }
    for (unsigned i=0;i<p.count;++i) {
        Corner &c=p.corners[i];
        if (c.openLoopUV<kBaseUV || c.floorUV<kBaseUV || c.floorUV>c.openLoopUV ||
            c.openLoopUV>c.ceilingUV || c.openLoopUV%kStepUV || c.floorUV%kStepUV)
            return false;
        uint32_t v=(c.openLoopUV-kBaseUV)/kStepUV, fl=(c.floorUV-kBaseUV)/kStepUV;
        uint32_t delta=i<p.activeCount ? (c.targetQuotient-p.baseQuotient[c.ro]+3)/4 : 0;
        if (v>255 || fl>255 || delta>511) return false;
        c.word=v|(fl<<8)|(delta<<16)|(c.ro<<25);
        if (i>=p.activeCount) c.word|=1u<<29;
    }
    const Data::OSMSource *src=Detail::osmSource(cluster,f.speedBin,p.osmCount);
    p.apmThresholdIndex=p.memAccThresholdIndex=0xff;
    for (unsigned i=0;i<40;++i) {
        const Data::OSMSource &s=src[i<p.osmCount ? i : p.osmCount-1];
        // Single-core turbo OSM rows reuse a CPR corner with a different clock
        // rate; these mappings are supplied by the pinned sagit clock table.
        if (s.corner>=p.activeCount) return false;
        uint32_t uv=p.corners[s.corner].ceilingUV;
        p.osm[i]={s.hz,s.frequency,(s.corner<<16)|(uv/1000),s.overrideValue,s.spare};
        if (i<p.osmCount) {
            if (p.apmThresholdIndex==0xff && uv>=800000) p.apmThresholdIndex=i;
            if (p.memAccThresholdIndex==0xff && uv>=852000) p.memAccThresholdIndex=i;
        }
    }
    // Upstream MAX_VIRTUAL_CORNER is 39. If no voltage reaches a crossover,
    // the sequencer threshold is placed outside the supported active corners.
    if (p.apmThresholdCorner==0xff) p.apmThresholdCorner=39;
    if (p.memAccThresholdCorner==0xff) p.memAccThresholdCorner=39;
    p.memAccThresholdPreIndex=p.memAccThresholdIndex==0xff ? 0xff :
        (p.memAccThresholdIndex ? p.memAccThresholdIndex-1 : 0xff);
    out=p;
    return true;
}
} // namespace QcomCPRh
