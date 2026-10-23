// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Baseband {
constexpr uint32_t ProtocolVersion = 1;
enum class Result : uint32_t {
    OK, Invalid, Unsupported, NotReady, Busy, Timeout, Authentication,
    NoMemory, IO, Stale, Quarantined
};
enum class State : uint32_t {
    Offline, Prepared, MBAStarting, MBAReady, Metadata, Loading, Authenticating,
    WaitingReady, Running, Stopping, Faulted
};
enum class Ownership : uint32_t { Host, Modem, Shared, Unknown };
enum Requirement : uint32_t {
    Firmware = 1, ReservedMemory = 2, SecureOwnership = 4, Clocks = 8,
    Reset = 16, SMP2PTransport = 32, GLINKTransport = 64, AppleTelephony = 128, DataPlane = 256,
    RemoteFilesystem = 512
};
constexpr uint32_t InitialRequirements = 1023;
enum Selector : uint32_t { GetInfo, GetStatus, SelectorCount };
constexpr uint32_t DiagnosticClient = 0x42424431;
constexpr size_t StatusScalars = 8;
inline uint16_t read16(const void *value) {
    const auto *p = static_cast<const uint8_t *>(value);
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t read32(const void *value) {
    const auto *p = static_cast<const uint8_t *>(value);
    return uint32_t(read16(p)) | uint32_t(read16(p + 2)) << 16;
}
inline uint64_t read64(const void *value) {
    const auto *p = static_cast<const uint8_t *>(value);
    return uint64_t(read32(p)) | uint64_t(read32(p + 4)) << 32;
}
inline void write16(void *value, uint16_t v) {
    auto *p = static_cast<uint8_t *>(value); p[0] = v; p[1] = v >> 8;
}
inline void write32(void *value, uint32_t v) {
    auto *p = static_cast<uint8_t *>(value); write16(p, v); write16(p + 2, v >> 16);
}
inline void write64(void *value, uint64_t v) {
    auto *p = static_cast<uint8_t *>(value); write32(p, uint32_t(v)); write32(p + 4, uint32_t(v >> 32));
}
inline bool range(uint64_t offset, uint64_t bytes, uint64_t limit) {
    return offset <= limit && bytes <= limit - offset;
}
inline bool overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return n && m && (a <= b ? b - a < n : a - b < m);
}
inline bool validCall(uint32_t selector, uint32_t inputs, uint32_t outputs) {
    return selector < SelectorCount && !inputs && outputs == StatusScalars;
}
struct Region {
    uint64_t address = 0, bytes = 0;
    bool valid() const {
        return address && bytes && !(address & 4095) && !(bytes & 4095)
            && range(address, bytes, uint64_t(1) << 32);
    }
};
} // namespace Baseband
