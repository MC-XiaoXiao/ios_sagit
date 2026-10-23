// SPDX-License-Identifier: GPL-2.0-only
// QUP v2 FIFO PIO, derived from the register contract in Linux spi-qup.c.
#pragma once
#include <SPIProtocol.h>

namespace QUPSPI {
enum Register : uint32_t {
    Config=0, State=4, Mode=8, Reset=12, Operational=0x18, Errors=0x1c,
    ErrorEnable=0x20, OpMask=0x28, Version=0x30, OutputCount=0x100,
    OutFIFO=0x110, WriteCount=0x150, InputCount=0x200, ReadCount=0x208,
    InFIFO=0x218, SPIConfig=0x300, IOControl=0x304, SPIErrors=0x308,
    SPIErrorEnable=0x30c
};
// IO provides ordered read32/write32, delayUS and monotonic nowUS.
// The firmware owns clock/pin/rail setup; an exclusive retained-resource handoff
// is required. Clock frequency is fixed for this controller's entire lifetime.
template<class IO> class Engine {
    IO &io;
    uint32_t frequency = 0, capacity = 0;
    uint64_t deadline = 0;
    bool fault = false;
    bool wait(uint32_t reg, uint32_t mask, uint32_t value) {
        for (;;) {
            if ((io.read32(reg) & mask) == value) return true;
            if (io.nowUS() >= deadline) return false;
            io.delayUS(1);
        }
    }
    bool state(uint32_t value) {
        if (!wait(State, 4, 4)) return false;
        const uint32_t old = io.read32(State);
        if ((old & 3) == 3 && value == 0) {
            io.write32(State, 2); io.write32(State, 2);
        } else io.write32(State, (old & ~3U) | value);
        return wait(State, 7, value | 4);
    }
    SPI::Result finish(SPI::Result result) {
        // Deassert CS even after a timeout. Reset/discard all partial FIFO data.
        io.write32(IOControl, io.read32(IOControl) & ~0x800U);
        io.write32(Reset, 1);
        // Separate bounded cleanup budget; failure latches the bus offline.
        deadline = io.nowUS() + 100;
        if (!wait(State, 7, 4)) { fault = true; return SPI::Result::IO; }
        return result;
    }
public:
    explicit Engine(IO &registers) : io(registers) {}
    bool configure(uint32_t hz) {
        if (frequency || hz < 100000 || hz > 1000000 ||
            (io.read32(Version) >> 28) != 2 || (io.read32(State) & 7) != 4) return false;
        const uint32_t mode = io.read32(Mode);
        const unsigned blocks[] = {4, 16, 32};
        const unsigned out = mode & 3, in = (mode >> 5) & 3;
        if (out > 2 || in > 2) return false;
        const unsigned outWords = blocks[out] * (2U << ((mode >> 2) & 7)) / 4;
        const unsigned inWords = blocks[in] * (2U << ((mode >> 7) & 7)) / 4;
        capacity = outWords < inWords ? outWords : inWords;
        frequency = hz;
        return true;
    }
    bool accepts(const SPI::Configuration &c) const {
        return SPI::valid(c) && c.frequency == frequency && !fault;
    }
    SPI::Result transfer(SPI::Transaction &t) {
        if (!SPI::valid(t)) return SPI::Result::Invalid;
        if (!frequency || fault) return SPI::Result::NotReady;
        if (!accepts(t.config) || t.length > capacity) return SPI::Result::Unsupported;
        if ((io.read32(State) & 7) != 4) {
            // Do not reset a controller which is unexpectedly owned/active.
            fault = true;
            return SPI::Result::NotReady;
        }
        uint8_t received[SPI::MaxBytes]{};
        deadline = io.nowUS() + t.timeoutUS;
        io.write32(Reset, 1);
        if (!state(0)) return finish(SPI::Result::Timeout);
        io.write32(Config, 0x107); // SPI mini-core, N=7, input + output
        io.write32(Mode, io.read32(Mode) & ~0xfc00U); // FIFO, no pack/unpack
        io.write32(ErrorEnable, 0); io.write32(SPIErrorEnable, 0);
        io.write32(OpMask, 0x300);
        io.write32(Errors, 0x3c); io.write32(SPIErrors, 3);
        io.write32(SPIConfig, t.config.mode == 0 ? 0x200 : 0);
        io.write32(IOControl, 1 | (t.config.chipSelect << 2) |
                   (t.config.mode == 3 ? 0x400 : 0));
        io.write32(OutputCount, 0); io.write32(InputCount, 0);
        io.write32(WriteCount, t.length); io.write32(ReadCount, t.length);
        if (!state(1) || !state(3)) return finish(SPI::Result::Timeout);
        io.write32(Operational, 0x100);
        for (size_t i = 0; i < t.length; ++i) {
            if (io.read32(Operational) & 0x40) return finish(SPI::Result::IO);
            // Unpacked SPI TX is left aligned; RX is right aligned.
            io.write32(OutFIFO, uint32_t(t.tx ? t.tx[i] : 0) << 24);
        }
        io.write32(IOControl, io.read32(IOControl) | 0x800); // hold native CS
        if (!state(1)) return finish(SPI::Result::Timeout);
        size_t count = 0;
        for (;;) {
            if ((io.read32(Errors) & 0x3c) || (io.read32(SPIErrors) & 3))
                return finish(SPI::Result::IO);
            uint32_t op = io.read32(Operational);
            if ((op & 0x20) && count < t.length) {
                received[count++] = uint8_t(io.read32(InFIFO));
                continue;
            }
            if (op & 0x300) io.write32(Operational, op & 0x300);
            if (count == t.length && (op & 0xc00) == 0xc00) break;
            if (io.nowUS() >= deadline) return finish(SPI::Result::Timeout);
            io.delayUS(1);
        }
        auto result = finish(SPI::Result::Success);
        if (result == SPI::Result::Success && t.rx)
            for (size_t i = 0; i < t.length; ++i) t.rx[i] = received[i];
        return result;
    }
};
}
