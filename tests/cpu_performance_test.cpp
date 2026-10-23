// SPDX-License-Identifier: BSD-2-Clause
#include <CPUPerformancePolicy.h>
#include <PowerUserProtocol.h>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
using namespace CPUPerformance;
static void setup(Policy &p,unsigned cores=4) {
    assert(p.configure(30000,20000,20000,100000));
    for(unsigned c=0;c<2;++c) for(unsigned mhz:{300U,600U,900U,1200U,1500U,1800U,2100U,2400U}) assert(p.addState(c,mhz*1000000));
    for(unsigned c=0;c<2;++c) for(unsigned i=0;i<cores;++i) assert(p.mapCPU(c*cores+i,c));
}
static Decision sample(Policy &p,Capture &capture,uint64_t t,uint32_t current=2400000000U,uint32_t cap=2400000000U,unsigned cores=4) {
    p.begin();for(unsigned i=0;i<cores*2;++i) {Snapshot s;auto read=capture.read(i,1,s);p.observe(i,t,read,s,true,Idle);}return p.decide(0,t,current,cap);
}
int main() {
    {
        Capture c;c.configure(20000);c.record(0,1,UI);Snapshot s;assert(c.read(0,1,s)==Empty);
        c.activate(1);c.record(0,100,NonUI);c.record(0,1100,UI);c.record(0,2100,Idle);
        assert(c.read(0,1,s)==Consistent && s.busy==2000 && s.demand==2400000 && s.boostFloor==800);
        c.record(0,2000,Realtime);Snapshot same;assert(c.read(0,1,same)==Consistent && same.stamp==2100 && same.qos==Realtime && same.boostFloor==1000 && same.busy==2000);
        c.deactivate();c.record(0,3100,NonUI);assert(c.read(0,1,same)==Consistent && same.stamp==2100);
        c.activate(2);c.record(0,4100,Background);assert(c.read(0,1,same)==Empty && c.read(0,2,same)==Consistent && same.busy==0);
        c.record(MaxCPUs,5000,UI);assert(c.read(MaxCPUs,2,same)==Empty);
    }
    {
        Policy p;setup(p);Capture c;c.configure(20000);c.activate(1);
        for(unsigned cpu=0;cpu<8;++cpu)c.record(cpu,100,Idle);
        auto d=sample(p,c,5000);assert(d.valid && d.hz==2400000000U && d.utilization==0);
        d=sample(p,c,35000);assert(d.hz==300000000);p.committed(0,35000);
        // A single busy core must boost its cluster, without averaging it to
        // 25% and leaving a serial task stuck at one quarter of maximum.
        c.record(0,35000,NonUI);c.record(0,40000,NonUI);
        d=sample(p,c,40000,300000000);assert(d.hz==2400000000U && d.utilization==250);
        c.record(0,40000,Idle);
        for(unsigned t=45000;t<=100000;t+=5000)d=sample(p,c,t);
        assert(d.hz==300000000 && d.qosFloor==0);
    }
    {
        Policy p;setup(p);Capture c;c.configure(20000);c.activate(1);
        for(unsigned cpu=0;cpu<8;++cpu)c.record(cpu,100,Idle);
        sample(p,c,1000,300000000);
        c.record(0,1200,UI);c.record(0,1300,Idle);
        auto d=sample(p,c,5000,300000000);assert(d.hz==2100000000U && d.qosFloor==800);
        auto limited=sample(p,c,10000,2100000000U,900000000);assert(limited.hz<=900000000);
        // QoS cannot override a user/thermal ceiling, even inside a boost.
        assert(limited.qosFloor==800);
        for(unsigned t=15000;t<=80000;t+=5000)d=sample(p,c,t);
        assert(d.qosFloor==0 && d.hz==300000000);
    }
    {
        Policy p;setup(p);Capture c;c.configure(20000);c.activate(1);
        for(unsigned cpu=0;cpu<8;++cpu)c.record(cpu,100,cpu==0 ? Background : Idle);
        auto d=sample(p,c,5000,300000000);assert(d.hz==2100000000U);
        d=sample(p,c,35000,2100000000U);assert(d.hz==2400000000U); // sustained background saturation is not a hard QoS cap
    }
    {
        Policy p;setup(p);p.begin();Snapshot s;
        p.observe(0,1000,Contended,s,true,Realtime);assert(!p.decide(0,1000,300000000,2400000000U).valid);
        p.reset();p.begin();p.observe(0,2000,Empty,s,true,NonUI);assert(p.decide(0,2000,300000000,2400000000U).hz==2400000000U);
        p.begin();p.observe(0,3000,Empty,s,false,Idle);assert(!p.decide(0,3000,300000000,2400000000U).valid);
        p.begin();s.stamp=9000;s.epoch=1;p.observe(0,8000,Consistent,s,true,NonUI);assert(!p.decide(0,8000,300000000,2400000000U).valid);
        assert(!p.addState(0,2400000000U) && !p.mapCPU(0,0) && !p.mapCPU(64,0));
        assert(!p.decide(0,9000,0,2400000000U).valid && !p.decide(9,9000,300000000,2400000000U).valid);
    }
    {
        // An unobserved interval cannot count toward continuous low demand.
        Policy p;setup(p);Capture c;c.configure(20000);c.activate(1);
        for(unsigned cpu=0;cpu<8;++cpu)c.record(cpu,100,Idle);
        sample(p,c,5000);
        assert(!sample(p,c,200000).valid);
        assert(sample(p,c,205000).hz==2400000000U);
        assert(sample(p,c,235000).hz==300000000);
    }
    {
        // The maximum gap also applies when the first scheduler callback
        // arrives after bootstrapping from the kernel CPU-state snapshot.
        Policy p;setup(p);Capture c;c.configure(20000);c.activate(1);
        sample(p,c,5000);
        c.record(0,199999,Idle);
        assert(!sample(p,c,200000).valid);
        assert(sample(p,c,205000).hz==2400000000U);
    }
    {
        // Neither writer contention nor an offline interval proves that a
        // background core remained saturated for the entire elapsed time.
        Policy p;setup(p);Snapshot s;
        p.begin();p.observe(0,5000,Empty,s,true,Background);
        assert(p.decide(0,5000,300000000,2400000000U).hz==2100000000U);
        p.begin();p.observe(0,10000,Contended,s,true,Background);
        assert(!p.decide(0,10000,2100000000U,2400000000U).valid);
        p.begin();p.observe(0,40000,Empty,s,true,Background);
        assert(p.decide(0,40000,2100000000U,2400000000U).hz==2100000000U);
        p.begin();p.observe(0,45000,Empty,s,false,Idle);
        assert(!p.decide(0,45000,2100000000U,2400000000U).valid);
        p.begin();p.observe(0,80000,Empty,s,true,Background);
        assert(p.decide(0,80000,2100000000U,2400000000U).hz==2100000000U);
    }
    {
        // A core returning online must not reuse its old busy/QoS record
        // indefinitely when the kernel snapshot already reports idle.
        Policy p;setup(p);Capture c;c.configure(20000);c.activate(1);
        c.record(0,1000,Realtime);Snapshot s;assert(c.read(0,1,s)==Consistent);
        p.begin();p.observe(0,5000,Consistent,s,true,Realtime);
        assert(p.decide(0,5000,300000000,2400000000U).qosFloor==1000);
        p.begin();p.observe(0,10000,Consistent,s,false,Idle);
        p.decide(0,10000,2400000000U,2400000000U);
        p.begin();p.observe(0,15000,Consistent,s,true,Idle);
        auto d=p.decide(0,15000,2400000000U,2400000000U);
        assert(d.valid && d.utilization==0 && d.demand==0 && d.qosFloor==0);
        c.record(0,16000,UI);assert(c.read(0,1,s)==Consistent);
        p.begin();p.observe(0,20000,Consistent,s,true,UI);
        assert(p.decide(0,20000,300000000,2400000000U).qosFloor==800);
    }
    {
        // Independent CPU writers and concurrent readers exercise the actual
        // capture memory ordering, not an alternate queue implementation.
        Capture c;c.configure(20000);c.activate(1);bool done=false;
        std::vector<std::thread> writers;
        for(unsigned cpu=0;cpu<4;++cpu)writers.emplace_back([&,cpu]{for(uint64_t t=1;t<=100000;++t)c.record(cpu,t,t&1?UI:Idle);});
        std::thread reader([&]{while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE)) for(unsigned cpu=0;cpu<4;++cpu){Snapshot s;if(c.read(cpu,1,s)==Consistent){assert(s.busy<=s.stamp && s.demand<=s.stamp*1600 && s.qos>=Idle && s.qos<=UserInitiated);}}});
        for(auto &w:writers)w.join();
        __atomic_store_n(&done,true,__ATOMIC_RELEASE);reader.join();
        for(unsigned cpu=0;cpu<4;++cpu){Snapshot s;assert(c.read(cpu,1,s)==Consistent && s.stamp==100000);}
    }
    {
        // Randomized event coalescing: the integrated busy time must match a
        // reference integral even when a core has several switches per tick.
        Policy p;setup(p);Capture capture;capture.configure(20000);capture.activate(1);
        uint32_t random=0x83512345,classes[8]={Idle,Idle,Idle,Idle,Idle,Idle,Idle,Idle};
        auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
        for(unsigned cpu=0;cpu<8;++cpu)capture.record(cpu,1,Idle);
        sample(p,capture,1,300000000);
        uint32_t current=300000000;
        for(unsigned step=1;step<=10000;++step) {
            uint64_t start=1+uint64_t(step-1)*5000,sum=0;
            for(unsigned cpu=0;cpu<8;++cpu) {
                uint32_t busy=0;
                for(unsigned part=0;part<5;++part) {
                    classes[cpu]=1+next()%9;capture.record(cpu,start+part*1000,classes[cpu]);
                    if(classes[cpu]!=Idle)busy+=1000;
                }
                capture.record(cpu,start+5000,classes[cpu]);
                if(cpu<4)sum+=busy*1000/5000;
            }
            uint32_t ceiling=(1+next()%8)*300000000U;
            if(current>ceiling)current=ceiling;
            auto decision=sample(p,capture,start+5000,current,ceiling);
            assert(decision.valid && decision.utilization==sum/4 && decision.hz<=ceiling && decision.hz%300000000==0);
            if(decision.hz!=current)p.committed(0,start+5000);
            current=decision.hz;
        }
    }
    {
        using namespace PowerUser;using namespace ClockController;
        assert(validate(SetAutomatic,2,1,0,1,false,OSM)==NotPrivileged);
        assert(validate(SetAutomatic,2,1,0,1,true,OSM)==Valid);
        assert(validate(SetAutomatic,2,1,1,2,true,OSM)==BadArguments);
        assert(validate(SetAutomatic,2,1,1,1,true,GCC)==UnsupportedSelector);
        assert(readable(GetPerformanceStatus) && readable(GetPerformanceUtilization) && !readable(SetAutomaticPerformance));
    }
    puts("CPU performance: busy/idle accounting, QoS burst hold, single-core peak, saturation, ceilings, hysteresis and concurrent capture passed");
}
