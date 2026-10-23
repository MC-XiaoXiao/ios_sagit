// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerService.h>
#include "SPMIRegulators.h"
class QcomSPMI : public QcomPowerService {
    OSDeclareDefaultStructors(QcomSPMI);
    QcomPower::SPMIRegulators fRegulators;
    IOService *fRTC = nullptr;
    UInt32 fRTCSID = 0, fRTCBase = 0;
    UInt32 fRTCBootSeconds = 0;
    UInt64 fRTCBootUptime = 0;
    bool fRTCBootReady = false;
    IOCommandGate *fBusGate = nullptr;
    bool fBusAdded = false;
    IOService *fRetainedBusClient = nullptr;
    static IOReturn busAction(OSObject *, void *, void *, void *, void *);
    void closeBus();
protected:
    IOReturn execute(ClockController::Request &) APPLE_KEXT_OVERRIDE;
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    bool copyRTCBootSample(UInt32 &, UInt64 &) const;
    // Serialized with regulator traffic on the controller's native workloop.
    IOReturn transfer(UInt8 sid, UInt16 address, UInt8 *, unsigned count, bool write);
    // A child with a retained-resource handoff can keep raw bus transactions
    // usable in the software-only low PM state. Does not enable any PMIC rail.
    IOReturn retainBus(IOService *child, bool acquire);
    bool irqChannel(UInt8 sid, UInt8 peripheral, UInt16 &apid) const;
    UInt32 irqStatus(UInt16 apid);
    void irqAccess(UInt16 apid, bool enable);
    void irqClear(UInt16 apid, UInt8 mask);
    void free() APPLE_KEXT_OVERRIDE;
};
