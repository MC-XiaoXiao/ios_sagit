// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <ClockControllerProtocol.h>
#include <IOKit/IOKernelReporters.h>

class GenericClockController : public IOService {
    OSDeclareAbstractStructors(GenericClockController);
    IOWorkLoop *fLoop = nullptr;
    IOCommandGate *fGate = nullptr;
    bool fAdded = false, fPM = false, fOnline = false;
    ClockController::Controller fKind = ClockController::GCC;
    UInt64 fCapabilities = ClockController::Clocks | ClockController::Domains | ClockController::Resets;
    UInt64 fTransactions = 0, fFailures = 0;
    IOSimpleReporter *fReports[4] = {};
    struct Telemetry { UInt64 channel; ClockController::Operation operation; UInt32 id, group; const char *name; };
    Telemetry fTelemetry[192] = {};
    unsigned fTelemetryCount = 0;
    bool startReports();
    static IOReturn refreshReports(OSObject *, void *, void *, void *, void *);
    static IOReturn dispatch(OSObject *, void *, void *, void *, void *);
protected:
    bool controllerOnline() const { return fOnline; }
    virtual IOReturn execute(ClockController::Request &) = 0;
    void setControllerKind(ClockController::Controller kind, UInt64 capabilities);
    IOReturn request(ClockController::Request &);
    bool addTelemetry(ClockController::Operation, UInt32 id, const char *name, UInt32 group);
public:
    ClockController::Controller controllerKind() const { return fKind; }
    IOReturn submit(ClockController::Request &r) { return request(r); }
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
    IOReturn configureReport(IOReportChannelList *, IOReportConfigureAction, void *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn updateReport(IOReportChannelList *, IOReportUpdateAction, void *, void *) APPLE_KEXT_OVERRIDE;
};
