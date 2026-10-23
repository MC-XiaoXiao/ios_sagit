// SPDX-License-Identifier: BSD-2-Clause
#include "QualcommWatchdogTimer.h"
#include "WatchdogHardware.h"
#include <IOKit/IOMessage.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

#define super IOWatchdog
OSDefineMetaClassAndStructors(QualcommWatchdogTimer, super);

bool QualcommWatchdogTimer::start(IOService *provider)
{
    auto *memory = provider->getDeviceMemoryWithIndex(0);
    auto *clock = OSDynamicCast(OSData, provider->getProperty("clock-frequency"));
    UInt32 frequency = 0;
    if (clock && clock->getLength() == sizeof(frequency))
        memcpy(&frequency, clock->getBytesNoCopy(), sizeof(frequency));
    if (!memory || memory->getPhysicalAddress() != 0x17817000 ||
        memory->getLength() != 0x1000 || provider->getDeviceMemoryWithIndex(1) ||
        frequency != QcomWatchdogHardware::Frequency) return false;
    fMap = provider->mapDeviceMemoryWithIndex(0);
    if (!fMap || !fMap->getVirtualAddress()) return false;
    fRegisters = reinterpret_cast<volatile UInt32 *>(fMap->getVirtualAddress());
    fLoop = IOWorkLoop::workLoop();
    fTimer = IOTimerEventSource::timerEventSource(this, tick);
    if (!fLoop || !fTimer || fLoop->addEventSource(fTimer) != kIOReturnSuccess) return false;
    PMinit(); fPMStarted = true;
    static IOPMPowerState states[2] = {
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
    };
    provider->joinPMtree(this); registerPowerDriver(this, states, 2);
    int selfTest = 0;
    PE_parse_boot_argn("qcom-watchdog-test", &selfTest, sizeof(selfTest));
    fSelfTest = selfTest != 0;
    UInt32 captureSeconds=0;
    if(PE_i_can_has_debugger(nullptr) &&
        PE_parse_boot_argn("watchdog-capture-seconds",&captureSeconds,sizeof(captureSeconds)) &&
        captureSeconds>=60 && captureSeconds<=3600) {
        UInt64 interval=0;nanoseconds_to_absolutetime(UInt64(captureSeconds)*1000000000ULL,&interval);
        fCaptureDeadline=mach_absolute_time()+interval;
        setProperty("WatchdogCaptureSeconds",captureSeconds,32);
    }
    // Take ownership of the bridge's watchdog before the framework configures it.
    QcomWatchdogHardware::disable(*this);
    setProperty("WatchdogTimeoutSeconds", 31, 32);
    setProperty("WatchdogFeedIntervalSeconds", 5, 32);
    setProperty("WatchdogSelfTest", fSelfTest);
    // Native start registers the IOWatchdog service, creates its locks and
    // userspace-monitor state, publishes policy and calls enableWatchdog().
    if (!super::start(provider, 0, 0)) { disableWatchdog(); return false; }
    fFrameworkStarted = true;
    // Native wdt=-1 disables watchdogd monitoring for an explicitly requested
    // diagnostic boot. Keep an independent, bounded hardware recovery guard.
    if (fCaptureDeadline && !socWatchdogEnabled() && !fSelfTest) {
        fCaptureActive = true;
        setProperty("WatchdogCaptureGuard", true);
        enableWatchdog();
        kprintf("QualcommWatchdogTimer: bounded diagnostic guard armed; native userspace monitoring disabled\n");
    }
    IOLog("QualcommWatchdogTimer: native IOWatchdog framework active\n");
    return true;
}

void QualcommWatchdogTimer::write(uint32_t offset, uint32_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    fRegisters[offset/4] = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

void QualcommWatchdogTimer::tick(OSObject *owner, IOTimerEventSource *timer)
{
    auto *self = OSDynamicCast(QualcommWatchdogTimer, owner);
    if (!self || !self->fRunning) return;
    if(self->fCaptureDeadline && mach_absolute_time()>=self->fCaptureDeadline) {
        // A bounded unattended capture still recovers if userspace stalls:
        // leave the real 31-second SoC watchdog armed and stop feeding it.
        self->setProperty("WatchdogCaptureExpired",true);
        kprintf("QualcommWatchdogTimer: capture window expired; SoC watchdog recovery pending\n");
        return;
    }
    // Apple hardware invokes this policy check from its watchdog interrupt.
    // Our polled backend does so before each feed. Missing watchdogd checkins
    // retain the framework's native timeout/panic path.
    if (self->fFrameworkStarted && !self->fCaptureActive) self->checkForTimeout();
    self->resetWatchdog();
    // If scheduling fails, leave the hardware armed so the machine recovers.
    timer->setTimeoutMS(5000);
}

void QualcommWatchdogTimer::enableWatchdog()
{
    if (!socWatchdogEnabled() && !fCaptureActive) return;
    fRunning = true;
    QcomWatchdogHardware::arm(*this, 31);
    setProperty("WatchdogEnabled", true);
    IOLog("QualcommWatchdogTimer: armed 31 seconds, %s\n", fSelfTest ? "self-test (no feeding)" : "framework checks and feeds every 5 seconds");
    if (!fSelfTest) fTimer->setTimeoutMS(5000);
}

void QualcommWatchdogTimer::resetWatchdog()
{
    if (!fRunning || fSelfTest) return;
    QcomWatchdogHardware::feed(*this);
    if (++fFeeds == 1) IOLog("QualcommWatchdogTimer: framework feeding active\n");
    setProperty("WatchdogFeedCount", fFeeds, 64);
}

void QualcommWatchdogTimer::disableWatchdog()
{
    // Closing the native client must not remove the unattended capture guard.
    if (fCaptureActive) return;
    fRunning = false;
    if (fTimer) fTimer->cancelTimeout();
    if (fRegisters) QcomWatchdogHardware::disable(*this);
    setProperty("WatchdogEnabled", false);
}

void QualcommWatchdogTimer::systemWillShutdown(IOOptionBits specifier)
{
    if (specifier == kIOMessageSystemWillPowerOff) {
        fCaptureActive = false;
        disableWatchdog(); // A powered-off device must not be restarted by its watchdog.
        IOLog("QualcommWatchdogTimer: disabled for system power-off\n");
    } else if (specifier == kIOMessageSystemWillRestart) {
        fCaptureActive = false;
        // Keep a hardware fallback if the firmware reset request does not finish.
        fRunning = false; fTimer->cancelTimeout();
        QcomWatchdogHardware::arm(*this, 5);
        IOLog("QualcommWatchdogTimer: 5-second firmware restart fallback\n");
    }
    IOService::systemWillShutdown(specifier);
}

IOReturn QualcommWatchdogTimer::newUserClient(task_t task, void *security, UInt32 type,
    OSDictionary *properties, IOUserClient **client)
{
    IOReturn status = super::newUserClient(task, security, type, properties, client);
    IOLog("QualcommWatchdogTimer: native user client type=%u status=0x%x\n", type, status);
    return status;
}

void QualcommWatchdogTimer::stop(IOService *provider)
{
    fCaptureActive = false;
    disableWatchdog();
    if (fPMStarted) { PMstop(); fPMStarted = false; }
    IOService::stop(provider);
}

void QualcommWatchdogTimer::free()
{
    fCaptureActive = false;
    disableWatchdog();
    if (fPMStarted) { PMstop(); fPMStarted = false; }
    if (fTimer && fLoop && fTimer->getWorkLoop()) fLoop->removeEventSource(fTimer);
    OSSafeReleaseNULL(fTimer); OSSafeReleaseNULL(fLoop); OSSafeReleaseNULL(fMap);
    fRegisters = nullptr;
    super::free();
}
