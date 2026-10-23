// SPDX-License-Identifier: GPL-2.0-only
// Execute the actual service and user client; real SDK/vtables are checked by
// the separate Kext build. The host shim does not emulate IOKit PM scheduling.
#include <KernelShim.h>
#include "../drivers/input/PMI8998Haptics/PMI8998Haptics.cpp"
#include "../drivers/input/PMI8998Haptics/PMI8998HapticsUserClient.cpp"
#include <cstdio>
constexpr uint64_t MS = 1000000;
struct Fixture {
    IOService *dt = new IOService;
    QcomSPMI *spmi = new QcomSPMI;
    PMI8998Haptics *driver = new PMI8998Haptics;
    std::array<uint8_t,65536> regs{};
    unsigned reads = 0, writes = 0;
    bool offline = false;
    Fixture(bool owned = true) {
        auto *data = new OSData;
        for (uint32_t v : {1u,owned?1u:0u,3u,0xc000u,5000u,3248u,800u,15000u})
            for (unsigned i=0;i<4;++i) data->bytes.push_back(uint8_t(v>>(8*i)));
        dt->setProperty("qcom,pmi8998-haptics",data);data->release();
        spmi->attach(dt);driver->attach(spmi);
        regs[0x103]=2;regs[0x104]=0x51;regs[0x105]=0x15;
        spmi->transport=[&](UInt8 sid,UInt16 address,UInt8 *p,unsigned n,bool write) {
            assert(gateDepth); // Entire adapter sequence is on the shared loop.
            if(write)++writes;else ++reads;
            if(offline)return -99;
            assert((sid==2 && address==0x103 && !write) || (sid==3 && (address&0xff00)==0xc000));
            for(unsigned i=0;i<n;++i){if(write)regs[address+i]=p[i];else p[i]=regs[address+i];}
            return 0;
        };
        testTime=100*MS;failEventAdd=0;failPM=false;eventAdds=0;failTimer=false;administrator=true;
    }
    ~Fixture() {
        driver->stop(spmi);driver->detach(spmi);driver->release();
        spmi->detach(dt);spmi->release();dt->release();
    }
    bool off()const{return !(regs[0xc046]&0x80);}
    void tick(uint64_t ms){testTime=ms*MS;spmi->busLoop->fire();}
    IOUserClient *client() {
        IOUserClient *c=nullptr;
        assert(driver->newUserClient(1,nullptr,0,nullptr,&c)==0 && c);return c;
    }
    void destroy(IOUserClient *c) {c->clientClose();c->stop(driver);c->detach(driver);c->release();}
};
static IOReturn call(IOUserClient *c,unsigned selector,const uint64_t *input=nullptr,unsigned inputs=0,
                     uint64_t *output=nullptr,unsigned outputs=0) {
    IOExternalMethodArguments a;
    a.selector=selector;a.scalarInput=input;a.scalarInputCount=inputs;
    a.scalarOutput=output;a.scalarOutputCount=outputs;
    return c->externalMethod(selector,&a,nullptr,nullptr,nullptr);
}
int main() {
    const int baseline=liveObjects;
    {
        Fixture f(false);assert(!f.driver->start(f.spmi));assert(!f.reads && !f.writes);
    }
    assert(liveObjects==baseline);
    for (int failure : {1,2,3}) {
        Fixture f;failEventAdd=failure<3?failure:0;failPM=failure==3;
        assert(!f.driver->start(f.spmi) && f.off());
    }
    assert(liveObjects==baseline);
    {
        Fixture f;assert(f.driver->start(f.spmi));
        assert(f.driver->getWorkLoop()==f.spmi->getWorkLoop());
        assert(f.driver->getProperty("HapticsInitialized")==kOSBooleanTrue);
        IOUserClient *denied=nullptr;administrator=false;
        assert(f.driver->newUserClient(1,nullptr,0,nullptr,&denied)==kIOReturnNotPrivileged && !denied);
        administrator=true;auto *one=f.client(),*two=f.client();
        uint64_t p[]={100,1000},status[Haptics::StatusScalars]={};
        assert(call(one,Haptics::Play,p,2)==0 && !f.off());
        assert(call(two,Haptics::Play,p,2)==kIOReturnExclusiveAccess);
        assert(call(two,Haptics::Stop)==kIOReturnExclusiveAccess);
        assert(call(two,Haptics::GetStatus,nullptr,0,status,Haptics::StatusScalars)==0 && status[11]==1);
        // Duplicate PM-on must not cancel the active deadline.
        f.driver->setPowerState(1,f.spmi);assert(f.spmi->busLoop->timer->pending && !f.off());
        f.tick(120);f.tick(200);f.tick(225);assert(f.off());
        assert(call(two,Haptics::Play,p,2)==0);
        // Closing a non-owner must not stop the owner's output.
        f.destroy(one);assert(!f.off());
        assert(two->clientClose()==0 && f.off());
        // A method that passed an earlier user-client check cannot play after
        // close: closed state is tested again inside the shared command gate.
        two->inactive=false;
        assert(f.driver->request(Haptics::Play,two,p,nullptr)==kIOReturnNotAttached);
        two->stop(f.driver);two->detach(f.driver);two->release();
        one=f.client();assert(call(one,Haptics::Play,p,2)==0);
        f.driver->setPowerState(0,f.spmi);assert(f.off());
        const auto reads=f.reads;f.tick(300);assert(f.reads==reads);
        assert(call(one,Haptics::Play,p,2)==kIOReturnOffline);
        f.driver->setPowerState(1,f.spmi);assert(call(one,Haptics::Play,p,2)==0);
        f.driver->setPowerState(2,f.spmi);assert(!f.off());
        f.destroy(one);assert(f.off());
    }
    assert(liveObjects==baseline);
    {
        Fixture f;assert(f.driver->start(f.spmi));auto *c=f.client();
        uint64_t p[]={100,1000};unsigned writes=f.writes;
        IOExternalMethodArguments a;a.selector=Haptics::Play;a.scalarInput=p;a.scalarInputCount=2;
        a.structureInputSize=1;
        assert(c->externalMethod(Haptics::Play,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument && f.writes==writes);
        a.structureInputSize=0;a.asyncWakePort=1;
        assert(c->externalMethod(Haptics::Play,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
        a.asyncWakePort=0;a.scalarInputCount=1;
        assert(c->externalMethod(Haptics::Play,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
        failTimer=true;assert(call(c,Haptics::Play,p,2)!=0 && f.off());failTimer=false;
        assert(call(c,Haptics::Play,p,2)==kIOReturnIOError); // Fault remains latched.
        f.destroy(c);
    }
    assert(liveObjects==baseline);
    {
        Fixture f;assert(f.driver->start(f.spmi));auto *c=f.client();uint64_t p[]={100,1000};
        assert(call(c,Haptics::Play,p,2)==0);
        f.offline=true;f.tick(105);
        assert(f.driver->getProperty("HapticsOutputKnownOff")==kOSBooleanFalse);
        f.offline=false;f.destroy(c);assert(f.off());
    }
    assert(liveObjects==baseline);
    puts("PMI8998 IOKit adapter: shared workloop, PM idempotency, client leases/close, ABI validation, timer failure, offline fault and cleanup passed");
}
