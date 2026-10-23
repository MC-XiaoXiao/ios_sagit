// SPDX-License-Identifier: GPL-2.0-only
#include "QcomGCC.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>

#define super GenericClockController
OSDefineMetaClassAndStructors(QcomGCC, super);

uint32_t QcomGCC::readRegister(void *context, uint32_t offset)
{
    auto *self = static_cast<QcomGCC *>(context);
    uint32_t value = self->fRegisters[offset / 4];
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
void QcomGCC::writeRegister(void *context, uint32_t offset, uint32_t value)
{
    auto *self = static_cast<QcomGCC *>(context);
    __asm__ volatile("dmb oshst" ::: "memory");
    self->fRegisters[offset / 4] = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
void QcomGCC::delay(void *, unsigned us) { IODelay(us); }

static uint32_t little32(const void *bytes)
{
    const auto *p = static_cast<const uint8_t *>(bytes);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool QcomGCC::configureOwnership(IOService *provider)
{
    const char *keys[] = {"qcom,owned-clocks", "qcom,owned-roots", "qcom,owned-domains", "qcom,owned-resets"};
    bool any = false;
    for (unsigned k = 0; k < 4; ++k) {
        OSObject *object = provider->getProperty(keys[k]);
        if (!object) continue;
        auto *data = OSDynamicCast(OSData, object);
        unsigned stride = k < 2 ? 8 : 4;
        if (!data || !data->getLength() || data->getLength() % stride) return false;
        any = true;
        const auto *p = static_cast<const uint8_t *>(data->getBytesNoCopy());
        for (unsigned off = 0; off < data->getLength(); off += stride) {
            uint32_t id = little32(p + off);
            bool ok = k == 0 ? fController.allowClock(id, little32(p + off + 4)) :
                k == 1 ? fController.allowRate(id, little32(p + off + 4)) :
                k == 2 ? fController.allowDomain(id) : fController.allowReset(id);
            if (!ok) return false; // Unknown or duplicate ownership is a loader error.
        }
    }
    // A contract is required for writes, but observation is useful during
    // bring-up without one. The loader must not copy an ownership template.
    auto *version = OSDynamicCast(OSData, provider->getProperty("qcom,gcc-handoff-version"));
    if ((any || provider->getProperty("qcom,gcc-handoff-version")) &&
        (!version || version->getLength() != 4 || little32(version->getBytesNoCopy()) != 1)) return false;
    // Dedicated MMCC/GPUCC input votes are owned by this common GCC service,
    // so separate controllers never race RMW on GCC's shared vote register.
    for (const auto &branch : GCC::branches) if (branch.pllDivider) fController.allowClock(branch.id);
    return true;
}

bool QcomGCC::start(IOService *provider)
{
    // MSM8998's GCC aperture. Don't infer register addresses from an Apple
    // gate array or permit an arbitrary DT MMIO window to select another IP.
    auto *memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory || memory->getPhysicalAddress() != 0x00100000 || memory->getLength() != 0x94000 ||
        provider->getDeviceMemoryWithIndex(1)) return false;
    fMap = provider->mapDeviceMemoryWithIndex(0);
    if (!fMap || fMap->getLength() < 0x94000 || !fMap->getVirtualAddress()) return false;
    fRegisters = reinterpret_cast<volatile UInt32 *>(fMap->getVirtualAddress());
    if (!fController.setIO({this, readRegister, writeRegister, delay}) ||
        !configureOwnership(provider) || !fController.initialize()) return false;
    for (const auto &root : GCC::roots) if (!addTelemetry(ClockController::GetRate,root.id,root.name,0)) return false;
    setControllerKind(ClockController::GCC,ClockController::Clocks | ClockController::Domains |
        ClockController::Resets | ClockController::ClockHandoff | ClockController::RetainedGPUState);
    if (!super::start(provider)) return false;
    setProperty("ClockControllerIDSpace", "qcom,gcc-msm8998");
    setProperty("SharedPLLAndVoltageOwner", "firmware-rpm");
    setProperty("SystemSuspendSupported", false);
    registerService(kIOServiceSynchronous);
    publishResource(ClockController::GCCReadyResource,this);
    return true;
}

IOReturn QcomGCC::execute(ClockController::Request &r)
{
    using namespace ClockController;
    GCC::Status status = GCC::Success;
    const bool modemMutation=(GCC::Controller::modemClock(r.id) &&
        (r.operation==RetainClock || r.operation==ReleaseClock || r.operation==SetRate || r.operation==AdoptClock)) ||
        (r.id==108 && (r.operation==AssertReset || r.operation==DeassertReset));
    if(modemMutation) {
        const bool owner=r.version==2 && reinterpret_cast<TargetedRequest &>(r).consumer==BasebandConsumer &&
            !reinterpret_cast<TargetedRequest &>(r).flags;
        if((fController.modemStateHeld() && !owner) || (owner && !fController.modemStateHeld()))
            return kIOReturnNotPermitted;
    }
    if(fController.uartStateHeld() && (((r.id==14 || r.id==53) && r.operation==SetRate) ||
        (r.id==53 && (r.operation==RetainClock || r.operation==ReleaseClock || r.operation==AdoptClock)) ||
        (r.id==30 && (r.operation==AssertReset || r.operation==DeassertReset)))) {
        if(r.version!=2 || reinterpret_cast<TargetedRequest &>(r).consumer!=BluetoothConsumer ||
            reinterpret_cast<TargetedRequest &>(r).flags)return kIOReturnNotPermitted;
    }
    switch (r.operation) {
    case RetainModemState: status=fController.retainModemState(r.value); break;
    case ValidateModemState: status=fController.validateModemState(r.value); break;
    case ReleaseModemState: status=fController.validateModemState(r.value,true); break;
    case RetainGPUState: status=fController.retainGPUState(r.value); break;
    case ValidateGPUState: status=fController.validateGPUState(r.value); break;
    case ReleaseGPUState: status=fController.validateGPUState(r.value,true); break;
    case GetResourceCount: r.value = GCC::RootCount+GCC::BranchCount; return kIOReturnSuccess;
    case GetResourceID:
        if (r.id >= GCC::RootCount+GCC::BranchCount) return kIOReturnBadArgument;
        r.value = r.id < GCC::RootCount ? GCC::roots[r.id].id : GCC::branches[r.id-GCC::RootCount].id;
        return kIOReturnSuccess;
    case FindClock:
        if (!r.name || strnlen(r.name, 128) == 128) return kIOReturnBadArgument;
        for (const auto &root : GCC::roots) if (!strcmp(root.name, r.name)) {
            r.id = root.id; return kIOReturnSuccess;
        }
        for (const auto &branch : GCC::branches) if (!strcmp(branch.name, r.name)) {
            r.id = branch.id; return kIOReturnSuccess;
        }
        return kIOReturnUnsupported;
    case GetName: r.name = fController.name(r.id); return r.name ? kIOReturnSuccess : kIOReturnUnsupported;
    case GetRate: r.value = fController.rate(r.id); return r.value ? kIOReturnSuccess : kIOReturnNotReady;
    case SetRate: status = fController.setRate(r.id, r.value); break;
    case RetainClock: status = fController.clock(r.id, true); break;
    case ReleaseClock: status = fController.clock(r.id, false); break;
    case RetainDomain: status = fController.domain(r.id, true); break;
    case ReleaseDomain: status = fController.domain(r.id, false); break;
    case GetDomainState:
        r.value = fController.domainState(r.id);
        return r.value == 0xffffffffU ? kIOReturnNotReady : kIOReturnSuccess;
    case AssertReset: status = fController.reset(r.id, true); break;
    case DeassertReset: status = fController.reset(r.id, false); break;
    case GetResetState:
        r.value=fController.resetState(r.id);
        return r.value==UINT32_MAX ? kIOReturnUnsupported : kIOReturnSuccess;
    case AdoptClock:
        if (r.version!=2 || r.value || reinterpret_cast<TargetedRequest &>(r).flags)
            return kIOReturnNotPermitted;
        if (reinterpret_cast<TargetedRequest &>(r).consumer==USBControllerConsumer)
            status = fController.adoptUSBClock(r.id);
        else if (reinterpret_cast<TargetedRequest &>(r).consumer==USB2PHYConsumer ||
                 reinterpret_cast<TargetedRequest &>(r).consumer==USB3PHYConsumer)
            status = fController.adoptUSBPHYClock(r.id,
                reinterpret_cast<TargetedRequest &>(r).consumer==USB3PHYConsumer);
        else if (reinterpret_cast<TargetedRequest &>(r).consumer==BluetoothConsumer)
            status = fController.adoptUARTClock(r.id,r.value);
        else return kIOReturnNotPermitted;
        break;
    case RestoreClock:status=fController.restoreUARTClock(r.id,r.value);break;
    default: return kIOReturnBadArgument;
    }
    if (status!=GCC::Success && r.version==2 &&
        reinterpret_cast<TargetedRequest &>(r).consumer==USBControllerConsumer) {
        const UInt32 offsets[]={0xf000,0xf004,0xf008,0xf00c,0xf010,0xf014,0xf018,0xf028,0xf02c,0x5018,0x82024};
        UInt32 snapshot[22]{};
        for (unsigned i=0;i<11;++i) {snapshot[2*i]=offsets[i];snapshot[2*i+1]=readRegister(this,offsets[i]);}
        setProperty("GCCUSBFailureRegisters",snapshot,sizeof(snapshot));
    }
    switch (status) {
    case GCC::Success: return kIOReturnSuccess;
    case GCC::BadArgument: return kIOReturnBadArgument;
    case GCC::Unsupported: return kIOReturnUnsupported;
    case GCC::NotReady: return kIOReturnNotReady;
    case GCC::Busy: return kIOReturnBusy;
    case GCC::Timeout: return kIOReturnTimeout;
    case GCC::Overflow: return kIOReturnNoResources;
    case GCC::IOFailure: return kIOReturnIOError;
    }
    return kIOReturnError;
}

void QcomGCC::free()
{
    OSSafeReleaseNULL(fMap); fRegisters = nullptr;
    super::free();
}
