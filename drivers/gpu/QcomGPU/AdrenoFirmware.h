// SPDX-License-Identifier: GPL-2.0-only
// Qualcomm MDT layout/metadata packing follows Linux v6.12 mdt_loader.c.
// The archive is a transport wrapper; signed ELF/hash/payload bytes are never
// rewritten. Only the secure monitor can authenticate the image.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace Adreno {
struct FirmwareBytes { const uint8_t *data = nullptr; uint32_t bytes = 0; };
class ZapArchive {
    const uint8_t *data = nullptr;
    uint32_t bytes = 0, count = 0;
    static uint32_t word(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
public:
    static constexpr uint32_t Magic = 0x50415a51, MDT = UINT32_MAX, Maximum = 32 * 1024 * 1024;
    bool open(const void *blob, size_t length) {
        data = nullptr; bytes = count = 0;
        if (!blob || length < 48 || length > Maximum) return false;
        const auto *p = static_cast<const uint8_t *>(blob);
        const auto entries = word(p + 8);
        if (word(p) != Magic || word(p + 4) != 1 || !entries || entries > 101 ||
            word(p + 12) != length || word(p + 16) != 16 || word(p + 20) || word(p + 24) || word(p + 28) ||
            32 + entries * 16 > length) return false;
        uint32_t end = 32 + entries * 16, previous = 0;
        for (unsigned i = 0; i < entries; ++i) {
            const auto *e = p + 32 + i * 16;
            const auto id = word(e), offset = word(e + 4), size = word(e + 8);
            if ((i ? id > 99 || (i > 1 && id <= previous) : id != MDT) || word(e + 12) || !size ||
                (offset & 15) || offset < end || offset > length || size > length - offset) return false;
            previous = id; end = offset + size;
        }
        if (end != length) return false;
        data = p; bytes = uint32_t(length); count = entries; return true;
    }
    FirmwareBytes file(uint32_t id) const {
        if (!data) return {};
        for (unsigned i = 0; i < count; ++i) {
            const auto *e = data + 32 + i * 16;
            if (word(e) == id) return {data + word(e + 4), word(e + 8)};
        }
        return {};
    }
};

class MDTImage {
    struct Segment { uint32_t address, memory, bytes, index; FirmwareBytes source; } segments[100]{};
    FirmwareBytes header{}, hash{};
    uint32_t count = 0, first = 0, extent = 0, metadata = 0, alignment = 4096;
    bool relocate = false, valid = false;
    static uint16_t half(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
    static uint32_t word(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
    static bool range(uint64_t size, uint64_t start, uint64_t length) { return start <= size && length <= size - start; }
    static void copy(uint8_t *out, const uint8_t *source, uint32_t bytes) { for (uint32_t i = 0; i < bytes; ++i) out[i] = source[i]; }
public:
    static constexpr uint32_t MaximumMetadata = 4 * 1024 * 1024, MaximumImage = 16 * 1024 * 1024;
    bool parse(const ZapArchive &archive) {
        valid = false; count = first = extent = metadata = 0; alignment = 4096; relocate = false;
        const auto mdt = archive.file(ZapArchive::MDT);
        if (!mdt.data || mdt.bytes < 52 || mdt.bytes > ZapArchive::Maximum) return false;
        const auto *p = mdt.data;
        if (word(p) != 0x464c457f || p[4] != 1 || p[5] != 1 || p[6] != 1 || word(p + 20) != 1 ||
            word(p + 28) != 52 || half(p + 40) != 52 || half(p + 42) != 32) return false;
        const unsigned phnum = half(p + 44);
        if (phnum < 3 || phnum > 100 || !range(mdt.bytes, 52, phnum * 32)) return false;
        const auto *ph = p + 52;
        const uint32_t headerBytes = word(ph + 16);
        if (word(ph) == 1 || word(ph + 4) || headerBytes < 52 + phnum * 32 || headerBytes > mdt.bytes ||
            headerBytes > MaximumMetadata) return false;
        bool split = false; unsigned hashIndex = 0;
        for (unsigned i = 0; i < phnum; ++i) {
            const auto *entry = ph + i * 32;
            if (word(entry + 16) && !range(mdt.bytes, word(entry + 4), word(entry + 16))) split = true;
            if ((word(entry + 24) & (7u << 24)) == (2u << 24)) {
                if (!i || hashIndex) return false;
                hashIndex = i;
            }
        }
        if (!hashIndex) return false;
        const auto *hashEntry = ph + hashIndex * 32;
        const uint32_t hashBytes = word(hashEntry + 16);
        if (!hashBytes || hashBytes > MaximumMetadata - headerBytes) return false;
        header = {p, headerBytes}; metadata = headerBytes + hashBytes;
        if (mdt.bytes == metadata) hash = {p + headerBytes, hashBytes};
        else if (range(mdt.bytes, word(hashEntry + 4), hashBytes)) hash = {p + word(hashEntry + 4), hashBytes};
        else { hash = archive.file(hashIndex); if (hash.bytes != hashBytes) return false; }
        uint64_t low = UINT32_MAX, high = 0;
        for (unsigned i = 0; i < phnum; ++i) {
            const auto *e = ph + i * 32;
            const uint32_t type = word(e), flags = word(e + 24), memory = word(e + 20);
            if (type != 1 || (flags & (7u << 24)) == (2u << 24) || !memory) continue;
            const uint32_t address = word(e + 12), size = word(e + 16), align = word(e + 28);
            const uint64_t end = uint64_t(address) + memory;
            if (size > memory || end > (1ULL << 32) || (align > 1 && (align & (align - 1))) || align > MaximumImage) return false;
            for (unsigned j = 0; j < count; ++j) if (address < uint64_t(segments[j].address) + segments[j].memory &&
                segments[j].address < end) return false;
            FirmwareBytes source{};
            if (size) {
                if (split) { source = archive.file(i); if (!source.data || source.bytes != size) return false; }
                else { if (!range(mdt.bytes, word(e + 4), size)) return false; source = {p + word(e + 4), size}; }
            }
            segments[count++] = {address, memory, size, i, source};
            if (address < low) low = address;
            if (end > high) high = end;
            if (align > alignment) alignment = align;
            relocate |= flags & (1u << 27);
        }
        high = (high + 4095) & ~4095ULL;
        // Qualcomm relocatable MDTs can link at a page-aligned offset (the
        // A540 ZAP uses 0x5000) with p_align=1 MiB. Allocation alignment
        // applies to the destination; segment offsets remain relative to low,
        // as in qcom_mdt_load(). PAS still authenticates the original metadata.
        if (!count || (low & 4095) || high <= low || high - low > MaximumImage ||
            (!relocate && !low)) return false;
        first = uint32_t(low); extent = uint32_t(high - low); valid = true; return true;
    }
    uint32_t metadataBytes() const { return valid ? metadata : 0; }
    uint32_t imageBytes() const { return valid ? extent : 0; }
    uint32_t physicalBase() const { return first; }
    uint32_t imageAlignment() const { return alignment; }
    bool relocatable() const { return relocate; }
    bool metadataInto(void *destination, uint32_t bytes) const {
        if (!valid || !destination || bytes < metadata) return false;
        auto *out = static_cast<uint8_t *>(destination);
        copy(out, header.data, header.bytes); copy(out + header.bytes, hash.data, hash.bytes); return true;
    }
    bool imageInto(void *destination, uint32_t bytes, uint64_t physical) const {
        if (!valid || !destination || bytes < extent || !physical || (physical & 4095) ||
            physical > (1ULL << 32) - extent ||
            (relocate ? physical & (alignment - 1) : physical != first)) return false;
        auto *out = static_cast<uint8_t *>(destination);
        for (uint32_t i = 0; i < extent; ++i) out[i] = 0; // BSS, holes and final padding
        for (unsigned i = 0; i < count; ++i) {
            const auto &s = segments[i];
            if (s.bytes) copy(out + (s.address - first), s.source.data, s.bytes);
        }
        return true;
    }
};
} // namespace Adreno
