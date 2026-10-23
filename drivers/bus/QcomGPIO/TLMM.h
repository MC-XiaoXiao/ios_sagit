// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "TLMMPins.h"
#include <InputIRQState.h>
namespace QcomTLMM {
enum Trigger : uint32_t { Rising = 1, Falling = 2, Both = 3, High = 4, Low = 8 };
inline bool validTrigger(uint32_t t) { return t == Rising || t == Falling || t == Both || t == High || t == Low; }
inline bool edge(uint32_t t) { return (t & 3) != 0; }
using InputIRQ::SummaryGuard;
inline uint32_t le32(const void *bytes) {
    const auto *p = static_cast<const uint8_t *>(bytes);
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
// All register operations are serialized by the IOKit controller lock.
template<class IO> class Controller {
    IO &io;
public:
    explicit Controller(IO &access) : io(access) {}
    bool read(unsigned pin, bool &value) {
        if (pin >= PinCount) return false;
        value = io.read32(PinBase[pin]+4)&1; return true;
    }
    bool configure(unsigned pin, unsigned pull, unsigned drive, bool output, bool value) {
        if (pin >= PinCount || pull > 3 || drive > 7) return false;
        uint32_t base = PinBase[pin], ctl = io.read32(base);
        // Program output data before enabling the driver, preventing glitches.
        if (output) set(pin, value);
        ctl &= ~0x3ffU;
        ctl |= pull | (drive<<6) | (output ? 1U<<9 : 0);
        io.write32(base, ctl); return true;
    }
    bool set(unsigned pin, bool value) {
        if (pin >= PinCount) return false;
        uint32_t offset = PinBase[pin]+4, old = io.read32(offset);
        io.write32(offset, (old & ~2U) | (value ? 2 : 0)); return true;
    }
    bool configureIRQ(unsigned pin, uint32_t trigger) {
        if (pin >= PinCount || !validTrigger(trigger)) return false;
        uint32_t offset = PinBase[pin]+8, old = io.read32(offset);
        // KPSS target=3. RAW_STATUS_EN stays set for edge sources even masked.
        uint32_t cfg = (old & ~0xffU) | (3U<<5) | (1U<<4);
        if (edge(trigger)) cfg |= (trigger<<2) | 2;
        else if (trigger == High) cfg |= 2;
        io.write32(offset, cfg);
        ack(pin); return true;
    }
    void mask(unsigned pin, uint32_t trigger) {
        uint32_t offset = PinBase[pin]+8, cfg = io.read32(offset) & ~1U;
        if (!edge(trigger)) cfg &= ~(1U<<4);
        io.write32(offset, cfg);
    }
    void unmask(unsigned pin) {
        uint32_t offset = PinBase[pin]+8;
        io.write32(offset, io.read32(offset) | 0x11);
    }
    bool pending(unsigned pin) { return io.read32(PinBase[pin]+12)&1; }
    bool routedPending(unsigned pin) {
        uint32_t cfg=io.read32(PinBase[pin]+8);
        return (cfg&1) && ((cfg>>5)&7)==3 && pending(pin);
    }
    // MSM8998 uses write-zero-to-clear, unlike later TLMM implementations.
    void ack(unsigned pin) { io.write32(PinBase[pin]+12, 0); }
};
}
