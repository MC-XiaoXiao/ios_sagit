// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerService.h>
#include "RPMTransport.h"
#include "RPMRegulators.h"
#include "RPMInterconnect.h"
#include "RPMClockHandoff.h"
#include "RPMModemClocks.h"
class QcomRPM : public QcomPowerService {
    OSDeclareDefaultStructors(QcomRPM);
    QcomPower::RPMTransport fTransport;
    QcomPower::RPMRegulators fRegulators;
    QcomPower::RPMInterconnect fInterconnect;
    QcomPower::RPMClockHandoff fClockHandoff;
    QcomPower::RPMModemClocks fModemClocks;
    bool fBusOwned=false;
    UInt32 fUSBClockHz=0;
    UInt64 fGPUSerial=0, fGPULease=0, fGPUFloor[4]{};
    bool fGPUFault=false;
    IOReturn gpuState(ClockController::Request &);
    QcomPower::Status initializeResources(bool bus);
    QcomPower::Status validateUSBClockBaseline();
protected:
    void signal() APPLE_KEXT_OVERRIDE;
    IOReturn execute(ClockController::Request &) APPLE_KEXT_OVERRIDE;
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
};
