// SPDX-License-Identifier: GPL-2.0-only
#include <InterconnectResources.h>
#include <RPMInterconnect.h>
#include <RPMClockHandoff.h>
#include <RPMRegulators.h>
#include "../drivers/gpu/QcomGPU/AdrenoInterconnect.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
using namespace QcomPower;
static constexpr unsigned ClockMessages=2*RPMHandoffClockCount+2;
static void configureHandoff(RPMClockHandoff &handoff,uint32_t rate=19200) {
    for (unsigned i=0;i<RPMHandoffClockCount;++i)
        // QDSS uses STATE; it must not inherit this fixture's KHz rate.
        assert(handoff.configure(i,i==19 ? 4u:(rpmHandoffClocks[i].branch ? 1u:rate),
            i==19 ? 4u:(rpmHandoffClocks[i].branch ? 1u:rate)));
    for(unsigned i=0;i<RPMHandoffClockCount;++i)if(rpmClockHasLimit(i))
        assert(handoff.configureLimit(i,i==6 ? 1u:(i==19 ? 4u:1000000u),i==6 ? 1u:(i==19 ? 4u:2000000u)));
}
struct Backend {
    RPMInterconnect rpm;
    RPMClockHandoff handoff;
    std::map<uint32_t,uint32_t> memory{{0xb008,0xa3},{0xb00c,0x82}};
    std::map<uint32_t,unsigned> refs{{71,1},{32,1}};
    unsigned calls=0, failAt=0, writes=0, failWrite=0, readError=0, packets=0, handoffPackets=0;
    bool after=false, reset=true;
    std::vector<unsigned> qosOrder;
    Backend(uint64_t bandwidthFloor=300,uint32_t clockFloor=19200000) {
        configureHandoff(handoff,clockFloor/1000);
        for (auto r:rpmBusResources) assert(rpm.configure(r.id,r.clock ? 1000000000ULL : 10000000000ULL,
            r.clock ? clockFloor : bandwidthFloor,r.clock ? clockFloor : 100));
    }
    Status send(uint32_t type,uint32_t id,unsigned set,const uint8_t *p,unsigned n) {
        ++packets; assert(set<2);
        if (type==0x72676761 || type==0x316b6c63 || type==0x326b6c63) {
            assert(n==12 && le32(p)==0x007a484b && le32(p+4)==4 && id<2);
        } else assert((type==0x73616d62 || type==0x766c7362) && n==16 && le32(p)==0x7762 && le32(p+4)==8);
        return OK;
    }
    Status limit(uint32_t id,bool clock,uint64_t &v) { return rpm.limit(id,clock,v); }
    Status floor(uint32_t id,bool,uint64_t &v) { return rpm.floor(id,0,v); }
    Status get(uint32_t id,unsigned set,bool clock,uint64_t &v) { return handoff.ready() ? rpm.get(id,set,clock,v) : NotReady; }
    Status vote(uint32_t id,unsigned set,bool clock,uint64_t v) {
        ++calls; const bool failed=calls==failAt;
        if (failed && !after) return Denied;
        const auto initialized=handoff.activate([&](uint32_t,uint32_t,unsigned,const uint8_t *,unsigned) { ++handoffPackets; return OK; });
        if (initialized!=OK) return initialized;
        auto s=rpm.vote(id,set,10,clock,v,[&](auto... x){return send(x...);});
        return failed && s==OK ? TimedOut : s;
    }
    Status clock(uint32_t id,bool enable) {
        ++calls; const bool failed=calls==failAt;
        if (failed && !after) return Denied;
        if (enable) ++refs[id]; else { assert(refs[id]>1); --refs[id]; }
        return failed ? TimedOut : OK;
    }
    Status isolated() { return reset ? OK : Busy; }
    uint32_t read(uint32_t offset) {
        assert(refs[71]>1 && refs[32]>1);
        if (readError) { --readError; return UINT32_MAX; }
        return memory.at(offset);
    }
    void write(uint32_t offset,uint32_t value) {
        assert(reset && refs[71]>1 && refs[32]>1);
        ++writes; memory.at(offset)=value; qosOrder.push_back(offset);
        if (writes==failWrite) readError=1;
    }
};
struct GPUBackend:Backend {
    GPUBackend() {
        memory[Adreno::GPUInterconnect<GPUBackend>::Enable]=0xa5a50001;
        assert(handoff.activate([](auto...){return OK;})==OK && rpm.activate([](auto...){return OK;})==OK);
    }
    Status vote(uint32_t id,unsigned set,bool clock,uint64_t value) {
        ++calls;if(calls==failAt && !after)return Denied;
        const auto s=rpm.vote(id,set,12,clock,value,[&](auto... v){return send(v...);});
        return calls==failAt && after && s==OK?TimedOut:s;
    }
    Status get(uint32_t id,bool clock,uint64_t &value){return rpm.get(id,0,clock,value);}
    uint32_t read(uint32_t offset){assert(offset==0xc300);if(readError){--readError;return UINT32_MAX;}return memory[offset];}
    void write(uint32_t offset,uint32_t value){assert(reset && offset==0xc300);memory[offset]=value;++writes;}
};
static void noReferences(Backend &b) {
    assert(b.refs[71]==1 && b.refs[32]==1);
    assert(b.memory.at(0xb008)==0xa3 && b.memory.at(0xb00c)==0x82);
    for (const auto &r:rpmBusResources) {
        uint64_t value=0,floor=0; const auto s=b.rpm.get(r.id,0,r.clock,value);
        assert(b.rpm.floor(r.id,0,floor)==OK);
        assert(s==NotReady || (s==OK && value==floor));
    }
}
int main() {
    {
        GPUBackend b;Adreno::GPUInterconnect<GPUBackend> path(b);
        Adreno::NoCContract contract{1,3,240000000,800000000,1,0};
        assert(path.acquire(contract)==OK && path.check()==OK && b.memory[0xc300]==0xa5a50000);
        b.reset=false;assert(path.release()==Busy);b.reset=true;
        assert(path.release()==OK && path.empty() && b.memory[0xc300]==0xa5a50001);
        GPUBackend failed;Adreno::GPUInterconnect<GPUBackend> uncertain(failed);failed.failAt=4;failed.after=true;
        assert(uncertain.acquire(contract)==TimedOut && uncertain.release()==Stale && !uncertain.empty());
        GPUBackend denied;Adreno::GPUInterconnect<GPUBackend> partial(denied);denied.failAt=4;
        assert(partial.acquire(contract)==Denied);denied.failAt=0;assert(partial.release()==OK && partial.empty());
    }
    {
        RPMInterconnect bus;assert(bus.configure(0x2002,1000000000,19200000,0));assert(bus.configure(0x2104,20000000000ULL,0,0));
        struct Event{bool clock;uint64_t value;};std::vector<Event> events;
        auto send=[&](uint32_t type,uint32_t,unsigned,const uint8_t *p,unsigned) {
            const bool clock=type==0x326b6c63;const auto value=clock?uint64_t(le32(p+8))*1000:le32(p+8)|(uint64_t(le32(p+12))<<32);
            events.push_back({clock,value});return OK;
        };
        assert(bus.vote(0x2104,0,10,false,3000000000ULL,send)==OK);
        assert(events.size()==2 && events[0].clock && !events[1].clock && events[0].value==286875000);
        events.clear();assert(bus.vote(0x2104,0,12,false,1000000000,send)==OK);
        assert(events.size()==2 && events[0].clock && events[0].value==382500000 && events[1].value==4000000000ULL);
        const auto before=events.size();assert(bus.validate(0x2104,0,12,false,UINT64_MAX)==Invalid && events.size()==before);
        events.clear();assert(bus.vote(0x2104,0,12,false,0,send)==OK);
        assert(events.size()==2 && !events[0].clock && events[1].clock && events[1].value==286875000);
        assert(bus.vote(0x2104,0,12,false,1000000000,[&](uint32_t type,auto... values) {
            return type==0x766c7362?Denied:send(type,values...);
        })==Denied);
        uint64_t actual=0;assert(bus.get(0x2002,0,true,actual)==OK && actual==286875000);
        assert(bus.get(0x2104,0,false,actual)==OK && actual==3000000000ULL);
        events.resize(2);
        assert(bus.vote(0x2104,0,10,false,0,[&](auto... values) {return events.size()>2?Denied:send(values...);})==Stale);
        uint64_t value=0;assert(bus.get(0x2002,0,true,value)==Stale && bus.get(0x2104,0,false,value)==Stale);
    }
    {
        RPMRegulators domains;RPMInterconnect bus;RPMClockHandoff clocks;configureHandoff(clocks);
        for(unsigned i=0;i<4;++i)assert(domains.configureDomain(0x1000+i,192,64));
        for(const auto &r:rpmBusResources)assert(bus.configure(r.id,r.clock?1000000000ULL:10000000000ULL,r.clock?19200000:100,r.clock?19200000:50));
        unsigned packets=0,scaling=0;
        auto send=[&](uint32_t type,uint32_t id,unsigned,const uint8_t *,unsigned) {
            ++packets;
            if(type==0x306b6c63 && id==2) {assert(domains.domainsReady() && bus.handoffReady() && packets>=8+ClockMessages+2*RPMBusResourceCount-1);++scaling;}
            return OK;
        };
        assert(domains.activateDomains(send)==OK && packets==8);
        assert(clocks.activate(send,[&] {return bus.activate(send);})==OK && packets==8+ClockMessages+2*RPMBusResourceCount && scaling==2);
        uint64_t value=0;assert(bus.get(0x2002,0,true,value)==OK && value==19200000);
        assert(bus.get(0x2104,0,false,value)==OK && value==100);
        assert(clocks.activate(send,[&] {return bus.activate(send);})==OK && packets==8+ClockMessages+2*RPMBusResourceCount);
        RPMInterconnect cold;
        for(const auto &r:rpmBusResources)assert(cold.configure(r.id,r.clock?1000000000ULL:10000000000ULL,r.clock?19200000:100,0));
        assert(cold.validate(0x2104,0,10,false,500)==OK && cold.activate(send)==OK);
        assert(cold.vote(0x2104,0,10,false,500,send)==OK);
        assert(cold.get(0x2104,0,false,value)==OK && value==600); // bootstrap cannot overwrite the first live consumer
    }
    {
        RPMClockHandoff missing;unsigned sends=0;
        for(unsigned i=0;i<26;++i)
            assert(missing.configure(i,i==19 ? 4u:(rpmHandoffClocks[i].branch ? 1u:19200u),0));
        for(unsigned i=0;i<RPMHandoffClockCount;++i)if(rpmClockHasLimit(i))
            assert(missing.configureLimit(i,i==6 ? 1u:(i==19 ? 4u:1000000u),i==6 ? 1u:(i==19 ? 4u:2000000u)));
        assert(missing.activate([&](auto...){++sends;return OK;})==Unsupported && !sends);
        assert(!missing.configure(26,uint32_t(INT32_MAX)+1,0) && missing.configure(26,192000,9600));
        assert(missing.configured());
        uint32_t floor=0;
        assert(missing.floor(0x6d656d70,0,0x007a484b,0,floor)==OK && floor==192000);
        assert(missing.floor(0x6d656d70,0,0x007a484b,1,floor)==OK && floor==9600);
    }
    {
        RPMClockHandoff h;
        assert(!h.configure(RPMHandoffClockCount,0,0) && !h.configure(6,0,0) && !h.configure(7,19199,0));
        assert(!h.configure(9,2,0));
        unsigned sends=0;
        assert(h.activate([&](auto...){++sends;return OK;})==Unsupported && !sends);
        configureHandoff(h); assert(!h.configure(0,0,0));
        auto send=[&](uint32_t type,uint32_t id,unsigned set,const uint8_t *p,unsigned n) {
            assert(le32(p+4)==4);
            if (sends<2*RPMHandoffClockCount) {
                const auto &c=rpmHandoffClocks[sends/2];
                assert(type==c.type && id==c.resource && le32(p)==c.key && set==sends%2);
                if (c.key==0x54415453) assert(le32(p+8)==4);
                assert(n==(rpmClockHasLimit(sends/2) ? 24u:12u));
                if(n==24)assert(le32(p+12)==0x0078616d && le32(p+16)==4);
            } else assert(n==12 && type==0x306b6c63 && id==2 && le32(p)==0x62616e45 && le32(p+8)==1 && set==(sends==2*RPMHandoffClockCount?1u:0u));
            ++sends; return OK;
        };
        assert(h.activate(send)==OK && sends==ClockMessages && h.ready());
        assert(h.activate(send)==OK && sends==ClockMessages);
    }
    for (bool after:{false,true}) for (unsigned stage=1;stage<=ClockMessages;++stage) {
        RPMClockHandoff h; configureHandoff(h); unsigned calls=0;
        assert(h.activate([&](auto...){++calls;return calls==stage ? (after?TimedOut:Denied) : OK;})==(after?TimedOut:Denied));
        assert(!h.ready()); const auto before=calls;
        const auto status=h.activate([&](auto...){++calls;return OK;});
        if (after) assert(status==Stale && calls==before);
        else assert(status==OK && h.ready() && calls==ClockMessages+1);
    }
    {
        RPMInterconnect r; uint64_t value=0; unsigned sends=0; uint64_t wire=0;
        auto send=[&](uint32_t,uint32_t,unsigned,const uint8_t *p,unsigned n) {
            ++sends; wire=le32(p+8); if(n==16) wire|=uint64_t(le32(p+12))<<32; return OK;
        };
        RPMInterconnect held;assert(held.configure(0x2104,10000,100,0));
        assert(held.gpuMinimum(0x2104,false)==NotReady);
        assert(held.vote(0x2104,0,10,false,500,send)==OK && wire==600);
        assert(held.gpuMinimum(0x2104,false)==OK && held.gpuMinimum(0x2104,false)==Busy);
        assert(held.vote(0x2104,0,10,false,0,send)==OK && wire==600); // no duplicate baseline vote
        assert(held.vote(0x2104,0,10,false,800,send)==OK && wire==900);
        assert(held.vote(0x2104,0,10,false,0,send)==OK && wire==600);
        assert(held.gpuMinimum(0x2104,true)==OK && wire==600);
        assert(held.vote(0x2104,0,10,false,0,send)==OK && wire==100);sends=0;
        assert(r.limit(0x2100,false,value)==Unsupported);
        assert(!r.configure(0x2000,1000000001,0,0));
        assert(r.configure(0x2000,1000000000,19200001,19200000));
        assert(!r.configure(0x2000,1000000000,0,0));
        assert(r.vote(0x2000,0,10,true,50000001,send)==OK && wire==50001);
        assert(r.get(0x2000,0,true,value)==OK && value==50001000);
        assert(r.vote(0x2000,0,3,true,80000000,send)==OK && wire==80000);
        assert(r.vote(0x2000,0,10,true,0,send)==OK && wire==80000 && sends==2);
        assert(r.vote(0x2000,0,3,true,0,send)==OK && wire==19201);
        assert(r.get(0x2000,1,true,value)==NotReady);
        assert(r.configure(0x2100,UINT64_MAX,3,1));
        const uint64_t big=(1ULL<<40)+7;
        assert(r.vote(0x2100,0,10,false,big,send)==OK && wire==big+3);
        assert(r.vote(0x2100,0,3,false,99,send)==OK && wire==big+102);
        assert(r.vote(0x2100,0,10,false,0,send)==OK && wire==102);
        assert(r.vote(0x2100,0,10,false,UINT64_MAX,send)==Invalid && wire==102);
        const auto deny=[](auto...){return Denied;};
        assert(r.vote(0x2100,0,3,false,0,deny)==Denied);
        assert(r.get(0x2100,0,false,value)==OK && value==102);
        assert(r.vote(0x2100,0,3,false,0,[](auto...){return TimedOut;})==TimedOut);
        assert(r.get(0x2100,0,false,value)==Stale && r.vote(0x2100,0,3,false,0,send)==Stale);
        assert(r.vote(0x2100,1,3,false,10,send)==OK && wire==11); // independent sleep set
        assert(r.vote(0x2100,2,3,false,0,send)==Invalid && r.vote(0x2000,0,16,true,0,send)==Invalid);
        assert(r.vote(0x2100,0,3,true,0,send)==Unsupported);
    }
    {
        Backend b; QcomNoC::USBPath<Backend> path(b);
        assert(path.supported()==OK && path.check()==NotReady);
        assert(path.acquire(240000000,800000000)==OK && b.calls==18 && b.packets==16 && b.handoffPackets==ClockMessages);
        assert(b.qosOrder==std::vector<unsigned>({0xb008,0xb008,0xb00c}));
        assert(b.memory.at(0xb008)==0xa5 && b.memory.at(0xb00c)==0x80);
        uint64_t value=0;
        for (unsigned i=0;i<3;++i) assert(b.rpm.get(0x2000+i,0,true,value)==OK && value==(i==2?100000000u:50000000u));
        b.reset=false; assert(path.check()==OK); const auto before=b.calls;
        assert(path.release()==Busy && b.calls==before && path.check()==NotReady);
        b.reset=true; assert(path.release()==OK && path.empty()); noReferences(b);
        assert(path.acquire(240000000,800000000)==OK && path.release()==OK); noReferences(b);
    }
    for (bool after:{false,true}) for (unsigned stage=1;stage<=18;++stage) {
        Backend b; QcomNoC::USBPath<Backend> path(b); b.failAt=stage; b.after=after;
        assert(path.acquire(240000000,800000000)==(after?TimedOut:Denied));
        b.failAt=0; const auto before=b.calls;
        if (after) { assert(path.faulted() && path.release()==Stale && b.calls==before); }
        else { assert(!path.faulted() && path.release()==OK && path.empty()); noReferences(b); }
    }
    for (unsigned write=1;write<=3;++write) {
        Backend b; QcomNoC::USBPath<Backend> path(b); b.failWrite=write;
        assert(path.acquire(240000000,800000000)==Failed && !path.faulted());
        assert(path.release()==OK && path.empty()); noReferences(b);
    }
    for (bool after:{false,true}) for (unsigned stage=1;stage<=10;++stage) {
        Backend b; QcomNoC::USBPath<Backend> path(b); assert(path.acquire(240000000,800000000)==OK);
        b.failAt=b.calls+stage; b.after=after;
        assert(path.release()==(after?TimedOut:Denied));
        b.failAt=0;
        if (after) assert(path.release()==Stale && path.faulted());
        else { assert(path.release()==OK && path.empty()); noReferences(b); }
    }
    for (unsigned write=1;write<=4;++write) {
        Backend b; QcomNoC::USBPath<Backend> path(b); assert(path.acquire(240000000,800000000)==OK);
        b.failWrite=b.writes+write; assert(path.release()==Failed);
        assert(path.release()==OK && path.empty()); noReferences(b);
    }
    {
        // Existing memory traffic must contribute to clock rate calculation.
        // max(firmware_clock, USB_clock) alone would under-vote BIMC here.
        Backend b(1280000000,128000000); QcomNoC::USBPath<Backend> path(b);
        assert(path.acquire(240000000,800000000)==OK);
        uint64_t value=0;
        assert(b.rpm.get(0x2002,0,true,value)==OK && value==145350000);
        assert(path.release()==OK); noReferences(b);
    }
    {
        Backend b; QcomNoC::USBPath<Backend> path(b); b.reset=false;
        assert(path.acquire(240000000,800000000)==Busy && !b.calls && path.empty());
        assert(path.acquire(0,1)==Invalid && path.acquire(2,1)==Invalid && path.acquire(UINT64_MAX,UINT64_MAX)==Invalid);
        assert(!b.calls && !b.writes);
    }
    puts("USB NoC: RPM u64 aggregation/floors, real path clocks/QoS, reset proof, partial cleanup and unknown ACK quarantine passed");
}
