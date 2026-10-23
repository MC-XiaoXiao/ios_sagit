// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <GenericClockController.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "GCCController.h"

class QcomGCC : public GenericClockController {
    OSDeclareDefaultStructors(QcomGCC);
    IOMemoryMap *fMap = nullptr;
    volatile UInt32 *fRegisters = nullptr;
    GCC::Controller fController;
    static uint32_t readRegister(void *, uint32_t);
    static void writeRegister(void *, uint32_t, uint32_t);
    static void delay(void *, unsigned);
    bool configureOwnership(IOService *);
protected:
    IOReturn execute(ClockController::Request &) APPLE_KEXT_OVERRIDE;
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
};
