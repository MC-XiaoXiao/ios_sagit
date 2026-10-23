// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "RPMRails.h"
#include <PowerVoltagePolicy.h>
namespace QcomPower {
class RPMRegulators {
    VoltageVotes votes[RPMRailCount + 4];
    uint32_t lower[RPMRailCount] = {}, upper[RPMRailCount] = {}, floors[RPMRailCount] = {};
    uint32_t domainFloor[4][2]{};
    unsigned domainMask=0, domainNext=0;
    unsigned railNext=0;
    bool domainFault=false,railFault=false;
    struct Prepared {unsigned index;uint32_t type,resource,key,value;};
    static bool validLevel(uint32_t value) {
        const uint32_t levels[]={0,16,32,48,64,128,192,256,320,384,416,448,512};
        for (uint32_t v:levels) if(v==value)return true;
        return false;
    }
    static int index(uint32_t id) {
        for (unsigned i = 0; i < RPMRailCount; ++i) if (rpmRails[i].id == id) return int(i);
        return -1;
    }
    Status prepare(uint32_t id,unsigned set,unsigned client,uint32_t value,bool level,Prepared &out) const {
        if(set>1 || client>=16)return Invalid;
        int i=index(id);uint32_t type,resource,key;
        if(level) {
            if(id<0x1000 || id>=0x1004 || !validLevel(value))return Invalid;
            if(value<domainFloor[id-0x1000][set])value=domainFloor[id-0x1000][set];
            i=RPMRailCount+id-0x1000;
            const uint32_t types[]={0x78637772,0x786d7772,0x63737772,0x6d737772};
            type=types[id-0x1000];resource=0;key=0x6c766c76;
        } else {
            if(i<0 || !upper[i])return Unsupported;
            const auto &r=rpmRails[i];
            if(value && (value<lower[i] || value>upper[i]))return Invalid;
            if(value) {
                value=r.minimum+((value-r.minimum+r.step-1)/r.step)*r.step;
                if(value>upper[i])return Invalid;
            }
            if(value<floors[i])value=floors[i];
            type=r.type;resource=r.resource;key=0x00007675;
        }
        uint32_t current=0;
        if(votes[i].get(set,current)==Stale)return Stale;
        out={unsigned(i),type,resource,key,value};return OK;
    }
public:
    bool configureDomain(uint32_t id,uint32_t active,uint32_t sleep) {
        if(id<0x1000 || id>=0x1004 || domainNext || domainFault || !validLevel(active) || !validLevel(sleep))return false;
        // CX/MX cannot start with an off active baseline while the AP kernel
        // and its fabric are running. This matches the bridge policy gate.
        if(id<0x1002 && !active)return false;
        const unsigned i=id-0x1000;
        if(domainMask&(1U<<i))return false;
        domainFloor[i][0]=active;domainFloor[i][1]=sleep;domainMask|=1U<<i;return true;
    }
    bool domainsConfigured() const {return domainMask==15;}
    Status minimumDomainLevel(uint32_t id,unsigned set,uint32_t &value) const {
        if(id<0x1000 || id>=0x1004 || set>1)return Invalid;
        const unsigned i=id-0x1000;
        if(!(domainMask&(1U<<i)))return Unsupported;
        value=domainFloor[i][set];return OK;
    }
    bool domainsReady() const {return !domainFault && domainNext==8;}
    bool railFloorsReady() const {return !railFault && railNext==2*RPMRailCount;}
    // Replay only explicit nonzero floors. An absent/zero floor is neither
    // an acknowledged off vote nor authority to lower an inherited owner.
    template<class Send> Status activateRails(Send send) {
        if(railFault)return Stale;
        for(;railNext<2*RPMRailCount;++railNext) {
            const unsigned i=railNext/2,setIndex=railNext%2;
            if(!floors[i])continue;
            const auto status=set(rpmRails[i].id,setIndex,0,floors[i],false,send);
            if(status!=OK) {
                if(status==TimedOut || status==Stale || status==Failed)railFault=true;
                return status;
            }
        }
        return OK;
    }
    template<class Send> Status activateDomains(Send send) {
        if(domainFault)return Stale;
        if(!domainsConfigured())return Unsupported;
        for(;domainNext<8;++domainNext) {
            const unsigned i=domainNext/2,setIndex=domainNext%2;
            const auto status=set(0x1000+i,setIndex,0,domainFloor[i][setIndex],true,send);
            if(status!=OK) {
                if(status==TimedOut || status==Stale || status==Failed)domainFault=true;
                return status;
            }
        }
        return OK;
    }
    bool configure(uint32_t id, uint32_t min, uint32_t max, uint32_t floor) {
        int i = index(id);
        if (i < 0 || upper[i] || railNext || railFault || min > max || min < rpmRails[i].minimum || max > rpmRails[i].maximum || floor > max ||
            (floor && floor < min) || (!max && rpmRails[i].step)) return false;
        // Switch resources are represented by 0/1, never by fabricated uV.
        if (!rpmRails[i].step || (floor && (floor-rpmRails[i].minimum)%rpmRails[i].step)) return false;
        lower[i] = min; upper[i] = max; floors[i] = floor;
        return true;
    }
    const char *name(uint32_t id) const {
        int i = index(id); if (i >= 0) return rpmRails[i].name;
        const char *names[] = {"vddcx_level", "vddmx_level", "ssccx_level", "sscmx_level"};
        return id >= 0x1000 && id < 0x1004 ? names[id-0x1000] : nullptr;
    }
    Status get(uint32_t id, unsigned set, uint32_t &value) const {
        int i = id >= 0x1000 && id < 0x1004 ? int(RPMRailCount+id-0x1000) : index(id);
        return i < 0 ? Unsupported : votes[i].get(set, value);
    }
    Status gpuMinimum(uint32_t id, bool release) {
        if (id!=0x1000 && id!=0x1001) return Unsupported;
        auto &v=votes[RPMRailCount+id-0x1000];
        return release ? v.releaseMinimum(0) : v.retainMinimum(0);
    }
    Status validate(uint32_t id,unsigned set,unsigned client,uint32_t value,bool level) const {
        Prepared request{};return prepare(id,set,client,value,level,request);
    }
    template <typename Send> Status set(uint32_t id, unsigned set, unsigned client, uint32_t value, bool level, Send send) {
        Prepared request{};const auto status=prepare(id,set,client,value,level,request);if(status!=OK)return status;
        return votes[request.index].vote(set, client, request.value, [&](unsigned state, uint32_t maximum) {
            uint8_t kvp[24] = {};
            put32(kvp, 0x6e657773); put32(kvp+4, 4); put32(kvp+8, maximum != 0);
            put32(kvp+12, request.key); put32(kvp+16, 4); put32(kvp+20, maximum);
            return send(request.type, request.resource, state, kvp, sizeof(kvp));
        });
    }
};
}
