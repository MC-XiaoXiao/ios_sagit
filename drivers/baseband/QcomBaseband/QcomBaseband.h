// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <BasebandProtocol.h>
namespace Baseband { class NativeController; }

class QcomBaseband : public IOService {
    OSDeclareDefaultStructors(QcomBaseband);
    struct FirmwareState;
    FirmwareState *fFirmware = nullptr;
    IOWorkLoop *fLoop = nullptr;
    IOCommandGate *fGate = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    Baseband::NativeController *fNative = nullptr;
    bool fActive = false;
    uint32_t fMissing = Baseband::InitialRequirements;
    uint32_t fReportedState = UINT32_MAX, fReportedMissing = UINT32_MAX;
    bool fReportedQuarantined = false;
    static bool validProvider(IOService *);
    static IOReturn gatedStatus(OSObject *, void *, void *, void *, void *);
    static IOReturn gatedStop(OSObject *, void *, void *, void *, void *);
    static IOReturn gatedNativeStart(OSObject *, void *, void *, void *, void *);
    static void nativeIRQ(OSObject *, IOInterruptEventSource *, int);
    static bool nativeWatchdog(OSObject *, IOFilterInterruptEventSource *);
    static void nativeTimer(OSObject *, IOTimerEventSource *);
    void nativeProgress();
    void nativeStatus();
    void cleanup();
public:
    IOService *probe(IOService *, SInt32 *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
    IOReturn status(uint32_t, uint64_t *);
};
class QcomBasebandUserClient : public IOUserClient {
    OSDeclareDefaultStructors(QcomBasebandUserClient);
    QcomBaseband *fController = nullptr;
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(UInt32, IOExternalMethodArguments *, IOExternalMethodDispatch *,
                           OSObject *, void *) APPLE_KEXT_OVERRIDE;
};
