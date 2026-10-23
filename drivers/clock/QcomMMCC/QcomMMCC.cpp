// SPDX-License-Identifier: GPL-2.0-only
#include <QcomClockBackend.h>
#include "ClockTables.h"
class QcomMMCC : public QcomClockBackend {
    OSDeclareDefaultStructors(QcomMMCC);
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        return startClock(provider,QcomMMCCData::description,ClockController::MMCC);
    }
};
OSDefineMetaClassAndStructors(QcomMMCC,QcomClockBackend);
