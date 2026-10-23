// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include "USBMMIO.h"
#include "USBPlatform.h"
#include "DWC3Core.h"
#include "USBPower.h"

class QcomUSB;

// A generation-scoped IOKit provider, published with name usb-host or
// usb-device. Native Family adapters attach by ordinary personality matching.
// Only one adapter can open it. Invalidation closes admission before hardware
// halt; failed halt keeps this nub, its client and the platform lease pinned.
class QcomUSBController : public IOService {
    OSDeclareDefaultStructors(QcomUSBController);
    friend class QcomUSB;
    QcomUSB *fBus = nullptr;
    IOWorkLoop *fHardwareLoop = nullptr;
    IOMapper *fMapper = nullptr;
    USB::PlatformContract fResources{};
    USB::ConnectorState fRoute{};
    IOSimpleLock *fLock = nullptr;
    IOService *fClient = nullptr;
    USB::Role fRole = USB::Role::None;
    UInt64 fEpoch = 0, fAdmissionEpoch = 0;
    UInt32 fHostFrameCounter = 0;
    bool fRetiring = false, fIsolated = false, fDMARetired = false;
    bool prepare(QcomUSB *, USB::Role, UInt64, const USB::ConnectorState &);
    void invalidate();
    void isolated();
    void dmaRetired();
    static IOReturn hardwareAction(OSObject *, void *, void *, void *, void *);
protected:
    bool handleOpen(IOService *, IOOptionBits, void *) APPLE_KEXT_OVERRIDE;
    void handleClose(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    bool handleIsOpen(const IOService *) const APPLE_KEXT_OVERRIDE;
public:
    void free() APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    bool accepts(IOService *, UInt64) const;
    bool dmaIsolated() const;
    bool dmaRecoveryReady() const;
    void reportFault(IOService *,UInt64);
    UInt64 epoch() const { return fEpoch; }
    USB::Role role() const { return fRole; }
    USB::PlatformContract resourceContract() const;
    IOMapper *copyDeviceMapper() const;
    // Native Host ports/controllers keep their own workloops. This synchronous
    // bridge executes only hardware work on the retained role workloop, after
    // checking admission again there. Use MMIO only during this callback and
    // do not call the Family there. Lock order is port -> controller -> role;
    // return before invoking a client completion or acquiring a port gate.
    using HardwareAction = IOReturn (*)(IOService *, USB::MMIO &, void *);
    IOReturn runHardwareAction(IOService *, UInt64, HardwareAction, void * = nullptr);
    // Called only under getWorkLoop's gate, and only while accepts is true.
    USB::MMIO *registers(IOService *, UInt64) const;
    DWC3::Core<USB::MMIO> *deviceCore(IOService *, UInt64) const;
    // Arm only on the role workloop after the HCD starts. Reads take only
    // the IRQ-safe admission lock, so Native frame queries never wait for a
    // workloop. Invalidate/close/disarm revoke this lease before halt/unmap.
    IOReturn armHostFrameCounter(IOService *, UInt64, UInt32 runtimeOffset);
    void disarmHostFrameCounter(IOService *, UInt64);
    IOReturn readHostFrameCounter(IOService *, UInt64, UInt32 &) const;
};

class QcomUSB : public IOService {
    OSDeclareDefaultStructors(QcomUSB);
    friend class QcomUSBController;
    friend class USB::RoleSwitch<QcomUSB>;
    friend class USB::ControllerPower<QcomUSB>;
    IOService *fPlatform = nullptr;
    IOService *fResources = nullptr;
    IOWorkLoop *fLoop = nullptr;
    IOCommandGate *fGate = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    IOMemoryMap *fCoreMap = nullptr, *fScratchMap = nullptr;
    IOMapper *fMapper = nullptr;
    const OSSymbol *fFunction = nullptr;
    USB::MMIO *fRegisters = nullptr, *fScratch = nullptr;
    DWC3::Core<USB::MMIO> *fCore = nullptr;
    USB::RoleSwitch<QcomUSB> *fRoles = nullptr;
    USB::ControllerPower<QcomUSB> *fPower = nullptr;
    QcomUSBController *fNub = nullptr;
    thread_call_t fWorker = nullptr;
    thread_call_t fPowerWorker = nullptr;
    IOLock *fPowerLock = nullptr;
    IONotifier *fSleepNotifier = nullptr;
    void *fSleepReference = nullptr;
    IOSimpleLock *fNotifyLock = nullptr;
    USB::PlatformContract fContract{};
    USB::ConnectorState fTarget{};
    USB::ConnectorFence fConnector;
    UInt64 fLease = 0;
    bool fOpened = false, fGateAdded = false, fTimerAdded = false;
    bool fReady = false, fStopping = false, fHardwareOwned = false, fPinned = false, fRoleIsolated = false;
    bool fPM = false, fSystemSleeping = false, fPowerWanted = true, fSleepUnsafe = false, fPowerAck = false;

    // RoleSwitch runs exclusively in fWorker. Platform functions and
    // synchronous service termination run outside the work-loop gate.
    USB::Result source(bool);
    USB::Result safePower(USB::Role, const USB::ConnectorState &);
    USB::Result configure(USB::Role, const USB::ConnectorState &);
    USB::Result start(USB::Role, UInt64, const USB::ConnectorState &);
    bool stop(USB::Role, UInt64);
    IOReturn platform(USB::PlatformRequest &);
    bool retireNub();
    bool cleanup();
    void pinFault();
    void schedule();
    void disableScheduling();
    bool stopping() const;
    bool acceptsConnection(const USB::ConnectorState &, UInt64 admissionEpoch = 0) const;
    void controllerFault(const USB::ConnectorState &,UInt64 admissionEpoch);
    UInt64 connectionEpoch() const;
    IOReturn refreshMapper();
    void closePowerAdmission();
    void drainRoleWork();
    USB::Result parkRole();
    USB::Result suspendResources();
    USB::Result resumeResources();
    USB::Result restartRole();
    void powerFault();
    USB::Result changePower(bool);
    static IOReturn sleepInterest(void *,void *,UInt32,IOService *,void *,vm_size_t);
    static void powerWork(void *,void *);
    enum class GateOperation : uintptr_t { Quiesce, RecordIsolation, Configure, AdoptNub, ArmTimer, DisableTimer };
    struct GateArguments { GateOperation operation; USB::Role role; QcomUSBController *nub; };
    IOReturn gated(GateOperation, USB::Role = USB::Role::None, QcomUSBController * = nullptr);
    static IOReturn action(OSObject *, void *, void *, void *, void *);
    static void timer(OSObject *, IOTimerEventSource *);
    static void work(thread_call_param_t, thread_call_param_t);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOReturn message(UInt32, IOService *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
};
