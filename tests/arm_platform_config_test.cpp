#include "ARMPlatformConfig.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace ARMPlatformConfig;

static void write64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}

int main()
{
    assert(enabled(nullptr, 0));
    assert(enabled("okay", 5));
    assert(enabled("ok", 3));
    assert(!enabled("disabled", 9));
    assert(!enabled("okay", 4));
    assert(!enabled("okay\0extra", 11));
    assert(!enabled("", 1));
    assert(!stringEquals(nullptr, 0, "cpu"));
    assert(!stringEquals("cp", 2, "cpu"));
    assert(stringEquals("cpu", 4, "cpu"));
    assert(conduit("smc", 4) == kSMC);
    assert(conduit("hvc", 4) == kHVC);
    assert(conduit("smc", 3) == kNoPSCI);
    assert(conduit(nullptr, 0) == kNoPSCI);
    assert(conduit("unknown", 8) == kNoPSCI);

    // Unaligned byte buffers must work; DT data isn't a typed C++ object.
    uint8_t buffer[49] = {};
    uint8_t *reg = buffer + 1;
    write64(reg, 0x0c1b0000);
    write64(reg + 8, 0x1000);
    assert(readLE64(reg) == 0x0c1b0000);
    assert(validReg(reg, 16, 1ULL << 40));
    assert(!validReg(nullptr, 16, 1ULL << 40));
    assert(!validReg(reg, 0, 1ULL << 40));
    assert(!validReg(reg, 15, 1ULL << 40));
    write64(reg, (1ULL << 40) - 0x1000);
    assert(validReg(reg, 16, 1ULL << 40));
    write64(reg, 1ULL << 40);
    assert(!validReg(reg, 16, 1ULL << 40));
    write64(reg, UINT64_MAX - 3);
    assert(!validReg(reg, 16, 1ULL << 40));
    write64(reg, 4);
    write64(reg + 8, UINT64_MAX - 3);
    assert(!validReg(reg, 16, UINT64_MAX));
    write64(reg + 8, 0);
    assert(!validReg(reg, 16, 1ULL << 40));
    write64(reg, 1);
    write64(reg + 8, 0x1000);
    assert(!validReg(reg, 16, 1ULL << 40));
    write64(reg, 0);
    assert(validReg(reg, 16, 1ULL << 40));
    write64(reg + 16, 0x17a00000);
    write64(reg + 24, 0x10000);
    assert(validReg(reg, 32, 1ULL << 40));
    write64(reg + 24, 3);
    assert(!validReg(reg, 32, 1ULL << 40));
    // Big-endian FDT bytes must not pass as the UART's Apple DT tuple.
    const uint8_t fdt[] = {0,0,0,0,0x0c,0x1b,0,0,0,0,0,0,0,0,0x10,0};
    assert(!validReg(fdt, sizeof(fdt), 1ULL << 40));

    const uint32_t affinities[] = {0,1,2,3,0x100,0x101,0x102,0x103};
    for (auto a : affinities) assert(validSDM835Affinity(a));
    assert(!validSDM835Affinity(4));
    assert(!validSDM835Affinity(0x104));
    assert(!validSDM835Affinity(0x200));
    assert(!validSDM835Affinity(0x10000));
    uint32_t seen = 0;
    for (auto a : affinities) {
        write64(reg, a);
        assert(addSDM835CPU(reg, 4, seen));
    }
    assert(seen == 0xff);
    assert(!addSDM835CPU(reg, 4, seen));
    assert(!addSDM835CPU(reg, 8, seen));
    assert(!addSDM835CPU(nullptr, 4, seen));
    write64(reg, 0x200);
    assert(!addSDM835CPU(reg, 4, seen));
    assert(seen == 0xff);
    write64(reg, 0x102);
    seen = 0;
    assert(addSDM835CPU(reg, 4, seen));
    assert(seen == 0x40);
    write64(reg, 0);
    write64(reg + 8, 0);
    write64(reg + 16, 0x100000000ULL);
    assert(validRanges(reg, 24, 1ULL << 40));
    assert(validRanges(nullptr, 0, 1ULL << 48));
    assert(!validRanges(nullptr, 24, 1ULL << 48));
    assert(!validRanges(reg, 23, 1ULL << 40));
    write64(reg + 8, (1ULL << 40) - 4);
    assert(!validRanges(reg, 24, 1ULL << 40));
    write64(reg + 8, 0);
    write64(reg, UINT64_MAX - 3);
    assert(!validRanges(reg, 24, 1ULL << 40));
    write64(reg, 0);
    write64(reg + 16, 0);
    assert(!validRanges(reg, 24, 1ULL << 40));
    std::puts("ARM platform: DT bounds, endianness, status, PSCI conduit and SDM835 affinity passed");
}
