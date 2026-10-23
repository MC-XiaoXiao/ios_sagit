#include <QUPSPI.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
using namespace QUPSPI;

struct Registers {
    uint32_t regs[256]{};
    uint64_t time = 0;
    unsigned writes = 0, transfers = 0;
    bool stall = false, resetStuck = false, done = false, injectError = false;
    std::vector<uint8_t> output, wire;
    std::deque<uint8_t> input;
    Registers() {
        regs[Version/4] = 0x20020001;
        regs[State/4] = 4;
        regs[Mode/4] = 1 | (2<<2) | (1<<5) | (2<<7); // 32 unpacked words
    }
    uint32_t read32(uint32_t reg) {
        if (reg == Operational) return (input.empty() ? 0 : 0x20) | (done ? 0xc00 : 0);
        if (reg == Errors && injectError && transfers) return 4;
        if (reg == InFIFO) { assert(!input.empty()); auto v = input.front(); input.pop_front(); return v; }
        return regs[reg/4];
    }
    void write32(uint32_t reg, uint32_t value) {
        ++writes;
        if (reg == Reset) {
            output.clear(); input.clear(); done = false;
            regs[State/4] = resetStuck ? 0 : 4;
            return;
        }
        if (reg == Errors || reg == SPIErrors || reg == Operational) return;
        if (reg == OutFIFO) {
            assert((value & 0xffffff) == 0); // actual QUP TX alignment
            output.push_back(value >> 24); return;
        }
        regs[reg/4] = value;
        if (reg == State && (value & 3) == 1 &&
            output.size() == regs[WriteCount/4] && regs[WriteCount/4]) {
            assert(regs[IOControl/4] & 0x800);
            assert(regs[ReadCount/4] == regs[WriteCount/4]);
            assert(regs[InputCount/4] == 0 && regs[OutputCount/4] == 0);
            assert((regs[Mode/4] & 0xfc00) == 0);
            wire = output; ++transfers;
            if (!stall) {
                for (auto byte : output) input.push_back(byte ^ 0xa5);
                done = true;
            }
        }
    }
    void delayUS(unsigned value) { time += value; }
    uint64_t nowUS() { return time; }
};
int main() {
    uint8_t tx[]{0x8f, 0x00, 0x7e, 0xff, 0x22, 0x80, 0x01};
    uint8_t rx[64]; memset(rx, 0xcc, sizeof(rx));
    SPI::Transaction t{{0, 1000000, 0}, tx, rx, sizeof(tx), 2000};
    Registers regs; Engine<Registers> engine(regs);
    assert(engine.configure(1000000));
    assert(!engine.configure(1000000));
    assert(engine.transfer(t) == SPI::Result::Success);
    assert(regs.wire == std::vector<uint8_t>(tx, tx + sizeof(tx)));
    for (unsigned i = 0; i < sizeof(tx); ++i) assert(rx[i] == (tx[i] ^ 0xa5));
    assert(rx[7] == 0xcc && !(regs.regs[IOControl/4] & 0x800));
    assert(regs.regs[SPIConfig/4] == 0x200 && regs.regs[Config/4] == 0x107);
    assert(regs.regs[State/4] == 4);
    t.config.mode = 3; t.config.chipSelect = 3;
    assert(engine.transfer(t) == SPI::Result::Success);
    assert(regs.regs[SPIConfig/4] == 0 && regs.regs[IOControl/4] == 0x40d);
    t.tx = nullptr;
    assert(engine.transfer(t) == SPI::Result::Success);
    for (unsigned i = 0; i < sizeof(tx); ++i) assert(rx[i] == 0xa5);
    t.tx = tx; t.rx = nullptr;
    assert(engine.transfer(t) == SPI::Result::Success);
    t.rx = rx;
    t.config.frequency = 500000;
    assert(engine.transfer(t) == SPI::Result::Unsupported);
    t.config.frequency = 1000000;
    t.length = 33; assert(engine.transfer(t) == SPI::Result::Unsupported);
    t.length = 65; assert(engine.transfer(t) == SPI::Result::Invalid);
    t.length = 0; assert(engine.transfer(t) == SPI::Result::Invalid);
    t.length = sizeof(tx);
    t.config.mode = 1; assert(engine.transfer(t) == SPI::Result::Invalid);
    t.config.mode = 0;
    for (uint32_t timeout : {0U, 99U, 20001U}) {
        t.timeoutUS = timeout; assert(engine.transfer(t) == SPI::Result::Invalid);
    }
    t.timeoutUS = 100;
    memset(rx, 0xcc, sizeof(rx)); regs.stall = true;
    auto before = regs.time;
    assert(engine.transfer(t) == SPI::Result::Timeout);
    assert(regs.time - before <= 200 && regs.time - before >= 100);
    for (auto byte : rx) assert(byte == 0xcc);
    assert(!(regs.regs[IOControl/4] & 0x800));
    regs.stall = false; regs.injectError = true;
    assert(engine.transfer(t) == SPI::Result::IO);
    for (auto byte : rx) assert(byte == 0xcc);
    regs.injectError = false; regs.resetStuck = true;
    assert(engine.transfer(t) == SPI::Result::IO);
    regs.resetStuck = false; regs.regs[State/4] = 4;
    before = regs.writes;
    assert(engine.transfer(t) == SPI::Result::NotReady && regs.writes == before);
    Registers busy; Engine<Registers> busyEngine(busy);
    assert(busyEngine.configure(1000000));
    busy.regs[State/4] = 5;
    assert(busyEngine.transfer(t) == SPI::Result::NotReady && busy.writes == 0);
    for (unsigned test = 0; test < 3; ++test) {
        Registers invalid;
        if (test == 0) invalid.regs[Version/4] = 0x10010001;
        if (test == 1) invalid.regs[State/4] = 5;
        if (test == 2) invalid.regs[Mode/4] = 3;
        Engine<Registers> rejected(invalid);
        assert(!rejected.configure(1000000) && invalid.writes == 0);
    }
    puts("SPI: FIFO, chip select, byte order, bounds, timeout and fault isolation passed");
}
