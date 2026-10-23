// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace USB {
// DMA preparation workers need ordering without retaining or invoking an
// MMIO object whose generation may already have been revoked. This object
// owns no mapping and performs no register access.
class DMAOrdering {
public:
    void dmaReadBarrier() { __asm__ volatile("dmb oshld" ::: "memory"); }
    void dmaWriteBarrier() { __asm__ volatile("dmb oshst" ::: "memory"); }
};
// arm64 device mappings are installed with kIOMapInhibitCache by the adapter.
// DMA buffers use IODMACommand and coherent memory, never this register map.
class MMIO {
    volatile uint8_t *base;
    size_t length;
    bool accessFailed = false;
    bool contains(uint32_t offset,unsigned width=4) {
        if (accessFailed || !base || (reinterpret_cast<uintptr_t>(base) & 3) ||
            (offset & (width-1)) || (reinterpret_cast<uintptr_t>(base)&(width-1)) || length < width || offset > length - width) {
            accessFailed = true; return false;
        }
        return true;
    }
public:
    MMIO(volatile void *mapping, size_t bytes) :
        base(static_cast<volatile uint8_t *>(mapping)), length(bytes) {}
    bool failed() const { return accessFailed; }
    // A revoked role closes its snapshot lease before halt/unmap. This
    // read-only path can then run under an IRQ-safe provider lock without
    // racing the ordinary access path's sticky fault bookkeeping.
    uint32_t readSnapshot32(uint32_t offset) const {
        if (!base || (reinterpret_cast<uintptr_t>(base)&3) || (offset&3) ||
            length<4 || offset>length-4) return UINT32_MAX;
        const auto value=*reinterpret_cast<volatile uint32_t *>(base+offset);
        __asm__ volatile("dmb oshld" ::: "memory");
        return value;
    }
    uint32_t read(uint32_t offset) {
        if (!contains(offset)) return UINT32_MAX;
        uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + offset);
        __asm__ volatile("dmb oshld" ::: "memory");
        return value;
    }
    void write(uint32_t offset, uint32_t value) {
        if (!contains(offset)) return;
        __asm__ volatile("dmb oshst" ::: "memory");
        *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    uint64_t read64(uint32_t offset) {
        if (!contains(offset,8)) return UINT64_MAX;
        const uint64_t value=*reinterpret_cast<volatile uint64_t *>(base+offset);
        __asm__ volatile("dmb oshld" ::: "memory");return value;
    }
    void write64(uint32_t offset,uint64_t value) {
        if (!contains(offset,8)) return;
        __asm__ volatile("dmb oshst" ::: "memory");
        *reinterpret_cast<volatile uint64_t *>(base+offset)=value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    void dmaReadBarrier() { __asm__ volatile("dmb oshld" ::: "memory"); }
    void dmaWriteBarrier() { __asm__ volatile("dmb oshst" ::: "memory"); }
    void delay(unsigned microseconds) {
        // Short hardware handshakes also run under an IOCommandGate. Keep
        // those bounded delays independent of scheduler/idle latency.
        uint64_t start, now, frequency;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
        uint64_t ticks = (frequency * microseconds + 999999) / 1000000;
        do { __asm__ volatile("yield; mrs %0, cntvct_el0" : "=r"(now)); } while (now - start < ticks);
    }
};
}
