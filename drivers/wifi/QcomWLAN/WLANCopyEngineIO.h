// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "WLANCopyEngine.h"
#include "WLANDMA.h"

namespace WLAN {
// WCN3990 register map. IO supplies retained MMIO access, barriers and an
// actual remote/stream isolation check. An index becoming idle is no such
// proof. CE6 is autonomous firmware DMA and may never be configured here.
template<class IO,unsigned Count,bool Receive> class CopyEngineIO {
    IO &io;
    CopyEngineRing<Count> ledger;
    volatile uint8_t *descriptors = nullptr;
    uint32_t capacities[Count]{};
    uint32_t base = 0;
    unsigned producer = 0, consumer = 0, maximum = 0;
    uint64_t generation = 0;
    bool configured = false, fault = false;
    void *ringOwner = nullptr;
    void (*quarantineRing)(void *) = nullptr;
    Result (*isolateRing)(void *,uint64_t) = nullptr;
    static constexpr uint32_t BaseLow = Receive ? 0x0c : 0, BaseHigh = Receive ? 0x10 : 4,
        Size = Receive ? 0x14 : 8, Producer = Receive ? 0x40 : 0x3c, Consumer = Receive ? 0x48 : 0x44;
    bool write(uint32_t offset,uint32_t value) {
        io.write(base + offset,value); return io.read(base + offset) == value;
    }
    Result poison() {
        fault = true; ledger.timeout();
        if (ringOwner) quarantineRing(ringOwner);
        return Result::Fault;
    }
public:
    explicit CopyEngineIO(IO &registers) : io(registers) {}
    bool quarantined() const { return fault || ledger.isQuarantined(); }
    bool ownsHardware() const { return generation != 0; }
    unsigned pending() const { return ledger.pending(); }
    template<class Buffer> Result configure(unsigned engine,uint64_t epoch,Buffer &ring,unsigned maximumBytes) {
        if (configured || generation) return Result::Busy;
        if (engine >= CopyEngineCount || engine == 6 || !epoch || !maximumBytes || maximumBytes > UINT16_MAX ||
            ring.currentState() != DMAState::Allocated || ring.capacity() != Count * sizeof(CEDescriptor) ||
            !ring.bytes() || !validDMA(ring.busAddress(),ring.capacity(),8)) return Result::Invalid;
        auto result = io.isolated(epoch); if (result != Result::Success) return result;
        base = 0x240000 + engine * 0x1000;
        // Only stopped, empty rings may adopt a fresh memory allocation.
        if (io.read(base + Producer) || io.read(base + Consumer)) return Result::Busy;
        result = ledger.begin(epoch); if (result != Result::Success) return result;
        result = ring.publish(epoch);
        if (result != Result::Success) { ledger.retireAfterIsolation(epoch); return result; }
        ringOwner = &ring;
        quarantineRing = [](void *p) { static_cast<Buffer *>(p)->quarantine(); };
        isolateRing = [](void *p,uint64_t e) { return static_cast<Buffer *>(p)->isolate(e); };
        generation = epoch; descriptors = static_cast<volatile uint8_t *>(ring.bytes()); maximum = maximumBytes;
        io.publishBarrier();
        const uint32_t old = io.read(base + 0x18);
        if (old == UINT32_MAX || !write(0x2c,0) || !write(0x34,0) ||
            !write(BaseLow,uint32_t(ring.busAddress())) || !write(BaseHigh,uint32_t(ring.busAddress() >> 32)) ||
            !write(Size,Count) || !write(0x18,(old & ~0xffffu & ~(7u << 17)) | maximumBytes)) return poison();
        configured = true; return Result::Success;
    }
    // address is an IODMACommand segment of a retained DMABuffer. Failure
    // after publication poisons the ledger and preserves its cookie.
    Result post(uint64_t epoch,uint64_t cookie,uint64_t address,uint32_t bytes,uint16_t metadata = 0) {
        if (!configured || fault || epoch != generation) return Result::NotReady;
        const auto errors = io.read(base + 0x38);
        if (errors == UINT32_MAX || (errors & 0x3e0)) return poison();
        if (!bytes || bytes > maximum || !validDMA(address,bytes) || metadata > 0xfff) return Result::Invalid;
        CEDescriptor descriptor{};
        if constexpr (Receive) {
            if (metadata) return Result::Invalid;
            write32(descriptor.bytes,uint32_t(address)); write32(descriptor.bytes + 4,uint32_t(address >> 32));
        } else {
            const auto result = encodeDescriptor(descriptor,address,bytes,metadata);
            if (result != Result::Success) return result;
        }
        unsigned index = 0; const auto result = ledger.submit(epoch,cookie,index);
        if (result != Result::Success) return result;
        if (index != producer) return poison();
        capacities[index] = bytes;
        for (unsigned i = 0; i < sizeof(descriptor); ++i) descriptors[index * sizeof(descriptor) + i] = descriptor.bytes[i];
        io.publishBarrier(); producer = (producer + 1) & (Count - 1);
        if (!write(Producer,producer)) return poison();
        return Result::Success;
    }
    Result completion(uint64_t epoch,uint64_t &cookie,uint32_t &bytes,uint16_t &metadata) {
        if (!configured || fault || epoch != generation) return Result::NotReady;
        const auto errors = io.read(base + 0x38);
        if (errors == UINT32_MAX || (errors & 0x3e0)) return poison();
        if (!ledger.pending()) return Result::NotReady;
        if constexpr (!Receive) {
            const auto hardware = io.read(base + Consumer);
            if (hardware >= Count || ((hardware - consumer) & (Count - 1)) > ledger.pending()) return poison();
            if (hardware == consumer) return Result::NotReady;
        }
        io.consumeBarrier(); CEDescriptor descriptor{};
        for (unsigned i = 0; i < sizeof(descriptor); ++i) descriptor.bytes[i] = descriptors[consumer * sizeof(descriptor) + i];
        bytes = read16(descriptor.bytes + 8); metadata = read16(descriptor.bytes + 10) >> 4;
        if constexpr (Receive) { if (!bytes) return Result::NotReady; }
        if (!bytes || bytes > capacities[consumer]) return poison();
        const auto result = ledger.complete(epoch,consumer,cookie);
        if (result != Result::Success) return poison();
        descriptors[consumer * sizeof(descriptor) + 8] = 0;
        descriptors[consumer * sizeof(descriptor) + 9] = 0;
        capacities[consumer] = 0; consumer = (consumer + 1) & (Count - 1);
        return Result::Success;
    }
    Result shutdown(uint64_t epoch) {
        if (!generation) return Result::Success;
        if (epoch != generation) return Result::NotReady;
        auto result = io.isolated(epoch);
        if (result != Result::Success) { fault = true; ledger.timeout(); return result; }
        if (!write(0x2c,0) || !write(0x34,0) || !write(Size,0) || !write(BaseLow,0) || !write(BaseHigh,0)) return poison();
        result = isolateRing(ringOwner,epoch); if (result != Result::Success) return poison();
        io.consumeBarrier(); result = ledger.retireAfterIsolation(epoch);
        if (result != Result::Success) return result;
        descriptors = nullptr; ringOwner = nullptr; configured = fault = false; generation = producer = consumer = 0;
        return Result::Success;
    }
};
}
