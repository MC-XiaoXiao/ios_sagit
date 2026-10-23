// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerHardware.h>

namespace QcomPower {
// MSM8998 USB3 -> A1NoC -> SNOC -> BIMC -> EBI path. AP-owned USB QoS
// is programmed by the NoC service; the following remote nodes use RPM BW.
// IDs are this driver's namespace, not Linux clock IDs or device-tree handles.
struct RPMBusResource { uint32_t id, type, resource; bool clock; };
constexpr RPMBusResource rpmBusResources[] = {
    {0x2000,0x72676761,1,true}, // aggr1, kHz
    {0x2001,0x316b6c63,1,true}, // SNOC bus1, kHz
    {0x2002,0x326b6c63,0,true}, // BIMC mem0, kHz
    {0x2100,0x766c7362,142,false}, // A1NoC -> SNOC slave
    {0x2101,0x73616d62,111,false}, // A1NoC -> SNOC master
    {0x2102,0x766c7362,24,false},  // SNOC -> BIMC slave
    {0x2103,0x73616d62,3,false},   // SNOC -> BIMC master
    {0x2104,0x766c7362,0,false},   // EBI slave
};
constexpr unsigned RPMBusResourceCount=sizeof(rpmBusResources)/sizeof(rpmBusResources[0]);
class RPMInterconnect {
    struct Resource {
        uint64_t maximum=0, floor[2]{}, requested[2][16]{}, applied[2]{};
        uint64_t retainedMinimum[2]{}, derivedMinimum[2]{};
        bool known[2]{}, fault[2]{};
    } resources[RPMBusResourceCount];
    unsigned handoffNext=0;
    bool handoffFault=false, bimcFault[2]{};
    static int index(uint32_t id) {
        for (unsigned i=0;i<RPMBusResourceCount;++i) if (rpmBusResources[i].id==id) return int(i);
        return -1;
    }
public:
    template<class Send> Status activate(Send send) {
        if(handoffFault)return Stale;
        for(const auto &r:resources)if(!r.maximum)return Unsupported;
        for(;handoffNext<2*RPMBusResourceCount;++handoffNext) {
            const auto &r=rpmBusResources[handoffNext/2];
            const auto status=vote(r.id,handoffNext%2,0,r.clock,0,send);
            if(status!=OK) {
                if(status==TimedOut || status==Stale || status==Failed)handoffFault=true;
                return status;
            }
        }
        return OK;
    }
    bool handoffReady() const {return !handoffFault && handoffNext==2*RPMBusResourceCount;}
    const char *name(uint32_t id) const {
        const int i=index(id);
        static const char *const names[]={"A1NoC requested Hz","SNOC requested Hz","BIMC requested Hz",
            "A1NoC-SNOC slave average B/s","A1NoC-SNOC master average B/s","SNOC-BIMC slave average B/s",
            "SNOC-BIMC master average B/s","EBI average B/s"};
        return i>=0 && resources[i].maximum ? names[i] : nullptr;
    }
    bool configure(uint32_t id, uint64_t maximum, uint64_t activeFloor, uint64_t sleepFloor) {
        const int i=index(id);
        if (i<0 || resources[i].maximum || !maximum || activeFloor>maximum || sleepFloor>maximum) return false;
        // The kHz wire value must fit u32, including round-up. Limit clocks
        // to the driver's Hz ABI range; bandwidth retains all 64 bits.
        if (rpmBusResources[i].clock) {
            if (maximum>UINT32_MAX || maximum%1000) return false;
            activeFloor=((activeFloor+999)/1000)*1000;
            sleepFloor=((sleepFloor+999)/1000)*1000;
            if (activeFloor>maximum || sleepFloor>maximum) return false;
        }
        auto &r=resources[i]; r.maximum=maximum; r.floor[0]=activeFloor; r.floor[1]=sleepFloor;
        return true;
    }
    Status limit(uint32_t id, bool clock, uint64_t &value) const {
        const int i=index(id);
        if (i<0 || rpmBusResources[i].clock!=clock || !resources[i].maximum) return Unsupported;
        value=resources[i].maximum; return OK;
    }
    Status floor(uint32_t id,unsigned set,uint64_t &value) const {
        const int i=index(id);
        if (set>1) return Invalid;
        if (i<0 || !resources[i].maximum) return Unsupported;
        value=resources[i].floor[set]; return OK;
    }
    Status get(uint32_t id, unsigned set, bool clock, uint64_t &value) const {
        const int i=index(id);
        if (set>1) return Invalid;
        if (i<0 || rpmBusResources[i].clock!=clock || !resources[i].maximum) return Unsupported;
        const auto &r=resources[i];
        if (r.fault[set] || (groupResource(i) && bimcFault[set])) return Stale;
        if (!r.known[set]) return NotReady;
        value=r.applied[set]; return OK; // acknowledged aggregate request
    }
    Status gpuMinimum(uint32_t id, bool release) {
        if (id!=0x2002 && id!=0x2104) return Unsupported;
        const int i=index(id); auto &r=resources[i];
        uint64_t value=0; const auto status=get(id,0,rpmBusResources[i].clock,value);
        if (status!=OK) return status;
        if (!release) {
            if (!value) return NotReady;
            if (r.retainedMinimum[0]) return Busy;
            r.retainedMinimum[0]=value;
        } else {
            if (!r.retainedMinimum[0] || value<r.retainedMinimum[0]) return Stale;
            r.retainedMinimum[0]=0;
        }
        return OK;
    }
    // Validate before connecting/activating the shared RPM handoff. Sends
    // are nonreentrant, serialized transactions on the controller's gate.
    Status validate(uint32_t id,unsigned set,unsigned consumer,bool clock,uint64_t value) const {
        uint64_t total=0;const int i=index(id);
        const auto status=preview(i,set,consumer,clock,value,total);if(status!=OK)return status;
        uint64_t rate=0;
        return bandwidthCoupled(i) ? bimcRate(set,id,total,rate) : OK;
    }
    template<class Send> Status vote(uint32_t id, unsigned set, unsigned consumer, bool clock, uint64_t value, Send send) {
        const int i=index(id);uint64_t total=0;
        auto status=preview(i,set,consumer,clock,value,total);if(status!=OK)return status;
        if(clock)value=((value+999)/1000)*1000;
        uint64_t rate=0,previous=0;
        if(bandwidthCoupled(i)) {
            previous=resources[index(0x2002)].derivedMinimum[set];
            status=bimcRate(set,id,total,rate);if(status!=OK)return status;
            // A raising bandwidth request cannot reach RPM until the shared
            // BIMC clock covers the combined EBI/SNOC average demand.
            if(rate>resources[index(0x2002)].derivedMinimum[set]) {
                status=setBimcMinimum(set,rate,send);if(status!=OK)return status;
            }
        }
        status=apply(i,set,total,send);
        if(status!=OK) {
            if(bandwidthCoupled(i) && !uncertain(status) &&
                resources[index(0x2002)].derivedMinimum[set]!=previous && setBimcMinimum(set,previous,send)!=OK) {
                bimcFault[set]=true;return Stale;
            }
            return status;
        }
        resources[i].requested[set][consumer]=value;
        if(bandwidthCoupled(i) && rate!=resources[index(0x2002)].derivedMinimum[set]) {
            status=setBimcMinimum(set,rate,send);
            // BW has already been acknowledged. Do not report a clean
            // rejection or let a caller retry this partial transaction.
            if(status!=OK){bimcFault[set]=true;return Stale;}
        }
        return OK;
    }
private:
    static bool uncertain(Status s) {return s==TimedOut || s==Stale || s==Failed;}
    bool bandwidthCoupled(int i) const {
        return i>=0 && resources[index(0x2002)].maximum &&
            (rpmBusResources[i].id==0x2103 || rpmBusResources[i].id==0x2104);
    }
    bool groupResource(int i) const {
        return i>=0 && (rpmBusResources[i].id==0x2002 || bandwidthCoupled(i));
    }
    uint64_t current(unsigned i,unsigned set) const {
        const auto &r=resources[i];uint64_t total=r.floor[set];
        for(uint64_t v:r.requested[set]) {
            if(rpmBusResources[i].clock){if(v>total)total=v;}else total+=v;
        }
        if(r.retainedMinimum[set]>total)total=r.retainedMinimum[set];
        if(r.derivedMinimum[set]>total)total=r.derivedMinimum[set];
        return total;
    }
    Status preview(int i,unsigned set,unsigned consumer,bool clock,uint64_t value,uint64_t &total) const {
        if(set>1 || consumer>=16)return Invalid;
        if(i<0 || rpmBusResources[i].clock!=clock || !resources[i].maximum)return Unsupported;
        const auto &r=resources[i];if(r.fault[set] || (groupResource(i) && bimcFault[set]))return Stale;
        if(value>r.maximum)return Invalid;
        if(clock)value=((value+999)/1000)*1000;
        total=r.floor[set];
        for(unsigned n=0;n<16;++n) {
            const auto v=n==consumer?value:r.requested[set][n];
            if(clock){if(v>total)total=v;}
            else {if(v>r.maximum-total)return Invalid;total+=v;}
        }
        if(r.retainedMinimum[set]>total)total=r.retainedMinimum[set];
        if(r.derivedMinimum[set]>total)total=r.derivedMinimum[set];
        return total<=r.maximum?OK:Invalid;
    }
    Status bimcRate(unsigned set,uint32_t id,uint64_t proposed,uint64_t &rate) const {
        if(bimcFault[set])return Stale;
        uint64_t total=0;
        const uint32_t ids[]={0x2103,0x2104};
        for(uint32_t resource:ids) {
            const auto &r=resources[index(resource)];if(r.fault[set])return Stale;
            const uint64_t v=id==resource?proposed:current(index(resource),set);
            if(v>total)total=v;
        }
        if(total>UINT64_MAX/153)return Invalid;
        // MSM8998 BIMC utilization 153%, two channels, 8 bytes per cycle.
        // Average is split over channels; peak remains in client clock votes.
        total*=153;total=total/100+(total%100!=0);
        total=total/2+(total%2!=0);rate=total/8+(total%8!=0);
        if(rate>UINT32_MAX-999)return Invalid;
        rate=((rate+999)/1000)*1000;
        return rate<=resources[index(0x2002)].maximum?OK:Invalid;
    }
    template<class Send> Status apply(unsigned i,unsigned set,uint64_t total,Send send) {
        auto &r=resources[i];if(r.known[set] && total==r.applied[set])return OK;
        const auto &wire=rpmBusResources[i];uint8_t kvp[16]{};
        put32(kvp,wire.clock?0x007a484b:0x00007762);put32(kvp+4,wire.clock?4:8);
        put32(kvp+8,wire.clock?uint32_t((total+999)/1000):uint32_t(total));
        if(!wire.clock)put32(kvp+12,uint32_t(total>>32));
        const auto status=send(wire.type,wire.resource,set,kvp,wire.clock?12u:16u);
        if(status!=OK) {
            if(uncertain(status)){r.fault[set]=true;if(groupResource(i))bimcFault[set]=true;}
            return status;
        }
        r.applied[set]=total;r.known[set]=true;return OK;
    }
    template<class Send> Status setBimcMinimum(unsigned set,uint64_t rate,Send send) {
        const unsigned i=index(0x2002);auto &r=resources[i];const auto before=r.derivedMinimum[set];
        if(r.fault[set] || bimcFault[set])return Stale;
        r.derivedMinimum[set]=rate;const auto status=apply(i,set,current(i,set),send);
        if(status!=OK && !uncertain(status))r.derivedMinimum[set]=before;
        return status;
    }

};
}
