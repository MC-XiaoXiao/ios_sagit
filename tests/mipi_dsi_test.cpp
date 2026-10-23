/* SPDX-License-Identifier: BSD-2-Clause */
#include "MIPIDSIProtocol.h"
#include "PanelProtocol.h"
#include <assert.h>
#include <array>
#include <vector>
#include <utility>

struct Registers {
    std::array<uint32_t, 128> values{};
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    bool complete = true, error = false, stuck = false;
    unsigned delays = 0;
    uint32_t read(uint32_t r) { return values[r / 4]; }
    void write(uint32_t r, uint32_t v) {
        writes.emplace_back(r, v);
        if (r == MIPIDSI::InterruptControl) {
            uint32_t status = values[r / 4] & (MIPIDSI::DMAComplete | MIPIDSI::BTAComplete | MIPIDSI::Error);
            values[r / 4] = (v & ~uint32_t(MIPIDSI::DMAComplete | MIPIDSI::BTAComplete | MIPIDSI::Error)) | (status & ~v);
        } else values[r / 4] = v;
        if (r == MIPIDSI::TriggerDMA) {
            values[MIPIDSI::Status / 4] = stuck ? uint32_t(MIPIDSI::DMAActive) : 0u;
            if (complete) values[MIPIDSI::InterruptControl / 4] |= MIPIDSI::DMAComplete | MIPIDSI::BTAComplete;
            if (error) values[MIPIDSI::InterruptControl / 4] |= MIPIDSI::Error;
        }
    }
    void delay(unsigned us) { assert(us == 10); ++delays; }
};

int main() {
    assert(MIPIDSI::controller6G(0, 0x03000104, 0x20000000));
    assert(!MIPIDSI::controller6G(0x02000000, 0, 0x20000000));
    assert(!MIPIDSI::controller6G(0, 0x02000104, 0x20000000));
    using namespace MIPIDSI;
    uint8_t out[4096], one[] = {0x11}, two[] = {0x51, 0xff}, three[] = {1, 2, 3};
    assert(packet(out, sizeof(out), 0x05, one, 1) == 4);
    assert(out[0] == 0x11 && out[1] == 0 && out[2] == 5 && out[3] == 0x80);
    assert(packet(out, sizeof(out), 0x15, two, 2) == 4 && out[1] == 0xff);
    assert(packet(out, sizeof(out), 0x39, three, 3) == 8);
    assert(out[0] == 3 && out[1] == 0 && out[3] == 0xc0 && out[6] == 3 && out[7] == 0xff);
    assert(!packet(out, sizeof(out), 0x15, one, 1));
    assert(!packet(out, sizeof(out), 0x39, nullptr, 3));
    assert(!packet(out, sizeof(out), 0x39, three, MaxPayload + 1));
    assert(!packet(out, 4, 0x39, three, 3));
    assert(!packet(out, sizeof(out), 0x7f, two, 2));
    uint8_t sequence[] = {0x05, 0, 1, 0, 120, 0, 0, 0, 0x11,
                          0x05, 0, 1, 0, 20, 0, 0, 0, 0x29};
    assert(panelSequence(sequence, sizeof(sequence)));
    assert(!panelSequence(sequence, sizeof(sequence) - 1));
    sequence[1] = 1; assert(!panelSequence(sequence, sizeof(sequence))); sequence[1] = 0;
    sequence[2] = 2; assert(!panelSequence(sequence, sizeof(sequence))); sequence[2] = 1;
    sequence[5] = 3; assert(!panelSequence(sequence, sizeof(sequence)));
    assert(packet(out, sizeof(out), 0x06, one, 1, true) == 4 && out[3] == 0xa0);
    uint8_t raw[16]{}, reply[10]{}; unsigned actual;
    raw[12] = 0x21; raw[13] = 0x9c;
    assert(response(raw, 2, reply, actual) == Result::Success && actual == 1 && reply[0] == 0x9c);
    raw[12] = 0x22; raw[14] = 0x55;
    assert(response(raw, 1, reply, actual) == Result::HardwareError && actual == 0);
    raw[6] = 0x1c; raw[7] = 4; raw[10] = 1; raw[11] = 2; raw[12] = 3; raw[13] = 4;
    assert(response(raw, 4, reply, actual) == Result::Success && actual == 4 && reply[3] == 4);
    raw[7] = 5;
    assert(response(raw, 4, reply, actual) == Result::HardwareError && actual == 0);
    raw[6] = 0x02;
    assert(response(raw, 4, reply, actual) == Result::HardwareError);
    PlatformContract c{1, 31, 4, 100000000, 19200000};
    assert(c.valid()); c.flags &= ~PlatformContract::PHYReady; assert(!c.valid());
    c.flags = 63; assert(c.valid()); c.lanes = 0; assert(!c.valid());
    Registers regs;
    regs.values[Control / 4] = Enable | VideoEnable | 0x1f0;
    regs.values[InterruptControl / 4] = 1u << 17;
    regs.values[DMAControl / 4] = 0x12340000;
    regs.values[TriggerControl / 4] = 0x5432;
    Host<Registers> host(regs);
    assert(host.transfer(0x90000000, 8, false, true) == Result::Success);
    assert(regs.values[Control / 4] == (Enable | VideoEnable | 0x1f0));
    assert(regs.values[DMAControl / 4] == 0x12340000);
    assert(regs.values[TriggerControl / 4] == 0x5432);
    assert(regs.values[InterruptControl / 4] == (1u << 17));
    auto writes = regs.writes.size();
    assert(host.transfer(0x100000000ULL, 4, false, true) == Result::BadArgument);
    assert(host.transfer(0xfffffffcu, 8, false, true) == Result::BadArgument);
    assert(regs.writes.size() == writes);
    regs.values[Status / 4] = DMAActive;
    assert(host.transfer(0x90000000, 4, false, true) == Result::Busy);
    assert(regs.writes.size() == writes);
    regs.values[Status / 4] = 0; regs.complete = false;
    assert(host.transfer(0x90000000, 4, false, true) == Result::Timeout);
    assert(regs.delays == 10000);
    regs.complete = true; regs.error = true;
    assert(host.transfer(0x90000000, 4, false, true) == Result::HardwareError);
    regs.error = false; regs.stuck = true;
    assert(host.transfer(0x90000000, 4, false, true) == Result::Timeout);
    assert(regs.values[Control / 4] & CommandEnable); // Still live: never undo ownership.
}
