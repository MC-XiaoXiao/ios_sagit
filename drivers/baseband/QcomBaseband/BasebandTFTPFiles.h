// SPDX-License-Identifier: GPL-2.0-only
// Immutable, explicitly supplied firmware files and per-boot temporary bytes.
#pragma once
#include "BasebandFirmware.h"

namespace Baseband { namespace TFTP {
constexpr size_t PathBytes = 128, CatalogBytes = 8 * 1024 * 1024, CatalogEntries = 256;
constexpr size_t TemporaryFiles = 16, TemporaryBytes = 64 * 1024, FileHandles = 32;
constexpr uint32_t CatalogMagic = 0x46544251, CatalogEntryBytes = 144;
inline bool same(const char *a, const char *b) {
    for (size_t i = 0; i < PathBytes; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
    return false;
}
inline bool prefix(const char *path, const char *root) {
    while (*root) if (*path++ != *root++) return false;
    return true;
}
inline bool pathValid(const char *path, bool &readOnly) {
    readOnly = false;
    if (!path) return false;
    size_t length = 0;
    while (length < PathBytes && path[length]) ++length;
    if (!length || length == PathBytes) return false;
    const char *p;
    if (prefix(path, "/readonly/firmware/image/")) { readOnly = true; p = path + 25; }
    else if (prefix(path, "/readwrite/")) p = path + 11;
    else return false;
    // Canonical path segments only. No filesystem traversal or alias expansion.
    const char *segment = p;
    for (;;) {
        const char c = *p;
        if (!c || c == '/') {
            const auto n = p - segment;
            if (!n || (n == 1 && segment[0] == '.') || (n == 2 && segment[0] == '.' && segment[1] == '.')) return false;
            if (!c) return true;
            segment = ++p; continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) return false;
        ++p;
    }
}
class Files {
    uint8_t *catalog = nullptr;
    uint32_t count = 0, nextHandle = 0;
    uint64_t epoch = 0;
    bool active = false;
    struct Temporary {
        char path[PathBytes]{};
        uint8_t *data = nullptr;
        uint32_t node = 0, size = 0, readers = 0;
        bool used = false, writer = false;
    } temporary[TemporaryFiles];
    struct Handle {
        uint32_t token = 0, index = 0;
        bool used = false, write = false, immutable = false;
    } handles[FileHandles];
    const uint8_t *entry(unsigned index) const { return catalog + 16 + CatalogEntryBytes * index; }
    Handle *find(uint32_t token) {
        for (auto &h : handles) if (h.used && h.token == token) return &h;
        return nullptr;
    }
    void clearTemporary() {
        for (auto &h : handles) h = {};
        for (auto &f : temporary) { delete[] f.data; f = {}; }
    }
public:
    Files() = default;
    Files(const Files &) = delete;
    Files &operator=(const Files &) = delete;
    ~Files() { clearTemporary(); delete[] catalog; }
    // The caller supplies actual bytes. An absent catalog is an empty set,
    // never a fabricated file. Copy before validating to make it immutable.
    Result bind(const void *data, size_t bytes) {
        if (active || catalog || (bytes && !data) || bytes > CatalogBytes) return Result::Invalid;
        if (!bytes) return Result::OK;
        if (bytes < 16) return Result::Invalid;
        auto *copy = new uint8_t[bytes]; if (!copy) return Result::NoMemory;
        const auto *source = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < bytes; ++i) copy[i] = source[i];
        const uint32_t entries = read32(copy + 8);
        bool valid = read32(copy) == CatalogMagic && read16(copy + 4) == 1 &&
            read16(copy + 6) == CatalogEntryBytes && entries <= CatalogEntries && read32(copy + 12) == bytes;
        size_t cursor = 16 + size_t(entries) * CatalogEntryBytes;
        valid &= cursor <= bytes;
        for (uint32_t i = 0; valid && i < entries; ++i) {
            const auto *e = copy + 16 + i * CatalogEntryBytes;
            const auto *name = reinterpret_cast<const char *>(e);
            size_t n = 0; while (n < PathBytes && name[n]) ++n;
            bool readOnly = false;
            valid = n < PathBytes && pathValid(name, readOnly) && readOnly;
            for (size_t j = n; j < PathBytes; ++j) valid &= e[j] == 0;
            for (uint32_t j = 0; j < i; ++j)
                valid &= !same(name, reinterpret_cast<const char *>(copy + 16 + j * CatalogEntryBytes));
            const uint32_t offset = read32(e + 128), length = read32(e + 132);
            valid &= !read64(e + 136) && offset == cursor && range(offset, length, bytes);
            if (!valid) break;
            const size_t end = size_t(offset) + length;
            cursor = (end + 3) & ~size_t(3);
            if (cursor > bytes) { valid = false; break; }
            for (size_t j = end; j < cursor; ++j) valid &= copy[j] == 0;
        }
        valid &= cursor == bytes;
        if (!valid) { delete[] copy; return Result::Invalid; }
        catalog = copy; count = entries; return Result::OK;
    }
    Result start(uint64_t generation) {
        if (active || !generation || generation <= epoch) return Result::Invalid;
        clearTemporary(); epoch = generation; active = true; return Result::OK;
    }
    // Only the boot owner retires this store after the remote is stopped.
    void stopped() { active = false; clearTemporary(); }
    uint32_t immutableCount() const { return count; }
    Result open(uint32_t node, const char *path, bool write, bool truncate, uint32_t &token) {
        token = 0; bool readOnly = false;
        if (!active) return Result::Stale;
        if (!pathValid(path, readOnly)) return Result::Unsupported;
        if (write && readOnly) return Result::Unsupported;
        Handle *freeHandle = nullptr;
        for (auto &h : handles) if (!h.used) { freeHandle = &h; break; }
        if (!freeHandle || nextHandle == UINT32_MAX) return Result::NoMemory;
        uint32_t index = 0;
        if (readOnly) {
            for (; index < count; ++index) if (same(path, reinterpret_cast<const char *>(entry(index)))) break;
            if (index == count) return Result::NotReady;
        } else {
            Temporary *found = nullptr, *empty = nullptr;
            for (auto &f : temporary) {
                if (f.used && f.node == node && same(f.path, path)) found = &f;
                if (!f.used && !empty) empty = &f;
            }
            if (!found) {
                if (!write) return Result::NotReady;
                if (!empty) return Result::NoMemory;
                auto *buffer = new uint8_t[TemporaryBytes]; if (!buffer) return Result::NoMemory;
                for (size_t i = 0; i < TemporaryBytes; ++i) buffer[i] = 0;
                found = empty; found->data = buffer; found->node = node; found->used = true;
                for (size_t i = 0; path[i]; ++i) found->path[i] = path[i];
            }
            if (found->writer || (write && found->readers)) return Result::Busy;
            index = unsigned(found - temporary);
            if (write) {
                found->writer = true;
                if (truncate) { found->size = 0; }
            } else ++found->readers;
        }
        *freeHandle = {++nextHandle, index, true, write, readOnly}; token = freeHandle->token;
        return Result::OK;
    }
    Result size(uint32_t token, uint64_t &bytes) {
        bytes = 0; auto *h = active ? find(token) : nullptr;
        if (!h) return Result::Stale;
        bytes = h->immutable ? read32(entry(h->index) + 132) : temporary[h->index].size;
        return Result::OK;
    }
    Result read(uint32_t token, uint64_t offset, void *data, size_t bytes) {
        auto *h = active ? find(token) : nullptr;
        if (!h || h->write) return Result::Stale;
        uint64_t length = 0; const auto r = size(token, length);
        if (r != Result::OK || !range(offset, bytes, length) || (bytes && !data)) return Result::Invalid;
        const auto *p = h->immutable ? catalog + read32(entry(h->index) + 128) : temporary[h->index].data;
        auto *out = static_cast<uint8_t *>(data);
        for (size_t i = 0; i < bytes; ++i) out[i] = p[offset + i];
        return Result::OK;
    }
    Result write(uint32_t token, uint64_t offset, const void *data, size_t bytes) {
        auto *h = active ? find(token) : nullptr;
        if (!h || !h->write || h->immutable) return Result::Stale;
        if (!range(offset, bytes, TemporaryBytes) || (bytes && !data)) return Result::NoMemory;
        auto &file = temporary[h->index];
        for (uint64_t i = file.size; i < offset; ++i) file.data[i] = 0;
        const auto *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < bytes; ++i) file.data[offset + i] = p[i];
        if (offset + bytes > file.size) file.size = uint32_t(offset + bytes);
        return Result::OK;
    }
    Result close(uint32_t token) {
        auto *h = find(token); if (!h) return Result::Stale;
        if (!h->immutable) {
            auto &f = temporary[h->index];
            if (h->write) f.writer = false; else --f.readers;
        }
        *h = {}; return Result::OK;
    }
};
}} // namespace Baseband::TFTP
