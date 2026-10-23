// SPDX-License-Identifier: GPL-2.0-only
#include <QcomClockBackend.h>
#include "ClockTables.h"
class QcomGPUCC : public QcomClockBackend {
    OSDeclareDefaultStructors(QcomGPUCC);
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        return startClock(provider,QcomGPUCCData::description,ClockController::GPUCC);
    }
};
OSDefineMetaClassAndStructors(QcomGPUCC,QcomClockBackend);
