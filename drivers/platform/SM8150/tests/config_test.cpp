#include "../SM8150Config.h"
#include <assert.h>
#include <stdio.h>

static void le64(uint8_t *p, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i) p[i] = value >> (8 * i);
}

int main()
{
    uint32_t seen = 0;
    uint8_t reg[8] = {};
    for (unsigned int i = 0; i < 8; ++i) {
        le64(reg, i << 8);
        assert(SM8150Config::addCPU(reg, 4, seen));
        assert(!SM8150Config::addCPU(reg, 4, seen));
        assert(SM8150Config::schedulerCluster(i << 8) == (i < 4 ? 0U : i < 7 ? 1U : 2U));
        assert(SM8150Config::schedulerCore(i << 8) == (i < 4 ? i : i < 7 ? i - 4 : 0));
    }
    assert(seen == 0xff);
    const uint32_t invalid[] = {1, 3, 0x101, 0x103, 0x800, 0x10000, 0x80000000, 0xffffffff};
    for (uint32_t affinity : invalid) {
        le64(reg, affinity);
        assert(!SM8150Config::addCPU(reg, 4, seen));
    }
    assert(!SM8150Config::addCPU(nullptr, 4, seen));
    assert(!SM8150Config::addCPU(reg, 8, seen));
    // A raw big-endian FDT cell must not accidentally pass Apple DT validation.
    const uint8_t fdtReg[] = {0, 0, 1, 0};
    assert(!SM8150Config::addCPU(fdtReg, 4, seen));
    uint8_t ranges[24] = {};
    le64(ranges + 16, 0x100000000ULL);
    assert(SM8150Config::identityRanges(ranges, sizeof(ranges)));
    ranges[0] = 1;
    assert(!SM8150Config::identityRanges(ranges, sizeof(ranges)));
    assert(!SM8150Config::identityRanges(nullptr, 0));
    uint8_t gic[32] = {};
    le64(gic, 0x17a00000); le64(gic + 8, 0x10000);
    le64(gic + 16, 0x17a60000); le64(gic + 24, 0x100000);
    assert(SM8150Config::gicRegisters(gic, sizeof(gic)));
    le64(gic + 16, 0x17b00000); // SDM835's redistributor must be rejected.
    assert(!SM8150Config::gicRegisters(gic, sizeof(gic)));
    assert(!SM8150Config::gicRegisters(gic, sizeof(gic) - 1));
    puts("SM8150: CPU affinity, scheduler groups, Apple DT MMIO validation passed");
}
