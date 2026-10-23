// SPDX-License-Identifier: GPL-2.0-only
#include "QcomClockBackend.h"
#include <libkern/libkern.h>
#define super QcomPowerService
OSDefineMetaClassAndAbstractStructors(QcomClockBackend, super);
static QcomPower::Status hardwareResult(IOReturn result)
{
    using namespace QcomPower;
    if (result == kIOReturnSuccess) return OK;
    if (result == kIOReturnNotReady || result == kIOReturnNotAttached) return NotReady;
    if (result == kIOReturnUnsupported) return Unsupported;
    if (result == kIOReturnTimeout) return TimedOut;
    if (result == kIOReturnNotPermitted) return Denied;
    return Failed;
}
QcomPower::Status QcomClockBackend::supply(void *context, uint32_t controller, uint32_t id, uint32_t value)
{
    auto *self = static_cast<QcomClockBackend *>(context);
    auto r = ClockController::targeted(static_cast<ClockController::Controller>(controller),
        controller == ClockController::RPM && id >= 0x1000 ? ClockController::SetDomainLevel : ClockController::SetVoltage,
        id, value, self->controllerKind());
    return hardwareResult(self->resourceRequest(r));
}
QcomPower::Status QcomClockBackend::parent(void *context, int source)
{
    auto *self = static_cast<QcomClockBackend *>(context);
    bool divided = source == QcomPower::GPLLDiv;
    if (self->fParentHeld[divided]) return QcomPower::OK;
    UInt32 id = self->fDescription->gpu ? (divided ? 185 : 186) : (divided ? 184 : 178);
    auto r = ClockController::targeted(ClockController::GCC, ClockController::RetainClock, id);
    auto result = hardwareResult(self->resourceRequest(r));
    if (result == QcomPower::OK) self->fParentHeld[divided] = true;
    return result;
}
bool QcomClockBackend::startClock(IOService *provider, const QcomPower::CCDescription &description, ClockController::Controller kind)
{
    const UInt64 addresses[] = {description.gpu ? 0x05065000ULL : 0x0c8c0000ULL,0x00100000};
    const UInt64 sizes[] = {description.gpu ? 0x9000ULL : 0x40000ULL,0x94000};
    if (!mapResources(provider, addresses, sizes, 2) || !fClock.configure(description, hardwareIO(), {this,supply,parent})) return false;
    fDescription = &description;
    unsigned count;
    const UInt8 *p = cells(provider,"qcom,owned-roots",8,count);
    if (count == UINT32_MAX || count > 64) return false;
    for (unsigned i = 0; i < count; ++i) if (!fClock.allowRoot(QcomPower::le32(p+8*i),QcomPower::le32(p+8*i+4))) return false;
    const char *keys[] = {"qcom,owned-clocks","qcom,owned-domains","qcom,owned-resets"};
    for (unsigned k = 0; k < 3; ++k) {
        p = cells(provider,keys[k],4,count);
        if (count == UINT32_MAX || count > 160) return false;
        for (unsigned i = 0; i < count; ++i) {
            UInt32 id = QcomPower::le32(p+4*i);
            bool ok = k == 0 ? fClock.allowClock(id) : k == 1 ? fClock.allowDomain(id) : fClock.allowReset(id);
            if (!ok) return false;
        }
    }
    p = cells(provider,"qcom,supply-points",20,count);
    if (count == UINT32_MAX || count > 192) return false;
    for (unsigned i = 0; i < count; ++i) {
        const UInt8 *row = p+20*i;
        if (!fClock.addSupply({QcomPower::le32(row),QcomPower::le32(row+4),QcomPower::le32(row+8),
            QcomPower::le32(row+12),QcomPower::le32(row+16)})) return false;
    }
    if (!fClock.initialize()) return false;
    setControllerKind(kind,ClockController::Clocks|ClockController::Domains|ClockController::Resets|
        (description.gpu ? ClockController::RetainedGPUState : 0));
    for (unsigned i = 0; i < description.rootCount; ++i)
        if (!addTelemetry(ClockController::GetRate,description.roots[i].id,description.roots[i].name,0)) return false;
    if (!super::start(provider)) return false;
    setProperty("ClockControllerIDSpace",description.gpu ? "qcom,msm8998-gpucc" : "qcom,mmcc-msm8998");
    setProperty("VoltageSequencing","acknowledged-before-clock-rise-after-clock-fall");
    registerService(description.gpu ? kIOServiceSynchronous : 0);
    if(description.gpu)publishResource(ClockController::GPUCCReadyResource,this);
    return true;
}
IOReturn QcomClockBackend::execute(ClockController::Request &r)
{
    using namespace ClockController;
    const auto &d = *fDescription;
    switch (r.operation) {
    case RetainGPUState: {
        const auto status=fClock.retainGPUState(r.value);
        if(status!=QcomPower::OK && !fGPUStateReported && d.gpu) {
            fGPUStateReported=true;
            IOLog("QcomGPUCC: retained state unavailable (%u), state=%x\n",unsigned(status),fClock.stateFlags());
            for(unsigned i=0;i<d.rootCount;++i) {
                uint32_t reg=0,command=0,configuration=0; int parent=0; uint64_t parentHz=0;
                const bool found=fClock.rootState(d.roots[i].id,reg,command,configuration,parent,parentHz);
                IOLog("QcomGPUCC: root=%u found=%u reg=%x cmd=%x cfg=%x parent=%d parent-hz=%llu rate=%llu\n",
                    d.roots[i].id,found,reg,command,configuration,parent,(unsigned long long)parentHz,
                    (unsigned long long)fClock.rate(d.roots[i].id));
            }
            const uint32_t offsets[]={0,4,0xc,0x2c,0x38,0x130,0x1004,0x1008,0x1020,0x1030,0x1034,
                0x1050,0x1054,0x1070,0x1074,0x1090,0x1094,0x1098,0x10a0,0x10a4,
                0x10b0,0x10b4,0x10d0,0x1100,0x1104,0x1120,0x1124};
            const auto io=hardwareIO();
            for(auto offset:offsets)
                IOLog("QcomGPUCC: offset=%x value=%x\n",offset,io.read(io.context,0,offset));
        }
        return convert(status);
    }
    case ValidateGPUState: return convert(fClock.validateGPUState(r.value));
    case ReleaseGPUState: return convert(fClock.validateGPUState(r.value,true));
    case GetName: r.name = fClock.name(r.id); return r.name ? kIOReturnSuccess : kIOReturnUnsupported;
    case FindClock:
        if (!r.name || strnlen(r.name,128) == 128) return kIOReturnBadArgument;
        for (unsigned i = 0; i < d.rootCount; ++i) if (!strcmp(r.name,d.roots[i].name)) { r.id = d.roots[i].id; return kIOReturnSuccess; }
        for (unsigned i = 0; i < d.branchCount; ++i) if (!strcmp(r.name,d.branches[i].name)) { r.id = d.branches[i].id; return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    case GetRate: r.value = fClock.rate(r.id); return r.value ? kIOReturnSuccess : kIOReturnNotReady;
    case SetRate: return convert(fClock.setRate(r.id,r.value));
    case GetMaximumRate: r.value = fClock.maxRate(r.id); return r.value ? kIOReturnSuccess : kIOReturnUnsupported;
    case RetainClock: return convert(fClock.clock(r.id,true));
    case ReleaseClock: return convert(fClock.clock(r.id,false));
    case RetainDomain: return convert(fClock.domain(r.id,true));
    case ReleaseDomain: return convert(fClock.domain(r.id,false));
    case GetDomainState: r.value = fClock.domainState(r.id); return r.value == UINT32_MAX ? kIOReturnNotReady : kIOReturnSuccess;
    case AssertReset: return convert(fClock.reset(r.id,true));
    case DeassertReset: return convert(fClock.reset(r.id,false));
    case GetResourceCount: r.value = d.rootCount+d.branchCount+2*d.pllCount; return kIOReturnSuccess;
    case GetResourceID:
        if (r.id < d.rootCount) r.value = d.roots[r.id].id;
        else if (r.id < d.rootCount+d.branchCount) r.value = d.branches[r.id-d.rootCount].id;
        else if (r.id < d.rootCount+d.branchCount+2*d.pllCount) {
            unsigned i = r.id-d.rootCount-d.branchCount;
            r.value = i&1 ? d.plls[i/2].outputID : d.plls[i/2].id;
        } else return kIOReturnBadArgument;
        return kIOReturnSuccess;
    default: return kIOReturnUnsupported;
    }
}
