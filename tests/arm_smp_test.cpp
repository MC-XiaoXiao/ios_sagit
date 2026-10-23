/* Exercise the production local-GIC handoff with warm firmware state. */
#include <pexpert/arm64/arm_smp.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

struct Registers {
    std::map<uint32_t, uint32_t> words;
    std::array<uint64_t, 8> typer{};
    std::vector<uint32_t> writes;
    uint64_t ticks = 0;
    bool stuckWake = false, stuckWrite = false, denyGroup = false, denyEnable = false;
    unsigned barriers = 0;

    static uint32_t read32(void *p, uint32_t off) {
        auto &r = *static_cast<Registers *>(p);
        uint32_t reg = off % ARM_GICR_STRIDE;
        if (reg == ARM_GICR_WAKER && r.stuckWake) return 4;
        if (reg == ARM_GICR_CTLR && r.stuckWrite) return 8;
        return r.words[off];
    }
    static uint64_t read64(void *p, uint32_t off) {
        assert(off % ARM_GICR_STRIDE == ARM_GICR_TYPER);
        return static_cast<Registers *>(p)->typer.at(off / ARM_GICR_STRIDE);
    }
    static void write32(void *p, uint32_t off, uint32_t val) {
        auto &r = *static_cast<Registers *>(p);
        uint32_t base = off & ~(ARM_GICR_STRIDE - 1), reg = off % ARM_GICR_STRIDE;
        r.writes.push_back(off);
        if (reg == ARM_GICR_ICENABLER0) r.words[base + ARM_GICR_ISENABLER0] &= ~val;
        else if (reg == ARM_GICR_ISENABLER0) {
            if (!r.denyEnable) r.words[off] |= val;
        } else if (reg == ARM_GICR_ICPENDR0 || reg == ARM_GICR_ICACTIVER0) r.words[off - 0x80] &= ~val;
        else if (reg != ARM_GICR_IGROUPR0 || !r.denyGroup) r.words[off] = val;
    }
    static void barrier(void *p) { static_cast<Registers *>(p)->barriers++; }
    static uint64_t counter(void *p) { return static_cast<Registers *>(p)->ticks++; }
    static void relax(void *) {}
    arm_gic_io io() { return { this, read32, read64, write32, barrier, counter, relax }; }

    Registers() {
        // Hardware frame order need not equal XNU's logical CPU order.
#if defined(ARM64_BOARD_CONFIG_SM8150)
        constexpr uint32_t affinities[] = { 0x600, 0x100, 0x400, 0x300, 0, 0x700, 0x200, 0x500 };
#else
        constexpr uint32_t affinities[] = { 0x102, 1, 0x100, 3, 0, 0x103, 2, 0x101 };
#endif
        for (unsigned i = 0; i < 8; i++) {
            typer[i] = uint64_t(affinities[i]) << 32 | (i == 7 ? 16 : 0);
            uint32_t base = i * ARM_GICR_STRIDE;
            words[base + ARM_GICR_WAKER] = 2;
            words[base + ARM_GICR_ISENABLER0] = 0xffffffff;
            words[base + 0x10200] = words[base + 0x10300] = 0xffffffff;
            words[base + ARM_GICR_ICFGR1] = 0xffffffff;
            words[base + ARM_GICR_IGROUPR0] = 0x20000000;
            words[base + ARM_GICR_IGRPMODR0] = 0xffffffff;
            words[base + ARM_GICR_IPRIORITYR0] = 0x11223344;
            words[base + ARM_GICR_IPRIORITYR0 + 16] = 0x55667788;
        }
    }
};

int main()
{
#if defined(ARM64_BOARD_CONFIG_SM8150)
    constexpr uint32_t affinities[] = { 0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700 };
    unsigned int groupCounts[3]{};
    for (uint32_t affinity : affinities) {
        auto group = sm8150_scheduler_group(affinity);
        assert(group < 3);
        assert(sm8150_group_core(affinity) == groupCounts[group]++);
        assert(arm_smp_cpu_bit(affinity) == 1U << (affinity >> 8));
    }
    assert(groupCounts[0] == 4 && groupCounts[1] == 3 && groupCounts[2] == 1);
    constexpr uint32_t invalidAffinities[] = { 1, 4, 0x101, 0x701, 0x800, 0x10000, UINT32_MAX };
    constexpr uint32_t probeAffinity = 0x100;
#else
    constexpr uint32_t affinities[] = { 0, 1, 2, 3, 0x100, 0x101, 0x102, 0x103 };
    constexpr uint32_t invalidAffinities[] = { 4, 0x104, 0x200, 0x10000, UINT32_MAX };
    constexpr uint32_t probeAffinity = 1;
#endif
    constexpr uint32_t mask = (1U << 19) | 1;
    uint32_t seen = 0;
    for (uint32_t affinity : affinities) {
        uint64_t value = 0;
        assert(arm_smp_sgi(affinity, 0, &value));
        assert(value == ((uint64_t(affinity >> 8) << 16) | (1ULL << (affinity & 3))));
        assert(!(seen & arm_smp_cpu_bit(affinity)));
        seen |= arm_smp_cpu_bit(affinity);
        Registers r;
        auto io = r.io();
        uint32_t local = UINT32_MAX;
        assert(arm_gic_find_cpu(&io, 8 * ARM_GICR_STRIDE, affinity, &local) == ARM_GIC_OK);
        auto before = r.words;
        assert(arm_gic_init_cpu(&io, local, 19, 0, 100, true) == ARM_GIC_OK);
        for (uint32_t off : r.writes) assert(off / ARM_GICR_STRIDE == local / ARM_GICR_STRIDE);
        assert(r.words[local + ARM_GICR_WAKER] == 0);
        assert(r.words[local + ARM_GICR_ISENABLER0] == 0xffffffff);
        assert(r.words[local + 0x10200] == (0xffffffffU & ~mask));
        assert(r.words[local + 0x10300] == (0xffffffffU & ~mask));
        assert(r.words[local + ARM_GICR_IGROUPR0] == (0x20000000 | mask));
        assert(r.words[local + ARM_GICR_IGRPMODR0] == 0xffffffffU);
        assert(r.words[local + ARM_GICR_IPRIORITYR0] == 0x11223380);
        assert(r.words[local + ARM_GICR_IPRIORITYR0 + 16] == 0x80667788);
        assert(r.words[local + ARM_GICR_ICFGR1] == (0xffffffffU & ~(2U << 6)));
        for (auto [off, valueBefore] : before) {
            if (off / ARM_GICR_STRIDE != local / ARM_GICR_STRIDE) assert(r.words[off] == valueBefore);
        }
        assert(r.barriers >= 4);
    }
    assert(seen == 0xff);
    for (uint32_t affinity : invalidAffinities) {
        uint64_t value = 123;
        assert(!arm_smp_sgi(affinity, 0, &value) && value == 123);
        assert(!arm_smp_cpu_bit(affinity));
    }
    uint64_t value;
    assert(!arm_smp_sgi(0, 16, &value));
    assert(!arm_smp_sgi(0, 0, nullptr));
    for (unsigned fault = 0; fault < 8; fault++) {
        Registers r;
        auto io = r.io();
        uint32_t local = 0;
        if (fault == 0) r.stuckWake = true;
        if (fault == 1) r.stuckWrite = true;
        if (fault == 2) r.denyGroup = true;
        if (fault == 3) r.denyEnable = true;
        if (fault == 4) r.typer[0] |= 2;
        if (fault == 5) r.typer[0] |= 16;
        if (fault == 6) r.typer[0] = r.typer[1];
        if (fault == 7) r.ticks = UINT64_MAX - 5; // deadline wraparound
        auto found = arm_gic_find_cpu(&io, 8 * ARM_GICR_STRIDE, probeAffinity, &local);
        if (fault >= 4 && fault <= 6) {
            assert(found == (fault == 4 ? ARM_GIC_BAD_LAYOUT : fault == 5 ? ARM_GIC_NO_CPU : ARM_GIC_DUPLICATE_CPU));
            assert(r.writes.empty());
        } else {
            assert(found == ARM_GIC_OK);
            auto result = arm_gic_init_cpu(&io, local, 19, 0, 10, true);
            assert(result == (fault == 0 ? ARM_GIC_WAKE_TIMEOUT : fault == 1 ? ARM_GIC_WRITE_TIMEOUT :
                fault == 7 ? ARM_GIC_OK : ARM_GIC_NOT_DELEGATED));
        }
    }
    Registers r;
    auto io = r.io();
    uint32_t local;
    assert(arm_gic_find_cpu(&io, 0, 0, &local) == ARM_GIC_BAD_LAYOUT);
    assert(arm_gic_find_cpu(&io, ARM_GICR_STRIDE + 1, 0, &local) == ARM_GIC_BAD_LAYOUT);
    assert(arm_gic_init_cpu(&io, 0, 32, 0, 10, true) == ARM_GIC_BAD_LAYOUT);
    assert(arm_gic_init_cpu(&io, 0, 19, 16, 10, true) == ARM_GIC_BAD_LAYOUT);
    // Secure firmware owns the group registers: zero reads are expected.
    r.words[ARM_GICR_IGROUPR0] = 0;
    r.words[ARM_GICR_IGRPMODR0] = 0;
    r.denyGroup = true;
    assert(arm_gic_init_cpu(&io, 0, 19, 0, 10, false) == ARM_GIC_OK);
    for (auto off : r.writes) assert(off != ARM_GICR_IGROUPR0 && off != ARM_GICR_IGRPMODR0);
    r.denyEnable = true;
    assert(arm_gic_init_cpu(&io, 0, 19, 0, 10, false) == ARM_GIC_NOT_DELEGATED);
    std::puts("ARM SMP: 8 affinities, shuffled redistributors, warm IRQ state, SGIs and handoff failures passed");
}
