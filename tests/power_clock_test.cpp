// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/clock/QcomMMCC/ClockTables.h"
#include "../drivers/clock/QcomGPUCC/ClockTables.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>
using namespace QcomPower;
struct Model {
    const CCDescription &d;
    std::map<uint64_t,uint32_t> mem;
    struct Event { unsigned type; uint32_t a,b; };
    std::vector<Event> events;
    unsigned failLatch=0; bool holdLatch=false;
    unsigned failUpdates=0; bool failVoltage=false, failDomain=false, failGate=false;
    uint32_t &at(uint32_t r,unsigned bank=0) { assert(!(r&3)); assert(r<(bank?0x94000U:(d.gpu?0x9000U:0x40000U))); return mem[(uint64_t(bank)<<32)|r]; }
    explicit Model(const CCDescription &data):d(data) {
        at(0,1)=0x80000001; at(4,1)=31; at(0x38,1)=0x4000;
        for(unsigned i=0;i<d.branchCount;++i) at(d.branches[i].reg)=0x80000000;
        for(unsigned i=0;i<d.domainCount;++i) at(d.domains[i].reg)=1;
        for(unsigned i=0;i<d.pllCount;++i) { const auto &p=d.plls[i]; at(p.reg)=0x80000005; at(p.reg+4)=32; }
    }
    static uint32_t read(void *c,unsigned bank,uint32_t r) { return static_cast<Model *>(c)->at(r,bank); }
    static void write(void *c,unsigned bank,uint32_t r,uint32_t v) {
        auto &m=*static_cast<Model *>(c); m.events.push_back({0,r,v}); m.at(r,bank)=v;
        if(bank) return;
        for(unsigned i=0;i<m.d.pllCount;++i) if(r==m.d.plls[i].reg) {
            if(v&(1U<<22)) {
                if(m.holdLatch) {}
                else if(m.failLatch) --m.failLatch;
                else if(v&(1U<<23)) m.at(r)|=1U<<29;
                else m.at(r)&=~((1U<<22)|(1U<<29));
            } else m.at(r)&=~(1U<<29);
        }
        for(unsigned i=0;i<m.d.rootCount;++i) if(r==m.d.roots[i].reg && (v&1)) {
            if(m.failUpdates) --m.failUpdates; else m.at(r)&=~1U;
        }
        for(unsigned i=0;i<m.d.domainCount;++i) { const auto &d=m.d.domains[i]; if(r!=d.reg || m.failDomain) continue;
            uint32_t s=d.status?d.status:d.reg;
            if(v&1) m.at(s)&=~0x80000000U; else m.at(s)|=0x80000000U;
        }
        for(unsigned i=0;i<m.d.branchCount;++i) { const auto &b=m.d.branches[i]; if(r!=b.reg || m.failGate) continue;
            if(v&b.mask) m.at(r)&=~0x80000000U; else m.at(r)|=0x80000000U;
        }
    }
    static void wait(void *,unsigned) {}
    static Status supply(void *c,uint32_t controller,uint32_t resource,uint32_t value) {
        auto &m=*static_cast<Model *>(c); assert(controller==2 || controller==3); m.events.push_back({1,resource,value}); return m.failVoltage?Denied:OK;
    }
    static Status parent(void *c,int p) { auto &m=*static_cast<Model *>(c); m.events.push_back({2,uint32_t(p),0}); return OK; }
    IO io() { return {this,read,write,wait,nullptr}; }
    SupplyIO supplyIO() { return {this,supply,parent}; }
    void parentRate(const CCRoot &root,const CCRate &rate) {
        int index=root.parents[rate.source]; if(index<0) return;
        uint64_t hz=uint64_t(rate.hz)*(rate.divider+1)/2;
        if(rate.n) hz=hz*rate.n/rate.m;
        const auto &p=d.plls[index]; at(p.reg+0xc)=0;
        at(p.reg+4)=hz/19200000; at(p.reg+0x38)=((hz%19200000)*65536+19199999)/19200000;
    }
};
static void setup(ClockHardware &h,Model &m,bool own=true) {
    assert(h.configure(m.d,m.io(),m.supplyIO()));
    for(unsigned i=0;i<m.d.rootCount;++i) { const auto &r=m.d.roots[i]; if(own) assert(h.allowRoot(r.id,1000000000));
        assert(h.addSupply({r.id,30000000,2,0x1000,128})); assert(h.addSupply({r.id,1000000000,2,0x1000,384})); }
    for(unsigned i=0;i<m.d.domainCount;++i) {const auto &d=m.d.domains[i]; if(own) assert(h.allowDomain(d.id));assert(h.addSupply({0x80000000U|d.id,0,2,0x1000,192}));}
    for(unsigned i=0;i<m.d.branchCount;++i) if(own) assert(h.allowClock(m.d.branches[i].id));
    for(unsigned i=0;i<m.d.resetCount;++i) if(own) assert(h.allowReset(m.d.resets[i].id));
    assert(h.initialize());
}
static void descriptors(const CCDescription &d) {
    std::set<uint32_t> ids,roots,domains;
    for(unsigned i=0;i<d.rootCount;++i) { const auto &r=d.roots[i]; assert(ids.insert(r.id).second);roots.insert(r.id); assert(r.reg+20<(d.gpu?0x9000U:0x40000U));
        for(unsigned j=0;j<r.count;++j) {const auto &p=r.rates[j];assert(p.hz && p.source<8 && p.divider<32 && p.n>=p.m);}
    }
    for(unsigned i=0;i<d.domainCount;++i) assert(domains.insert(d.domains[i].id).second);
    for(unsigned i=0;i<d.branchCount;++i) {const auto &b=d.branches[i]; assert(ids.insert(b.id).second); assert(b.root==UINT32_MAX || roots.count(b.root)); assert(b.domain<0 || domains.count(b.domain));}
    for(unsigned i=0;i<d.pllCount;++i) {assert(ids.insert(d.plls[i].id).second);assert(ids.insert(d.plls[i].outputID).second);}
}
int main() {
    assert(QcomMMCCData::description.rootCount==39 && QcomMMCCData::description.branchCount==91 && QcomMMCCData::description.domainCount==9);
    assert(QcomGPUCCData::description.rootCount==4 && QcomGPUCCData::description.branchCount==5 && QcomGPUCCData::description.domainCount==2);
    unsigned points=0;
    {
        Model m(QcomGPUCCData::description); ClockHardware h;setup(h,m);
        uint64_t token=0;assert(h.retainGPUState(token)==NotReady && !token && m.events.empty());
        for(unsigned i=0;i<m.d.domainCount;++i) {const auto &d=m.d.domains[i];m.at(d.reg)=0x80000000U;if(d.status)m.at(d.status)=0x80000000U;}
        for(unsigned i=0;i<m.d.branchCount;++i)m.at(m.d.branches[i].reg)=1;
        m.at(0x1074)=0x103;m.at(0x2c)=1;m.at(0xc)=7;
        assert(h.retainGPUState(token)==OK && token && m.events.empty());
        uint64_t other=0;assert(h.retainGPUState(other)==Busy && !other);
        assert(h.domain(2,true)==Busy && h.clock(7,false)==Busy && h.reset(2,true)==Busy && h.setRate(3,400000000)==Busy);
        assert(h.validateGPUState(token)==OK && h.validateGPUState(token,true)==OK && m.events.empty());
        const auto old=token;token=0;assert(h.retainGPUState(token)==OK && token>old);
        assert(h.validateGPUState(old,true)==Stale && h.validateGPUState(token)==OK);
        m.at(0x1074)^=1;assert(h.validateGPUState(token)==Stale);
        m.at(0x1074)^=1;assert(h.validateGPUState(token,true)==Stale && h.clock(7,false)==Busy && m.events.empty());
    }
    for(const auto *d:{&QcomMMCCData::description,&QcomGPUCCData::description}) {
        descriptors(*d);
        Model m(*d); ClockHardware h; setup(h,m,false); assert(m.events.empty());
        assert(h.setRate(d->roots[0].id,19200000)==Unsupported && h.clock(d->branches[0].id,true)==Unsupported);
        for(unsigned r=0;r<d->rootCount;++r) for(unsigned i=0;i<d->roots[r].count;++i) {
            const auto &root=d->roots[r];const auto &rate=root.rates[i]; if(root.parents[rate.source]==NoParent || (rate.n && !root.width)) continue;
            Model n(*d); n.parentRate(root,rate); ClockHardware c;setup(c,n);
            Status result=c.setRate(root.id,rate.hz); if(result!=OK) fprintf(stderr,"rate %s %u: status %u\n",root.name,rate.hz,result); assert(result==OK); uint64_t actual=c.rate(root.id),error=actual>rate.hz?actual-rate.hz:rate.hz-actual;
            assert(error<=rate.hz/10000+1); ++points;
            assert(!n.events.empty() && n.events[0].type==1); // supply ACK before PLL/RCG writes
        }
    }
    {
        const auto &d=QcomGPUCCData::description; Model m(d);ClockHardware h;setup(h,m);
        assert(h.rate(10)==19200000 && h.setRate(3,400000000)==OK && h.rate(3)==400000048);
        // Raw vendor wire encoding, independently of the logical-rate API:
        // 800MHz PLL, unity HID (1), fixed gfx-core /2 =>400MHz.
        assert((m.at(0x1074)&0x71f)==0x101);
        m.at(0x1074)=0x103;assert(h.rate(3)==200000024); // old extra divider really halves the core
        m.at(0x1074)=0x101;
        m.events.clear(); assert(h.setRate(3,19200000)==Invalid); // below Fabia VCO range
        assert(h.setRate(3,300000000)==OK);
        assert(h.clock(7,true)==OK && h.clock(7,true)==OK);
        assert(h.setRate(3,400000000)==Busy && h.domain(2,false)==Busy);
        assert(h.clock(7,false)==OK && h.setRate(3,400000000)==Busy);
        assert(h.clock(7,false)==OK && h.setRate(3,400000000)==OK);
        assert(h.clock(7,false)==Invalid);
        m.events.clear(); m.failVoltage=true; uint32_t before=m.at(0x1074);
        assert(h.setRate(3,500000000)==Denied && m.at(0x1074)==before && m.events.size()==1);
    }
    {
        Model m(QcomGPUCCData::description); ClockHardware h;setup(h,m);
        assert(h.setRate(2,50000000)==OK); // GPLL foreign parent through single GCC owner
        bool parent=false;for(const auto &e:m.events) parent|=e.type==2;assert(parent);
        assert(h.setRate(4,19200000)==OK);m.events.clear();assert(h.setRate(2,19200000)==OK);
        unsigned clock=0,lower=0;for(unsigned i=0;i<m.events.size();++i) { if(!m.events[i].type && m.events[i].a==0x1030)clock=i; if(m.events[i].type==1 && m.events[i].b==128)lower=i; }
        assert(lower>clock); // no rail drop before lower rate UPDATE
        assert(h.setRate(3,400000000)==OK);m.failUpdates=1;uint64_t before=h.rate(3);
        assert(h.setRate(3,500000000)==TimedOut && h.rate(3)==before); // PLL+RCG rollback, voltage retained
        m.failUpdates=2;assert(h.setRate(3,500000000)==Stale && h.rate(3)==0 && h.clock(7,true)==Stale);
    }
    {
        Model m(QcomGPUCCData::description);ClockHardware h;setup(h,m);assert(h.setRate(3,400000000)==OK);
        m.failDomain=true;assert(h.domain(1,true)==TimedOut && h.clock(7,true)==Stale);
        Model n(QcomGPUCCData::description); n.at(0x1074)=7<<8;n.at(0x1098)&=~0x80000000U;n.at(0x1098)|=1;
        ClockHardware unknown;setup(unknown,n);assert(unknown.setRate(2,50000000)==NotReady && n.events.empty()); // unknown live demand forbids voltage changes
    }
    {
        Model m(QcomGPUCCData::description);m.at(0)|=1U<<23; ClockHardware h;setup(h,m);
        assert(h.setRate(3,400000000)==OK);m.failLatch=1;
        assert(h.setRate(3,500000000)==TimedOut && h.rate(3)==400000048);
        m.holdLatch=true;assert(h.setRate(3,500000000)==Stale && h.rate(3)==0);
    }
    {
        Model m(QcomMMCCData::description);ClockHardware h;setup(h,m);
        const auto &r=m.d.resets[0];m.at(r.reg)=0x80000000;assert(h.reset(r.id,true)==OK && m.at(r.reg)==0x80000001); assert(h.reset(r.id,false)==OK && m.at(r.reg)==0x80000000);
        // Configuration rejects duplicate/non-monotonic supply data and unknown resources.
        ClockHardware bad;assert(bad.configure(m.d,m.io(),m.supplyIO())); assert(!bad.allowRoot(UINT32_MAX,1));
        assert(bad.addSupply({m.d.roots[0].id,100,2,0x1000,192}));assert(!bad.addSupply({m.d.roots[0].id,200,2,0x1000,128}));
    }
    printf("CC: %u table rates, descriptor bounds, GPU Fabia, shared supplies, gate/domain references, ownership and rollback passed\n",points);
}
