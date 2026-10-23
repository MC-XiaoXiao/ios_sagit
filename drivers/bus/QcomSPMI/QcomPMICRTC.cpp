// SPDX-License-Identifier: GPL-2.0-only
#include "QcomPMICRTC.h"
#include "QcomSPMI.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#define super IORTC
OSDefineMetaClassAndStructors(QcomPMICRTC, super);

static uint64_t rtcUptime()
{
    uint64_t ticks, ns;
    clock_get_uptime(&ticks);
    absolutetime_to_nanoseconds(ticks, &ns);
    return ns;
}

bool QcomPMICRTC::start(IOService *provider)
{
    IOLog("QcomPMICRTC: starting IORTC provider\n");
    auto *spmi = OSDynamicCast(QcomSPMI, provider);
    if (!spmi || !super::start(provider)) return false;
    fLock = IOSimpleLockAlloc();
    if (!fLock) return false;
    UInt32 seconds = 0;
    UInt64 uptime = 0;
    if (!spmi->copyRTCBootSample(seconds, uptime)) {
        IOLog("QcomPMICRTC: PM8998 boot sample unavailable; IORTC not published\n");
        return false;
    }
    fCalendar.set(seconds, 0, uptime);
    setProperty("RTCSource", "PM8998-SPMI-counter/XNU-uptime");
    setProperty("RTCRawBootSeconds", seconds, 32);
    setProperty("RTCPersistentSetSupported", false);
    setProperty("RTCAlarmSupported", false);
    setProperty("RTCVendorOffsetImported", false);
    registerService();
    // Publish the real provider only after its clock backend is ready.
    publishResource("IORTC", this);
    IOLog("QcomPMICRTC: IORTC ready, raw PMIC seconds=%u (vendor UTC offset not imported)\n", seconds);
    return true;
}

void QcomPMICRTC::getUTCTimeOfDay(clock_sec_t *secs, clock_nsec_t *nsecs)
{
    uint64_t seconds = 0;
    uint32_t ns = 0;
    auto state = IOSimpleLockLockDisableInterrupt(fLock);
    bool valid = fCalendar.get(rtcUptime(), seconds, ns);
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    *secs = valid ? seconds : 0; *nsecs = valid ? ns : 0;
}

void QcomPMICRTC::setUTCTimeOfDay(clock_sec_t secs, clock_nsec_t nsecs)
{
    // Clock corrections are volatile; preserve Android/firmware PMIC state.
    auto state = IOSimpleLockLockDisableInterrupt(fLock);
    fCalendar.set(secs, nsecs, rtcUptime());
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
}
long QcomPMICRTC::getGMTTimeOfDay() { clock_sec_t s; clock_nsec_t n; getUTCTimeOfDay(&s, &n); return s; }
void QcomPMICRTC::setGMTTimeOfDay(long s) { if (s >= 0) setUTCTimeOfDay(s, 0); }
void QcomPMICRTC::setAlarmEnable(IOOptionBits enable) { if (enable) IOLog("QcomPMICRTC: wake alarms unsupported\n"); }
bool QcomPMICRTC::requestTerminate(IOService *, IOOptionBits) { return false; }
void QcomPMICRTC::stop(IOService *provider)
{
    if (getResourceService()->getProperty("IORTC") == this)
        getResourceService()->removeProperty("IORTC");
    super::stop(provider);
}
void QcomPMICRTC::free()
{
    if (fLock) IOSimpleLockFree(fLock);
    super::free();
}
