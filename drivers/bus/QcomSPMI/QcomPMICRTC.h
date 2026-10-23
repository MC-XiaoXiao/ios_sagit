// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/rtc/IORTCController.h>
#include <IOKit/IOLocks.h>
#include "PMICRTC.h"
class QcomPMICRTC : public IORTC {
    OSDeclareDefaultStructors(QcomPMICRTC);
    IOSimpleLock *fLock = nullptr;
    QcomPower::RTCCalendar fCalendar;
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    long getGMTTimeOfDay() APPLE_KEXT_OVERRIDE;
    void setGMTTimeOfDay(long) APPLE_KEXT_OVERRIDE;
    void getUTCTimeOfDay(clock_sec_t *, clock_nsec_t *) APPLE_KEXT_OVERRIDE;
    void setUTCTimeOfDay(clock_sec_t, clock_nsec_t) APPLE_KEXT_OVERRIDE;
    void setAlarmEnable(IOOptionBits) APPLE_KEXT_OVERRIDE;
};
