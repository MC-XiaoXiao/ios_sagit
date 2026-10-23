// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace WLAN {
constexpr uint32_t ProtocolVersion = 1;
constexpr uint32_t DiagnosticClient = 0;
constexpr uint64_t RegisterBase = 0x18800000;
constexpr uint64_t RegisterSize = 0x800000;
constexpr uint32_t CopyEngineCount = 12;
constexpr unsigned DMAAddressBits = 35;
constexpr uint64_t MinimumMSABytes = 0x100000;

enum class Result : uint32_t { Success, Invalid, NotReady, Busy, Unsupported, Timeout, Fault };
enum class State : uint32_t { Detached, WaitingForResources, Starting, TransportReady, Stopping, Fault };
enum Resource : uint32_t {
    Power = 1U << 0, Clock = 1U << 1, Interconnect = 1U << 2,
    DMA = 1U << 3, Firmware = 1U << 4, CopyEngines = 1U << 5
};
constexpr uint32_t AllResources = Power | Clock | Interconnect | DMA | Firmware | CopyEngines;
enum Missing : uint32_t {
    PlatformBackend = 1U << 0, FirmwareService = 1U << 1, BoardData = 1U << 2,
    DeviceMapper = 1U << 3, NativeInterfaceABI = 1U << 4
};
constexpr uint32_t InitialMissing = PlatformBackend | FirmwareService | BoardData | DeviceMapper | NativeInterfaceABI;
enum Selector : uint32_t { GetInfo = 0, GetStatus = 1, SetPower = 2, SelectorCount = 3 };
constexpr unsigned InfoScalars = 6;
constexpr unsigned StatusScalars = 6;

inline bool validCall(uint32_t selector, unsigned inputs, unsigned outputs, uint64_t value = 0) {
    switch (selector) {
    case GetInfo: return inputs == 0 && outputs == InfoScalars;
    case GetStatus: return inputs == 0 && outputs == StatusScalars;
    case SetPower: return inputs == 1 && outputs == 0 && value <= 1;
    default: return false;
    }
}
inline bool validDMA(uint64_t address, uint64_t bytes, unsigned alignment = 1) {
    constexpr uint64_t limit = uint64_t(1) << DMAAddressBits;
    return bytes && address && alignment && !(alignment & (alignment - 1)) &&
        !(address & (alignment - 1)) && address < limit && bytes <= limit - address;
}
inline uint16_t read16(const void *bytes) {
    const auto *p = static_cast<const uint8_t *>(bytes);
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t read32(const void *bytes) {
    const auto *p = static_cast<const uint8_t *>(bytes);
    return uint32_t(read16(p)) | uint32_t(read16(p + 2)) << 16;
}
inline void write16(void *bytes, uint16_t value) {
    auto *p = static_cast<uint8_t *>(bytes); p[0] = uint8_t(value); p[1] = uint8_t(value >> 8);
}
inline void write32(void *bytes, uint32_t value) {
    auto *p = static_cast<uint8_t *>(bytes); write16(p, uint16_t(value)); write16(p + 2, uint16_t(value >> 16));
}
inline const char *stateName(State state) {
    switch (state) {
    case State::Detached: return "detached";
    case State::WaitingForResources: return "waiting-for-resources";
    case State::Starting: return "starting";
    case State::TransportReady: return "transport-ready";
    case State::Stopping: return "stopping";
    case State::Fault: return "fault";
    }
    return "invalid";
}
}
