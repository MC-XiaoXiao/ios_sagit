// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <GenericSPI.h>
#include "QUPSPI.h"

class QcomSPI : public GenericSPIController {
    OSDeclareDefaultStructors(QcomSPI);
    IOMemoryMap *fMap = nullptr;
    IOService *fOwner = nullptr;
    struct Registers {
        volatile uint8_t *base = nullptr;
        uint32_t read32(uint32_t);
        void write32(uint32_t, uint32_t);
        void delayUS(unsigned);
        uint64_t nowUS();
    } fRegisters;
    QUPSPI::Engine<Registers> fEngine{fRegisters};
protected:
    SPI::Result execute(SPI::Transaction &) APPLE_KEXT_OVERRIDE;
    bool accepts(const SPI::Configuration &) const APPLE_KEXT_OVERRIDE;
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
};
