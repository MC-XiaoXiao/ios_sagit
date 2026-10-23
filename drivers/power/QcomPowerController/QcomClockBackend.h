// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "QcomPowerService.h"
#include <QcomClockHardware.h>
class QcomClockBackend : public QcomPowerService {
    OSDeclareAbstractStructors(QcomClockBackend);
    QcomPower::ClockHardware fClock;
    const QcomPower::CCDescription *fDescription = nullptr;
    bool fParentHeld[2] = {};
    bool fGPUStateReported = false;
    static QcomPower::Status supply(void *, uint32_t, uint32_t, uint32_t);
    static QcomPower::Status parent(void *, int);
protected:
    bool startClock(IOService *, const QcomPower::CCDescription &, ClockController::Controller kind);
    IOReturn execute(ClockController::Request &) APPLE_KEXT_OVERRIDE;
};
