// SPDX-License-Identifier: GPL-2.0-only
#include "OSMHandoff.h"
#include "OSMSecureIO.h"
#include <assert.h>
#include <stdio.h>
#include <vector>
using namespace QcomOSMHandoff;
struct Monitor {
    std::vector<QcomSecure::Registers> calls;
    int mode = 0;
    void smc(QcomSecure::Registers &r) {
        calls.push_back(r);
        if ((r.x[0] & 0xffff) == 0x601) { r.x[0] = 0; r.x[1] = 1; return; }
        if (mode == 1 && calls.back().x[0] != 1) {r.x[0] = 1; r.x[6] = 0xabc; return;}
        if (mode == 2) { r.x[0] = uint64_t(-12); return; }
        if (mode == 3) { r.x[0] = 1; r.x[6] = 0x123; return; }
        r.x[0] = 0; r.x[1] = 0xa55a;
    }
    void sleep(unsigned) { assert(false); }
};
int main() {
    uint32_t regs[2][0x2000 / 4] = {};
    auto read = [&](unsigned c, uint32_t off) { return regs[c][off / 4]; };
    Record r{};
    assert(validate(r, read) == Status::Uncommitted);
    r.version = Version; r.bytes = sizeof(r); r.chipID = 292;
    r.chipVersion = 0x20001; r.boardID = 30; r.flags = Required;
    r.auxHz = 300000000; r.osmHz = 200000000; r.xoHz = 19200000;
    r.firmwareProfile = QcomOSMTZ::Profile;
    for (unsigned c = 0; c < 2; ++c) {
        r.firmwareChecks[c] = QcomOSMTZ::CompleteChecks;
        r.enable[c] = regs[c][0x1004 / 4] = 1;
        for (unsigned row = 0; row < 40; ++row) {
            regs[c][(0x1150 + row * 32) / 4] = row;
            regs[c][(0x1154 + row * 32) / 4] = 0x4040012 + row;
            regs[c][(0x1158 + row * 32) / 4] = 700 + row;
        }
        r.initialDesired[c] = regs[c][0x1f10 / 4] = 20 + c;
        regs[c][(0x1154 + r.initialDesired[c] * 32) / 4] =
            0x4040000 | (QcomOSMClock::BootHz[c] / 19200000);
        r.lutDigest[c] = lutDigest(read, c);
    }
    assert(validate(r, read) == Status::Accepted);
    for (unsigned c = 0; c < 2; ++c)
        for (unsigned row = 0; row < 40; ++row) {
            constexpr unsigned columns[] = {0, 4, 8, 12, 20};
            for (unsigned column : columns) {
                regs[c][(0x1150 + row * 32 + column) / 4] ^= 1;
                assert(validate(r, read) == Status::LUTChanged);
                regs[c][(0x1150 + row * 32 + column) / 4] ^= 1;
            }
        }
    regs[1][0x1004 / 4] = 0; assert(validate(r, read) == Status::Disabled);
    regs[1][0x1004 / 4] = 1;
    regs[1][0x1f10 / 4] = 1; assert(validate(r, read) == Status::DesiredChanged);
    regs[1][0x1f10 / 4] = r.initialDesired[1];
    const auto oldIndex=r.initialDesired[0];
    r.initialDesired[0]=regs[0][0x1f10 / 4]=0;
    assert(validate(r,read)==Status::BootRateChanged);
    r.initialDesired[0]=regs[0][0x1f10 / 4]=oldIndex;
    r.initialDesired[0]=40; assert(!valid(r)); r.initialDesired[0]=oldIndex;
    r.reserved[1] = 1; assert(!valid(r)); r.reserved[1] = 0;
    r.version = 1; assert(!valid(r)); r.version = Version;
    r.firmwareProfile = 0; assert(!valid(r)); r.firmwareProfile = QcomOSMTZ::Profile;
    for (unsigned c = 0; c < 2; ++c)
        for (unsigned bit = 0; bit < 8; ++bit) {
            r.firmwareChecks[c] ^= 1U << bit; assert(!valid(r));
            r.firmwareChecks[c] ^= 1U << bit;
        }
    for (unsigned bit = 0; bit < 8; ++bit) {
        r.flags ^= 1U << bit; assert(!valid(r)); r.flags ^= 1U << bit;
    }
    r.chipVersion = 0x10000; assert(!valid(r)); r.chipVersion = 0x20000;
    assert(valid(r));
    Monitor monitor; QcomOSMInit::SecureIO<Monitor> secure(monitor);
    uint32_t v = 0;
    assert(!secure.read(0x179c033c, v));
    assert(secure.probe());
    const auto beforeDenied = monitor.calls.size();
    assert(!secure.read(0x179c0300, v));
    assert(!secure.write(0x179c03dc, 1)); // SEQ55 is a protected TZ-owned value.
    assert(!secure.write(0x179c23e0, 1)); // SEQ56, other cluster.
    assert(monitor.calls.size() == beforeDenied);
    monitor.mode = 1; assert(secure.read(0x179c033c, v)); assert(v == 0xa55a);
    const auto &resume = monitor.calls.back();
    assert(resume.x[0] == 1 && resume.x[1] == 1 && resume.x[2] == 0x179c033c &&
           resume.x[3] == 0 && resume.x[6] == 0xabc);
    assert(secure.write(0x179c03e4, 0x12345678));
    assert(monitor.calls.back().x[3] == 0x12345678);
    monitor.mode = 2; assert(!secure.write(0x179c03e4, 1));
    const auto count = monitor.calls.size();
    assert(!secure.read(0x179c033c, v) && monitor.calls.size() == count);
    Monitor stuck; QcomOSMInit::SecureIO<Monitor> interrupted(stuck);
    assert(interrupted.probe()); stuck.mode = 3;
    assert(!interrupted.read(0x179c033c, v)); assert(stuck.calls.size() == 1027);
    puts("OSM handoff and SCM atomic IO tests passed");
}
