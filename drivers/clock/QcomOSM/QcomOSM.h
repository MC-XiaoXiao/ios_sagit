// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerService.h>
#include "OSMController.h"
#include <CPUPerformanceAdapter.h>
class QcomOSM : public QcomPowerService {
    OSDeclareDefaultStructors(QcomOSM);
    QcomPower::OSMController fController;
    CPUPerformanceAdapter *fPolicy=nullptr;
    IOMemoryMap *fClockMaps[2]={};
    bool fHandoffAccepted=false;
    void publishPowerStatus();
    void startPolicy();
    static bool policyOnline(void *);
    static uint32_t policyCount(void *,unsigned);
    static uint32_t policyFrequency(void *,unsigned,unsigned);
    static uint32_t policyCurrent(void *,unsigned);
    static uint32_t policyCeiling(void *,unsigned);
    static IOReturn policySet(void *,unsigned,uint32_t);
    static IOReturn snapshot(OSObject *,void *,void *,void *,void *);
protected:
    IOReturn execute(ClockController::Request &) APPLE_KEXT_OVERRIDE;
public:
    OSObject *copyProperty(const char *) const APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
};
