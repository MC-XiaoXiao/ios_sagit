// SPDX-License-Identifier: GPL-2.0-only
// QUP v2 register/tag definitions from Linux i2c-qup.c; see docs/touch/reference.
#pragma once
#include <I2CProtocol.h>

namespace QUPI2C {
enum Register : uint32_t {
    Config=0, State=4, Mode=8, Reset=12, Operational=0x18, Errors=0x1c,
    ErrorEnable=0x20, OpMask=0x28, Version=0x30, OutputCount=0x100,
    OutFIFO=0x110, WriteCount=0x150, InputCount=0x200, ReadCount=0x208,
    InFIFO=0x218, Clock=0x400, Status=0x404, Master=0x408
};
constexpr uint32_t BusErrors = 0x38000fc, CoreErrors = 0x7c;
struct Packet {
    uint8_t tx[I2C::MaxBytes + 4 * I2C::MaxMessages]{};
    uint8_t rx[I2C::MaxBytes + 2 * I2C::MaxMessages]{};
    unsigned txSize = 0, rxSize = 0;
};
inline bool encode(const I2C::Transaction &t, Packet &p) {
    if (!I2C::valid(t)) return false;
    p.txSize = p.rxSize = 0;
    for (unsigned i = 0; i < t.count; ++i) {
        auto &m = t.messages[i];
        const bool read = m.direction == I2C::Direction::Read, last = i + 1 == t.count;
        p.tx[p.txSize++] = 0x81;
        p.tx[p.txSize++] = (t.address << 1) | read;
        p.tx[p.txSize++] = read ? (last ? 0x87 : 0x86) : (last ? 0x83 : 0x82);
        p.tx[p.txSize++] = static_cast<uint8_t>(m.length); // 0 encodes 256
        if (read) p.rxSize += m.length + 2;
        else for (size_t j = 0; j < m.length; ++j) p.tx[p.txSize++] = m.bytes[j];
    }
    return true;
}
inline void decode(const I2C::Transaction &t, const Packet &p) {
    unsigned offset = 0;
    for (unsigned i = 0; i < t.count; ++i) {
        auto &m = t.messages[i];
        if (m.direction != I2C::Direction::Read) continue;
        offset += 2; // QUP's receive tag and count, not sensor data
        for (size_t j = 0; j < m.length; ++j) m.bytes[j] = p.rx[offset++];
    }
}
inline bool clockControl(uint32_t source, uint32_t speed, uint32_t &value) {
    if (source < 1000000 || source > 50000000 || (speed != 100000 && speed != 400000)) return false;
    unsigned ratio = source / speed;
    if (ratio < 8) return false;
    unsigned divider = speed == 100000 ? ratio / 2 - 3 : (ratio - 6) * 2 / 3;
    if (divider > 255) return false;
    value = (3 << 8) | divider;
    if (speed > 100000) value |= (divider / 2) << 16;
    return true;
}

// IO supplies read32/write32, delayUS and monotonic nowUS. All loops use one
// absolute deadline, including error cleanup. No unbounded IRQ waits or DMA.
template<class IO> class Engine {
    IO &io;
    unsigned outBlock = 0, inBlock = 0, outSize = 0, inSize = 0;
    uint32_t clock = 0;
    uint64_t deadline = 0;
    bool expired() { return io.nowUS() >= deadline; }
    bool state(uint32_t desired) {
        while (!(io.read32(State) & 4)) {
            if (expired()) return false;
            io.delayUS(1);
        }
        io.write32(State, desired);
        while ((io.read32(State) & 7) != (desired | 4)) {
            if (expired()) return false;
            io.delayUS(1);
        }
        return true;
    }
    I2C::Result finish(I2C::Result result) {
        io.write32(Reset, 1); // release any partial transaction, discard FIFO
        return result;
    }
public:
    explicit Engine(IO &registers) : io(registers) {}
    bool configure(uint32_t source, uint32_t speed) {
        if (!clockControl(source, speed, clock)) return false;
        const auto mode = io.read32(Mode);
        const unsigned blocks[] = {4, 16, 32};
        unsigned out = mode & 3, in = (mode >> 5) & 3;
        if (out > 2 || in > 2) return false;
        outBlock = blocks[out]; inBlock = blocks[in];
        outSize = outBlock * (2 << ((mode >> 2) & 7));
        inSize = inBlock * (2 << ((mode >> 7) & 7));
        return true;
    }
    I2C::Result transfer(I2C::Transaction &t) {
        Packet packet;
        if (!encode(t, packet)) return I2C::Result::Invalid;
        if (!outSize || !inSize) return I2C::Result::NotReady;
        deadline = io.nowUS() + t.timeoutUS;
        // Never abort another master's in-flight transaction at entry.
        while (io.read32(Status) & 0x100) {
            if (expired()) return I2C::Result::Timeout;
            io.delayUS(10);
        }
        io.write32(Reset, 1);
        if (!state(0)) return finish(I2C::Result::Timeout);
        io.write32(Config, 0x207 | (packet.rxSize ? 0 : 0x80));
        while (!(io.read32(State) & 0x10)) {
            if (expired()) return finish(I2C::Result::Unsupported);
            io.delayUS(1);
        }
        const bool txBlock = packet.txSize > outSize, rxBlock = packet.rxSize > inSize;
        io.write32(Mode, 0xc000 | (txBlock ? 0x400 : 0) | (rxBlock ? 0x1000 : 0));
        io.write32(Master, 1);
        io.write32(ErrorEnable, 0); // polling backend: QUP IRQ remains masked
        // PIO needs service flags even when polled. Masking bits 8/9 is a BAM
        // setting: on MSM8998 it suppresses FIFO service while data is ready.
        // The controller's GIC interrupt remains unregistered/masked.
        io.write32(OpMask, 0);
        io.write32(Errors, CoreErrors); io.write32(Status, BusErrors);
        io.write32(OutputCount, txBlock ? packet.txSize : 0);
        io.write32(WriteCount, txBlock ? 0 : packet.txSize);
        io.write32(InputCount, rxBlock ? packet.rxSize : 0);
        io.write32(ReadCount, rxBlock ? 0 : packet.rxSize);
        if (!state(1)) return finish(I2C::Result::Timeout);
        io.write32(Clock, clock);
        if (!state(3)) return finish(I2C::Result::Timeout);
        unsigned sent = 0, received = 0;
        auto write = [&](unsigned count) {
            for (unsigned n = 0; n < count && sent < packet.txSize; n += 4) {
                uint32_t word = 0;
                for (unsigned b = 0; b < 4 && sent < packet.txSize; ++b)
                    word |= uint32_t(packet.tx[sent++]) << (8 * b);
                io.write32(OutFIFO, word);
            }
        };
        if (!txBlock) write(packet.txSize);
        if (!state(1)) return finish(I2C::Result::Timeout);
        for (;;) {
            const uint32_t status = io.read32(Status), errors = io.read32(Errors);
            if (status & 8) return finish(I2C::Result::Nack);
            if ((status & BusErrors) || (errors & CoreErrors)) return finish(I2C::Result::BusError);
            auto op = io.read32(Operational);
            if (txBlock && (op & 0x100) && (op & 0x1000)) {
                io.write32(Operational, 0x100);
                write(outBlock);
            }
            if ((op & 0x200) && (!rxBlock || (op & 0x2000))) {
                io.write32(Operational, 0x200);
                unsigned bytes = rxBlock ? inBlock : packet.rxSize;
                for (unsigned n = 0; n < bytes && received < packet.rxSize; n += 4) {
                    uint32_t word = io.read32(InFIFO);
                    for (unsigned b = 0; b < 4 && received < packet.rxSize; ++b) {
                        packet.rx[received++] = word & 0xff;
                        word >>= 8;
                    }
                }
            }
            // In FIFO mode the output-service flag is also the completion
            // signal; MAX_OUTPUT_DONE can lag it on real QUP v2 hardware.
            // Require an empty FIFO and idle bus before accepting that path.
            const bool txDone=(op&0x400) || (!txBlock && (op&0x100) && !(op&0x10));
            if (sent == packet.txSize && received == packet.rxSize &&
                txDone && (!packet.rxSize || (op & 0x800)) && !(io.read32(Status) & 0x100)) {
                decode(t, packet);
                return finish(I2C::Result::Success);
            }
            if (expired()) return finish(I2C::Result::Timeout);
            io.delayUS(10);
        }
    }
};
}
