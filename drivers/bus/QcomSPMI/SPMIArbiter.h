// SPDX-License-Identifier: GPL-2.0-only
// MSM8998 PMIC arbiter v2/v3, register format from Linux spmi-pmic-arb.c.
#pragma once
#include <QcomPowerHardware.h>
namespace QcomPower {
class SPMIArbiter : public Registers {
    uint16_t apids[4096] = {};
    uint8_t owners[512] = {};
    bool initialized = false, fault = false;
    unsigned ee = 0;
public:
    bool irqChannel(uint8_t sid, uint8_t peripheral, uint16_t &apid) const {
        if (!initialized || sid > 15) return false;
        unsigned index = (unsigned(sid)<<8) | peripheral;
        if (!apids[index] || owners[apids[index]-1] != ee) return false;
        apid = apids[index]-1; return true;
    }
    uint32_t irqStatus(uint16_t apid) { return apid < 512 ? rd(0x1000U*apid+4, 3)&255 : 0; }
    void irqAccess(uint16_t apid, bool enable) { if (apid < 512) wr(0x1000U*apid, enable ? 1 : 0, 3); }
    void irqClear(uint16_t apid, uint8_t mask) { if (apid < 512) wr(0x1000U*apid+8, mask, 3); }
    Status initialize(IO access, unsigned executionEnvironment) {
        setIO(access);
        if (initialized || executionEnvironment > 5) return Invalid;
        uint32_t version = rd(0);
        if (version < 0x20010000 || version >= 0x40000000) return Unsupported;
        ee = executionEnvironment;
        for (unsigned i = 0; i < 512; ++i) {
            uint32_t entry = rd(0x800+4*i);
            if (!entry) continue;
            unsigned ppid = (entry >> 8) & 0xfff;
            if (apids[ppid]) return Invalid;
            apids[ppid] = static_cast<uint16_t>(i+1);
            owners[i] = rd(0x700+4*i, 4)&7;
        }
        initialized = true; return OK;
    }
    Status transfer(uint8_t sid, uint16_t address, uint8_t *data, unsigned count, bool write) {
        if (!initialized) return NotReady;
        if (fault) return Stale;
        if (sid > 15 || !data || !count || count > 8 || (address&255)+count > 256) return Invalid;
        unsigned ppid = (unsigned(sid)<<8) | (address>>8);
        if (!apids[ppid]) return Unsupported;
        unsigned apid = apids[ppid]-1, bank = write ? 1 : 2;
        if (write && owners[apid] != ee) return Denied;
        uint32_t offset = 0x8000*apid+0x1000*ee;
        if (write) for (unsigned i = 0; i < count; i += 4) {
            uint32_t word = 0;
            for (unsigned j = 0; j < 4 && i+j < count; ++j) word |= uint32_t(data[i+j]) << (8*j);
            wr(offset+0x10+i, word, bank);
        }
        wr(offset, (write ? 0U : 1U<<27) | ((address&255)<<4) | (count-1), bank);
        for (unsigned i = 0; i <= 1000; ++i) {
            uint32_t status = rd(offset+8, bank);
            if (status&1) {
                if (status&4) return Denied;
                if (status&10) return Failed;
                if (!write) for (unsigned n = 0; n < count; ++n)
                    data[n] = static_cast<uint8_t>(rd(offset+0x18+(n&~3U),bank) >> (8*(n&3)));
                return OK;
            }
            if (i != 1000) wait(1);
        }
        fault = true; return TimedOut;
    }
};
}
