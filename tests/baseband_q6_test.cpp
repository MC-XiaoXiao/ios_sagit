// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandQ6.h"
#include <cassert>
#include <cstdio>
#include <tuple>
#include <vector>
using namespace Baseband;
struct Registers {
    uint32_t regs[2][1024]{};
    uint64_t clock = 0;
    bool stuckClock = false, fail = false;
    std::vector<std::tuple<unsigned, uint32_t, uint32_t>> writes;
    bool read(unsigned space, uint32_t off, uint32_t &v) {
        assert(space < 2 && !(off & 3) && off < 4096);
        v = regs[space][off / 4]; return !fail;
    }
    bool write(unsigned space, uint32_t off, uint32_t v) {
        assert(space < 2 && !(off & 3) && off < 4096);
        writes.emplace_back(space, off, v); regs[space][off / 4] = v; return !fail;
    }
    uint64_t nowUS() { return clock; }
    void delayUS(unsigned n) { if (!stuckClock) clock += n; }
};
int main() {
    Registers io; Q6::Core<Registers> core(io);
    assert(core.start(0) == Result::Invalid && io.writes.empty());
    assert(core.start(uint64_t(1) << 32) == Result::Invalid && io.writes.empty());
    io.regs[1][Q6::PBLStatus / 4] = 1;
    assert(core.start(0x94500000) == Result::OK);
    assert(io.regs[0][Q6::Strap / 4] == 0x20 && io.regs[0][Q6::Memory / 4] == 0x1fffffff);
    assert(io.regs[0][Q6::Reset / 4] == 4 && io.regs[0][Q6::GFMux / 4] == 2);
    unsigned bank = 28, count = 0; uint32_t expected = 0;
    for (const auto &entry : io.writes)
        if (std::get<0>(entry) == 0 && std::get<1>(entry) == Q6::Memory) {
            expected |= 1u << bank--;
            assert(std::get<2>(entry) == expected); ++count;
        }
    assert(count == 29);
    io.regs[1][Q6::MBAStatus / 4] = 2;
    assert(core.waitMBA() == Result::OK);
    io.regs[1][Q6::MBAStatus / 4] = uint32_t(-3);
    assert(core.waitMBA() == Result::Authentication);
    assert(core.authenticateMetadata(0x94400000) == Result::Authentication);
    io.regs[1][Q6::MBAStatus / 4] = 3;
    assert(core.authenticateMetadata(0x94400000) == Result::OK);
    assert(io.regs[1][Q6::Command / 4] == 1 && !io.regs[1][Q6::CodeLength / 4]);
    assert(core.beginLoad(0x8d000000) == Result::OK && io.regs[1][Q6::Command / 4] == 2);
    assert(core.loaded(8192) == Result::OK);
    io.regs[1][Q6::MBAStatus / 4] = 4;
    assert(core.waitAuthentication() == Result::OK);
    Registers timeout; Q6::Core<Registers> blocked(timeout);
    timeout.regs[0][Q6::XO / 4] = 1u << 31;
    assert(blocked.start(0x94500000) == Result::Timeout);
    assert(timeout.clock == 200 && timeout.regs[0][Q6::Memory / 4] == 0);
    timeout.stuckClock = true;
    assert(blocked.wait(0, Q6::XO, 1u << 31, 0, 50) == Result::Timeout);
    Registers error; Q6::Core<Registers> rejected(error);
    error.regs[1][Q6::PBLStatus / 4] = 2;
    assert(rejected.start(0x94500000) == Result::Authentication);
    error.fail = true;
    assert(rejected.beginLoad(0x8d000000) == Result::IO);
    std::puts("baseband Q6 boot/register/failure checks passed");
}
