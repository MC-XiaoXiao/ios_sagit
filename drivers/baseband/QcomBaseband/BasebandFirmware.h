// SPDX-License-Identifier: GPL-2.0-only
// Qualcomm MDT layout: Linux v6.12 mdt_loader.c and qcom_q6v5_mss.c.
#pragma once
#include <BasebandProtocol.h>

namespace Baseband {
constexpr uint32_t MDTTypeMask = 7u << 24, MDTHash = 2u << 24,
    MDTRelocatable = 1u << 27;
constexpr unsigned MaxSegments = 96;
struct Segment {
    uint32_t index = 0, fileOffset = 0, physical = 0, fileBytes = 0,
        memoryBytes = 0, flags = 0;
};
// A validated immutable monolithic ELF32/Hexagon view. The caller retains the
// source bytes until the remote loader has finished consuming them.
class FirmwareImage {
    const uint8_t *data = nullptr;
    size_t size = 0;
    Segment loads[MaxSegments]{};
    unsigned count = 0;
    uint32_t first = 0, span = 0, total = 0, headerSize = 0,
        hashOffset = 0, hashSize = 0;
    bool relocation = false;
public:
    void clear() {
        data = nullptr; size = 0; count = 0; first = span = total = 0;
        headerSize = hashOffset = hashSize = 0; relocation = false;
    }
    Result parse(const void *buffer, size_t bytes) {
        clear();
        if (!buffer || bytes < 52 || bytes > 256u * 1024u * 1024u)
            return Result::Invalid;
        const auto *p = static_cast<const uint8_t *>(buffer);
        if (read32(p) != 0x464c457f || p[4] != 1 || p[5] != 1 || p[6] != 1 ||
            read16(p + 16) != 2 || read16(p + 18) != 164 ||
            read32(p + 20) != 1 || read16(p + 40) != 52 ||
            read16(p + 42) != 32 || read32(p + 28) != 52)
            return Result::Unsupported;
        const unsigned phnum = read16(p + 44);
        const size_t tableEnd = 52u + size_t(phnum) * 32u;
        if (phnum < 3 || phnum > MaxSegments || tableEnd > bytes)
            return Result::Invalid;
        const auto *ph0 = p + 52;
        const uint32_t headers = read32(ph0 + 16);
        if (read32(ph0) == 1 || read32(ph0 + 4) != 0 ||
            headers < tableEnd || headers > bytes)
            return Result::Invalid;
        unsigned n = 0, hashes = 0;
        uint64_t lo = UINT64_MAX, hi = 0, sum = 0;
        uint32_t hashAt = 0, hashBytes = 0;
        bool reloc = false;
        for (unsigned i = 1; i < phnum; ++i) {
            const auto *h = p + 52 + 32 * i;
            const uint32_t type = read32(h), offset = read32(h + 4),
                address = read32(h + 12), file = read32(h + 16),
                memory = read32(h + 20), flags = read32(h + 24);
            if ((flags & MDTTypeMask) == MDTHash) {
                if (++hashes != 1 || !file || !range(offset, file, bytes) ||
                    offset < headers)
                    return Result::Invalid;
                hashAt = offset; hashBytes = file; continue;
            }
            if (type != 1 || !memory) continue;
            // Split .mdt/.bNN inputs must first be imported with all segments.
            // A missing segment never becomes a zero-filled runnable image.
            if (file > memory || (file && !range(offset, file, bytes)) ||
                !range(address, memory, uint64_t(1) << 32))
                return Result::Invalid;
            if (file && offset < headers) return Result::Invalid;
            for (unsigned j = 0; j < n; ++j)
                if (overlaps(address, memory, loads[j].physical, loads[j].memoryBytes))
                    return Result::Invalid;
            loads[n++] = {i, offset, address, file, memory, flags};
            if (address < lo) lo = address;
            const uint64_t end = (uint64_t(address) + memory + 4095) & ~4095ULL;
            if (end > hi) hi = end;
            sum += memory;
            reloc |= (flags & MDTRelocatable) != 0;
        }
        if (hashes != 1 || !n || lo >= hi || hi - lo > UINT32_MAX ||
            sum > UINT32_MAX || (lo & 4095))
            return Result::Invalid;
        for (unsigned i = 0; i < n; ++i)
            if (overlaps(loads[i].fileOffset, loads[i].fileBytes, hashAt, hashBytes))
                return Result::Invalid;
        data = p; size = bytes; count = n;
        first = uint32_t(lo); span = uint32_t(hi - lo); total = uint32_t(sum);
        headerSize = headers; hashOffset = hashAt; hashSize = hashBytes;
        relocation = reloc;
        return Result::OK;
    }
    bool valid() const { return data != nullptr; }
    const uint8_t *bytes() const { return data; }
    size_t byteCount() const { return size; }
    unsigned segmentCount() const { return count; }
    const Segment *segment(unsigned i) const { return valid() && i < count ? &loads[i] : nullptr; }
    uint32_t lowestAddress() const { return first; }
    uint32_t memorySpan() const { return span; }
    uint32_t codeLength() const { return total; }
    bool relocatable() const { return relocation; }
    size_t metadataBytes() const { return size_t(headerSize) + hashSize; }
    Result metadata(void *out, size_t capacity) const {
        if (!valid() || !out || capacity < metadataBytes()) return Result::Invalid;
        auto *dst = static_cast<uint8_t *>(out);
        // A separate DMA allocation is mandatory: assembling metadata in the
        // original ELF would destroy the hash segment's original file offset.
        if (overlaps(reinterpret_cast<uintptr_t>(dst), metadataBytes(),
                     reinterpret_cast<uintptr_t>(data), size)) return Result::Invalid;
        for (size_t i = 0; i < headerSize; ++i) dst[i] = data[i];
        for (size_t i = 0; i < hashSize; ++i) dst[headerSize + i] = data[hashOffset + i];
        return Result::OK;
    }
    Result destination(const Region &region, unsigned index, uint64_t &address) const {
        const auto *s = segment(index);
        if (!s || !region.valid()) return Result::Invalid;
        const uint64_t base = relocation ? first : region.address;
        if (s->physical < base || !range(s->physical - base, s->memoryBytes, region.bytes))
            return Result::Invalid;
        address = region.address + (s->physical - base);
        return Result::OK;
    }
    Result validateRegion(const Region &region) const {
        if (!valid() || !region.valid()) return Result::Invalid;
        for (unsigned i = 0; i < count; ++i) {
            uint64_t address;
            if (destination(region, i, address) != Result::OK) return Result::Invalid;
        }
        return Result::OK;
    }
    // Sink writes into a reserved, host-owned destination and performs cache
    // maintenance. Validate the complete image before the first write.
    template<class Sink> Result load(const Region &region, Sink &sink) const {
        if (validateRegion(region) != Result::OK) return Result::Invalid;
        for (unsigned i = 0; i < count; ++i) {
            uint64_t address = 0; destination(region, i, address);
            const auto &s = loads[i];
            if (s.fileBytes && !sink.copy(address, data + s.fileOffset, s.fileBytes))
                return Result::IO;
            if (s.memoryBytes > s.fileBytes &&
                !sink.zero(address + s.fileBytes, s.memoryBytes - s.fileBytes))
                return Result::IO;
            if (!sink.publish(address, s.memoryBytes)) return Result::IO;
        }
        return Result::OK;
    }
};
} // namespace Baseband
