// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace XHCI {
struct alignas(16) TRB {
    uint32_t lo, hi, status, control;
    uint64_t parameter() const { return uint64_t(hi) << 32 | lo; }
    unsigned type() const { return (control >> 10) & 63; }
};
static_assert(sizeof(TRB) == 16 && alignof(TRB) == 16, "xHCI DMA TRB layout");
constexpr uint32_t Cycle = 1, ToggleCycle = 2, Chain = 1u << 4;
constexpr uint32_t IOC = 1u << 5, Immediate = 1u << 6, InterruptShort = 1u << 2;
enum Type : unsigned {
    Normal = 1, Setup = 2, Data = 3, Status = 4, Iso = 5, Link = 6,
    EventData = 7, TransferNoop = 8, EnableSlot = 9, DisableSlot = 10,
    AddressDevice = 11, ConfigureEndpoint = 12, EvaluateContext = 13,
    ResetEndpoint = 14, StopEndpoint = 15, SetDequeue = 16, ResetDevice = 17,
    CommandNoop = 23, TransferEvent = 32, CommandEvent = 33, PortEvent = 34,
    MicroFrameWrapEvent = 39
};
inline TRB trb(uint64_t parameter, uint32_t status, uint32_t control) {
    return {uint32_t(parameter), uint32_t(parameter >> 32), status, control};
}
inline bool segment(uint64_t address, unsigned entries, unsigned bits) {
    return entries >= 16 && entries <= 4096 &&
        USB::dmaRange(address, uint64_t(entries) * 16, bits, 64) &&
        (address & 0xffff) + uint64_t(entries) * 16 <= 65536;
}

// A single segment with a Toggle Cycle Link TRB. DMA memory must be coherent;
// barriers alone cannot replace cache maintenance for noncoherent mappings.
// The IOKit adapter serializes enqueue/retire and associates each TD with its
// request; it retires only after a hardware completion, or a proven halt.
template<class Order> class ProducerRing {
    Order &order;
    volatile TRB *memory = nullptr;
    uint64_t base = 0;
    unsigned entries = 0, enqueueAt = 0, dequeueAt = 0, outstanding = 0;
    uint32_t cycle = Cycle;
public:
    explicit ProducerRing(Order &barriers) : order(barriers) {}
    ProducerRing(const ProducerRing &)=delete;
    ProducerRing &operator=(const ProducerRing &)=delete;
    bool initialize(TRB *cpu, uint64_t dma, unsigned count, unsigned bits) {
        if (memory || !cpu || (reinterpret_cast<uintptr_t>(cpu) & 63) ||
            !segment(dma, count, bits)) return false;
        memory = cpu; base = dma; entries = count;
        for (unsigned i = 0; i < entries; ++i) {
            memory[i].lo = memory[i].hi = memory[i].status = memory[i].control = 0;
        }
        memory[entries - 1].lo = uint32_t(base);
        memory[entries - 1].hi = uint32_t(base >> 32);
        memory[entries - 1].control = Link << 10 | ToggleCycle;
        order.dmaWriteBarrier();
        return true;
    }
    // Keep one data slot empty in addition to the Link. Without this gap a
    // completion of the last data TRB could let software overwrite the Link
    // cycle before the hardware has actually traversed that Link.
    unsigned available() const { return memory ? entries - 2 - outstanding : 0; }
    unsigned pending() const { return outstanding; }
    uint64_t startAddress() const { return base; }
    uint64_t nextAddress() const { return base + uint64_t(enqueueAt) * 16; }
    uint32_t producerCycle() const { return cycle; }
    unsigned dataEntries() const { return entries ? entries-1 : 0; }
    unsigned allocationBytes() const { return entries*sizeof(TRB); }
    uint64_t following(unsigned offset) const {
        return entries ? base+uint64_t((enqueueAt+offset)%(entries-1))*16 : 0;
    }
    // A physical error event reports a residue for one payload TRB. Read
    // only entries still owned by this queue, before retiring their TD.
    bool copyPending(uint64_t address,TRB &out) {
        if (!memory || address<base || (address-base)&15 || address-base>=uint64_t(entries-1)*16) return false;
        const auto at=unsigned((address-base)/16);
        if ((at+entries-1-dequeueAt)%(entries-1)>=outstanding) return false;
        order.dmaReadBarrier();const volatile auto &item=memory[at];
        out={item.lo,item.hi,item.status,item.control};return true;
    }

    USB::Result submit(const TRB *items, unsigned count, uint64_t &lastAddress) {
        if (!memory) return USB::Result::NotReady;
        if (!items || !count || count > entries - 2) return USB::Result::Invalid;
        if (count > available()) return USB::Result::Busy;
        // The source may not alias the live DMA ring. Validation precedes any
        // writes, so an invalid TD cannot leave a half-published chain.
        uintptr_t source = reinterpret_cast<uintptr_t>(items);
        uintptr_t destination = reinterpret_cast<uintptr_t>(memory);
        if (source > UINTPTR_MAX - uint64_t(count) * sizeof(TRB) ||
            (source < destination + uint64_t(entries) * sizeof(TRB) &&
             source + uint64_t(count) * sizeof(TRB) > destination)) return USB::Result::Invalid;
        for (unsigned i = 0; i < count; ++i) {
            if (!items[i].type() || items[i].type() == Link) return USB::Result::Invalid;
        }
        const unsigned first = enqueueAt;
        const uint32_t firstControl = (items[0].control & ~Cycle) | cycle;
        for (unsigned i = 0; i < count; ++i) {
            const TRB &v = items[i];
            memory[enqueueAt].lo = v.lo; memory[enqueueAt].hi = v.hi;
            memory[enqueueAt].status = v.status;
            uint32_t control = (v.control & ~Cycle) | cycle;
            // Hold the first TRB until every TRB and any intervening Link is
            // visible. The host can already be fetching earlier queued TDs.
            if (i == 0) control ^= Cycle;
            memory[enqueueAt].control = control;
            lastAddress = base + uint64_t(enqueueAt) * 16;
            if (++enqueueAt == entries - 1) {
                // Transfer TDs that cross a Link retain CH; commands do not.
                memory[enqueueAt].control = Link << 10 | ToggleCycle | cycle |
                    ((i + 1 < count && (v.control & Chain)) ? Chain : 0);
                enqueueAt = 0; cycle ^= Cycle;
            }
        }
        outstanding += count; // bookkeeping precedes publishing ownership
        order.dmaWriteBarrier();
        memory[first].control = firstControl;
        order.dmaWriteBarrier(); // doorbell may follow only after this barrier
        return USB::Result::Success;
    }

    bool retire(unsigned count, uint64_t expectedLast) {
        if (!memory || !count || count > outstanding) return false;
        unsigned last = (dequeueAt + count - 1) % (entries - 1);
        if (expectedLast != base + uint64_t(last) * 16) return false;
        dequeueAt = (last + 1) % (entries - 1); outstanding -= count;
        return true;
    }
};

// Event segments have no Link TRB. The hardware owns the producer cycle and
// software advances ERDP after handling each copied event.
template<class Order> class EventRing {
    Order &order;
    const volatile TRB *memory = nullptr;
    uint64_t base = 0;
    unsigned entries = 0, dequeueAt = 0;
    uint32_t cycle = Cycle;
public:
    explicit EventRing(Order &barriers) : order(barriers) {}
    EventRing(const EventRing &)=delete;
    EventRing &operator=(const EventRing &)=delete;
    bool initialize(TRB *cpu, uint64_t dma, unsigned count, unsigned bits) {
        if (memory || !cpu || (reinterpret_cast<uintptr_t>(cpu) & 63) ||
            !segment(dma, count, bits)) return false;
        for (unsigned i = 0; i < count; ++i) cpu[i] = {};
        order.dmaWriteBarrier();
        memory = cpu; base = dma; entries = count;
        return true;
    }
    bool peek(TRB &event) {
        if (!memory) return false;
        const volatile TRB &v = memory[dequeueAt];
        uint32_t control = v.control;
        if ((control & Cycle) != cycle) return false;
        order.dmaReadBarrier();
        event = {v.lo, v.hi, v.status, control};
        return true;
    }
    // Call once for each successfully peeked and handled event. The adapter
    // writes dequeueAddress() | ERDP.EHB to the interrupter after draining.
    void consume() {
        if (++dequeueAt == entries) { dequeueAt = 0; cycle ^= Cycle; }
    }
    uint64_t dequeueAddress() const { return base + uint64_t(dequeueAt) * 16; }
};

// Split a DMA segment at xHCI's 64 KiB boundaries. No TRB may span one.
// Caller computes TD Size from the entire request (not each SG segment) and
// selects IOC/CH after all segments have been described.
inline USB::Result appendData(TRB *output, unsigned capacity, unsigned &used,
    uint64_t address, uint32_t bytes, unsigned bits, Type type = Normal,
    uint32_t flags = 0) {
    if (!output || !bytes || !USB::dmaRange(address, bytes, bits) ||
        (type != Normal && type != Data && type != Iso) ||
        (flags & ~(Chain | IOC | InterruptShort | (1u << 9) | (type == Data ? 1u << 16 : 0))))
        return USB::Result::Invalid;
    unsigned needed = unsigned(((address & 65535) + uint64_t(bytes) + 65535) / 65536);
    if (used > capacity || needed > capacity - used) return USB::Result::Busy;
    while (bytes) {
        uint32_t length = 65536 - uint32_t(address & 65535);
        if (length > bytes) length = bytes;
        output[used++] = trb(address, length, uint32_t(type) << 10 | flags);
        address += length; bytes -= length;
    }
    return USB::Result::Success;
}
}
