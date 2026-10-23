// SPDX-License-Identifier: GPL-2.0-only
#include "QcomSPMI.h"
#include "QcomPMICRTC.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#define super QcomPowerService
OSDefineMetaClassAndStructors(QcomSPMI, super);
bool QcomSPMI::start(IOService *provider)
{
    IOLog("QcomSPMI: starting %s\n", provider->getName());
    const UInt64 addresses[] = {0x0800f000,0x08400000,0x09400000,0x0a400000,0x0800a000};
    const UInt64 sizes[] = {0x1000,0x1000000,0x1000000,0x220000,0x3000};
    if (!mapResources(provider, addresses, sizes, 5)) return false;
    auto access = hardwareIO();
    IOLog("QcomSPMI: initializing arbiter\n");
    auto initialized = fRegulators.initialize(access, 0);
    if (initialized != QcomPower::OK) {
        IOLog("QcomSPMI: arbiter initialization failed (%u)\n", initialized);
        return false;
    }
    IOLog("QcomSPMI: arbiter ready\n");
    unsigned count; const UInt8 *config = cells(provider, "qcom,pm8005-regulators", 20, count);
    if (count == UINT32_MAX || count > 4) return false;
    for (unsigned i = 0; i < count; ++i)
        if (!fRegulators.configure(QcomPower::le32(config+20*i), QcomPower::le32(config+20*i+4),
            QcomPower::le32(config+20*i+8), QcomPower::le32(config+20*i+12), QcomPower::le32(config+20*i+16))) return false;
    const UInt8 *gpu = cells(provider,"qcom,gpu-gfx-handoff",sizeof(QcomPower::GPURegulatorHandoff),count);
    if(count==UINT32_MAX || count>1)return false;
    if(gpu) {
        QcomPower::GPURegulatorHandoff h{};memcpy(&h,gpu,sizeof(h));
        if(fRegulators.configureGPU(h)!=QcomPower::OK)return false;
    }
    const UInt8 *rtc = cells(provider, "qcom,pmic-rtc", 8, count);
    if (count == UINT32_MAX || count > 1) return false;
    if (rtc) {
        fRTCSID = QcomPower::le32(rtc); fRTCBase = QcomPower::le32(rtc+4);
        if (fRTCSID > 15 || fRTCBase != 0x6000) return false;
        IOLog("QcomSPMI: reading PM8998 RTC boot sample\n");
        // No consumers or controller workloop exist yet. This boot-only read
        // cannot race regulator transactions or depend on a PM command gate.
        auto result = QcomPower::readPMICRTC(fRegulators.bus(), fRTCSID, fRTCBase, fRTCBootSeconds);
        if (result == QcomPower::OK) {
            UInt64 ticks;
            clock_get_uptime(&ticks);
            absolutetime_to_nanoseconds(ticks, &fRTCBootUptime);
            fRTCBootReady = true;
            IOLog("QcomSPMI: PM8998 RTC boot seconds=%u\n", fRTCBootSeconds);
        } else IOLog("QcomSPMI: PM8998 RTC boot read failed (%u)\n", result);
    }
    setControllerKind(ClockController::SPMI, ClockController::Voltage | (gpu?ClockController::RetainedGPUState:0));
    const char *names[] = {"pm8005_s1 register voltage","pm8005_s2 register voltage","pm8005_s3 register voltage","pm8005_s4 register voltage"};
    for (unsigned i = 0; i < 4; ++i) if (!addTelemetry(ClockController::GetVoltage,i+1,names[i],1)) return false;
    IOLog("QcomSPMI: starting controller services\n");
    if (!super::start(provider)) return false;
    fBusGate = IOCommandGate::commandGate(this, busAction);
    if (!fBusGate || getWorkLoop()->addEventSource(fBusGate)!=kIOReturnSuccess) {
        stop(provider); return false;
    }
    fBusAdded = true;
    IOLog("QcomSPMI: controller services ready\n");
    setProperty("ClockControllerIDSpace", "qcom,pm8005-regulators");
    setProperty("VoltageObservation", "SPMI-register-readback");
    if (fRTCBase) {
        auto *clock = new QcomPMICRTC;
        if (clock && clock->init() && clock->attach(this)) {
            if (clock->start(this)) fRTC = clock;
            else { clock->stop(this); clock->detach(this); clock->release(); }
        } else if (clock) clock->release();
        if (!fRTC) IOLog("QcomSPMI: configured PMIC RTC unavailable\n");
    }
    registerService(); return true;
}
bool QcomSPMI::copyRTCBootSample(UInt32 &seconds, UInt64 &uptime) const
{
    if (!fRTCBootReady) return false;
    seconds = fRTCBootSeconds; uptime = fRTCBootUptime;
    return true;
}
void QcomSPMI::stop(IOService *provider)
{
    closeBus();
    if (fRTC) { fRTC->stop(this); fRTC->detach(this); OSSafeReleaseNULL(fRTC); }
    super::stop(provider);
}
void QcomSPMI::free()
{
    closeBus();
    OSSafeReleaseNULL(fBusGate); super::free();
}
void QcomSPMI::closeBus()
{
    if (!fBusAdded) return;
    fBusGate->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<QcomSPMI *>(owner);
        self->fBusAdded = false;
        auto *previous = self->fRetainedBusClient;
        self->fRetainedBusClient = nullptr;
        OSSafeReleaseNULL(previous);
        return kIOReturnSuccess;
    });
    getWorkLoop()->removeEventSource(fBusGate);
}
namespace {
struct BusRequest { UInt8 sid; UInt16 address; UInt8 *data; unsigned count; bool write; };
}
IOReturn QcomSPMI::busAction(OSObject *owner, void *arg, void *childArg, void *acquire, void *)
{
    auto *self=static_cast<QcomSPMI *>(owner);
    if (!self->fBusAdded) return kIOReturnNotReady;
    if (childArg) {
        auto *child = static_cast<IOService *>(childArg);
        if (acquire) {
            if (self->fRetainedBusClient && self->fRetainedBusClient != child) return kIOReturnExclusiveAccess;
            if (!self->fRetainedBusClient) { child->retain(); self->fRetainedBusClient = child; }
        } else {
            if (self->fRetainedBusClient != child) return kIOReturnNotOpen;
            auto *previous = self->fRetainedBusClient;
            self->fRetainedBusClient = nullptr;
            previous->release();
        }
        return kIOReturnSuccess;
    }
    if (!self->controllerOnline() && !self->fRetainedBusClient) return kIOReturnOffline;
    auto &r=*static_cast<BusRequest *>(arg);
    if(r.write && !self->fRegulators.rawWriteAllowed(r.sid,r.address))return kIOReturnBusy;
    return convert(self->fRegulators.bus().transfer(r.sid,r.address,r.data,r.count,r.write));
}
IOReturn QcomSPMI::retainBus(IOService *child, bool acquire)
{
    if (!child || child->getProvider() != this) return kIOReturnBadArgument;
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    return fBusAdded ? fBusGate->runAction(busAction, nullptr, child,
        acquire ? reinterpret_cast<void *>(1) : nullptr) : kIOReturnNotReady;
}
IOReturn QcomSPMI::transfer(UInt8 sid, UInt16 address, UInt8 *data, unsigned count, bool write)
{
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    if (!fBusAdded) return kIOReturnNotReady;
    BusRequest r{sid,address,data,count,write}; return fBusGate->runAction(busAction,&r);
}
bool QcomSPMI::irqChannel(UInt8 sid, UInt8 peripheral, UInt16 &apid) const
{ return fRegulators.bus().irqChannel(sid,peripheral,apid); }
UInt32 QcomSPMI::irqStatus(UInt16 apid) { return fRegulators.bus().irqStatus(apid); }
void QcomSPMI::irqAccess(UInt16 apid, bool enable) { fRegulators.bus().irqAccess(apid,enable); }
void QcomSPMI::irqClear(UInt16 apid, UInt8 mask) { fRegulators.bus().irqClear(apid,mask); }
IOReturn QcomSPMI::execute(ClockController::Request &r)
{
    using namespace ClockController;
    if(r.operation==RetainGPUState)return convert(fRegulators.retainGPUState(r.value));
    if(r.operation==ValidateGPUState || r.operation==ReleaseGPUState)
        return convert(fRegulators.validateGPUState(r.value,r.operation==ReleaseGPUState));
    if (r.operation == GetResourceCount) { r.value = 4; return kIOReturnSuccess; }
    if (r.operation == GetResourceID) { if (r.id >= 4) return kIOReturnBadArgument; r.value = r.id+1; return kIOReturnSuccess; }
    if (r.id < 1 || r.id > 4) return kIOReturnBadArgument;
    if (r.operation == GetName) {
        const char *names[] = {"pm8005_s1","pm8005_s2","pm8005_s3","pm8005_s4"}; r.name = names[r.id-1]; return kIOReturnSuccess;
    }
    uint32_t value = 0;
    if (r.operation == GetVoltage) { auto result = fRegulators.get(r.id, value); r.value = value; return convert(result); }
    if (r.operation != SetVoltage || r.version != 2 || r.value > UINT32_MAX) return kIOReturnUnsupported;
    auto &target = reinterpret_cast<TargetedRequest &>(r);
    if (target.flags) return kIOReturnUnsupported; // no independent sleep set on a direct AP rail.
    auto result = fRegulators.set(r.id, target.consumer, static_cast<UInt32>(r.value));
    if (result == QcomPower::OK) { result = fRegulators.get(r.id, value); r.value = value; }
    return convert(result);
}
