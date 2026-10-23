// SPDX-License-Identifier: GPL-2.0-only
#include "QcomRPM.h"
#include "RPMResourceHandoff.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/libkern.h>
#define super QcomPowerService
OSDefineMetaClassAndStructors(QcomRPM, super);
void QcomRPM::signal() { auto io = hardwareIO(); io.write(this, 1, 8, 1); }
bool QcomRPM::start(IOService *provider)
{
    unsigned stage=1;
    auto reject=[&] {provider->setProperty("RPMStartupStage",stage,32);return false;};
    provider->setProperty("RPMStartupStage",stage,32);
    const UInt64 addresses[] = {0x00778000,0x17911000}, sizes[] = {0x7000,0x1000};
    if (!mapResources(provider, addresses, sizes, 2)) return reject();
    stage=2;
    unsigned count; const UInt8 *config = cells(provider, "qcom,regulator-limits", 16, count);
    if (count == UINT32_MAX || count > QcomPower::RPMRailCount) return reject();
    for (unsigned i = 0; i < count; ++i)
        if (!fRegulators.configure(QcomPower::le32(config+16*i), QcomPower::le32(config+16*i+4),
            QcomPower::le32(config+16*i+8), QcomPower::le32(config+16*i+12))) return reject();
    stage=3;
    config=cells(provider,"qcom,rpm-domain-handoff",16,count);
    if(count==UINT32_MAX || (count && count!=4))return reject();
    auto *handoffVersion=OSDynamicCast(OSData,provider->getProperty("qcom,rpm-handoff-version"));
    const UInt32 version=handoffVersion && handoffVersion->getLength()==4 ? QcomPower::le32(handoffVersion->getBytesNoCopy()) : 0;
    if(handoffVersion && version!=1 && version!=2 && version!=3)return reject();
    if(count && version!=1 && version!=2 && version!=3)return reject();
    for(unsigned i=0;i<count;++i) {
        const auto *p=config+16*i;
        if(QcomPower::le32(p+12) || !fRegulators.configureDomain(QcomPower::le32(p),QcomPower::le32(p+4),QcomPower::le32(p+8)))return reject();
    }
    stage=4;
    config=cells(provider,"qcom,rpm-interconnect-handoff",32,count);
    if (count==UINT32_MAX || (count && count!=QcomPower::RPMBusResourceCount)) return reject();
    const bool interconnect=count!=0;
    if (interconnect && version!=1 && version!=2 && version!=3) return reject();
    if (interconnect && !fRegulators.domainsConfigured()) return reject();
    for (unsigned i=0;i<count;++i) {
        const auto *p=config+32*i;
        auto word64=[](const UInt8 *p) { return UInt64(QcomPower::le32(p)) | UInt64(QcomPower::le32(p+4))<<32; };
        if (QcomPower::le32(p+4) || !fInterconnect.configure(QcomPower::le32(p),word64(p+8),word64(p+16),word64(p+24))) return reject();
    }
    stage=5;
    config=cells(provider,"qcom,rpm-clock-handoff",16,count);
    if (count==UINT32_MAX || (interconnect ? count!=QcomPower::RPMHandoffClockCount : count!=0)) return reject();
    for (unsigned i=0;i<count;++i) {
        const auto *p=config+16*i;
        if (QcomPower::le32(p+12) || !fClockHandoff.configure(QcomPower::le32(p),QcomPower::le32(p+4),QcomPower::le32(p+8))) return reject();
    }
    stage=6;
    config=cells(provider,"qcom,rpm-clock-limit-handoff",16,count);
    if(count==UINT32_MAX || (interconnect ? count!=QcomPower::RPMHandoffClockLimitCount : count!=0))return reject();
    for(unsigned i=0;i<count;++i) {
        const auto *p=config+16*i;
        if(QcomPower::le32(p+12) || !fClockHandoff.configureLimit(QcomPower::le32(p),QcomPower::le32(p+4),QcomPower::le32(p+8)))return reject();
    }
    if(interconnect && !fClockHandoff.configured())return reject();
    if (interconnect) for (unsigned i=0;i<3;++i) for (unsigned set=0;set<2;++set) {
        const auto &r=QcomPower::rpmBusResources[i]; UInt32 baseline=0,cap=0; UInt64 floor=0,maximum=0;
        if (fClockHandoff.floor(r.type,r.resource,0x007a484b,set,baseline)!=QcomPower::OK ||
            fClockHandoff.limit(r.type,r.resource,set,cap)!=QcomPower::OK ||
            fInterconnect.floor(r.id,set,floor)!=QcomPower::OK || floor!=UInt64(baseline)*1000 ||
            fInterconnect.limit(r.id,true,maximum)!=QcomPower::OK || (cap && maximum>UInt64(cap)*1000))return reject();
    }
    fBusOwned=interconnect;
    stage=7;
    config=cells(provider,QcomPower::USBClockBaselineProperty,4,count);
    if(count==UINT32_MAX || count>1)return reject();
    if(count) {
        fUSBClockHz=QcomPower::le32(config);
        if(!fBusOwned || !QcomPower::usbCoreCXLevel(fUSBClockHz))return reject();
    }
    stage=8;
    auto *channel=OSDynamicCast(OSData,provider->getProperty("qcom,rpm-transport-handoff"));
    auto *cold=OSDynamicCast(OSData,provider->getProperty("qcom,rpm-transport-cold-handoff"));
    QcomPower::RPMTransportCold coldState{};
    if(cold) {
        if(cold->getLength()!=sizeof(coldState))return reject();
        memcpy(&coldState,cold->getBytesNoCopy(),sizeof(coldState));if(!coldState.valid())return reject();
    }
    stage=9;
    if(version==2) {
        if(!interconnect || !fRegulators.domainsConfigured() || !channel || channel->getLength()!=sizeof(QcomPower::RPMTransportHandoff))return reject();
        QcomPower::RPMTransportHandoff handoff{};memcpy(&handoff,channel->getBytesNoCopy(),sizeof(handoff));
        if(fTransport.adopt(hardwareIO(),0x7000,handoff)!=QcomPower::OK)return reject();
        setProperty("BootTransportAdopted",true);
    } else if(version==3) {
        if(channel || !cold || cold->getLength()!=sizeof(QcomPower::RPMTransportCold) ||
            (interconnect && !fRegulators.domainsConfigured()))return reject();
        if(fTransport.initializeCold(hardwareIO(),0x7000,coldState)!=QcomPower::OK)return reject();
        setProperty("BootTransportColdInitialized",true);
    } else {
        if(channel || cold || fTransport.initialize(hardwareIO(),0x7000)!=QcomPower::OK)return reject();
    }
    stage=10;
    setControllerKind(ClockController::RPM, ClockController::Voltage | (fRegulators.domainsConfigured() ? ClockController::Domains:0) |
        (interconnect ? ClockController::Clocks | ClockController::Interconnect | ClockController::RetainedGPUState | ClockController::ResourceHandoff : 0));
    for (const auto &rail : QcomPower::rpmRails) if (rail.step)
        if (!addTelemetry(ClockController::GetVoltage,rail.id,rail.name,1)) return reject();
    const char *domainNames[] = {"vddcx requested level","vddmx requested level","ssccx requested level","sscmx requested level"};
    for (unsigned i = 0; i < 4; ++i)
        if (!addTelemetry(ClockController::GetDomainLevel,0x1000+i,domainNames[i],2)) return reject();
    stage=11;
    if (!super::start(provider)) return reject();
    setProperty("ClockControllerIDSpace", "qcom,rpm-msm8998");
    setProperty("VoltageObservation", "acknowledged-request-not-measurement");
    setProperty("Transport", "GLINK-RPM-intentless");
    provider->setProperty("RPMStartupStage",12,32);
    registerService(kIOServiceSynchronous);
    publishResource(ClockController::RPMReadyResource,this);return true;
}
IOReturn QcomRPM::execute(ClockController::Request &r)
{
    using namespace ClockController;
    if(r.operation==GetModemClockVote || r.operation==SetModemClockVote) {
        if(!ClockController::valid(r))return kIOReturnBadArgument;
        if(!fBusOwned)return kIOReturnUnsupported;
        const auto &target=reinterpret_cast<const TargetedRequest &>(r);
        QcomPower::Status status;
        if(r.operation==SetModemClockVote) {
            status=fModemClocks.validate(r.id,target.flags&1,target.consumer,r.value,fClockHandoff);
            if(status!=QcomPower::OK)return convert(status);
            status=initializeResources(true);if(status!=QcomPower::OK)return convert(status);
            status=fModemClocks.vote(r.id,target.flags&1,target.consumer,r.value,fClockHandoff,
                [&](uint32_t type,uint32_t id,unsigned set,const uint8_t *kvp,uint32_t bytes) {
                    return fTransport.write(type,id,set,kvp,bytes);
                });
            if(status!=QcomPower::OK)return convert(status);
        }
        status=fTransport.check();if(status!=QcomPower::OK)return convert(status);
        UInt32 value=0;status=fModemClocks.get(r.id,target.flags&1,fClockHandoff,value);
        if(status==QcomPower::OK)r.value=value;
        return convert(status);
    }
    if(r.operation==PrepareUSBResources) {
        if(!ClockController::valid(r))return kIOReturnBadArgument;
        if(!fBusOwned || !fUSBClockHz)return kIOReturnUnsupported;
        const auto status=QcomPower::prepareUSBRPMResources(fTransport,fRegulators,fClockHandoff,fInterconnect,r.id,
            [&]{return validateUSBClockBaseline();});
        if(status==QcomPower::OK)setProperty("NonzeroVoltageFloorsAcknowledged",fRegulators.railFloorsReady());
        return convert(status);
    }
    if (gpuOperation(r.operation)) return gpuState(r);
    const bool busClock=r.operation==GetRate || r.operation==SetRate || r.operation==GetMaximumRate || r.operation==GetMinimumRate;
    const bool busBandwidth=r.operation==GetBandwidth || r.operation==SetBandwidth || r.operation==GetMaximumBandwidth || r.operation==GetMinimumBandwidth;
    if (busClock || busBandwidth) {
        if (r.version!=2) return kIOReturnBadArgument;
        const auto &target=reinterpret_cast<const TargetedRequest &>(r);
        if (r.operation==GetMaximumRate || r.operation==GetMaximumBandwidth)
            return convert(fInterconnect.limit(r.id,busClock,r.value));
        if (r.operation==GetMinimumRate || r.operation==GetMinimumBandwidth) {
            UInt64 maximum=0;
            const auto owned=fInterconnect.limit(r.id,busClock,maximum);
            return owned==QcomPower::OK ? convert(fInterconnect.floor(r.id,target.flags&1,r.value)) : convert(owned);
        }
        if (r.operation==GetRate || r.operation==GetBandwidth) {
            UInt64 maximum=0;
            const auto owned=fInterconnect.limit(r.id,busClock,maximum);
            if (owned!=QcomPower::OK) return convert(owned);
            if (!fClockHandoff.ready()) return kIOReturnNotReady;
            auto status=fTransport.check();
            return status==QcomPower::OK ? convert(fInterconnect.get(r.id,target.flags&1,busClock,r.value)) : convert(status);
        }
        if (target.consumer!=USBInterconnectConsumer &&
            !(target.consumer==GPUConsumer && (r.id==0x2002 || r.id==0x2104))) return kIOReturnNotPermitted;
        // Reject an unowned or wrong-kind resource before connecting or
        // sending anything on the common RPM transport.
        UInt64 maximum=0;
        auto status=fInterconnect.limit(r.id,busClock,maximum);
        if (status!=QcomPower::OK) return convert(status);
        status=fInterconnect.validate(r.id,target.flags&1,target.consumer,busClock,r.value);
        if(status!=QcomPower::OK)return convert(status);
        status=initializeResources(true);if(status!=QcomPower::OK)return convert(status);
        return convert(fInterconnect.vote(r.id,target.flags&1,target.consumer,busClock,r.value,
            [&](uint32_t type,uint32_t id,unsigned set,const uint8_t *kvp,uint32_t bytes) {
                return fTransport.write(type,id,set,kvp,bytes);
            }));
    }
    if (r.operation == GetResourceCount) { r.value = QcomPower::RPMRailCount+4+(fBusOwned ? QcomPower::RPMBusResourceCount : 0); return kIOReturnSuccess; }
    if (r.operation == GetResourceID) {
        const auto busStart=QcomPower::RPMRailCount+4;
        if (r.id >= busStart+(fBusOwned ? QcomPower::RPMBusResourceCount : 0)) return kIOReturnBadArgument;
        r.value = r.id>=busStart ? QcomPower::rpmBusResources[r.id-busStart].id :
            r.id < QcomPower::RPMRailCount ? QcomPower::rpmRails[r.id].id : 0x1000+r.id-QcomPower::RPMRailCount;
        return kIOReturnSuccess;
    }
    if (r.operation == GetName) {
        r.name=fInterconnect.name(r.id); if (!r.name) r.name=fRegulators.name(r.id);
        return r.name ? kIOReturnSuccess : kIOReturnUnsupported;
    }
    if (r.version != 2) return kIOReturnBadArgument;
    auto &target = reinterpret_cast<TargetedRequest &>(r);
    uint32_t value = 0;
    if (r.operation == GetVoltage || r.operation == GetDomainLevel) {
        if ((r.operation == GetDomainLevel) != (r.id >= 0x1000)) return kIOReturnBadArgument;
        const auto status=fTransport.check();if(status!=QcomPower::OK)return convert(status);
        auto result = fRegulators.get(r.id, target.flags&1, value); r.value = value; return convert(result);
    }
    if (r.operation != SetVoltage && r.operation != SetDomainLevel) return kIOReturnUnsupported;
    if (r.value > UINT32_MAX) return kIOReturnBadArgument;
    if(r.operation==SetDomainLevel && !fRegulators.domainsConfigured())return kIOReturnUnsupported;
    auto result=fRegulators.validate(r.id,target.flags&1,target.consumer,static_cast<UInt32>(r.value),r.operation==SetDomainLevel);
    if(result!=QcomPower::OK)return convert(result);
    result=initializeResources(false);if(result!=QcomPower::OK)return convert(result);
    result = fRegulators.set(r.id, target.flags&1, target.consumer, static_cast<UInt32>(r.value),
        r.operation == SetDomainLevel, [&](uint32_t type, uint32_t id, unsigned set, const uint8_t *kvp, uint32_t bytes) {
            return fTransport.write(type, id, set, kvp, bytes);
        });
    if (result == QcomPower::OK && fRegulators.get(r.id, target.flags&1, value) == QcomPower::OK) r.value = value;
    return convert(result);
}
QcomPower::Status QcomRPM::initializeResources(bool bus)
{
    using namespace QcomPower;
    if(bus && (!fBusOwned || !fClockHandoff.configured()))return Unsupported;
    const auto status=initializeRPMResources(fTransport,fRegulators,fClockHandoff,fInterconnect,bus,
        [&]{return validateUSBClockBaseline();});
    if(status==OK)setProperty("NonzeroVoltageFloorsAcknowledged",fRegulators.railFloorsReady());
    return status;
}
QcomPower::Status QcomRPM::validateUSBClockBaseline()
{
    using namespace ClockController;using namespace QcomPower;
    if(!fUSBClockHz)return OK; // External owners retain the original contract.
    auto r=targeted(GCC,GetRate,163,0,USBControllerConsumer);
    const auto result=resourceRequest(r);
    if(r.request.version!=2 || r.request.size!=sizeof(r) || !ClockController::valid(r.request) || r.controller!=GCC ||
        r.consumer!=USBControllerConsumer || r.request.operation!=GetRate || r.request.id!=163 || r.flags)return Failed;
    if(result!=kIOReturnSuccess)return result==kIOReturnUnsupported ? Unsupported:
        (result==kIOReturnNotReady || result==kIOReturnNotAttached || result==kIOReturnOffline) ? NotReady:
        result==kIOReturnTimeout ? TimedOut:Failed;
    return QcomPower::validateUSBClockBaseline(fRegulators,fUSBClockHz,r.request.value);
}
IOReturn QcomRPM::gpuState(ClockController::Request &r)
{
    using namespace ClockController;
    const bool acquire=r.operation==RetainGPUState;
    if (fGPUFault) return kIOReturnIOError;
    if (acquire ? (r.value || fGPULease || fGPUSerial==UINT64_MAX) : (!r.value || r.value!=fGPULease))
        return acquire && fGPULease ? kIOReturnBusy : kIOReturnBadArgument;
    if(acquire && fRegulators.domainsConfigured()) {
        const auto status=initializeResources(true);if(status!=QcomPower::OK)return convert(status);
    }
    UInt64 values[4]{};
    auto unknown=[](QcomPower::Status s) {return s==QcomPower::TimedOut || s==QcomPower::Stale || s==QcomPower::Failed;};
    const auto transport=fTransport.check();bool ambiguous=unknown(transport);
    bool valid=fBusOwned && fClockHandoff.ready() && transport==QcomPower::OK;
    for (unsigned i=0;i<2;++i) {
        UInt32 level=0;
        const auto status=fRegulators.get(0x1000+i,0,level);ambiguous|=unknown(status);
        valid &= status==QcomPower::OK && level;
        values[i]=level;
    }
    const auto clock=fInterconnect.get(0x2002,0,true,values[2]);ambiguous|=unknown(clock);
    const auto bandwidth=fInterconnect.get(0x2104,0,false,values[3]);ambiguous|=unknown(bandwidth);
    valid &= clock==QcomPower::OK && values[2] && bandwidth==QcomPower::OK && values[3];
    if (!acquire) for (unsigned i=0;i<4;++i) valid &= values[i]>=fGPUFloor[i];
    if (!valid) {
        if (fGPULease || ambiguous) fGPUFault=true;
        return acquire && !ambiguous ? kIOReturnNotReady : kIOReturnIOError;
    }
    if (r.operation==ValidateGPUState) return kIOReturnSuccess;
    if (acquire) {
        r.value=fGPULease=++fGPUSerial;
        for (unsigned i=0;i<4;++i) fGPUFloor[i]=values[i];
    }
    // These policy operations only preserve/remove software floors. They do
    // not send an RPM message or substitute guessed levels for firmware state.
    const bool release=!acquire;
    if (fRegulators.gpuMinimum(0x1000,release)!=QcomPower::OK ||
        fRegulators.gpuMinimum(0x1001,release)!=QcomPower::OK ||
        fInterconnect.gpuMinimum(0x2002,release)!=QcomPower::OK ||
        fInterconnect.gpuMinimum(0x2104,release)!=QcomPower::OK) {
        fGPUFault=true; return kIOReturnIOError;
    }
    if (release) fGPULease=0;
    return kIOReturnSuccess;
}
