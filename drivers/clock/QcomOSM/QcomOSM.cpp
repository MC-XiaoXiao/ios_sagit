// SPDX-License-Identifier: GPL-2.0-only
#include "QcomOSM.h"
#include "OSMHandoff.h"
#include <IOKit/pwr_mgt/RootDomain.h>
#include <machine/machine_routines.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <IOKit/IOWorkLoop.h>
#include <mach/mach_time.h>
#define super QcomPowerService
OSDefineMetaClassAndStructors(QcomOSM, super);
bool QcomOSM::start(IOService *provider)
{
    const UInt64 address[] = {0x179c0000}, size[] = {0x4000};
    if (!mapResources(provider, address, size, 1)) return false;
    auto result = fController.initialize(hardwareIO());
    // A failed probe remains observable in IORegistry. It never authorizes
    // writes, and initialize() clears all partially accepted bank snapshots.
    auto handoffStatus = QcomOSMHandoff::Status::Missing;
    uint32_t handoffVersion = 0, firmwareProfile = 0;
    auto *data = OSDynamicCast(OSData, provider->getProperty("qcom,cpu-osm-handoff"));
    if (data) {
        handoffStatus = QcomOSMHandoff::Status::Invalid;
        if (data->getLength() == sizeof(QcomOSMHandoff::Record)) {
            QcomOSMHandoff::Record record;
            bcopy(data->getBytesNoCopy(), &record, sizeof(record));
            handoffVersion = record.version;
            const auto io = hardwareIO();
            handoffStatus = QcomOSMHandoff::validate(record, [&](unsigned c, uint32_t off) {
                return io.read(io.context, 0, c * 0x2000 + off);
            });
            if (handoffStatus == QcomOSMHandoff::Status::Accepted)
                firmwareProfile = record.firmwareProfile;
        }
    }
    fHandoffAccepted = result == QcomPower::OK &&
        handoffStatus == QcomOSMHandoff::Status::Accepted;
    setControllerKind(ClockController::OSM, ClockController::Clocks | ClockController::Performance | ClockController::AutomaticPerformance);
    if (!addTelemetry(ClockController::GetRate, 0, "Power cluster desired frequency", 0) ||
        !addTelemetry(ClockController::GetRate, 1, "Performance cluster desired frequency", 0) ||
        !addTelemetry(ClockController::GetVoltage, 0, "Power cluster LUT voltage", 1) ||
        !addTelemetry(ClockController::GetVoltage, 1, "Performance cluster LUT voltage", 1)) return false;
    if (!super::start(provider)) return false;
    // Optional read-only clock-source evidence, independent of whether OSM
    // ownership was transferred. Mapping failure never authorizes a write.
    const uint64_t clockBases[]={0x17910000,0x17810000};
    for (unsigned c=0;c<2;++c) {
        auto *memory=IODeviceMemory::withRange(clockBases[c],0x8000);
        fClockMaps[c]=memory ? memory->map(kIOMapInhibitCache|kIOMapReadOnly) : nullptr;
        OSSafeReleaseNULL(memory);
    }
    if (fHandoffAccepted) {
        // Recheck every online physical cluster before starting the policy.
        // An offline performance cluster may legitimately park on aux.
        const auto *topology=ml_get_topology_info();
        bool checked[2]={}, ready=topology && topology->num_cpus && topology->num_cpus<=8;
        for (unsigned i=0;ready && i<topology->num_cpus;++i) {
            const unsigned c=(topology->cpus[i].phys_id>>8)&255;
            if (c>1 || !fClockMaps[c]) { ready=false; break; }
            if (checked[c]) continue;
            checked[c]=true;
            const auto base=fClockMaps[c]->getVirtualAddress();
            auto clock=QcomOSMClock::observe([&](uint32_t off) {
                const uint32_t value=*reinterpret_cast<volatile uint32_t *>(base+off);
                __asm__ volatile("dmb oshld" ::: "memory"); return value;
            });
            ready=clock.localPLLHz()==QcomOSMClock::BootHz[c];
        }
        if (!ready) {
            fHandoffAccepted=false;
            handoffStatus=QcomOSMHandoff::Status::ClockNotReady;
        }
    }
    setProperty("ClockControllerIDSpace", "qcom,cpu-clock-osm-msm8998");
    setProperty("CPUVoltageOwner", "OSM-CPRh");
    setProperty("FrequencyObservation", "accepted-desired-P-state-not-measurement");
    // Enabled LUTs alone do not prove CPRh calibration or ownership transfer.
    setProperty("OSMInitializationHandoffAccepted", fHandoffAccepted);
    setProperty("OSMTZBootParametersMatched", fHandoffAccepted);
    setProperty("OSMTZCompletionEvidence", "protected-post-init-depends-on-boot-chain");
    setProperty("OSMHandoffStatus", QcomOSMHandoff::name(handoffStatus));
    setProperty("OSMHandoffVersion", handoffVersion, 32);
    setProperty("OSMFirmwareProfile", firmwareProfile, 32);
    setProperty("OSMLUTValidated", result == QcomPower::OK);
    const auto &diagnostic = fController.initializationDiagnostic();
    setProperty("OSMInitializationFailure", static_cast<UInt32>(diagnostic.reason), 32);
    setProperty("OSMInitializationBank", diagnostic.bank, 32);
    setProperty("OSMInitializationRow", diagnostic.row, 32);
    setProperty("OSMRawIndex", diagnostic.rawIndex, 32);
    setProperty("OSMRawFrequency", diagnostic.rawFrequency, 32);
    setProperty("OSMRawVoltage", diagnostic.rawVoltage, 32);
    setProperty("OSMRawEnable", diagnostic.rawEnable, 32);
    publishPowerStatus();
    startPolicy();
    registerService(); return true;
}
bool QcomOSM::policyOnline(void *p) {
    auto *self=static_cast<QcomOSM *>(p);
    return self->fHandoffAccepted && self->controllerOnline();
}
uint32_t QcomOSM::policyCount(void *p,unsigned c) {return static_cast<QcomOSM *>(p)->fController.stateCount(c);}
uint32_t QcomOSM::policyFrequency(void *p,unsigned c,unsigned row) {return static_cast<QcomOSM *>(p)->fController.state(c,row,false);}
uint32_t QcomOSM::policyCurrent(void *p,unsigned c) {return static_cast<QcomOSM *>(p)->fController.rate(c);}
uint32_t QcomOSM::policyCeiling(void *p,unsigned c) {return static_cast<QcomOSM *>(p)->fController.limit(c);}
IOReturn QcomOSM::policySet(void *p,unsigned c,uint32_t hz) {return convert(static_cast<QcomOSM *>(p)->fController.setRate(c,hz));}
void QcomOSM::startPolicy()
{
    if (!fHandoffAccepted) {
        setProperty("CPUAutomaticPerformanceStartStatus",static_cast<UInt32>(kIOReturnNotReady),32);
        setProperty("CPUAutomaticPerformanceAvailable",false);return;
    }
    setProperty("CPUAutomaticPerformanceStartStatus",static_cast<UInt32>(kIOReturnBadArgument),32);
    const auto *topology=ml_get_topology_info();
    if (!topology || topology->version!=CPU_TOPOLOGY_VERSION || !topology->num_cpus || topology->num_cpus>8) {
        setProperty("CPUAutomaticPerformanceAvailable",false);return;
    }
    CPUPerformanceAdapter::Mapping mapping[8]={};
    unsigned clusterIDs[2]={UINT32_MAX,UINT32_MAX},counts[2]={};
    for (unsigned i=0;i<topology->num_cpus;++i) {
        const auto &cpu=topology->cpus[i];
        // OSM bank follows the physical MPIDR cluster, never an assumption
        // that logical CPUs 0..3 and 4..7 belong to particular clusters.
        unsigned bank=(cpu.phys_id>>8)&255;
        if ((cpu.phys_id&~0x103ULL) || bank>1 || cpu.cpu_id>=CPUPerformance::MaxCPUs ||
            ++counts[bank]>4 || (clusterIDs[bank]!=UINT32_MAX && clusterIDs[bank]!=cpu.cluster_id)) {
            setProperty("CPUAutomaticPerformanceAvailable",false);return;
        }
        clusterIDs[bank]=cpu.cluster_id;mapping[i]={cpu.cpu_id,bank};
    }
    if (counts[0] && counts[1] && clusterIDs[0]==clusterIDs[1]) {setProperty("CPUAutomaticPerformanceAvailable",false);return;}
    auto *policy=new CPUPerformanceAdapter;
    CPUPerformanceAdapter::Backend backend={this,policyOnline,policyCount,policyFrequency,policyCurrent,policyCeiling,policySet};
    bool initialized=policy && policy->init();
    bool started=initialized && policy->start(getWorkLoop(),backend,mapping,topology->num_cpus,2);
    setProperty("CPUAutomaticPerformanceStartStatus",static_cast<UInt32>(initialized ? policy->startStatus() : kIOReturnNoMemory),32);
    if (!started) {
        if (policy) {policy->stop();policy->release();}
        setProperty("CPUAutomaticPerformanceAvailable",false);return;
    }
    fPolicy=policy;
    setProperty("CPUAutomaticPerformanceAvailable",true);
    setProperty("CPUPerformancePolicy","XNU-perfcontrol-load-QoS");
    setProperty("CPUPerformanceSamplingPeriodUS",5000,32);
    setProperty("CPUPerformanceLoweringDelayUS",30000,32);
    setProperty("CPUPerformanceQoSHoldUS",20000,32);
}
void QcomOSM::stop(IOService *provider)
{
    if(fPolicy) fPolicy->stop();
    super::stop(provider);
}
void QcomOSM::free()
{
    if(fPolicy) fPolicy->stop();
    for (auto &map:fClockMaps) OSSafeReleaseNULL(map);
    OSSafeReleaseNULL(fPolicy);super::free();
}
IOReturn QcomOSM::snapshot(OSObject *owner,void *output,void *,void *,void *)
{
    auto *self=static_cast<QcomOSM *>(owner);
    if (!self->controllerOnline()) return kIOReturnNotReady;
    char text[2048]={};unsigned used=0;
    const auto io=self->hardwareIO();
    for (unsigned c=0;c<2;++c) {
        const uint64_t begin=mach_absolute_time();
        const uint32_t control=io.read(io.context,0,c*0x2000+0x1f00);
        const uint32_t cycles=io.read(io.context,0,c*0x2000+0x1f04);
        const uint32_t index=io.read(io.context,0,c*0x2000+0x1f10);
        const uint32_t state=io.read(io.context,0,c*0x2000+0x1c00);
        const uint32_t pc=io.read(io.context,0,c*0x2000+0x1c74);
        QcomOSMClock::State clock{};
        auto *map=self->fClockMaps[c];
        if (map && map->getLength()==0x8000) {
            const auto base=map->getVirtualAddress();
            clock=QcomOSMClock::observe([&](uint32_t off) {
                const uint32_t value=*reinterpret_cast<volatile uint32_t *>(base+off);
                __asm__ volatile("dmb oshld" ::: "memory"); return value;
            });
        }
        const auto *clocks=clock.words;
        const uint64_t end=mach_absolute_time();
        // Read on demand under the policy's work-loop gate. This neither
        // changes policy nor resets the free-running 32-bit hardware counter.
        // Desired frequency is a request; counter deltas are separate evidence.
        auto *policy=self->fPolicy;
        int n=snprintf(text+used,sizeof(text)-used,
            "CPU_OSM_SNAPSHOT version=3 cluster=%u begin=%llu end=%llu cycle_control=0x%x cycles=%u index=%u desired_hz=%u minimum_hz=%u maximum_hz=%u ceiling_hz=%u policy_status=0x%x utilization_permille=%u demand=%u transitions=%llu dropped=%llu osm_state=0x%x osm_pc=0x%x clock_valid=%u clock_stable=%u rcgr_cmd=0x%x rcgr_cfg=0x%x pll_mode=0x%x pll_l=0x%x pll_word8=0x%x pll_user=0x%x pll_status=0x%x local_pll_configured_hz=%u\n",
            c,begin,end,control,cycles,index,self->fController.rate(c),
            self->fController.minimum(c),self->fController.maximum(c),self->fController.limit(c),
            policy ? policy->status(c) : CPUPerformanceAdapter::Unavailable,
            policy ? policy->utilization(c) : 0,policy ? policy->demand(c) : 0,
            policy ? policy->transitions(c) : 0,policy ? policy->dropped(c) : 0,state,pc,
            map && map->getLength()==0x8000,clock.stable,clocks[0],clocks[1],clocks[2],clocks[3],clocks[4],clocks[5],clocks[6],clock.localPLLHz());
        if (n<0 || static_cast<unsigned>(n)>=sizeof(text)-used) return kIOReturnNoSpace;
        used+=static_cast<unsigned>(n);
    }
    auto *value=OSString::withCString(text);
    *static_cast<OSObject **>(output)=value;
    return value ? kIOReturnSuccess : kIOReturnNoMemory;
}
OSObject *QcomOSM::copyProperty(const char *key) const
{
    if (!key || strcmp(key,"CPUPerformanceSnapshot")) return super::copyProperty(key);
    auto *loop=getWorkLoop();OSObject *result=nullptr;
    if (loop) loop->runAction(snapshot,const_cast<QcomOSM *>(this),&result);
    return result;
}
void QcomOSM::publishPowerStatus()
{
    UInt32 percent = 100;
    for (unsigned i = 0; i < 2; ++i) {
        UInt32 max = fController.maximum(i), limit = fController.limit(i);
        if (!max || !limit) return; // no invented CPU status on uninitialized OSM.
        UInt32 value = static_cast<UInt32>(UInt64(limit)*100/max);
        if (value < percent) percent = value;
    }
    setProperty(kIOPMCPUPowerLimitProcessorSpeedKey,percent,32);
    IOPMrootDomain *root = getPMRootDomain(); if (!root) return;
    // Preserve another policy/thermal provider's dictionary if it owns the
    // standard status. This driver publishes only its own ceiling, not TSENS.
    OSObject *snapshot = root->copyProperty(kIOPMRootDomainPowerStatusKey);
    auto *status = OSDynamicCast(OSDictionary,snapshot);
    auto *existing = status ? OSDynamicCast(OSDictionary,status->getObject(kIOPMCPUPowerLimitsKey)) : nullptr;
    auto *owner = existing ? OSDynamicCast(OSNumber,existing->getObject("PowerPolicyProvider")) : nullptr;
    bool occupied = existing && (!owner || owner->unsigned64BitValue() != getRegistryEntryID());
    OSSafeReleaseNULL(snapshot);
    if (occupied) return;
    OSDictionary *limits = OSDictionary::withCapacity(2);
    OSNumber *speed = OSNumber::withNumber(percent,32), *identity = OSNumber::withNumber(getRegistryEntryID(),64);
    const OSSymbol *event = OSSymbol::withCStringNoCopy(kIOPMCPUPowerLimitsKey);
    if (limits && speed && identity && event && limits->setObject(kIOPMCPUPowerLimitProcessorSpeedKey,speed) &&
        limits->setObject("PowerPolicyProvider",identity)) root->systemPowerEventOccurred(event,limits);
    OSSafeReleaseNULL(limits); OSSafeReleaseNULL(speed); OSSafeReleaseNULL(identity); OSSafeReleaseNULL(event);
}
IOReturn QcomOSM::execute(ClockController::Request &r)
{
    using namespace ClockController;
    if (r.operation == GetResourceCount) { r.value = 2; return kIOReturnSuccess; }
    if (r.operation == GetResourceID) { if (r.id > 1) return kIOReturnBadArgument; r.value = r.id; return kIOReturnSuccess; }
    if (r.operation == GetPerformanceStateFrequency || r.operation == GetPerformanceStateVoltage) {
        r.value = fController.state(r.id >> 16,r.id & 0xffff,r.operation == GetPerformanceStateVoltage);
        return r.value ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    if (r.id > 1) return kIOReturnBadArgument;
    if (r.operation == GetName) { r.name = r.id ? "perfcl_clk" : "pwrcl_clk"; return kIOReturnSuccess; }
    switch (r.operation) {
    case GetPerformanceStateCount: r.value = fController.stateCount(r.id); break;
    case GetRate: r.value = fController.rate(r.id); break;
    case GetVoltage: r.value = fController.voltage(r.id); break;
    case GetMinimumRate: r.value = fController.minimum(r.id); break;
    case GetMaximumRate: r.value = fController.maximum(r.id); break;
    case GetPerformanceLimit: r.value = fController.limit(r.id); break;
    case SetRate: {
        if (!fHandoffAccepted) return kIOReturnNotReady;
        auto result=convert(fController.setRate(r.id,r.value));
        if(result==kIOReturnSuccess && fPolicy) fPolicy->manual(r.id);
        return result;
    }
    case SetAutomaticPerformance:
        if(r.value>1) return kIOReturnBadArgument;
        return fPolicy ? fPolicy->setAutomatic(r.id,r.value!=0) : kIOReturnNotReady;
    case GetAutomaticPerformance:
        r.value=fPolicy && (fPolicy->status(r.id)&CPUPerformanceAdapter::Automatic);return kIOReturnSuccess;
    case GetPerformanceUtilization:
        if(!fPolicy) return kIOReturnNotReady;
        r.value=fPolicy->utilization(r.id);return kIOReturnSuccess;
    case GetPerformanceDemand:
        if(!fPolicy) return kIOReturnNotReady;
        r.value=fPolicy->demand(r.id);return kIOReturnSuccess;
    case GetPerformanceTransitions:
        if(!fPolicy) return kIOReturnNotReady;
        r.value=fPolicy->transitions(r.id);return kIOReturnSuccess;
    case GetPerformanceDroppedSamples:
        if(!fPolicy) return kIOReturnNotReady;
        r.value=fPolicy->dropped(r.id);return kIOReturnSuccess;
    case GetPerformanceStatus:
        r.value=fPolicy ? fPolicy->status(r.id) : CPUPerformanceAdapter::Unavailable;return kIOReturnSuccess;
    case SetPerformanceLimit: {
        if (!fHandoffAccepted) return kIOReturnNotReady;
        auto result = fController.setLimit(r.id, r.value); r.value = fController.limit(r.id);
        if (result == QcomPower::OK) publishPowerStatus();
        return convert(result);
    }
    default: return kIOReturnUnsupported;
    }
    return r.value ? kIOReturnSuccess : kIOReturnNotReady;
}
