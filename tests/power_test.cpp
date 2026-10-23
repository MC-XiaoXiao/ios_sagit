// SPDX-License-Identifier: GPL-2.0-only
#include <RPMTransport.h>
#include <RPMRegulators.h>
#include <RPMClockHandoff.h>
#include <RPMInterconnect.h>
#include <RPMResourceHandoff.h>
#include <SPMIRegulators.h>
#include <OSMController.h>
#include <PowerUserProtocol.h>
#include "../boot/bridge/gpu_power.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
using namespace QcomPower;
struct Memory {
    std::map<uint64_t,uint32_t> words;
    unsigned writes=0, time=0;
    uint32_t &at(unsigned bank,uint32_t offset) { assert(!(offset&3)); return words[(uint64_t(bank)<<32)|offset]; }
    static uint32_t read(void *c,unsigned b,uint32_t o) { return static_cast<Memory *>(c)->at(b,o); }
    static void write(void *c,unsigned b,uint32_t o,uint32_t v) { auto &m=*static_cast<Memory *>(c); ++m.writes; m.at(b,o)=v; }
    static void wait(void *c,unsigned us) { static_cast<Memory *>(c)->time+=us; }
    static void kick(void *) {}
    IO io() { return {this,read,write,wait,kick}; }
};
struct Peer : Memory {
    static constexpr uint32_t tx=0, rx=0x808, length=2048, size=0x2000;
    bool respond=true, stale=false, denied=false, fragment=false, staleOnly=false, otherServices=false;
    unsigned requests=0,denyAt=0,dropAt=0,closeAt=0;
    unsigned openAckChannel=1,openAckWord=0;
    // External RPM retains omitted MAX fields and clamps the NPA request.
    // Start below USB's required NoC floor to exercise the real transfer.
    uint32_t clockRequested[2]{},clockMaximum[2]{9600,4800},clockEffective[2]{};
    // Independent RPM/PMIC side: omitted fields are retained, and hardware
    // SW_EN is written before PIN even when both arrive in one KVP message.
    struct ClockBuffer {
        unsigned sw=0,pin=0,hardwareSW=0,hardwarePin=0;
        bool pinLevel=true;
        std::vector<bool> samples;
        bool enabled() const {return hardwareSW || (hardwarePin && pinLevel);}
        void reset(unsigned controls,bool level) {
            sw=hardwareSW=controls&1;pin=hardwarePin=(controls>>1)&1;pinLevel=level;
            samples={enabled()};
        }
        void apply(unsigned software,unsigned pins) {
            if(hardwareSW!=software) {hardwareSW=software;samples.push_back(enabled());}
            if(hardwarePin!=pins) {hardwarePin=pins;samples.push_back(enabled());}
            sw=software;pin=pins;
        }
        void stable() const {for(bool sample:samples)assert(sample==samples.front());}
        void finish(unsigned controls) const {
            assert(sw==(controls&1) && pin==((controls>>1)&1));
            assert(hardwareSW==sw && hardwarePin==pin);
            unsigned transitions=0;for(unsigned n=1;n<samples.size();++n)transitions+=samples[n]!=samples[n-1];
            assert(transitions<=1); // No off gap, pulse, or needless temporary enable.
        }
    } buffers[6][2];
    std::vector<uint8_t> request;
    std::vector<std::vector<uint8_t>> history;
    Peer() { at(0,size-256)=0x67727430; at(0,size-252)=2;
        at(0,size-248)=0x61703272; at(0,size-244)=tx; at(0,size-240)=length;
        at(0,size-236)=0x72326170; at(0,size-232)=rx; at(0,size-228)=length;
        for(auto &buffer:buffers) {buffer[0].reset(1,true);buffer[1].reset(2,true);} }
    void stableClocks() const {for(const auto &buffer:buffers)for(const auto &set:buffer)set.stable();}
    void frame(unsigned command,unsigned channel,unsigned param,const std::vector<uint8_t> &data={}) {
        unsigned n=align8(8+data.size()), h=at(0,rx+4), t=at(0,rx);
        assert(length-((h+length-t)%length)>n+8);
        std::vector<uint8_t> bytes(n); put32(bytes.data(),command|(channel<<16)); put32(bytes.data()+4,param);
        for (unsigned i=0;i<data.size();++i) bytes[8+i]=data[i];
        for (unsigned i=0;i<n;i+=4) at(0,rx+8+(h+i)%length)=le32(bytes.data()+i);
        at(0,rx+4)=(h+n)%length;
    }
    void ack(uint32_t id) {
        std::vector<uint8_t> p(denied?32:20); put32(p.data(),0x00716572); put32(p.data()+4,p.size()-8);
        put32(p.data()+8,0x2367736d); put32(p.data()+12,4); put32(p.data()+16,id);
        if (denied) { put32(p.data()+20,0x00727265); put32(p.data()+24,4); put32(p.data()+28,1); }
        if (fragment) {
            std::vector<uint8_t> a(20),b(8+p.size()-12); put32(a.data(),12); put32(a.data()+4,p.size()-12);
            memcpy(a.data()+8,p.data(),12); put32(b.data(),p.size()-12); memcpy(b.data()+8,p.data()+12,p.size()-12);
            frame(9,7,0,a); frame(12,7,0,b);
        } else { std::vector<uint8_t> a(8+p.size()); put32(a.data(),p.size()); memcpy(a.data()+8,p.data(),p.size()); frame(9,7,0,a); }
    }
    static void notify(void *c) {
        auto &m=*static_cast<Peer *>(c);
        while (m.at(0,tx)!=m.at(0,tx+4)) {
            uint32_t t=m.at(0,tx); auto word=[&](unsigned n){return m.at(0,tx+8+(t+n)%length);};
            unsigned cmd=word(0)&65535, n=8;
            if(cmd==2) n=align8(8+word(4));
            if(cmd==9) n=align8(16+word(8));
            if (m.respond) {
                if(cmd==0) { m.frame(0,1,0); m.frame(1,1,0); }
                if(cmd==2) {
                    if (m.otherServices) {
                        const char other[]="rpm_rbcpr";
                        m.frame(2,4,sizeof(other),{other,other+sizeof(other)});
                        m.frame(3,4,0);
                    }
                    m.frame(4,m.openAckChannel,m.openAckWord); const char name[]="rpm_requests"; m.frame(2,7,13,{name,name+13});
                }
                if(cmd==9) {
                    ++m.requests; unsigned bytes=word(8); m.request.resize(bytes);
                    for(unsigned i=0;i<bytes;++i) m.request[i]=word(16+(i&~3U))>>(8*(i&3));
                    assert(le32(m.request.data())==0x00716572);
                    m.history.push_back(m.request);
                    const bool previousDenied=m.denied;
                    m.denied|=m.requests==m.denyAt;
                    if(!m.denied && le32(m.request.data()+16)==0x72676761 && le32(m.request.data()+20)==1) {
                        const auto set=le32(m.request.data()+12),kvps=le32(m.request.data()+24);assert(set<2);
                        for(unsigned at=28;at<28+kvps;at+=12) {
                            assert(at+12<=m.request.size() && le32(m.request.data()+at+4)==4);
                            const auto key=le32(m.request.data()+at),value=le32(m.request.data()+at+8);
                            if(key==0x007a484b)m.clockRequested[set]=value;
                            else if(key==0x0078616d)m.clockMaximum[set]=value;
                        }
                        m.clockEffective[set]=m.clockMaximum[set] && m.clockRequested[set]>m.clockMaximum[set] ? m.clockMaximum[set]:m.clockRequested[set];
                    }
                    if(!m.denied && le32(m.request.data()+16)==0x616b6c63) {
                        const auto id=le32(m.request.data()+20),set=le32(m.request.data()+12),kvps=le32(m.request.data()+24);
                        if(id>=1 && id<=6) {
                            assert(set<2);auto &buffer=m.buffers[id-1][set];auto sw=buffer.sw,pin=buffer.pin;
                            for(unsigned at=28;at<28+kvps;at+=12) {
                                assert(at+12<=m.request.size() && le32(m.request.data()+at+4)==4);
                                const auto key=le32(m.request.data()+at),value=le32(m.request.data()+at+8);
                                if(key==0x6e657773)sw=value;else {assert(key==0x62636370);pin=value;}
                            }
                            buffer.apply(sw,pin);
                        }
                    }
                    uint32_t id=le32(m.request.data()+8);
                    if(m.requests!=m.dropAt) {if(m.stale)m.ack(id-1);if(!m.staleOnly)m.ack(id);}
                    m.denied=previousDenied;
                    if(m.requests==m.closeAt)m.frame(3,7,0);
                }
            }
            m.at(0,tx)=(t+n)%length;
        }
    }
    IO io() { auto a=Memory::io(); a.kick=notify; return a; }
};
// Exercise the bridge's actual startup against the same RPM ACK peer. Only
// hardware completion bits are modeled; this does not validate real timing.
struct GPUClocks : Memory {
    bool stall=false;
    uint32_t crcPLL=0,crcFraction=0,crcRCGR=0;
    GPUClocks() {
        at(1,0)=0x80000001;at(1,4)=31;at(1,0xc)=1;at(1,0x38)=0x4000;at(1,0x71004)=1;
        at(0,0x14)=1;at(0,0x1004)=at(0,0x1094)=1;at(0,0x130)=17;
        for(const auto &b:QcomGPUCCData::branches)at(0,b.reg)=0x80000000;
    }
    static void write(void *c,unsigned bank,uint32_t r,uint32_t v) {
        auto &m=*static_cast<GPUClocks *>(c);Memory::write(c,bank,r,v);
        if(!bank && r==0x10a0 && v==0x00800000) {
            assert(m.at(0,0x1098)&1);m.crcPLL=m.at(0,4);m.crcFraction=m.at(0,0x38);
            m.crcRCGR=m.at(0,0x1074);
        }
        if(bank) {
            for(const auto &b:GCC::branches)if(r==b.reg && b.mode!=3) {
                if(v&b.mask)m.at(1,b.halt)&=~0x80000000u;else m.at(1,b.halt)|=0x80000000u;
            }
            return;
        }
        if(r==0 && (v&(1u<<22)))m.at(0,r)&=~((1u<<22)|(1u<<29));
        if(r==0x2c && !m.stall) {if(v)m.at(0,0)|=0x80000000u;else m.at(0,0)&=~0x80000000u;}
        for(const auto &root:QcomGPUCCData::roots)if(r==root.reg && (v&1))m.at(0,r)&=~1u;
        for(const auto &d:QcomGPUCCData::domains)if(r==d.reg) {
            auto &status=m.at(0,d.status?d.status:d.reg);
            if(v&1)status&=~0x80000000u;else status|=0x80000000u;
        }
        for(const auto &b:QcomGPUCCData::branches)if(r==b.reg) {
            if(v&b.mask)m.at(0,r)&=~0x80000000u;else m.at(0,r)|=0x80000000u;
        }
    }
    static uint32_t gccRead(void *c,uint32_t r) {return read(c,1,r);}
    static void gccWrite(void *c,uint32_t r,uint32_t v) {write(c,1,r,v);}
    IO io() {auto a=Memory::io();a.write=write;return a;}
    GCC::RegisterIO gccIO() {return {this,gccRead,gccWrite,Memory::wait};}
};
struct PMIC : Memory {
    std::map<unsigned,uint8_t> pmic;
    unsigned commands=0, denyAfter=0; bool hang=false, ready=true;
    PMIC() { at(0,0)=0x30000000; at(0,0x800)=0x11400; at(4,0x700)=0;
        pmic[4]=0x1c; pmic[5]=0xa; pmic[0x40]=0x20; pmic[0x41]=3; pmic[0x46]=128; pmic[8]=128; }
    static void write(void *c,unsigned b,uint32_t o,uint32_t v) {
        auto &m=*static_cast<PMIC *>(c); Memory::write(c,b,o,v);
        if ((b!=1 && b!=2) || (o&0x7fff)!=0) return;
        ++m.commands; unsigned offset=(v>>4)&255, n=(v&7)+1; m.at(b,o+8)=0;
        if(m.hang) return;
        if(m.denyAfter && m.commands>=m.denyAfter) { m.at(b,o+8)=5; return; }
        if(b==1) for(unsigned i=0;i<n;++i) m.pmic[offset+i]=m.at(b,o+0x10+(i&~3U))>>(8*(i&3));
        else for(unsigned i=0;i<n;i+=4) { uint32_t data=0; for(unsigned j=0;j<4 && i+j<n;++j) data|=uint32_t(offset+i+j==8 ? (m.ready?128:0) : m.pmic[offset+i+j])<<(8*j); m.at(b,o+0x18+i)=data; }
        m.at(b,o+8)=1;
    }
    IO io() { auto a=Memory::io(); a.write=write; return a; }
};
static void osmTable(Memory &m) {
    for(unsigned c=0;c<2;++c) { unsigned b=c*0x2000; m.at(0,b+0x1004)=1;
        for(unsigned i=0;i<40;++i) { unsigned row=b+0x1150+32*i; m.at(0,row)=i;
            // Last two rows are single-core boost, never eligible.
            m.at(0,row+4)=(1U<<26)|((i>=38?1:4)<<16)|(i<2?20+10*i:40);
            m.at(0,row+8)=i<2?700+100*i:900; }
    }
}
struct OSMFaultMemory : Memory {
    enum Failure { DropRequest, ChangeRollbackRow, DisableOSM, DropRollback,
        AcceptedDisable, AcceptedIndex, AcceptedFrequency, AcceptedVoltage,
        AcceptedOverride, AcceptedSpare } failure=DropRequest;
    std::vector<uint32_t> desiredWrites;
    static void write(void *context,unsigned bank,uint32_t offset,uint32_t value) {
        auto &m=*static_cast<OSMFaultMemory *>(context);++m.writes;
        if (offset!=0x1f10) {m.at(bank,offset)=value;return;}
        m.desiredWrites.push_back(value);
        if (m.desiredWrites.size()==1) {
            if (m.failure >= AcceptedDisable) {
                m.at(bank,offset)=value;
                const uint32_t target=0x1150+32*value;
                if (m.failure==AcceptedDisable) m.at(0,0x1004)=0;
                if (m.failure==AcceptedIndex) m.at(0,target)^=1;
                if (m.failure==AcceptedFrequency) m.at(0,target+4)^=1;
                if (m.failure==AcceptedVoltage) m.at(0,target+8)^=1;
                if (m.failure==AcceptedOverride) m.at(0,target+12)^=1;
                if (m.failure==AcceptedSpare) m.at(0,target+20)^=1;
                return;
            }
            // Fail the target readback. Some failures invalidate the old
            // state while the request is pending.
            if (m.failure==ChangeRollbackRow) m.at(0,0x1158)=1100;
            if (m.failure==DisableOSM) m.at(0,0x1004)=0;
            if (m.failure==DropRollback) m.at(0,0x1f10)=2;
            return;
        }
        if (m.failure!=DropRollback) m.at(bank,offset)=value;
    }
    IO io() {auto access=Memory::io();access.write=write;return access;}
};
static void resourceHandoffModels() {
    struct Policy {
        RPMRegulators rails;
        RPMClockHandoff clocks;
        RPMInterconnect bus;
        enum : unsigned {Count=8+8+2*RPMHandoffClockCount+2*RPMBusResourceCount+2};
        static_assert(Count==88,"complete AP resource ACK sequence includes PIMEM");
        Policy(bool domains=true,bool clockPolicy=true,uint32_t qdssActive=4,uint32_t qdssSleep=3,bool limitPolicy=true,
            unsigned activeControls=2,unsigned sleepControls=1,uint32_t cxFloor=192) {
            const uint32_t ids[]={257,258,268,280},uv[]={880000,1200000,1800000,3088000};
            for(unsigned n=0;n<4;++n) {
                for(const auto &r:rpmRails)if(r.id==ids[n])assert(rails.configure(r.id,r.minimum,r.maximum,uv[n]));
            }
            // A zero floor is deliberately not converted into an off write.
            assert(rails.configure(rpmRails[0].id,rpmRails[0].minimum,rpmRails[0].maximum,0));
            if(domains)for(unsigned i=0;i<4;++i)assert(rails.configureDomain(0x1000+i,i ? 192:cxFloor,64));
            if(clockPolicy)for(unsigned i=0;i<RPMHandoffClockCount;++i) {
                const auto &c=rpmHandoffClocks[i];
                const bool paired=c.type==0x616b6c63 && c.resource>=1 && c.resource<=6;
                const unsigned bit=c.key==0x6e657773 ? 0u:1u;
                assert(clocks.configure(i,paired ? (activeControls>>bit)&1u:
                    (i==19 ? qdssActive:(i==6 ? 1u:(i==26 ? 192000u:((i==0 || i==2 || i==3 || i==7) ? 19200u:0u)))),
                    paired ? (sleepControls>>bit)&1u:(i==19 ? qdssSleep:(i==26 ? 9600u:0u))));
            }
            if(clockPolicy && limitPolicy)for(unsigned i=0;i<RPMHandoffClockCount;++i)if(rpmClockHasLimit(i))
                assert(clocks.configureLimit(i,i==6 ? 1u:(i==19 ? 4u:1000000u),i==6 ? 1u:(i==19 ? 4u:2000000u)));
            for(const auto &r:rpmBusResources)
                assert(bus.configure(r.id,r.clock ? 1000000000ULL:10000000000ULL,r.clock ? 19200000:100,0));
        }
        Status initialize(RPMTransport &transport,bool withBus=true) {
            return initializeRPMResources(transport,rails,clocks,bus,withBus);
        }
        bool ready() const {return rails.railFloorsReady() && rails.domainsReady() && clocks.ready() && bus.handoffReady();}
    };
    auto scaling=[](const Peer &p) {
        unsigned n=0;
        for(const auto &message:p.history)
            if(le32(message.data()+16)==0x306b6c63 && le32(message.data()+20)==2)++n;
        return n;
    };
    const uint32_t usbRates[]={60000000,120000000,150000000},usbLevels[]={64,128,256};
    for(unsigned rate=0;rate<3;++rate)for(uint32_t floor:{16u,32u,48u,64u,128u,192u,256u,320u,384u,512u}) {
        Peer p;RPMTransport t;Policy state(true,true,4,3,true,2,1,floor);
        assert(t.initialize(p.io(),Peer::size)==OK);const auto writes=p.writes;
        const auto status=prepareUSBRPMResources(t,state.rails,state.clocks,state.bus,usbRates[rate]);
        if(floor<usbLevels[rate])assert(status==Unsupported && p.writes==writes && !p.requests && !state.ready());
        else assert(status==OK && p.requests==Policy::Count && state.ready());
    }
    {
        Peer p;RPMTransport t;Policy state(true,true,4,3,true,2,1,64);uint32_t floor=0,observed=0;
        assert(t.initialize(p.io(),Peer::size)==OK);const auto writes=p.writes;
        assert(state.rails.minimumDomainLevel(0x1000,0,floor)==OK && floor==64);
        assert(state.rails.minimumDomainLevel(0x1000,1,floor)==OK && floor==64);
        assert(state.rails.minimumDomainLevel(0x1004,0,floor)==Invalid && state.rails.minimumDomainLevel(0x1000,2,floor)==Invalid);
        // Another owner's temporary ACKed NOM vote must not qualify a
        // 150 MHz USB lease backed by only a LOW_SVS persistent floor.
        assert(state.rails.set(0x1000,0,5,256,true,[](auto...){return OK;})==OK);
        assert(state.rails.get(0x1000,0,observed)==OK && observed==256);
        assert(prepareUSBRPMResources(t,state.rails,state.clocks,state.bus,150000000)==Unsupported && p.writes==writes && !p.requests);
        for(uint32_t invalid:{0u,1u,19200000u,UINT32_MAX})
            assert(prepareUSBRPMResources(t,state.rails,state.clocks,state.bus,invalid)==Invalid && p.writes==writes);
        Policy missing(false);
        assert(prepareUSBRPMResources(t,missing.rails,missing.clocks,missing.bus,60000000)==Unsupported && p.writes==writes);
    }
    const uint64_t inheritedRates[]={19200000,60000000,120000000,150000000};
    for(auto inherited:inheritedRates)for(unsigned rate=0;rate<3;++rate)for(bool withBus:{false,true})for(bool enough:{false,true}) {
        const unsigned required=inherited>133330000 || usbRates[rate]>133330000 ? 256u:
            inherited>66670000 || usbRates[rate]>66670000 ? 128u:64u;
        const unsigned floor=enough ? required:required==256 ? 128u:required==128 ? 64u:48u;
        Peer p;RPMTransport t;Policy state(true,true,4,3,true,2,1,floor);
        assert(t.initialize(p.io(),Peer::size)==OK);const auto writes=p.writes;unsigned preflights=0;
        const auto status=initializeRPMResources(t,state.rails,state.clocks,state.bus,withBus,[&]{
            ++preflights;return validateUSBClockBaseline(state.rails,usbRates[rate],inherited);
        });
        assert(preflights==1);
        if(enough)assert(status==OK && p.requests==(withBus ? unsigned(Policy::Count):16u));
        else assert(status==Unsupported && p.writes==writes && !p.requests);
    }
    for(bool withBus:{false,true})for(auto failure:{NotReady,Unsupported,TimedOut,Failed}) {
        Peer p;RPMTransport t;Policy state;assert(t.initialize(p.io(),Peer::size)==OK);const auto writes=p.writes;
        assert(initializeRPMResources(t,state.rails,state.clocks,state.bus,withBus,[&]{return failure;})==failure &&
            p.writes==writes && !p.requests);
    }
    Peer good;RPMTransport transport;Policy policy;
    assert(transport.initialize(good.io(),Peer::size)==OK && policy.initialize(transport)==OK && policy.ready());
    assert(good.requests==Policy::Count && scaling(good)==2);
    good.stableClocks();for(const auto &buffer:good.buffers){buffer[0].finish(2);buffer[1].finish(1);}
    // Confirm that the external model detects the unsafe old order and the
    // tempting combined-KVP substitute before testing the real transport.
    Peer::ClockBuffer wrong;wrong.reset(1,true);wrong.apply(0,1);
    assert(wrong.samples.size()==3 && wrong.samples.front() && !wrong.samples[1] && wrong.samples.back());
    for(unsigned initial=0;initial<4;++initial)for(unsigned target=0;target<4;++target)for(bool level:{false,true}) {
        Peer p;RPMTransport t;Policy state(true,true,4,3,true,target,target^3u);
        for(auto &buffer:p.buffers){buffer[0].reset(initial,level);buffer[1].reset(initial^3u,level);}
        assert(t.initialize(p.io(),Peer::size)==OK && state.initialize(t)==OK && state.ready());
        assert(p.requests==Policy::Count && scaling(p)==2);
        for(const auto &buffer:p.buffers){buffer[0].finish(target);buffer[1].finish(target^3u);}
    }
    assert(good.clockMaximum[0]==1000000 && good.clockMaximum[1]==2000000 && good.clockEffective[0]==19200);
    for(unsigned i=0;i<RPMHandoffClockCount;++i)for(unsigned set=0;set<2;++set) {
        const auto &message=good.history[16+2*i+set];
        assert(le32(message.data()+24)==(rpmClockHasLimit(i) ? 24u:12u));
        if(rpmClockHasLimit(i))assert(le32(message.data()+40)==0x0078616d && le32(message.data()+44)==4 &&
            le32(message.data()+48)==(i==6 ? 1u:(i==19 ? 4u:(set ? 2000000u:1000000u))));
    }
    const uint32_t resources[]={1,2,12,24},uv[]={880000,1200000,1800000,3088000};
    for(unsigned n=0;n<8;++n) {
        const auto &message=good.history[n];
        assert(le32(message.data()+12)==n%2 && le32(message.data()+16)==0x616f646c &&
            le32(message.data()+20)==resources[n/2] && le32(message.data()+24)==24 &&
            le32(message.data()+28)==0x6e657773 && le32(message.data()+36)==1 &&
            le32(message.data()+40)==0x7675 && le32(message.data()+48)==uv[n/2]);
    }
    for(unsigned n=8;n<16;++n)assert(le32(good.history[n].data()+40)==0x6c766c76);
    for(unsigned set=0;set<2;++set) {
        const auto &message=good.history[16+2*26+set];
        assert(le32(message.data()+12)==set && le32(message.data()+16)==0x6d656d70 &&
            !le32(message.data()+20) && le32(message.data()+28)==0x007a484b &&
            le32(message.data()+36)==(set ? 9600u:192000u));
    }
    for(unsigned active=0;active<=4;++active)for(unsigned sleep=0;sleep<=4;++sleep) {
        Peer p;RPMTransport t;Policy state(true,true,active,sleep);
        assert(t.initialize(p.io(),Peer::size)==OK && state.initialize(t)==OK && state.ready());
        assert(p.requests==Policy::Count && scaling(p)==2);
        for(unsigned set=0;set<2;++set) {
            const auto &message=p.history[16+2*19+set];
            assert(le32(message.data()+12)==set && le32(message.data()+16)==0x306b6c63 &&
                le32(message.data()+20)==1 && le32(message.data()+28)==0x54415453 &&
                le32(message.data()+36)==(set ? sleep:active));
        }
    }
    for(uint32_t invalid:{5u,uint32_t(INT32_MAX),UINT32_MAX})for(unsigned set=0;set<2;++set) {
        RPMClockHandoff clocks;unsigned sends=0;uint32_t value=0;
        assert(!clocks.configure(19,set ? 0u:invalid,set ? invalid:0u));
        assert(clocks.floor(0x306b6c63,1,0x54415453,set,value)==Unsupported && !clocks.configured());
        assert(clocks.activate([&](auto,auto,auto,const auto *,auto) {++sends;return OK;})==Unsupported && !sends);
        assert(clocks.configure(19,4,3) && clocks.floor(0x306b6c63,1,0x54415453,0,value)==OK && value==4);
        assert(clocks.floor(0x306b6c63,1,0x54415453,1,value)==OK && value==3);
    }
    assert(policy.initialize(transport)==OK && good.requests==Policy::Count);
    uint32_t observed=0;
    assert(policy.rails.get(rpmRails[0].id,0,observed)==NotReady);
    assert(!policy.rails.configure(rpmRails[1].id,rpmRails[1].minimum,rpmRails[1].maximum,0));
    auto send=[&](auto type,auto id,auto set,const auto *data,auto bytes) {return transport.write(type,id,set,data,bytes);};
    assert(policy.rails.set(257,0,7,0,false,send)==OK && good.requests==Policy::Count);
    assert(policy.rails.get(257,0,observed)==OK && observed==880000);
    assert(policy.rails.set(257,0,7,960000,false,send)==OK && good.requests==Policy::Count+1);
    assert(policy.initialize(transport)==OK && good.requests==Policy::Count+1);
    assert(policy.rails.set(257,0,7,0,false,send)==OK && policy.rails.get(257,0,observed)==OK && observed==880000);
    for(unsigned n=1;n<=Policy::Count;++n) {
        Peer p;p.denyAt=n;RPMTransport t;Policy state;assert(t.initialize(p.io(),Peer::size)==OK);
        assert(state.initialize(t)==Denied && !state.ready() && p.requests==n);
        p.stableClocks();
        if(n<Policy::Count-1)assert(!scaling(p));
        p.denyAt=0;assert(state.initialize(t)==OK && state.ready() && p.requests==Policy::Count+1);
        p.stableClocks();for(const auto &buffer:p.buffers){buffer[0].finish(2);buffer[1].finish(1);}
    }
    for(unsigned n=1;n<=Policy::Count;++n) {
        Peer p;p.dropAt=n;RPMTransport t;Policy state;assert(t.initialize(p.io(),Peer::size)==OK);
        assert(state.initialize(t)==TimedOut && !state.ready() && p.requests==n);
        p.stableClocks();
        if(n<Policy::Count-1)assert(!scaling(p));
        const auto sent=p.requests;assert(state.initialize(t)!=OK && p.requests==sent);
    }
    for(unsigned missing=0;missing<3;++missing) {
        Peer p;RPMTransport t;Policy incomplete(missing!=0,missing!=1,4,3,missing!=2);
        assert(t.initialize(p.io(),Peer::size)==OK);const auto writes=p.writes;
        assert(incomplete.initialize(t)==Unsupported && p.writes==writes && !p.requests);
    }
    const unsigned limitIDs[]={0,1,2,3,4,5,6,7,8,12,19,26};
    static_assert(sizeof(limitIDs)/sizeof(limitIDs[0])==RPMHandoffClockLimitCount,"complete clock cap test catalogue");
    for(auto omitted:limitIDs) {
        Peer p;RPMTransport t;Policy state(true,true,4,3,false);
        for(auto i:limitIDs)if(i!=omitted)assert(state.clocks.configureLimit(i,i==6 ? 1u:(i==19 ? 4u:1000000u),i==6 ? 1u:(i==19 ? 4u:2000000u)));
        assert(t.initialize(p.io(),Peer::size)==OK);const auto writes=p.writes;
        assert(state.initialize(t)==Unsupported && p.writes==writes && !p.requests);
    }
    for(uint32_t cap:{0u,1000000u,UINT32_MAX}) {
        Peer p;RPMTransport t;Policy state(true,true,4,3,false);
        for(auto i:limitIDs)assert(state.clocks.configureLimit(i,i==0 ? cap:(i==6 ? 1u:(i==19 ? 4u:1000000u)),i==6 ? 1u:(i==19 ? 4u:2000000u)));
        assert(t.initialize(p.io(),Peer::size)==OK && state.initialize(t)==OK);
        assert(p.clockMaximum[0]==cap && p.clockEffective[0]==19200);
    }
    for(unsigned set=0;set<2;++set) {
        Peer p;RPMTransport t;Policy state(true,true,4,3,false);
        for(auto i:limitIDs)assert(state.clocks.configureLimit(i,i==6 ? 1u:(i==19 ? 4u:((i==0 && !set) ? 500000u:1000000u)),
            i==6 ? 1u:(i==19 ? 4u:((i==0 && set) ? 500000u:2000000u))));
        assert(t.initialize(p.io(),Peer::size)==OK);const auto writes=p.writes;
        assert(state.initialize(t)==Invalid && p.writes==writes && !p.requests);
    }
    {
        RPMClockHandoff before,after;uint32_t value=0;
        assert(!before.configureLimit(9,0,0) && !before.configureLimit(RPMHandoffClockCount,0,0));
        assert(!before.configureLimit(6,2,0) && !before.configureLimit(19,0,5));
        assert(before.configureLimit(0,10000,0) && !before.configure(0,19200,0));
        assert(before.configure(0,10000,0) && !before.configureLimit(0,0,0));
        assert(after.configure(0,19200,19200) && !after.configureLimit(0,9600,0) && !after.configureLimit(0,0,9600));
        assert(after.configureLimit(0,0,UINT32_MAX) && after.limit(0x72676761,1,0,value)==OK && !value);
        assert(after.limit(0x72676761,1,1,value)==OK && value==UINT32_MAX && after.limit(0x72676761,1,2,value)==Invalid);
    }
    {
        Peer p;RPMTransport t;Policy state;assert(t.initialize(p.io(),Peer::size)==OK);
        const auto writes=p.writes;
        assert(state.rails.validate(257,0,7,880001,false)==OK && p.writes==writes);
        assert(state.rails.validate(257,0,7,1300001,false)==OK && p.writes==writes);
        assert(state.rails.validate(257,0,7,1328001,false)==Invalid && p.writes==writes);
        assert(state.rails.validate(0,0,7,1,false)==Unsupported && p.writes==writes);
        assert(state.rails.validate(257,2,7,880000,false)==Invalid && p.writes==writes);
        assert(state.rails.validate(0x1000,0,7,193,true)==Invalid && p.writes==writes);
        assert(state.initialize(t,false)==OK && p.requests==16 && !state.clocks.ready());
        assert(state.initialize(t)==OK && p.requests==Policy::Count && state.ready());
        p.frame(3,7,0);const auto sent=p.requests;
        assert(state.initialize(t)!=OK && p.requests==sent); // cached/no-send success revoked
    }
    {
        Peer p;p.closeAt=Policy::Count;RPMTransport t;Policy state;
        assert(t.initialize(p.io(),Peer::size)==OK && state.initialize(t)!=OK && p.requests==Policy::Count);
    }
    {
        Peer p;p.respond=false;RPMTransport t;Policy state;
        assert(t.initialize(p.io(),Peer::size)==OK && state.initialize(t)==TimedOut && !p.requests);
    }
    puts("RPM handoff: real ACK peer,6 PMIC SW/PIN pairs across32 control/pin cases with no extra transitions, explicit PIMEM floors and12 active/sleep MAX limits, inherited NoC clamp replaced, all25 QDSS STATE pairs, rail/domain/clock/bus/scaling order, all88 error/lost-ACK steps preserve clock control, resumable denials and cached/last-ACK close revocation passed");
}
int main() {
    resourceHandoffModels();
    {
        struct ColdPeer:Peer {
            ColdPeer() {for(unsigned i=0;i<8;++i)at(0,0x6f00+4*i)=at(0,Peer::size-256+4*i);}
        };
        auto frame=[](uint32_t next) {
            RPMTransportCold h;h.flags=h.Required;h.ramBytes=0x7000;h.txOffset=Peer::tx;h.txLength=Peer::length;
            h.rxOffset=Peer::rx;h.rxLength=Peer::length;h.nextMessageID=next;return h;
        };
        uint8_t kvp[12]{};put32(kvp+4,4);put32(kvp+8,192);
        for(unsigned at:{Peer::tx,Peer::tx+4,Peer::rx,Peer::rx+4}) {
            Peer p;p.at(0,at)=8;RPMTransport legacy;assert(legacy.initialize(p.io(),Peer::size)==Stale && !p.writes);
        }
        for(auto next:{1u,1001u,UINT32_MAX}) {
            ColdPeer p;RPMTransport t;const auto h=frame(next);
            p.frame(0,1,0); // a fresh remote negotiation must survive adoption
            assert(t.initializeCold(p.io(),0x7000,h)==OK && t.connect(1000)==OK);
            assert(t.write(1,1,0,kvp,12,1000)==OK && le32(p.request.data()+8)==next);
            assert(t.write(1,1,0,kvp,12,1000)==OK && le32(p.request.data()+8)==(next==UINT32_MAX ? 1u:next+1));
            // The bridge can consume the cold fence, then yield its one live
            // channel to Native. Adoption must not negotiate a second one.
            RPMTransportHandoff live{};assert(t.handoff(live)==OK && !t.ready());
            const auto before=p.writes;RPMTransport native;
            assert(native.adopt(p.io(),0x7000,live)==OK && p.writes==before);
            assert(t.write(1,1,0,kvp,12,1000)==NotReady);
            assert(native.write(1,1,0,kvp,12,1000)==OK && p.requests==3 &&
                le32(p.request.data()+8)==live.messageID+1);
        }
        for(unsigned f=0;f<12;++f) {
            ColdPeer p;RPMTransport t;auto h=frame(7);
            switch(f) {
            case 0:h.flags=0;break;
            case 1:h.nextMessageID=0;break;
            case 2:h.txLength+=8;break;
            case 3:p.at(0,Peer::tx)=8;break;
            case 4:p.at(0,Peer::tx+4)=8;break;
            case 5:p.at(0,Peer::rx)=8;break;
            case 6:p.frame(9,7,0);break;
            case 7:p.frame(11,1,0);break;
            case 8:h.txOffset=UINT32_MAX-7;break;
            case 9:h.rxLength=0xfffffff8;break;
            case 10:h.rxOffset=h.txOffset;break;
            case 11:h.reserved=1;break;
            }
            assert(t.initializeCold(p.io(),0x7000,h)!=OK && !p.writes);
        }
        ColdPeer p;RPMTransport t;assert(t.initializeCold(p.io(),0x7000,frame(81))==OK && t.connect(1000)==OK);
        p.stale=p.staleOnly=true;assert(t.write(1,1,0,kvp,12,200)==TimedOut && !t.ready());
    }
    {
        using namespace ClockController;
        static_assert(sizeof(Request)==32 && sizeof(TargetedRequest)==48,"wire layout");
        auto r=targeted(RPM,SetVoltage,1,800000,4); assert(valid(r.request));
        r.flags=2; assert(!valid(r.request)); r.flags=0; r.consumer=16; assert(!valid(r.request));
        Request old{1,sizeof(Request),SetRate,2,0,nullptr}; assert(valid(old)); old.operation=SetVoltage; assert(!valid(old));
        auto gpu=targeted(GPUCC,RetainGPUState,0,0,GPUConsumer);assert(valid(gpu.request));
        gpu.consumer=0;assert(!valid(gpu.request));gpu.consumer=GPUConsumer;
        gpu.flags=1;assert(!valid(gpu.request));gpu.flags=0;
        gpu.controller=MMCC;assert(!valid(gpu.request));
        assert(!PowerUser::readable(RetainGPUState) && !PowerUser::readable(ReleaseGPUState));
        for(unsigned i=0;i<=GetPerformanceStateVoltage;++i) if(PowerUser::readable(i)) assert(i!=SetRate && i!=SetVoltage && i!=SetDomainLevel && i!=AssertReset);
    }
    {
        using namespace PowerUser; using namespace ClockController;
        assert(validate(GetInfo,0,3,0,0,false,OSM)==Valid);
        assert(validate(GetInfo,0,2,0,0,false,OSM)==BadArguments);
        for(unsigned op=0;op<=GetPerformanceStateVoltage;++op) {
            assert((validate(ReadResource,2,1,op,0,false,OSM)==Valid)==readable(op));
        }
        assert(validate(ReadResource,2,1,GetVoltage,UINT64_MAX,true,SPMI)==BadArguments);
        assert(validate(ReadResource,2,1,UINT64_MAX,0,true,RPM)==BadArguments);
        for (unsigned op:{GetBandwidth,SetBandwidth,GetMaximumBandwidth,GetMinimumBandwidth})
            assert(validate(ReadResource,2,1,op,0,true,RPM)==BadArguments);
        assert(validate(SetLimit,2,1,0,0,false,OSM)==NotPrivileged);
        assert(validate(SetLimit,2,1,0,0,true,OSM)==Valid);
        assert(validate(SetLimit,2,1,2,0,true,OSM)==BadArguments);
        assert(validate(SetCPUFrequency,2,1,0,100,true,GPUCC)==UnsupportedSelector);
        assert(validate(SetCPUFrequency,2,1,0,0,true,OSM)==BadArguments);
        assert(validate(SetCPUFrequency,2,1,1,768000000,true,OSM)==Valid);
        assert(validate(SelectorCount,2,1,0,0,true,OSM)==UnsupportedSelector);
    }
    {
        VoltageVotes v; unsigned sends=0; uint32_t applied=0;
        auto send=[&](unsigned s,uint32_t n){assert(s<2);++sends;applied=n;return OK;};
        VoltageVotes held;uint32_t current=0;
        assert(held.retainMinimum(0)==NotReady);
        assert(held.vote(0,1,192,send)==OK && held.retainMinimum(0)==OK && held.retainMinimum(0)==Busy);
        assert(held.vote(0,1,0,send)==OK && held.get(0,current)==OK && current==192);
        assert(held.vote(0,2,256,send)==OK && held.vote(0,2,0,send)==OK && applied==192);
        assert(held.releaseMinimum(0)==OK && held.get(0,current)==OK && current==192);
        assert(held.vote(0,1,0,send)==OK && applied==0);sends=0;
        uint32_t value=0; assert(v.get(0,value)==NotReady);
        assert(v.vote(0,1,800000,send)==OK && applied==800000);
        assert(v.vote(0,2,700000,send)==OK && sends==1);
        assert(v.vote(0,1,0,send)==OK && applied==700000);
        assert(v.vote(1,2,600000,send)==OK && v.get(0,value)==OK && value==700000);
        assert(v.vote(0,2,0,[](unsigned,uint32_t){return Denied;})==Denied);
        assert(v.get(0,value)==OK && value==700000);
        assert(v.vote(0,2,0,[](unsigned,uint32_t){return TimedOut;})==TimedOut);
        assert(v.vote(0,3,900000,send)==Stale && v.get(0,value)==Stale);
    }
    for(bool fragment:{false,true}) {
        Peer p; p.fragment=fragment; p.stale=true; p.otherServices=true; p.openAckWord=12; RPMTransport t;
        assert(t.initialize(p.io(),Peer::size)==OK && t.connect(2000)==OK);
        uint32_t openAckWord=0; assert(t.observedOpenAck(openAckWord) && openAckWord==12);
        uint8_t kvp[12]={}; put32(kvp,0x6c766c76);put32(kvp+4,4);put32(kvp+8,192);
        for(unsigned i=0;i<80;++i) assert(t.write(0x78637772,0,0,kvp,sizeof(kvp),1000)==OK);
        assert(p.requests==80 && le32(p.request.data()+16)==0x78637772);
        uint8_t bandwidth[16]{}; put32(bandwidth,0x7762); put32(bandwidth+4,8);
        put32(bandwidth+8,0xabcdef01); put32(bandwidth+12,0x12345678);
        assert(t.write(0x766c7362,142,0,bandwidth,sizeof(bandwidth),1000)==OK);
        assert(le32(p.request.data()+24)==16 && le32(p.request.data()+32)==8 &&
            le32(p.request.data()+36)==0xabcdef01 && le32(p.request.data()+40)==0x12345678);
        uint8_t mixed[216]{};
        for (unsigned pos=0;pos<sizeof(mixed);) {
            const unsigned length=pos<168 ? 4 : 8;
            put32(mixed+pos,0x7762+pos); put32(mixed+pos+4,length);
            put32(mixed+pos+8,0x12345678);
            if (length==8) put32(mixed+pos+12,0xabcdef01);
            pos+=8+length;
        }
        assert(t.write(0x766c7362,142,0,mixed,sizeof(mixed),1000)==OK);
        assert(p.request.size()==244 && !memcmp(p.request.data()+28,mixed,sizeof(mixed)));
        const auto before=p.requests;
        assert(t.write(1,1,0,mixed,217,100)==Invalid && p.requests==before);
        for (unsigned length:{0u,1u,5u,12u,UINT32_MAX}) {
            put32(bandwidth+4,length);
            assert(t.write(1,1,0,bandwidth,sizeof(bandwidth),100)==Invalid && p.requests==before);
        }
        put32(bandwidth+4,8);
        assert(t.write(1,1,0,bandwidth,15,100)==Invalid && p.requests==before);
        assert(t.check()==OK);
        p.denied=true; assert(t.write(1,1,1,kvp,12,1000)==Denied);
        p.respond=false; assert(t.write(1,1,0,kvp,12,1000)==TimedOut);
        p.respond=true; assert(t.write(1,1,0,kvp,12)==NotReady);
    }
    {
        Peer p;RPMTransport boot,native;RPMTransportHandoff h{};
        uint8_t kvp[12]{};put32(kvp+4,4);put32(kvp+8,128);
        assert(boot.initialize(p.io(),Peer::size)==OK && boot.connect(1000)==OK);
        assert(boot.handoff(h)==NotReady && !h.valid()); // no acknowledged request yet
        assert(boot.write(1,1,0,kvp,12,1000)==OK && boot.handoff(h)==OK && h.valid());
        const unsigned writes=p.writes;
        assert(!boot.ready() && boot.connect(100)==NotReady && boot.write(1,1,0,kvp,12)==NotReady);
        auto bad=h;bad.reserved[0]=1;RPMTransport malformed;
        assert(malformed.adopt(p.io(),Peer::size,bad)==Invalid && p.writes==writes);
        bad=h;bad.txHead=(h.txHead+8)%h.txLength;RPMTransport moved;
        assert(moved.adopt(p.io(),Peer::size,bad)==Stale && p.writes==writes);
        assert(native.adopt(p.io(),Peer::size,h)==OK && p.writes==writes); // no FIFO reset/renegotiation
        p.stale=true;assert(native.write(1,1,0,kvp,12,1000)==OK);
        assert(le32(p.request.data()+8)==h.messageID+1); // stale bridge ACK cannot satisfy it
        RPMTransportHandoff next{};assert(native.handoff(next)==OK);
        p.frame(1,1,0);RPMTransport reset;
        assert(reset.adopt(p.io(),Peer::size,next)==Stale && !reset.ready());
    }
    for(unsigned mode=0;mode<3;++mode) {
        Peer peer;RPMTransport t;RPMRegulators rails;GPUClocks m;GCC::Controller g;ClockHardware cc;
        BridgeGPU::PowerStartup boot(g,cc,rails,t);
        const uint32_t hz=mode==1?710000097:257000000,corner=BridgeGPU::requiredGPUCorner(hz);
        if(mode==1){m.at(0,0x1004)=0;m.at(0,0x1008)=0x80000000;} // retained CX, collapsed GX
        assert(g.setIO(m.gccIO()));for(auto id:{75,76,77,78,176,185,186})assert(g.allowClock(id));assert(g.initialize());
        assert(boot.probe(m.io(),{1,hz,800000,0},corner,corner) && !m.writes);
        assert(!boot.start() && !m.writes); // no unacknowledged supply can enable clocks
        assert(t.initialize(peer.io(),Peer::size)==OK && t.connect(1000)==OK);
        for(unsigned i=0;i<4;++i)assert(rails.configureDomain(0x1000+i,corner,0));
        assert(rails.activateDomains([&](auto type,auto id,auto set,const auto *p,auto n){return t.write(type,id,set,p,n,1000);})==OK);
        m.stall=mode==2;
        if(m.stall){assert(!boot.start() && m.writes);continue;}
        assert(boot.start() && m.at(0,8)==63 && cc.rate(7)>=hz && cc.rate(7)-hz<=hz/10000+1);
        assert(m.crcPLL==26 && m.crcFraction==0xc556); //514MHz PLL, CRC at257MHz
        assert((m.crcRCGR&0x71f)==0x101 && (m.at(0,0x1074)&0x71f)==0x101);
        uint64_t a=0,b=0;assert(g.retainGPUState(a)==GCC::Success && cc.retainGPUState(b)==OK);
        struct Blocks {
            std::vector<uint32_t> writes;
            uint32_t read(uint32_t r){return (r==0xa88d*4 || r==0xa88b*4)?1u<<20:0;}
            void write(uint32_t r,uint32_t v){assert(v==0x778000);writes.push_back(r);}
            void delay(unsigned){}
        } blocks;
        assert(boot.enableBlocks(blocks) && blocks.writes==std::vector<uint32_t>({0xa887*4,0xa881*4}));
        assert(!BridgeGPU::requiredGPUCorner(300000000));
    }
    {
        Peer p; p.at(0,Peer::size-240)=Peer::size-256; RPMTransport t; assert(t.initialize(p.io(),Peer::size)==Invalid && !p.writes);
        Peer q; RPMTransport u; assert(u.initialize(q.io(),Peer::size)==OK); q.frame(3,7,0); assert(u.connect(1000)==Stale);
        Peer wrong; wrong.openAckChannel=2; wrong.openAckWord=12; RPMTransport bad;
        assert(bad.initialize(wrong.io(),Peer::size)==OK && bad.connect(1000)==Invalid);
        assert(!bad.ready() && !wrong.requests);
        Peer n; n.respond=false; RPMTransport z; assert(z.initialize(n.io(),Peer::size)==OK && z.connect(100)==TimedOut);
        Peer x; RPMTransport f; assert(f.initialize(x.io(),Peer::size)==OK && f.connect(1000)==OK);
        x.frame(9,7,0,std::vector<uint8_t>(8)); uint8_t kvp[12]={}; put32(kvp+4,4);
        assert(f.write(1,1,0,kvp,12,100)==Stale && !f.ready());
        Peer c; RPMTransport checked; assert(checked.initialize(c.io(),Peer::size)==OK && checked.connect(1000)==OK);
        c.frame(3,7,0); assert(checked.check()==Stale && !checked.ready());
    }
    {
        RPMRegulators r; const auto &rail=rpmRails[0]; uint32_t uv=rail.minimum+rail.step*2;
        assert(r.configure(rail.id,rail.minimum,rail.maximum,0));
        assert(!r.configure(rail.id,rail.minimum,rail.maximum,0));
        unsigned sends=0;
        auto send=[&](uint32_t type,uint32_t id,unsigned set,const uint8_t *p,uint32_t n){
            ++sends; assert(type==rail.type && id==rail.resource && set==0 && n==24);
            assert(le32(p)==0x6e657773 && le32(p+12)==0x7675 && le32(p+20)==uv);return OK;};
        assert(r.set(rail.id,0,1,uv-1,false,send)==OK);
        assert(r.set(rail.id,0,2,uv,false,send)==OK && sends==1);
        assert(r.set(rail.id,0,3,rail.maximum+1,false,send)==Invalid);
        auto level=[](uint32_t type,uint32_t id,unsigned set,const uint8_t *p,uint32_t){assert(type==0x78637772 && !id && set==1 && le32(p+12)==0x6c766c76 && le32(p+20)==192);return OK;};
        assert(r.set(0x1000,1,4,192,true,level)==OK);
        assert(r.set(0x1000,1,4,193,true,level)==Invalid);
        RPMRegulators inherited;uint32_t current=0;unsigned messages=0;
        assert(!inherited.configureDomain(0x1000,0,64) && !inherited.configureDomain(0x1001,0,64));
        assert(!inherited.configureDomain(0x1000,193,0));
        for(unsigned i=0;i<4;++i)assert(inherited.configureDomain(0x1000+i,192,64));
        assert(!inherited.configureDomain(0x1000,192,64));
        auto baseline=[&](uint32_t,uint32_t,unsigned state,const uint8_t *p,uint32_t) {
            ++messages;assert(le32(p+20)==(state?64U:192U));return OK;
        };
        assert(inherited.get(0x1000,0,current)==NotReady && inherited.domainsConfigured() && !inherited.domainsReady());
        assert(inherited.activateDomains(baseline)==OK && messages==8 && inherited.domainsReady());
        assert(inherited.set(0x1000,0,5,0,true,baseline)==OK && messages==8);
        assert(inherited.get(0x1000,0,current)==OK && current==192);
        assert(inherited.activateDomains(baseline)==OK && messages==8);
        RPMRegulators uncertain;for(unsigned i=0;i<4;++i)assert(uncertain.configureDomain(0x1000+i,192,64));
        assert(uncertain.activateDomains([](auto...){return TimedOut;})==TimedOut);
        assert(uncertain.activateDomains(baseline)==Stale && messages==8);
    }
    {
        PMIC p; SPMIRegulators r; assert(r.initialize(p.io(),0)==OK);
        assert(r.configure(1,1,320000,1352000,0)); uint32_t value;
        assert(r.get(1,value)==OK && value==800000);
        assert(r.set(1,0,900000)==Denied && r.set(1,1,900001)==OK);
        assert(r.get(1,value)==OK && value==904000);
        assert(r.set(1,2,850000)==OK && r.set(1,1,0)==OK);
        assert(r.get(1,value)==OK && value==852000);
        assert(r.set(1,2,0)==OK && r.get(1,value)==OK && value==800000); // inherited floor
        p.denyAfter=p.commands+7; assert(r.set(1,1,1000000)==Stale);
        assert(r.set(1,1,1100000)==Stale);
    }
    {
        PMIC p; p.at(4,0x700)=1; SPMIArbiter a; assert(a.initialize(p.io(),0)==OK);
        uint8_t data[8]={1,2,3,4,5,6,7,8}; unsigned before=p.writes;
        assert(a.transfer(1,0x1440,data,8,true)==Denied && p.writes==before);
        assert(a.transfer(1,0x14ff,data,2,false)==Invalid);
        assert(a.transfer(1,0x1540,data,1,false)==Unsupported);
        assert(a.transfer(1,0x1440,data,2,false)==OK && data[0]==0x20 && data[1]==3);
        p.hang=true; assert(a.transfer(1,0x1440,data,2,false)==TimedOut);
        assert(a.transfer(1,0x1440,data,2,false)==Stale);
        PMIC q; q.at(0,0x804)=q.at(0,0x800); SPMIArbiter b; assert(b.initialize(q.io(),0)==Invalid);
        PMIC v; v.at(0,0)=0x40000000; SPMIArbiter c; assert(c.initialize(v.io(),0)==Unsupported);
    }
    {
        PMIC p;SPMIRegulators r;assert(r.initialize(p.io(),0)==OK && r.configure(1,1,524000,1100000,800000));
        assert(r.configureGPU({1,1,800000,0})==OK);
        uint64_t token=0;assert(r.retainGPUState(token)==OK && token);
        assert(r.set(1,3,896000)==Busy && !r.rawWriteAllowed(1,0x1440) && r.rawWriteAllowed(1,0x6040));
        assert(r.validateGPUState(token)==OK);
        p.pmic[0x40]^=4;assert(r.validateGPUState(token)==Stale && r.validateGPUState(token,true)==Stale);
        assert(r.set(1,3,896000)==Busy); // retain the physical rail on drift
    }
    {
        PMIC p; SPMIRegulators r; assert(r.initialize(p.io(),0)==OK && r.configure(1,1,320000,1352000,0));
        p.ready=false; p.pmic[0x61]=3; assert(r.set(1,1,1000000)==TimedOut); assert(r.set(1,1,1100000)==Stale);
    }
    {
        for(auto failure:{OSMFaultMemory::DropRequest,OSMFaultMemory::ChangeRollbackRow,
                          OSMFaultMemory::DisableOSM,OSMFaultMemory::DropRollback}) {
            OSMFaultMemory m;m.failure=failure;osmTable(m);OSMController o;
            assert(o.initialize(m.io())==OK);
            auto result=o.setRate(0,576000000);
            if (failure==OSMFaultMemory::DropRequest) {
                assert(result==TimedOut && o.rate(0)==384000000);
                assert((m.desiredWrites==std::vector<uint32_t>{1,0}));
                assert(o.setRate(0,576000000)==OK); // validated rollback permits retry
            } else {
                assert(result==Stale && o.rate(0)==0);
                assert(m.desiredWrites.size()==(failure==OSMFaultMemory::DropRollback ? 2U : 1U));
                unsigned writes=m.writes;
                assert(o.setRate(0,576000000)==Stale && m.writes==writes);
                assert(o.setRate(1,576000000)==OK); // quarantine is per cluster
            }
        }
    }
    {
        // The desired-state ACK can succeed while firmware changes the target
        // row or disables OSM. Quarantine without writing a stale rollback.
        for (auto failure : {OSMFaultMemory::AcceptedDisable, OSMFaultMemory::AcceptedIndex,
                             OSMFaultMemory::AcceptedFrequency, OSMFaultMemory::AcceptedVoltage,
                             OSMFaultMemory::AcceptedOverride, OSMFaultMemory::AcceptedSpare}) {
            OSMFaultMemory m; m.failure=failure; osmTable(m); OSMController o;
            assert(o.initialize(m.io())==OK);
            assert(o.setRate(0,576000000)==Stale && o.rate(0)==0);
            assert(m.desiredWrites.size()==1 && m.desiredWrites[0]==1);
            const unsigned writes=m.writes;
            assert(o.setRate(0,576000000)==Stale && m.writes==writes);
            assert(o.setRate(1,576000000)==OK);
        }
    }
    {
        Memory m;osmTable(m);OSMController o;assert(o.initialize(m.io())==OK);
        m.at(0,0x1f10)=37; // Duplicate quad-core rows are valid rollback states.
        assert(o.setRate(0,576000000)==OK);
    }
    {
        // A corrupt 8-bit PLL multiplier must not wrap a >4 GHz frequency
        // into the accepted sub-GHz range before validation.
        Memory m;osmTable(m);
        for(unsigned i=0;i<40;++i) {
            m.at(0,0x1154+32*i)=(1U<<26)|(4U<<16)|255;
            m.at(0,0x1158+32*i)=900;
        }
        OSMController o;assert(o.initialize(m.io())==Invalid && !m.writes);
    }
    {
        // Do not use an unknown desired index (including single-core turbo)
        // as the rollback target for an otherwise valid quad-core request.
        for(unsigned index:{38U,40U,UINT32_MAX}) {
            Memory m;osmTable(m);OSMController o;assert(o.initialize(m.io())==OK);
            m.at(0,0x1f10)=index;
            assert(o.setRate(0,576000000)==Stale && !m.writes);
            m.at(0,0x1f10)=0;
            assert(o.setRate(0,576000000)==Stale && !m.writes);
            assert(o.setRate(1,576000000)==OK);
        }
    }
    {
        Memory m; OSMController c; assert(c.initialize(m.io())==NotReady && !m.writes); assert(c.setRate(0,384000000)==NotReady);
        Memory n; osmTable(n); OSMController o; assert(o.initialize(n.io())==OK && !n.writes);
        n.at(0,0x1f10)=37; // Duplicate quad-core boot index remains a known rate.
        assert(o.rate(0)==768000000);
        n.at(0,0x1f10)=38;assert(o.rate(0)==0); // Single-core turbo stays excluded.
        n.at(0,0x1f10)=0;
        assert(o.stateCount(0)==3 && o.state(0,2,false)==768000000 && o.state(0,2,true)==900000 && !o.state(0,3,false));
        assert(o.minimum(0)==384000000 && o.maximum(1)==768000000);
        assert(o.setRate(0,576000000)==OK && o.rate(0)==576000000 && o.voltage(0)==800000);
        assert(o.setRate(0,600000000)==Unsupported);
        assert(o.setLimit(0,500000000)==OK && o.rate(0)==384000000 && o.limit(0)==384000000);
        assert(o.setRate(0,768000000)==Denied && o.setLimit(0,0)==OK && o.setRate(0,768000000)==OK);
        n.at(0,0x1158+32*2)=1000; assert(o.setRate(0,768000000)==Stale && o.rate(0)==0);
        assert(o.setRate(1,768000000)==OK); // independent cluster quarantine
        Memory v; osmTable(v); v.at(0,0x1150)=1; OSMController bad; assert(bad.initialize(v.io())==Invalid && !v.writes);
    }
    {
        using Failure=OSMController::InitializationFailure;
        Memory m;osmTable(m);OSMController o;
        // Firmware handoff retries must replace, rather than append to, the
        // bounded table. Enough retries to overflow the old 40-entry array.
        for(unsigned retry=0;retry<20;++retry) {
            assert(o.initialize(m.io())==OK && !m.writes);
            assert(o.initializationDiagnostic().reason==Failure::None);
            assert(o.stateCount(0)==3 && o.stateCount(1)==3);
            assert(o.state(0,0,false)==384000000 && o.state(0,2,false)==768000000);
        }
        m.at(0,0x1004)=m.at(0,0x3004)=0;
        assert(o.initialize(m.io())==NotReady);
        assert(o.initializationDiagnostic().reason==Failure::Disabled);
        for(unsigned bank=0;bank<2;++bank) {
            assert(!o.stateCount(bank) && !o.minimum(bank) && !o.maximum(bank) && !o.limit(bank));
            assert(o.setRate(bank,384000000)==NotReady && !m.writes);
        }
        // A later fresh snapshot can recover a quarantined bank; old faults,
        // ceilings and alias indices must not survive the new initialization.
        osmTable(m);assert(o.initialize(m.io())==OK);
        m.at(0,0x1158+32)=850;
        assert(o.setRate(0,576000000)==Stale && !m.writes);
        osmTable(m);assert(o.initialize(m.io())==OK);
        assert(o.setRate(0,576000000)==OK && o.rate(0)==576000000);
    }
    {
        using Failure=OSMController::InitializationFailure;
        for(auto failure:{Failure::IndexMismatch,Failure::CoreCount,Failure::FrequencyRange,
                          Failure::VoltageRange,Failure::FrequencyOrder,Failure::VoltageOrder,
                          Failure::DuplicateVoltage}) {
            Memory m;osmTable(m);OSMController o;assert(o.initialize(m.io())==OK);
            // Fail the second bank after the first replacement bank has been
            // fully parsed. Neither old nor partial replacement states survive.
            unsigned row=failure==Failure::FrequencyOrder || failure==Failure::DuplicateVoltage ? 2:1;
            const unsigned offset=0x2000+0x1150+32*row;
            switch(failure) {
            case Failure::IndexMismatch:m.at(0,offset)=99;break;
            case Failure::CoreCount:m.at(0,offset+4)=(1U<<26)|(2U<<16)|30;break;
            case Failure::FrequencyRange:m.at(0,offset+4)=(1U<<26)|(4U<<16)|255;break;
            case Failure::VoltageRange:m.at(0,offset+8)=1600;break;
            case Failure::FrequencyOrder:m.at(0,offset+4)=(1U<<26)|(4U<<16)|25;break;
            case Failure::VoltageOrder:m.at(0,offset+8)=600;break;
            case Failure::DuplicateVoltage:m.at(0,offset+4)=(1U<<26)|(4U<<16)|30;break;
            default:assert(false);
            }
            const uint32_t rawIndex=m.at(0,offset),rawFrequency=m.at(0,offset+4),rawVoltage=m.at(0,offset+8);
            assert(o.initialize(m.io())==Invalid);
            const auto &diagnostic=o.initializationDiagnostic();
            assert(diagnostic.reason==failure && diagnostic.bank==1 && diagnostic.row==row);
            assert(diagnostic.rawIndex==rawIndex && diagnostic.rawFrequency==rawFrequency &&
                   diagnostic.rawVoltage==rawVoltage && diagnostic.rawEnable==1);
            for(unsigned bank=0;bank<2;++bank) {
                assert(!o.stateCount(bank) && !o.rate(bank) && !o.voltage(bank));
                assert(!o.minimum(bank) && !o.maximum(bank) && !o.limit(bank));
                assert(o.setRate(bank,576000000)==NotReady && !m.writes);
            }
            osmTable(m);assert(o.initialize(m.io())==OK && o.stateCount(0)==3 && o.stateCount(1)==3);
            assert(o.initializationDiagnostic().reason==Failure::None);
        }
    }
    {
        using Failure=OSMController::InitializationFailure;
        Memory m;osmTable(m);OSMController o;
        for(unsigned i=0;i<40;++i) m.at(0,0x1154+32*i)=(1U<<26)|(1U<<16)|40;
        assert(o.initialize(m.io())==Invalid && !m.writes);
        assert(o.initializationDiagnostic().reason==Failure::NoQuadCoreStates);
        assert(o.initializationDiagnostic().bank==0 && o.initializationDiagnostic().row==OSMController::NoLocation);
        // Missing callbacks must be rejected before dereference; a failed
        // rebind also revokes access to the formerly valid hardware snapshot.
        for(unsigned missing=0;missing<3;++missing) {
            osmTable(m);assert(o.initialize(m.io())==OK);
            auto access=m.io();
            if(missing==0)access.read=nullptr;
            else if(missing==1)access.write=nullptr;
            else access.wait=nullptr;
            assert(o.initialize(access)==Invalid && !m.writes);
            assert(o.initializationDiagnostic().reason==Failure::MissingIO);
            assert(o.initializationDiagnostic().bank==OSMController::NoLocation);
            assert(!o.stateCount(0) && !o.stateCount(1) && !o.rate(0) && !o.rate(1));
            assert(o.setRate(0,384000000)==NotReady && o.setRate(1,384000000)==NotReady);
        }
    }
    puts("Power: GLINK framing/ACK/wrap/timeouts, voltage votes, SPMI permissions/readback, OSM calibrated LUT/limits and scalar protocol passed");
}
