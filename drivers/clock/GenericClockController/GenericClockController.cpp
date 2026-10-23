// SPDX-License-Identifier: BSD-2-Clause
#include "GenericClockController.h"
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSSymbol.h>
#include <machine/machine_routines.h>
#include "PowerUserClient.h"

#define super IOService
OSDefineMetaClassAndAbstractStructors(GenericClockController, super);

void GenericClockController::setControllerKind(ClockController::Controller kind, UInt64 capabilities)
{
    fKind = kind; fCapabilities = capabilities;
}

bool GenericClockController::addTelemetry(ClockController::Operation op, UInt32 id, const char *name, UInt32 group)
{
    if (fAdded || fTelemetryCount == 192 || group > 2 || !name) return false;
    UInt64 channel = 0x51434f4d00000000ULL | (UInt64(group) << 24) | id;
    for (unsigned i = 0; i < fTelemetryCount; ++i) if (fTelemetry[i].channel == channel) return false;
    fTelemetry[fTelemetryCount++] = {channel, op, id, group, name}; return true;
}

bool GenericClockController::startReports()
{
    const IOReportUnit units[] = {
        __IOR_MAKEUNIT(kIOReportQuantityFrequency, kIOReportScaleUnity),
        __IOR_MAKEUNIT(kIOReportQuantityVoltage, kIOReportScaleMicro),
        kIOReportUnitNone, kIOReportUnitEvents
    };
    const char *groups[] = {"Clock configured frequency", "Regulator voltage", "RPM requested level", "Power transactions"};
    for (unsigned i = 0; i < 4; ++i) {
        fReports[i] = IOSimpleReporter::with(this, kIOReportCategoryPower, units[i]);
        if (!fReports[i]) return false;
    }
    for (unsigned i = 0; i < fTelemetryCount; ++i) {
        const auto &t = fTelemetry[i];
        if (fReports[t.group]->addChannel(t.channel, t.name) != kIOReturnSuccess ||
            fReports[t.group]->setValue(t.channel, -1) != kIOReturnSuccess) return false;
    }
    if (fReports[3]->addChannel(0x51434f4dff000001ULL, "Transactions") != kIOReturnSuccess ||
        fReports[3]->addChannel(0x51434f4dff000002ULL, "Failures") != kIOReturnSuccess) return false;
    for (unsigned i = 0; i < 4; ++i) {
        bool used = i == 3;
        for (unsigned j = 0; j < fTelemetryCount; ++j) used |= fTelemetry[j].group == i;
        if (used && IOReportLegend::addReporterLegend(this, fReports[i], groups[i], getName()) != kIOReturnSuccess) return false;
    }
    setProperty("PowerTelemetryUnavailableValue", static_cast<UInt64>(-1), 64);
    return true;
}

bool GenericClockController::start(IOService *provider)
{
    if (!super::start(provider)) return false;
    fLoop = IOWorkLoop::workLoop();
    if (fLoop) fGate = IOCommandGate::commandGate(this, dispatch);
    if (!fGate || fLoop->addEventSource(fGate) != kIOReturnSuccess) {
        stop(provider); return false;
    }
    fAdded = true;
    if (!setProperty(ClockController::Marker, true)) { stop(provider); return false; }
    if (!setProperty("ClockControllerKind", fKind, 32) ||
        !setProperty("PowerControlProtocolVersion", 2, 32)) { stop(provider); return false; }
    setProperty("PowerControlCapabilities", fCapabilities | ClockController::UserAccess, 64);
    // Both states retain clocks, voltage and register context. The lower state
    // stops new transactions; system suspend is owned by the platform expert.
    IOPMPowerState states[2] = {};
    states[0].version = states[1].version = 1;
    states[0].outputPowerCharacter = states[0].inputPowerRequirement = kIOPMPowerOn;
    states[1].capabilityFlags = kIOPMDeviceUsable;
    states[1].outputPowerCharacter = states[1].inputPowerRequirement = kIOPMPowerOn;
    fOnline = true;
    PMinit(); fPM = true;
    provider->joinPMtree(this);
    if (registerPowerDriver(this, states, 2) != kIOReturnSuccess) { stop(provider); return false; }
    changePowerStateTo(1);
    if (!startReports()) { stop(provider); return false; }
    fCapabilities |= ClockController::Reporting;
    setProperty("PowerControlCapabilities", fCapabilities | ClockController::UserAccess, 64);
    return true;
}

IOReturn GenericClockController::dispatch(OSObject *owner, void *arg, void *power, void *, void *)
{
    auto *self = static_cast<GenericClockController *>(owner);
    if (power) { self->fOnline = reinterpret_cast<uintptr_t>(arg) != 0; return kIOReturnSuccess; }
    if (!self->fOnline) return kIOReturnOffline;
    auto &r = *static_cast<ClockController::Request *>(arg);
    if (r.operation == ClockController::GetCapabilities) {
        r.value = self->fCapabilities | ClockController::UserAccess; return kIOReturnSuccess;
    }
    IOReturn result = self->execute(r);
    ++self->fTransactions;
    if (result != kIOReturnSuccess) ++self->fFailures;
    return result;
}

IOReturn GenericClockController::request(ClockController::Request &r)
{
    if (!ClockController::valid(r)) return kIOReturnBadArgument;
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    if ((r.version == 1 && fKind != ClockController::GCC) || (r.version == 2 &&
        reinterpret_cast<ClockController::TargetedRequest &>(r).controller != fKind)) return kIOReturnUnsupported;
    if (r.operation != ClockController::FindClock) r.name = nullptr;
    return fAdded ? fGate->runAction(dispatch, &r) : kIOReturnNotReady;
}

IOReturn GenericClockController::callPlatformFunction(const OSSymbol *name, bool wait,
    void *p1, void *p2, void *p3, void *p4)
{
    if (!name) return kIOReturnBadArgument;
    if (!name->isEqualTo(ClockController::Function))
        return super::callPlatformFunction(name, wait, p1, p2, p3, p4);
    auto *r = static_cast<ClockController::Request *>(p1);
    if (!r || p2 || p3 || p4) return kIOReturnBadArgument;
    return request(*r);
}

IOReturn GenericClockController::setPowerState(unsigned long state, IOService *)
{
    if (state > 1) return kIOPMNoSuchState;
    if (fAdded) fGate->runAction(dispatch, reinterpret_cast<void *>(state), reinterpret_cast<void *>(1));
    return IOPMAckImplied;
}

IOWorkLoop *GenericClockController::getWorkLoop() const { return fLoop; }
bool GenericClockController::requestTerminate(IOService *, IOOptionBits) { return false; }
void GenericClockController::stop(IOService *provider)
{
    if (fAdded) fGate->runAction(dispatch, nullptr, reinterpret_cast<void *>(1));
    if (fPM) { PMstop(); fPM = false; }
    if (fAdded) { fLoop->removeEventSource(fGate); fAdded = false; }
    super::stop(provider);
}
void GenericClockController::free()
{
    if (fAdded) { fLoop->removeEventSource(fGate); fAdded = false; }
    OSSafeReleaseNULL(fGate); OSSafeReleaseNULL(fLoop);
    for (auto &report : fReports) OSSafeReleaseNULL(report);
    super::free();
}

IOReturn GenericClockController::refreshReports(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<GenericClockController *>(owner);
    for (unsigned i = 0; i < self->fTelemetryCount; ++i) {
        const auto &t = self->fTelemetry[i];
        auto r = ClockController::targeted(self->fKind, t.operation, t.id);
        IOReturn result = self->fOnline ? self->execute(r.request) : kIOReturnOffline;
        self->fReports[t.group]->setValue(t.channel, result == kIOReturnSuccess && r.request.value <= INT64_MAX ?
            static_cast<SInt64>(r.request.value) : -1);
    }
    self->fReports[3]->setValue(0x51434f4dff000001ULL, static_cast<SInt64>(self->fTransactions));
    self->fReports[3]->setValue(0x51434f4dff000002ULL, static_cast<SInt64>(self->fFailures));
    return kIOReturnSuccess;
}
IOReturn GenericClockController::configureReport(IOReportChannelList *channels, IOReportConfigureAction action,
    void *result, void *destination)
{
    if (!channels || !result || channels->nchannels > 1024) return kIOReturnBadArgument;
    IOReturn status = super::configureReport(channels, action, result, destination);
    for (auto *report : fReports) if (report) {
        IOReturn r = report->configureReport(channels, action, result, destination);
        if (r != kIOReturnSuccess) status = r;
    }
    return status;
}
IOReturn GenericClockController::updateReport(IOReportChannelList *channels, IOReportUpdateAction action,
    void *result, void *destination)
{
    if (!channels || !result || channels->nchannels > 1024) return kIOReturnBadArgument;
    if (fAdded && action == kIOReportCopyChannelData) fGate->runAction(refreshReports);
    IOReturn status = super::updateReport(channels, action, result, destination);
    for (auto *report : fReports) if (report) {
        IOReturn r = report->updateReport(channels, action, result, destination);
        if (r != kIOReturnSuccess) status = r;
    }
    return status;
}

IOReturn GenericClockController::newUserClient(task_t task, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
    if (!handler || type != 0) return kIOReturnBadArgument;
    *handler = nullptr;
    auto *client = new PowerUserClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, securityID, type, properties) || !client->attach(this)) {
        client->release(); return kIOReturnNotPermitted;
    }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
    *handler = client; return kIOReturnSuccess;
}
