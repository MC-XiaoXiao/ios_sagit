// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <WLANProtocol.h>

namespace WLAN {
enum class PipeUse : uint8_t { HTC, ControlRX, WMIRX, WMITX, HTTTX, HTTRX, FirmwareOnly, Diagnostic, MicrocontrollerRX, HTTRX2, HTTRX3, PacketLog };
struct PipeConfiguration { PipeUse use; uint16_t sourceEntries, destinationEntries, maximumBytes; };
// Host configuration observed in ath10k SNOC. Firmware target pipe directions
// differ for CE5; these are HOST rings, not a WLAN_CFG QMI message.
constexpr PipeConfiguration Pipes[CopyEngineCount] = {
    {PipeUse::HTC,16,0,2048}, {PipeUse::ControlRX,0,512,2048},
    {PipeUse::WMIRX,0,64,2048}, {PipeUse::WMITX,32,0,2048},
    {PipeUse::HTTTX,2048,0,256}, {PipeUse::HTTRX,0,512,512},
    {PipeUse::FirmwareOnly,0,0,0}, {PipeUse::Diagnostic,2,2,2048},
    {PipeUse::MicrocontrollerRX,0,128,2048}, {PipeUse::HTTRX2,0,512,2048},
    {PipeUse::HTTRX3,0,512,2048}, {PipeUse::PacketLog,0,512,2048}
};
struct alignas(8) CEDescriptor { uint8_t bytes[16]; };
static_assert(sizeof(CEDescriptor) == 16, "WCN3990 uses 64-bit CE descriptors");

inline Result encodeDescriptor(CEDescriptor &out, uint64_t address, uint32_t length, uint16_t metadata) {
    if (!length || length > UINT16_MAX || metadata > 0xfff || !validDMA(address, length)) return Result::Invalid;
    // Initial path supports one contiguous buffer only; no gather/byte swap.
    CEDescriptor descriptor = {};
    write32(descriptor.bytes, uint32_t(address));
    write32(descriptor.bytes + 4, uint32_t(address >> 32));
    write16(descriptor.bytes + 8, uint16_t(length));
    write16(descriptor.bytes + 10, uint16_t(metadata << 4));
    out = descriptor; return Result::Success;
}

// Software ownership ledger, separate from device ring memory. A producer
// must use IODMACommand segments, publish coherent descriptors and issue the
// device barrier BEFORE advancing the hardware producer index. No physical
// address casts or MMIO operations are performed by this ledger.
template<unsigned Count> class CopyEngineRing {
    static_assert(Count >= 2 && Count <= 2048 && !(Count & (Count - 1)), "power-of-two CE ring");
    uint64_t cookies[Count] = {};
    unsigned producer = 0, consumer = 0;
    uint64_t generation = 0, lastGeneration = 0;
    bool quarantined = false;
public:
    unsigned pending() const { return (producer - consumer) & (Count - 1); }
    bool isQuarantined() const { return quarantined; }
    Result begin(uint64_t epoch) {
        if (generation || pending() || quarantined) return Result::Busy;
        if (!epoch || epoch <= lastGeneration) return Result::Invalid;
        generation = lastGeneration = epoch; producer = consumer = 0; return Result::Success;
    }
    Result submit(uint64_t epoch, uint64_t cookie, unsigned &index) {
        if (!generation || epoch != generation || quarantined) return Result::NotReady;
        if (!cookie) return Result::Invalid;
        if (pending() == Count - 1) return Result::Busy;
        // A repeated token would make completion ownership ambiguous.
        for (unsigned n = consumer; n != producer; n = (n + 1) & (Count - 1))
            if (cookies[n] == cookie) return Result::Invalid;
        index = producer; cookies[producer] = cookie; producer = (producer + 1) & (Count - 1);
        return Result::Success;
    }
    Result complete(uint64_t epoch, unsigned hardwareIndex, uint64_t &cookie) {
        if (!generation || epoch != generation || quarantined) return Result::NotReady;
        if (hardwareIndex >= Count || !pending() || hardwareIndex != consumer) return Result::Invalid;
        cookie = cookies[consumer]; cookies[consumer] = 0;
        consumer = (consumer + 1) & (Count - 1); return Result::Success;
    }
    void timeout() { if (pending()) quarantined = true; }
    // The caller must first prove CE/remote DMA isolation and account for all
    // canceled cookies. A timeout alone may not call this method.
    Result retireAfterIsolation(uint64_t epoch) {
        if (!generation || epoch != generation) return Result::NotReady;
        for (auto &cookie : cookies) cookie = 0;
        producer = consumer = 0; generation = 0; quarantined = false; return Result::Success;
    }
};
}
