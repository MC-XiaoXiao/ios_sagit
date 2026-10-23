// SPDX-License-Identifier: BSD-2-Clause
#include "BluetoothUART.h"
#include "BluetoothLoader.h"
#include "BluetoothConfiguration.h"
#include "BluetoothController.h"
#include "BluetoothSerial.h"
#include "BluetoothControl.h"
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <deque>
using namespace Bluetooth;
struct Registers {
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    std::vector<uint8_t> wire;
    std::deque<uint32_t> rx;
    uint32_t txLeft = 0, snapshot = 0, errors = 0, packing = 0, forced = 0;
    bool txReady = true, stale = false, physicalBusy = false, holdWire = false;
    uint32_t dma = 0;
    uint32_t read(uint32_t r)
    {
        if (r == UARTDM::SR)
            return (txReady ? UARTDM::TXReady : 0) | ((txLeft || physicalBusy) ? 0 : UARTDM::FIFOEmpty)
                | (rx.empty() ? 0 : UARTDM::RXReady) | errors;
        if (r == UARTDM::ISR)
            return stale ? UARTDM::RXStale : 0;
        if (r == UARTDM::Snapshot)
            return snapshot;
        if (r == UARTDM::RXFS)
            return packing << 7;
        if (r == UARTDM::DMEN)
            return dma;
        if (r == UARTDM::FIFO) {
            assert(!rx.empty());
            const auto x = rx.front();
            rx.pop_front();
            return x;
        }
        return 0;
    }
    void write(uint32_t r, uint32_t v)
    {
        writes.emplace_back(r, v);
        if (r == UARTDM::NCF) {
            assert(!txLeft);
            txLeft = v;
        }
        if (r == UARTDM::FIFO) {
            for (unsigned i = 0; i < 4 && txLeft; ++i, --txLeft) {
                wire.push_back(uint8_t(v));
                v >>= 8;
            }
            physicalBusy = holdWire;
        }
        if (r == UARTDM::CR && v == 0x400)
            ++forced;
        if (r == UARTDM::CR && v == 0x80)
            stale = false;
        if (r == UARTDM::CR && v == 0x20)
            txLeft = 0;
    }
};
struct Sink {
    std::vector<uint8_t> received;
    Result bytes(const uint8_t* p, size_t n)
    {
        received.insert(received.end(), p, p + n);
        return Result::Success;
    }
};
static void uart()
{
    Registers registers;
    UARTPort<Registers> uart(registers);
    registers.dma = 8;
    assert(uart.takeover() == Result::Unsupported && registers.writes.empty());
    registers.dma = 0;
    assert(uart.takeover() == Result::Success);
    registers.writes.clear();
    assert(uart.configure(9600, true) == Result::Unsupported && registers.writes.empty());
    assert(uart.configure(3200000, true) == Result::Success);
    bool divisor = false, flow = false, noDMA = false;
    for (auto w : registers.writes) {
        divisor |= w.first == UARTDM::CSR && w.second == 0xff;
        flow |= w.first == UARTDM::MR1 && (w.second & 0xc0) == 0xc0;
        noDMA |= w.first == UARTDM::DMEN && !w.second;
    }
    assert(divisor && flow && noDMA);
    const uint8_t packet[] = { 1, 3, 0x0c, 0 }, wake = 0xfd;
    assert(uart.write(packet, 4) == Result::Success && uart.write(&wake, 1) == Result::Success);
    assert(registers.writes.back()
        == std::make_pair(UARTDM::IMR, UARTDM::RXStale | UARTDM::RXLevel | UARTDM::TXLevel));
    assert(uart.drained() == Result::Success);
    assert(registers.writes.back() == std::make_pair(UARTDM::IMR, UARTDM::RXStale | UARTDM::RXLevel));
    assert(registers.wire == std::vector<uint8_t>({ 1, 3, 0x0c, 0, 0xfd }));
    registers.txReady = false;
    assert(uart.write(packet, 4) == Result::Success);
    assert(uart.drained() == Result::Pending);
    assert(uart.configure(115200, true) == Result::Busy);
    registers.txReady = true;
    assert(uart.drained() == Result::Success);
    for (unsigned i = 0; i < 8; ++i)
        assert(uart.write(packet, 4) == Result::Success);
    assert(uart.write(packet, 4) == Result::Busy);
    assert(uart.drained() == Result::Success);
    Sink sink;
    registers.rx.push_back(0x04030201);
    assert(uart.receive(sink) == Result::Success && sink.received.size() == 4);
    registers.rx.push_back(0xff070605);
    registers.snapshot = 7;
    registers.stale = true;
    assert(uart.receive(sink) == Result::Success
        && sink.received == std::vector<uint8_t>({ 1, 2, 3, 4, 5, 6, 7 }));
    registers.rx.push_back(0xeeeeeeee);
    registers.snapshot = 3;
    registers.stale = true;
    assert(uart.receive(sink) == Result::Success && sink.received.size() == 10);
    registers.errors = 0x10;
    assert(uart.receive(sink) == Result::Fault && uart.faulted());
    assert(uart.stop() == Result::Success && !uart.enabled());
    assert(uart.write(packet, 4) == Result::NotReady);
    Registers invalid;
    UARTPort<Registers> bad(invalid);
    assert(bad.configure(115200, false) == Result::Success);
    invalid.snapshot = UARTDM::RXLimit + 1;
    invalid.stale = true;
    assert(bad.receive(sink) == Result::Fault);
    Registers slow;
    UARTPort<Registers> ordered(slow);
    assert(ordered.configure(115200, true) == Result::Success);
    slow.holdWire = true;
    assert(ordered.write(packet, 4) == Result::Success && ordered.write(&wake, 1) == Result::Success);
    assert(ordered.drained() == Result::Pending && slow.wire.size() == 4 && ordered.queuedPackets() == 2);
    assert(ordered.configure(3200000, true) == Result::Busy);
    slow.physicalBusy = false;
    slow.holdWire = false;
    assert(ordered.drained() == Result::Success && slow.wire.back() == wake);
    slow.packing = 3;
    assert(ordered.receive(sink) == Result::Success && slow.forced == 1);
    assert(ordered.receive(sink) == Result::Success && slow.forced == 1); // asynchronous forced stale
    slow.packing = 0;
    slow.rx.push_back(0x00ccbbaa);
    slow.snapshot = 3;
    slow.stale = true;
    assert(ordered.receive(sink) == Result::Success && sink.received.back() == 0xcc);
}
struct FirmwareUART {
    std::vector<std::vector<uint8_t>> commands;
    bool stalled = false;
    unsigned baudChanges = 0, flowChanges = 0;
    Result write(const uint8_t* p, size_t n)
    {
        assert(validPacket(p, n, true));
        commands.emplace_back(p, p + n);
        return Result::Success;
    }
    Result drained()
    {
        return stalled ? Result::Pending : Result::Success;
    }
    Result runtimeBaud()
    {
        ++baudChanges;
        return Result::Success;
    }
    Result beginBaudChange()
    {
        ++flowChanges;
        return Result::Success;
    }
};
static void loader()
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        std::vector<uint8_t> p(600, 0), n(16, 0);
        write32(p.data(), uint32_t((p.size() - 4) << 8) | 1);
        write16(p.data() + 16, 1);
        write16(p.data() + 18, 3);
        p[14] = uint8_t(mode);
        write32(n.data(), (12 << 8) | 2);
        FirmwareView patch, nvm;
        assert(parseFirmware(p.data(), p.size(), FirmwareType::Patch, patch) == Result::Success);
        assert(parseFirmware(n.data(), n.size(), FirmwareType::NVM, nvm) == Result::Success);
        FirmwareUART uart;
        FirmwareLoader<FirmwareUART> loader(uart);
        assert(loader.begin(patch, nvm, { 1, 4, 3 }, 0) == Result::Pending);
        assert(uart.commands.size() == 1 && uart.commands[0][4] == 0x19);
        assert(loader.advance(1) == Result::Pending);
        const uint8_t version[] = { 4, 0xff, 14, 0, 2, 1, 0, 0, 0, 0, 0, 3, 0, 4, 0, 0, 0 };
        assert(loader.receive(version, sizeof(version), 2) == Result::Success);
        assert(uart.flowChanges == 1);
        assert(uart.commands.back() == std::vector<uint8_t>({ 1, 0x48, 0xfc, 1, 0x11 }));
        assert(loader.advance(3) == Result::Pending && loader.advance(52) == Result::Pending);
        assert(!uart.baudChanges);
        assert(loader.advance(53) == Result::Pending && uart.baudChanges == 1);
        const uint8_t ack[] = { 4, 0xff, 3, 0, 4, 0 };
        const uint8_t cc[] = { 4, 0x0e, 4, 1, 0, 0xfc, 0 };
        for (uint64_t time = 54;
            time < 1000 && loader.currentPhase() != FirmwareLoader<FirmwareUART>::Phase::Complete; ++time) {
            const auto phase = loader.currentPhase();
            if (phase == FirmwareLoader<FirmwareUART>::Phase::PatchDrain
                || phase == FirmwareLoader<FirmwareUART>::Phase::NVMDrain) {
                const auto& cmd = uart.commands.back();
                const bool last = cmd.size() < FirmwareSegmentBytes + 6;
                if (phase == FirmwareLoader<FirmwareUART>::Phase::NVMDrain || !(mode & 1) || last)
                    assert(loader.receive(ack, sizeof(ack), time) == Result::Success);
            }
            const auto r = loader.advance(time);
            assert(r == Result::Pending || r == Result::Success);
            // Optional CC can follow VSE after the state continuation.
            assert(loader.receive(cc, sizeof(cc), time) == Result::Success);
        }
        assert(loader.currentPhase() == FirmwareLoader<FirmwareUART>::Phase::Complete);
        assert(uart.commands.size() == 6); // version, baud, 3 patch chunks, NVM
        FirmwareUART late;
        FirmwareLoader<FirmwareUART> timeout(late);
        assert(timeout.begin(patch, nvm, { 1, 4, 3 }, 0) == Result::Pending);
        assert(timeout.advance(2000) == Result::Timeout);
        assert(timeout.receive(version, sizeof(version), 2001) == Result::Fault);
        FirmwareUART wrong;
        FirmwareLoader<FirmwareUART> mismatch(wrong);
        assert(mismatch.begin(patch, nvm, { 1, 5, 3 }, 0) == Result::Pending);
        assert(mismatch.receive(version, sizeof(version), 1) == Result::Invalid);
        FirmwareUART delayed;
        FirmwareLoader<FirmwareUART> delayedEvent(delayed);
        assert(delayedEvent.begin(patch, nvm, { 1, 4, 3 }, 0) == Result::Pending);
        assert(delayedEvent.receive(version, sizeof(version), 2000) == Result::Timeout
            && delayed.commands.size() == 1);
    }
}
static void serialSettings()
{
    SerialSettings settings;
    uint32_t value = 230400;
    assert(settings.event(DataRate, false, value) == Result::Success && settings.logicalBaud == 115200);
    value = 0;
    assert(settings.event(DataRate, true, value) == Result::Success && value == 230400);
    value = 7;
    assert(settings.event(DataSize, false, value) == Result::Unsupported);
    value = 16;
    assert(settings.event(DataSize, false, value) == Result::Success);
    value = 0;
    assert(settings.event(DataIntegrity, false, value) == Result::Unsupported);
    assert(settings.event(RXDataIntegrity, false, value) == Result::Success);
    value = 0x24;
    assert(settings.event(FlowControl, false, value) == Result::Success);
    value = 0x10;
    assert(settings.event(FlowControl, false, value) == Result::Unsupported);
    value = 0;
    assert(settings.event(TXLowWater, false, value) == Result::Success);
    value = 2;
    assert(settings.event(TXHighWater, false, value) == Result::Success);
    value = 3;
    assert(settings.event(TXLowWater, false, value) == Result::Invalid);
    value = 255;
    assert(settings.event(XoffByte, false, value) == Result::Success);
    value = 0;
    assert(settings.event(XoffByte, true, value) == Result::Success && value == 255);
    value = 256;
    assert(settings.event(XonByte, false, value) == Result::Invalid);
    value = 1;
    assert(settings.event(LineBreak, false, value) == Result::Unsupported);
    value = 2;
    assert(settings.event(StopBits, false, value) == Result::Success);
    assert(settings.event(0xffff, true, value) == Result::Unsupported);
}
static void ring()
{
    ByteRing<7> ring;
    uint8_t p[] = { 1, 2, 3, 4, 5, 6, 7, 8 }, out[8] { };
    assert(ring.push(p, 8) == 7 && ring.available() == 0);
    assert(ring.pop(out, 5) == 5 && out[4] == 5);
    assert(ring.push(p, 4) == 4 && ring.size() == 6);
    assert(ring.pop(out, 8) == 6 && out[0] == 6 && out[1] == 7 && out[2] == 1 && out[5] == 4);
    assert(!ring.size() && !ring.peek(7));
}
static void nvmAdmission()
{
    const uint8_t image[] = { 2, 46, 0, 0, 2, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 17, 0, 3, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0, 0x11, 27, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    FirmwareView nvm;
    assert(parseFirmware(image, sizeof(image), FirmwareType::NVM, nvm) == Result::Success);
    assert(transportNVM(nvm));
    const uint8_t mac[] = { 6, 4, 3, 2, 1, 0 }, wrong[] = { 6, 4, 3, 2, 1, 1 };
    assert(localAddress(nvm, mac) && !localAddress(nvm, wrong));
    std::vector<uint8_t> changed(image, image + sizeof(image));
    changed[34] = 0;
    assert(parseFirmware(changed.data(), changed.size(), FirmwareType::NVM, nvm) == Result::Success);
    assert(!transportNVM(nvm));
}
static void nativeControl()
{
    NativeTermios options;
    assert(validNativeTermios(options));
    options.control |= 0x1000;
    assert(!validNativeTermios(options));
    options.control &= ~0x1000ULL;
    options.input |= 0x200;
    assert(!validNativeTermios(options));
    options.input = 0;
    options.outputSpeed = 115200;
    assert(!validNativeTermios(options));
    options.inputSpeed = 115200;
    assert(validNativeTermios(options));
    options.local = 1;
    assert(!validNativeTermios(options));
    ControlSession session;
    assert(!session.connect(0, 1) && !session.connect(1, 0));
    assert(session.connect(3, 7) && session.unit() == 3);
    assert(!session.connect(4, 7) && !session.matches(3, 8) && session.matches(3, 7));
    assert(!session.disconnect(4) && session.matches(3, 7));
    assert(session.disconnect(3) && !session.matches(3, 7));
    assert(session.connect(3, 8) && !session.matches(3, 7) && session.matches(3, 8));
    assert(!session.requiresReset() && !session.handoff(3,8));
    assert(session.disconnect(3));
    assert(session.connect(4,9,true) && session.requiresReset() && !session.sealed());
    assert(!session.handoff(4,8) && !session.handoff(3,9) && session.requiresReset());
    assert(session.handoff(4,9) && session.sealed() && !session.requiresReset());
    assert(!session.handoff(4,9) && session.disconnect(4));
    assert(session.connect(4,10,true) && session.requiresReset());
    assert(session.disconnect(4) && !session.requiresReset() && !session.sealed());
}
int main()
{
    uart();
    loader();
    ring();
    serialSettings();
    nvmAdmission();
    nativeControl();
    puts("Bluetooth UARTDM MMIO, ordered TX/drain, RX snapshots, ROM/firmware boot and native serial "
         "settings/queues passed");
}
