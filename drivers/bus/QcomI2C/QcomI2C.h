// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <GenericI2C.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "QUPI2C.h"
class QcomI2C : public GenericI2CController {
    OSDeclareDefaultStructors(QcomI2C);
    IOMemoryMap *fMap = nullptr;
    IOService *fClock = nullptr;
    unsigned fClockMask = 0;
    bool clock(unsigned, uint32_t, uint64_t &);
    void releaseClocks();
    struct Registers {
        volatile uint8_t *base = nullptr;
        uint32_t read32(uint32_t);
        void write32(uint32_t, uint32_t);
        void delayUS(unsigned);
        uint64_t nowUS();
    } fRegisters;
    QUPI2C::Engine<Registers> fEngine{fRegisters};
protected:
    I2C::Result execute(I2C::Transaction &) APPLE_KEXT_OVERRIDE;
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
};
