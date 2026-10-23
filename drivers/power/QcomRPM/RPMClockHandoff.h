// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerHardware.h>
namespace QcomPower {
// MSM8998's RPM clock resources, from its clock/ICC hardware tables. Active
// and sleep peers share a resource/key; they are not independent registers.
struct RPMHandoffClock { uint32_t type, resource, key; bool branch; uint32_t maximum=INT32_MAX; };
constexpr RPMHandoffClock rpmHandoffClocks[] = {
    {0x72676761,1,0x007a484b,false}, // A1NoC
    {0x72676761,2,0x007a484b,false}, // A2NoC
    {0x326b6c63,0,0x007a484b,false}, // BIMC
    {0x316b6c63,1,0x007a484b,false}, // SNOC
    {0x316b6c63,2,0x007a484b,false}, // CNOC
    {0x69786d6d,0,0x007a484b,false}, // MMSS NoC
    {0x306b6c63,0,0x62616e45,true},  // XO
    {0x316b6c63,0,0x007a484b,false}, // PCNoC
    {0x00006563,0,0x007a484b,false}, // CE
    {0x616b6c63,11,0x6e657773,true}, // DIV1
    {0x616b6c63,12,0x6e657773,true}, // DIV2
    {0x616b6c63,13,0x6e657773,true}, // DIV3
    {0x00617069,0,0x007a484b,false}, // IPA
    {0x616b6c63,1,0x6e657773,true},  // LN BB1 software
    {0x616b6c63,2,0x6e657773,true},  // LN BB2 software
    {0x616b6c63,3,0x6e657773,true},  // LN BB3 software
    {0x616b6c63,1,0x62636370,true},  // LN BB1 pin control
    {0x616b6c63,2,0x62636370,true},  // LN BB2 pin control
    {0x616b6c63,3,0x62636370,true},  // LN BB3 pin control
    // STATE is an enum, not a frequency. The RPM node maximum is 4;
    // accepting a larger request would let an ACK hide NPA clamping.
    {0x306b6c63,1,0x54415453,false,4}, // QDSS state
    {0x616b6c63,4,0x6e657773,true},  // RF1 software
    {0x616b6c63,5,0x6e657773,true},  // RF2 software
    {0x616b6c63,6,0x6e657773,true},  // RF3 software
    {0x616b6c63,4,0x62636370,true},  // RF1 pin control
    {0x616b6c63,5,0x62636370,true},  // RF2 pin control
    {0x616b6c63,6,0x62636370,true},  // RF3 pin control
    // ClockDxe also defines /clk/pmem (pmem0, KHz). Preserve its AP vote
    // before enabling global scaling; its low-power request is not a floor.
    {0x6d656d70,0,0x007a484b,false}, // PIMEM
};
constexpr unsigned RPMHandoffClockCount=sizeof(rpmHandoffClocks)/sizeof(rpmHandoffClocks[0]);
constexpr bool rpmClockHasLimit(unsigned index) {
    return index<RPMHandoffClockCount && (!rpmHandoffClocks[index].branch || rpmHandoffClocks[index].key==0x62616e45);
}
constexpr unsigned rpmClockLimitCount() {
    unsigned count=0;for(unsigned i=0;i<RPMHandoffClockCount;++i)if(rpmClockHasLimit(i))++count;return count;
}
constexpr unsigned RPMHandoffClockLimitCount=rpmClockLimitCount();
static_assert(RPMHandoffClockLimitCount==12,"RPM NPA clock limit catalogue");
class RPMClockHandoff {
    uint32_t values[RPMHandoffClockCount][2]{};
    uint32_t limits[RPMHandoffClockCount][2]{};
    bool owned[RPMHandoffClockCount]{}, limitOwned[RPMHandoffClockCount]{}, fault=false;
    unsigned next=0;
    static bool compatible(uint32_t value,uint32_t limit) {return !limit || value<=limit;}
    unsigned requestIndex(unsigned slot,unsigned set) const {
        const auto &c=rpmHandoffClocks[slot];
        if(c.key!=0x6e657773 && c.key!=0x62636370)return slot;
        for(unsigned other=0;other<RPMHandoffClockCount;++other) {
            const auto &peer=rpmHandoffClocks[other];
            if(peer.type!=c.type || peer.resource!=c.resource ||
                peer.key!=(c.key==0x6e657773 ? 0x62636370u:0x6e657773u))continue;
            const unsigned sw=c.key==0x6e657773 ? slot:other, pin=c.key==0x62636370 ? slot:other;
            const unsigned first=slot<other ? slot:other, second=slot<other ? other:slot;
            // RPM writes hardware SW before PIN even in a combined request.
            // ACK the desired enabled control before withdrawing its peer.
            // Both off remains an explicit off policy, without a new enable.
            const unsigned leading=values[sw][set] ? sw:(values[pin][set] ? pin:first);
            return slot==first ? leading:(leading==first ? second:first);
        }
        return slot;
    }
public:
    bool configure(unsigned index,uint32_t active,uint32_t sleep) {
        if (index>=RPMHandoffClockCount || owned[index] || next) return false;
        if (active>rpmHandoffClocks[index].maximum || sleep>rpmHandoffClocks[index].maximum) return false;
        if (rpmHandoffClocks[index].branch && (active>1 || sleep>1)) return false;
        if ((index==6 && active!=1) || (index==7 && active<19200)) return false;
        if(limitOwned[index] && (!compatible(active,limits[index][0]) || !compatible(sleep,limits[index][1])))return false;
        owned[index]=true; values[index][0]=active; values[index][1]=sleep;
        return true;
    }
    bool configureLimit(unsigned index,uint32_t active,uint32_t sleep) {
        if(!rpmClockHasLimit(index) || limitOwned[index] || next)return false;
        const auto &c=rpmHandoffClocks[index];
        const auto maximum=c.branch ? 1u:c.maximum;
        if(maximum!=INT32_MAX && (active>maximum || sleep>maximum))return false;
        if(owned[index] && (!compatible(values[index][0],active) || !compatible(values[index][1],sleep)))return false;
        limitOwned[index]=true;limits[index][0]=active;limits[index][1]=sleep;return true;
    }
    bool configured() const {
        for(unsigned i=0;i<RPMHandoffClockCount;++i)if(!owned[i] || (rpmClockHasLimit(i) && !limitOwned[i]))return false;
        return true;
    }
    Status floor(uint32_t type,uint32_t resource,uint32_t key,unsigned set,uint32_t &value) const {
        if (set>1) return Invalid;
        for (unsigned i=0;i<RPMHandoffClockCount;++i) {
            const auto &c=rpmHandoffClocks[i];
            if (c.type==type && c.resource==resource && c.key==key && owned[i]) { value=values[i][set]; return OK; }
        }
        return Unsupported;
    }
    Status limit(uint32_t type,uint32_t resource,unsigned set,uint32_t &value) const {
        if(set>1)return Invalid;
        for(unsigned i=0;i<RPMHandoffClockCount;++i) {
            const auto &c=rpmHandoffClocks[i];
            if(c.type==type && c.resource==resource && limitOwned[i]) {value=limits[i][set];return OK;}
        }
        return Unsupported;
    }
    bool ready() const { return !fault && next==2*RPMHandoffClockCount+2; }
    template<class Send,class Resources> Status activate(Send send,Resources resources) {
        if (fault) return Stale;
        if (!configured()) return Unsupported;
        for (;next<2*RPMHandoffClockCount+2;++next) {
            // All additional inherited resource floors must be acknowledged
            // before enabling global clock scaling in either set.
            if(next==2*RPMHandoffClockCount) {
                const auto status=resources();
                if(status!=OK) {
                    if(status==TimedOut || status==Stale || status==Failed)fault=true;
                    return status;
                }
            }
            uint32_t type,id,key,value; unsigned set,index=RPMHandoffClockCount;
            if (next<2*RPMHandoffClockCount) {
                set=next%2; index=requestIndex(next/2,set);
                const auto &c=rpmHandoffClocks[index];
                type=c.type; id=c.resource; key=c.key; value=values[index][set];
            } else {
                // Clock scaling is global. Preserve ALL owned clock baselines
                // before enabling either set; never enable it for USB alone.
                type=0x306b6c63; id=2; key=0x62616e45; value=1;
                set=next==2*RPMHandoffClockCount ? 1u : 0u;
            }
            uint8_t kvp[24]{};unsigned bytes=12;put32(kvp,key);put32(kvp+4,4);put32(kvp+8,value);
            if(rpmClockHasLimit(index)) {
                // RPM retains omitted fields. Transfer the declared cap with
                // its request so an old LIMIT_MAX cannot silently clamp it.
                put32(kvp+12,0x0078616d);put32(kvp+16,4);put32(kvp+20,limits[index][set]);bytes=24;
            }
            const auto status=send(type,id,set,kvp,bytes);
            if (status!=OK) {
                if (status==TimedOut || status==Stale || status==Failed) fault=true;
                return status;
            }
        }
        return OK;
    }
    template<class Send> Status activate(Send send) {return activate(send,[] {return OK;});}
};
}
