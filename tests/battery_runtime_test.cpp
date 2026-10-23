// SPDX-License-Identifier: GPL-2.0-only
#include <KernelShim.h>
#include "../drivers/power/QcomBattery/QcomBattery.cpp"
#include <cstdio>
static UInt64 number(IOService *s,const char *key){
    auto *n=dynamic_cast<OSNumber *>(s->getProperty(key));assert(n);return n->value;
}
int main(){
    const int baseline=liveObjects;
    for(int failure : {0,1,2,3}) {
        auto *dt=new IOService;
        auto *data=new OSData;
        for(uint32_t v : {1,2,0x4000,0x4100,0x1000,0x1600,5000})
            for(unsigned i=0;i<4;++i)data->bytes.push_back(v>>(i*8));
        dt->setProperty("qcom,battery-observer",data);data->release();
        auto *spmi=new QcomSPMI;spmi->attach(dt);
        bool offline=true;unsigned reads=0;
        std::array<uint8_t,65536> r{};
        r[0x103]=2;r[0x104]=0x51;r[0x105]=0x15;r[0x4005]=0x10;r[0x4105]=0x11;
        r[0x4046]=0x80;r[0x4010]=2;r[0x4009]=r[0x400a]=128;
        r[0x41a0]=r[0x41a6]=0;r[0x41a1]=r[0x41a7]=0x7d;
        r[0x4106]=1;r[0x4150]=0xa8;r[0x4151]=4;r[0x1006]=7;
        spmi->transport=[&](UInt8 sid,UInt16 address,UInt8 *p,unsigned n,bool write){
            assert(!write && sid==2);++reads;
            if(offline)return -99;
            for(unsigned i=0;i<n;++i)p[i]=r[address+i];
            return 0;
        };
        auto *driver=new QcomBattery;driver->attach(spmi);
        eventAdds=0;failEventAdd=failure==1 || failure==2 ? failure:0;failPM=failure==3;
        if(failure)assert(!driver->start(spmi));
        else {
            assert(driver->start(spmi) && driver->registrations==1);
            assert(driver->children.empty()); // initial failure never fabricates a source
            assert(driver->getProperty("BatteryTelemetryValid")==kOSBooleanFalse);
            assert(number(driver,"BatteryReadError")==uint32_t(PMICBattery::Result::IOFailure));
            assert(number(driver,"BatteryLastIOError")==uint32_t(-99));
            offline=false;driver->getWorkLoop()->fire();
            assert(driver->children.size()==1);
            auto *source=dynamic_cast<QcomBatteryPowerSource *>(driver->children.front());
            assert(source && source->registrations==1);
            assert(number(source,"CurrentCapacity")==50 && number(source,"MaxCapacity")==100);
            assert(number(source,"Voltage")==3906 && number(source,"Temperature")==2485);
            assert(!source->getProperty("DesignCapacity") && !source->getProperty("CycleCount"));
            unsigned notified=source->notifications;
            driver->getWorkLoop()->fire();
            assert(source->notifications==notified); // unchanged data does not spam native clients
            r[0x4106]=0;driver->getWorkLoop()->fire();
            assert(!source->getProperty("Temperature") && source->notifications>notified);
            offline=true;driver->getWorkLoop()->fire();
            assert(source->getProperty("BatteryTelemetryValid")==kOSBooleanFalse);
            for(auto *key:{"CurrentCapacity","MaxCapacity","Voltage","Amperage","ExternalConnected","IsCharging"})
                assert(!source->getProperty(key));
            assert(source->getProperty("ErrorCondition"));
            offline=false;driver->getWorkLoop()->fire();
            assert(source->getProperty("BatteryTelemetryValid")==kOSBooleanTrue);
            assert(!source->getProperty("ErrorCondition") && source->registrations==1);
            driver->setPowerState(0,spmi);
            const unsigned pausedReads=reads;
            driver->getWorkLoop()->fire();
            assert(reads==pausedReads && !source->getProperty("CurrentCapacity"));
            driver->setPowerState(1,spmi);
            assert(reads>pausedReads && number(source,"CurrentCapacity")==50);
            r[0x4107]=4;driver->getWorkLoop()->fire();
            assert(source->getProperty("BatteryInstalled")==kOSBooleanFalse);
            assert(!source->getProperty("CurrentCapacity") && !source->getProperty("Voltage"));
            assert(driver->setPowerState(2,spmi)==kIOPMNoSuchState);
            driver->stop(spmi);
            assert(driver->children.empty() && !driver->getWorkLoop());
            const unsigned stoppedReads=reads;
            driver->setPowerState(1,spmi);assert(reads==stoppedReads);
        }
        driver->detach(spmi);driver->release();
        spmi->detach(dt);spmi->release();dt->release();
        assert(liveObjects==baseline);
    }
    puts("Battery IOKit adapter: delayed publication, native notifications, fault recovery, missing battery, PM and rollback passed");
}
