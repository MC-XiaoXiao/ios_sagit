// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandRMTFS.h"

namespace Baseband { namespace RMTFS {
constexpr uint32_t Readable = 1, Writable = 2;
inline int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
inline bool uuid(const char *text, size_t bytes, uint8_t *out) {
    if (!text || !out || bytes != 36) return false;
    unsigned index = 0;
    for (unsigned i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i++] != '-') return false; continue; }
        const int a = hex(text[i++]), b = hex(text[i++]);
        if (a < 0 || b < 0) return false;
        out[index++] = uint8_t((a << 4) | b);
    }
    return index == 16;
}
struct MediaPolicy {
    uint8_t uuid[16]{}; // canonical UUID byte order, not GPT's mixed-endian disk encoding
    uint64_t start = 0, bytes = 0;
    uint32_t lun = 0, access = 0;
    bool valid() const {
        uint8_t nonzero = 0; for (auto b : uuid) nonzero |= b;
        return nonzero && start && bytes && !(start & (SectorBytes - 1)) && !(bytes & (SectorBytes - 1)) &&
            range(start, bytes, UINT64_MAX) && lun < 8 && (access == Readable || access == (Readable | Writable));
    }
};
// An explicit boot policy identifies all four original partitions by UUID,
// LUN and exact byte range. It is not a partition-label search or NV content.
// No default UUIDs/ranges are provided. The native backend independently
// compares every field with the matched IOMedia under the same QcomUFS bus.
struct StoragePolicy {
    static constexpr uint32_t Magic = 0x50534652;
    static constexpr size_t Bytes = 224, EntryBytes = 48;
    uint64_t epoch = 0;
    MediaPolicy media[PartitionCount]{};
    bool valid() const {
        if (!epoch) return false;
        for (unsigned i = 0; i < PartitionCount; ++i) {
            if (!media[i].valid()) return false;
            for (unsigned j = 0; j < i; ++j) {
                bool equal = true; for (unsigned n = 0; n < 16; ++n) equal &= media[i].uuid[n] == media[j].uuid[n];
                if (equal || (media[i].lun == media[j].lun && overlaps(media[i].start, media[i].bytes,
                    media[j].start, media[j].bytes))) return false;
            }
        }
        return true;
    }
    Result decode(const void *buffer, size_t length) {
        *this = {};
        if (!buffer || length != Bytes) return Result::Invalid;
        const auto *p = static_cast<const uint8_t *>(buffer);
        if (read32(p) != Magic || read16(p + 4) != 1 || read16(p + 6) != Bytes ||
            read32(p + 16) != PartitionCount || read32(p + 20) || read64(p + 24)) return Result::Invalid;
        epoch = read64(p + 8);
        for (unsigned i = 0; i < PartitionCount; ++i) {
            const auto *entry = p + 32 + i * EntryBytes;
            if (read64(entry + 40)) { *this = {}; return Result::Invalid; }
            for (unsigned j = 0; j < 16; ++j) media[i].uuid[j] = entry[j];
            media[i].start = read64(entry + 16); media[i].bytes = read64(entry + 24);
            media[i].lun = read32(entry + 32); media[i].access = read32(entry + 36);
        }
        if (!valid()) { *this = {}; return Result::Invalid; }
        return Result::OK;
    }
};

// The reset/resource owner calls acquire while MSS is held in reset, and
// retire only after AXI drain/reset. Both AP and MSS have R/W, without execute.
// Any uncertain completion pins the region, its mapping and the parent owner.
template<class Backend> class SharedRegion {
    Backend &io;
    bool mapped = false, shared = false, pinned = false;
    Result pin() { pinned = true; return Result::Quarantined; }
public:
    explicit SharedRegion(Backend &backend) : io(backend) {}
    bool ready() const { return mapped && shared && !pinned; }
    bool quarantined() const { return pinned; }
    bool canClose() const { return !mapped && !shared && !pinned; }
    void quarantine() { pinned = true; }
    Result acquire() {
        if (pinned) return Result::Quarantined;
        if (mapped || shared) return Result::Busy;
        auto r = io.confirm();
        if (r != Result::OK) return r == Result::Quarantined ? pin() : r;
        r = io.map();
        if (r == Result::Quarantined) return pin();
        if (r != Result::OK) return io.unmap() ? r : pin();
        mapped = true;
        r = io.assign(Ownership::Host, Ownership::Shared);
        if (r == Result::Quarantined) return pin();
        if (r != Result::OK) { if (!io.unmap()) return pin(); mapped = false; return r; }
        shared = true; return Result::OK;
    }
    Result retire(bool remoteStopped) {
        if (pinned) return Result::Quarantined;
        if (!mapped && !shared) return Result::OK;
        if (!remoteStopped) return Result::NotReady;
        if (shared && io.assign(Ownership::Shared, Ownership::Host) != Result::OK) return pin();
        shared = false;
        if (!io.unmap()) return pin();
        mapped = false; return Result::OK;
    }
};

// Backend performs one completed, aligned I/O and returns the actual byte
// count. The adapter handles 512-byte RMTFS requests on 512..16K physical
// blocks. A partial-block write requires an exclusive media open throughout
// read/modify/write; the native binder establishes it before ready().
template<class Backend> class StorageBlocks {
    Backend &io;
    uint8_t work[TransferBytes]{};
    Result chunk(unsigned part, bool write, uint64_t offset, size_t bytes) {
        size_t actual = 0;
        const auto r = io.transfer(part, write, offset, work, bytes, actual);
        if (actual > bytes) return Result::Quarantined;
        return r == Result::OK && actual != bytes ? Result::IO : r;
    }
    Result transfer(unsigned part, bool write, uint64_t offset, uint8_t *buffer, size_t bytes) {
        uint64_t capacity = 0; uint32_t block = 0; bool writable = false;
        auto r = io.geometry(part, capacity, block, writable);
        if (r != Result::OK) return r;
        if (block < SectorBytes || block > TransferBytes || (block & (block - 1)) ||
            !capacity || capacity % block) return Result::Invalid;
        if ((bytes && !buffer) || (offset & (SectorBytes - 1)) || (bytes & (SectorBytes - 1)) ||
            !range(offset, bytes, capacity)) return Result::Invalid;
        if (write && !writable) return Result::Unsupported;
        while (bytes) {
            const auto start = offset & ~uint64_t(block - 1);
            const size_t prefix = size_t(offset - start);
            const size_t n = prefix || bytes < block ? (bytes < block - prefix ? bytes : block - prefix) :
                (bytes < sizeof(work) ? bytes & ~size_t(block - 1) : sizeof(work));
            const size_t aligned = prefix || n < block ? block : n;
            if (!write || prefix || n < block) {
                r = chunk(part, false, start, aligned); if (r != Result::OK) return r;
            }
            if (write) {
                for (size_t i = 0; i < n; ++i) work[prefix + i] = buffer[i];
                r = chunk(part, true, start, aligned); if (r != Result::OK) return r;
            } else for (size_t i = 0; i < n; ++i) buffer[i] = work[prefix + i];
            offset += n; buffer += n; bytes -= n;
        }
        return Result::OK;
    }
public:
    explicit StorageBlocks(Backend &backend) : io(backend) {}
    Result read(unsigned part, uint64_t offset, void *buffer, size_t bytes) {
        return transfer(part, false, offset, static_cast<uint8_t *>(buffer), bytes);
    }
    Result write(unsigned part, uint64_t offset, const void *buffer, size_t bytes) {
        return transfer(part, true, offset, const_cast<uint8_t *>(static_cast<const uint8_t *>(buffer)), bytes);
    }
};
}} // namespace Baseband::RMTFS
