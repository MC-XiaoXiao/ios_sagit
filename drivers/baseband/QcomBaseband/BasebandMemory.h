// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandBoot.h"
#include "BasebandResources.h"

namespace Baseband {
// Opaque little-endian EFI -> XNU record. Addresses are never taken from a
// user-client request. The flags attest only final-map reservation/copy, not
// PBL/MBA authentication or an initial secure VMID assignment.
struct MemoryHandoff {
    static constexpr uint32_t Magic = 0x4f484242, Version = 1, Bytes = 224;
    static constexpr uint32_t CatalogVersion = 2, CatalogRecordBytes = 272, MaxBytes = CatalogRecordBytes;
    static constexpr uint32_t Verified = 7;
    uint16_t version = Version;
    uint32_t flags = 0;
    uint64_t epoch = 1;
    BootLayout layout{};
    Region image{}, arena{};
    uint64_t mbaOffset = 0, mbaBytes = 0, modemOffset = 0, modemBytes = 0;
    ResourcePolicy policy{512, 512, 4};
    uint8_t mbaHash[32]{}, modemHash[32]{};
    uint64_t filesOffset = 0, filesBytes = 0;
    uint8_t filesHash[32]{};
    uint32_t wireBytes() const { return version == CatalogVersion ? CatalogRecordBytes : Bytes; }
    uint64_t bootImageBytes() const { return (modemOffset + modemBytes + 0x3fff) & ~UINT64_C(0x3fff); }
    uint64_t imageBytes() const {
        return version == CatalogVersion ? (filesOffset + filesBytes + 0x3fff) & ~UINT64_C(0x3fff) : bootImageBytes();
    }
    bool valid(bool complete = true) const {
        if (!epoch || !policy.valid() || !mbaBytes || mbaBytes > 0x200000 || mbaOffset ||
            !modemBytes || modemBytes > 0x4000000 || modemOffset != ((mbaBytes + 15) & ~UINT64_C(15))) return false;
        bool a = false, b = false;
        for (unsigned i = 0; i < 32; ++i) { a |= mbaHash[i] != 0; b |= modemHash[i] != 0; }
        if (!a || !b || layout.modem.address != 0x8d000000 || layout.modem.bytes != 0x7000000 ||
            layout.mba.address != 0x94500000 || layout.mba.bytes != 0x200000) return false;
        bool fileHash = false;
        for (unsigned i = 0; i < 32; ++i) fileHash |= filesHash[i] != 0;
        if (version == Version) {
            if (filesOffset || filesBytes || fileHash) return false;
        } else if (version == CatalogVersion) {
            if (filesBytes < 16 || filesBytes > 8 * 1024 * 1024 || filesOffset != bootImageBytes() || !fileHash) return false;
        } else return false;
        if (!complete) return !flags && !layout.metadata.address && !layout.metadata.bytes &&
            !image.address && !image.bytes && !arena.address && !arena.bytes;
        if (flags != Verified || !layout.valid() || layout.metadata.bytes != 0x4000 ||
            image.bytes != imageBytes() || !arena.valid() || (arena.address & 0x1fffff) ||
            arena.bytes < 0x4000000 || arena.bytes > 0x40000000 || (arena.bytes & 0x1fffff)) return false;
        const Region regions[] = {layout.modem, layout.mba, layout.metadata, image, arena};
        for (unsigned i = 0; i < 5; ++i) {
            const auto &r = regions[i];
            if (!r.valid() || r.address < 0x80000000 || (r.address & 0x3fff) || (r.bytes & 0x3fff) ||
                (i != 4 && !range(r.address, r.bytes, UINT64_C(1) << 32)) ||
                overlaps(r.address, r.bytes, 0x86000000, 0x200000)) return false;
            for (unsigned j = 0; j < i; ++j)
                if (overlaps(r.address, r.bytes, regions[j].address, regions[j].bytes)) return false;
        }
        return true;
    }
    Result decode(const void *buffer, size_t bytes, bool complete = true) {
        *this = {};
        if (!buffer || (bytes != Bytes && bytes != CatalogRecordBytes)) return Result::Invalid;
        const auto *p = static_cast<const uint8_t *>(buffer);
        version = read16(p + 4);
        if (read32(p) != Magic || (version != Version && version != CatalogVersion) ||
            bytes != wireBytes() || read16(p + 6) != bytes ||
            read32(p + 12) || read32(p + 148) || read64(p + 216)) return Result::Invalid;
        flags = read32(p + 8); epoch = read64(p + 16);
        Region *regions[] = {&layout.modem, &layout.mba, &layout.metadata, &image, &arena};
        for (unsigned i = 0; i < 5; ++i) *regions[i] = {read64(p + 24 + i * 16), read64(p + 32 + i * 16)};
        mbaOffset = read64(p + 104); mbaBytes = read64(p + 112);
        modemOffset = read64(p + 120); modemBytes = read64(p + 128);
        policy = {read32(p + 136), read32(p + 140), read32(p + 144)};
        for (unsigned i = 0; i < 32; ++i) { mbaHash[i] = p[152 + i]; modemHash[i] = p[184 + i]; }
        if (version == CatalogVersion) {
            filesOffset = read64(p + 224); filesBytes = read64(p + 232);
            for (unsigned i = 0; i < 32; ++i) filesHash[i] = p[240 + i];
        }
        return valid(complete) ? Result::OK : Result::Invalid;
    }
    Result encode(void *buffer, size_t bytes, bool complete = true) const {
        if (!buffer || bytes != wireBytes() || !valid(complete)) return Result::Invalid;
        auto *p = static_cast<uint8_t *>(buffer);
        for (unsigned i = 0; i < bytes; ++i) p[i] = 0;
        write32(p, Magic); write16(p + 4, version); write16(p + 6, uint16_t(bytes));
        write32(p + 8, flags); write64(p + 16, epoch);
        const Region regions[] = {layout.modem, layout.mba, layout.metadata, image, arena};
        for (unsigned i = 0; i < 5; ++i) { write64(p + 24 + i * 16, regions[i].address); write64(p + 32 + i * 16, regions[i].bytes); }
        write64(p + 104, mbaOffset); write64(p + 112, mbaBytes);
        write64(p + 120, modemOffset); write64(p + 128, modemBytes);
        write32(p + 136, policy.cx); write32(p + 140, policy.mx); write32(p + 144, policy.qdss);
        for (unsigned i = 0; i < 32; ++i) { p[152 + i] = mbaHash[i]; p[184 + i] = modemHash[i]; }
        if (version == CatalogVersion) {
            write64(p + 224, filesOffset); write64(p + 232, filesBytes);
            for (unsigned i = 0; i < 32; ++i) p[240 + i] = filesHash[i];
        }
        return Result::OK;
    }
};
} // namespace Baseband
