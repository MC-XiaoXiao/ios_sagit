// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GenericClockController.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <QcomPowerHardware.h>
class QcomPowerService : public GenericClockController {
    OSDeclareAbstractStructors(QcomPowerService);
    IOMemoryMap *fMaps[5] = {};
    static uint32_t read(void *, unsigned, uint32_t);
    static void write(void *, unsigned, uint32_t, uint32_t);
    static void wait(void *, unsigned);
    static void kick(void *);
protected:
    virtual void signal() {}
    bool mapResources(IOService *, const UInt64 *addresses, const UInt64 *sizes, unsigned count);
    QcomPower::IO hardwareIO() { return {this, read, write, wait, kick}; }
    IOReturn resourceRequest(ClockController::TargetedRequest &);
    static IOReturn convert(QcomPower::Status);
    static const UInt8 *cells(IOService *, const char *, unsigned stride, unsigned &count);
public:
    void free() APPLE_KEXT_OVERRIDE;
};
