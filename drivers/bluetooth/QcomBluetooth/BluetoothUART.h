// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <BluetoothProtocol.h>

namespace Bluetooth {
namespace UARTDM {
    constexpr uint32_t MR1 = 0, MR2 = 4, CSR = 8, SR = 8, CR = 0x10, IMR = 0x14, ISR = 0x14;
    constexpr uint32_t IPR = 0x18, TFWR = 0x1c, RFWR = 0x20, DMRX = 0x34, Snapshot = 0x38, DMEN = 0x3c,
                       NCF = 0x40, RXFS = 0x50, FIFO = 0x70;
    constexpr uint32_t RXReady = 1, TXReady = 4, FIFOEmpty = 8, Errors = 0x70, RXStale = 8, RXLevel = 16;
    constexpr uint32_t TXLevel = 1;
    constexpr uint32_t RXLimit = 0xffffff;
    struct Baud {
        uint32_t rate, clock, code;
    };
    constexpr Baud BaudTable[]
        = { { 2400, 3686400, 0x44 }, { 115200, 7372800, 0xcc }, { 3200000, 51200000, 0xff } };
    inline const Baud* baud(uint32_t speed)
    {
        for (const auto& entry : BaudTable)
            if (entry.rate == speed)
                return &entry;
        return nullptr;
    }
}

// MMIO is supplied only after the platform owns rails, clocks and pins.
// All methods run under the controller gate. No DMA, polling loops or writes
// to the console UART; service() has a fixed per-invocation FIFO budget.
template <class Registers> class UARTPort {
    Registers& io;
    struct Packet {
        uint16_t length = 0;
        uint8_t bytes[MaxH4Frame] { };
    } queue[8];
    unsigned first = 0, count = 0, sent = 0;
    uint32_t consumed = 0, remaining = 0;
    bool online = false, stale = false, forcePending = false, activeTX = false, failed = false;
    bool rxEnabled = false;
    uint32_t interruptMask = 0;
    void interrupts()
    {
        if (!online)
            return;
        const uint32_t mask = failed ? 0
                                     : (rxEnabled ? UARTDM::RXStale | UARTDM::RXLevel : 0)
                | (count && (!activeTX || sent < queue[first].length) ? UARTDM::TXLevel : 0);
        if (mask != interruptMask) {
            io.write(UARTDM::IMR, mask);
            interruptMask = mask;
        }
    }
    Result fault()
    {
        failed = true;
        io.write(UARTDM::IMR, 0);
        interruptMask = 0;
        return Result::Fault;
    }

public:
    explicit UARTPort(Registers& registers)
        : io(registers)
    {
    }
    bool enabled() const
    {
        return online;
    }
    bool faulted() const
    {
        return failed;
    }
    void receiveInterrupts(bool enable)
    {
        rxEnabled = enable;
        interrupts();
    }
    Result flowControl(bool enable)
    {
        if (!online || failed)
            return Result::NotReady;
        io.write(UARTDM::MR1, enable ? (1U << 6) | (1U << 7) | 47U : 0);
        io.write(UARTDM::CR, enable ? 0xd0 : 0xe0);
        return Result::Success;
    }
    unsigned queuedPackets() const
    {
        return count;
    }
    Result takeover()
    {
        if (online)
            return Result::Busy;
        // Do not disable a firmware-owned BAM/DMA engine without its own
        // drain protocol. Only the explicit PIO handoff is supported here.
        if (io.read(UARTDM::DMEN))
            return Result::Unsupported;
        io.write(UARTDM::IMR, 0);
        io.write(UARTDM::CR, 0x0a);
        io.write(UARTDM::CR, 0x10);
        io.write(UARTDM::CR, 0x20);
        return io.read(UARTDM::SR) & UARTDM::FIFOEmpty ? Result::Success : Result::Fault;
    }
    Result configure(uint32_t speed, bool flow)
    {
        using namespace UARTDM;
        const auto* entry = baud(speed);
        if (!entry)
            return Result::Unsupported;
        if (online && (count || !(io.read(SR) & FIFOEmpty)))
            return Result::Busy;
        io.write(IMR, 0);
        interruptMask = 0;
        io.write(DMEN, 0);
        io.write(CR, 0x0a);
        io.write(CR, 0x100);
        io.write(CR, 0x10);
        io.write(CR, 0x20);
        io.write(CR, 0x30);
        io.write(CR, 0x600);
        io.write(CR, 0x80);
        io.write(CSR, entry->code);
        io.write(MR2, 0x34);
        // 64-byte RX FIFO: auto-RFR threshold 48 bytes. CTS gates TX.
        io.write(MR1, flow ? (1U << 6) | (1U << 7) | 47U : 0);
        io.write(IPR, 31);
        io.write(RFWR, 0);
        io.write(TFWR, 0);
        io.write(DMRX, RXLimit);
        io.write(CR, 0x05);
        io.write(CR, flow ? 0xd0 : 0xe0);
        io.write(CR, 0x500);
        first = count = sent = 0;
        consumed = remaining = 0;
        activeTX = stale = forcePending = failed = false;
        online = true;
        rxEnabled = true;
        interrupts();
        return Result::Success;
    }
    Result write(const uint8_t* bytes, size_t length)
    {
        if (!online || failed)
            return Result::NotReady;
        if (!bytes || !length || length > MaxH4Frame)
            return Result::Invalid;
        if (count == 8)
            return Result::Busy;
        auto& packet = queue[(first + count) % 8];
        packet.length = uint16_t(length);
        for (size_t i = 0; i < length; ++i)
            packet.bytes[i] = bytes[i];
        ++count;
        interrupts();
        return Result::Success;
    }
    Result serviceTX()
    {
        using namespace UARTDM;
        if (!online || failed)
            return Result::NotReady;
        for (unsigned budget = 0; budget < 128 && count; ++budget) {
            auto& packet = queue[first];
            if (!activeTX) {
                if (!(io.read(SR) & FIFOEmpty))
                    return Result::Pending;
                io.write(CR, 0x300);
                io.write(NCF, packet.length);
                (void)io.read(NCF);
                activeTX = true;
                sent = 0;
            }
            if (sent == packet.length) {
                if (!(io.read(SR) & FIFOEmpty))
                    return Result::Pending;
                first = (first + 1) % 8;
                --count;
                activeTX = false;
                interrupts();
                continue;
            }
            if (!(io.read(SR) & TXReady))
                return Result::Pending;
            uint32_t word = 0;
            for (unsigned shift = 0; shift < 32 && sent < packet.length; shift += 8)
                word |= uint32_t(packet.bytes[sent++]) << shift;
            io.write(FIFO, word);
            interrupts();
        }
        return count ? Result::Pending : Result::Success;
    }
    Result drained()
    {
        const auto result = serviceTX();
        if (result != Result::Success)
            return result;
        return io.read(UARTDM::SR) & UARTDM::FIFOEmpty ? Result::Success : Result::Pending;
    }
    template <class Sink> Result receive(Sink& sink)
    {
        using namespace UARTDM;
        if (!online || failed)
            return Result::NotReady;
        for (unsigned budget = 0; budget < 128; ++budget) {
            const uint32_t status = io.read(SR);
            if (status & Errors)
                return fault(); // never deliver corrupted HCI bytes
            if (!stale && (io.read(ISR) & RXStale)) {
                const uint32_t total = io.read(Snapshot);
                if (total < consumed || total > RXLimit)
                    return fault();
                remaining = total - consumed;
                stale = true;
                forcePending = false;
            }
            if (stale && !remaining) {
                io.write(CR, 0x80);
                io.write(DMRX, RXLimit);
                io.write(CR, 0x500);
                consumed = 0;
                stale = false;
                continue;
            }
            if (!(status & RXReady)) {
                // Flush the packing buffer only once, then wait for the
                // hardware snapshot; RXFS is not an authoritative byte count.
                if (!stale && !forcePending && ((io.read(RXFS) >> 7) & 7)) {
                    io.write(CR, 0x400);
                    forcePending = true;
                }
                return Result::Success;
            }
            const unsigned bytes = stale && remaining < 4 ? remaining : 4;
            if (consumed > RXLimit - bytes)
                return fault();
            uint32_t word = io.read(FIFO);
            uint8_t data[4];
            for (unsigned i = 0; i < bytes; ++i) {
                data[i] = uint8_t(word);
                word >>= 8;
            }
            consumed += bytes;
            if (stale)
                remaining -= bytes;
            const Result result = sink.bytes(data, bytes);
            if (result == Result::Pending)
                return Result::Success;
            if (result != Result::Success)
                return fault();
        }
        return Result::Success;
    }
    // This UART uses PIO only. Disable the IRQ source before calling stop;
    // resetting both engines proves they cannot consume driver-owned buffers.
    Result stop()
    {
        if (!online)
            return Result::Success;
        io.write(UARTDM::IMR, 0);
        io.write(UARTDM::DMEN, 0);
        io.write(UARTDM::CR, 0x0a);
        io.write(UARTDM::CR, 0x10);
        io.write(UARTDM::CR, 0x20);
        online = false;
        activeTX = false;
        first = count = sent = 0;
        consumed = remaining = 0;
        stale = forcePending = false;
        interruptMask = 0;
        rxEnabled = false;
        return Result::Success;
    }
};
}
