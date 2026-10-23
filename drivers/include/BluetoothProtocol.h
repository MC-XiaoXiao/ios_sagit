// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Bluetooth {
constexpr uint32_t ProtocolVersion = 1;
constexpr uint32_t DiagnosticClient = 0;
constexpr uint32_t BootstrapClient = 1;
constexpr const char* BootstrapEntitlement = "org.darwin.bluetooth.bootstrap";
constexpr uint64_t UARTBase = 0x0c171000; // MSM8998 BLSP1 UART3, separate from the console
constexpr uint64_t UARTSize = 0x1000;
constexpr uint32_t UARTInterrupt = 109; // GIC SPI, Apple vector is SPI + 32
constexpr uint32_t InitialBaud = 115200;
constexpr uint32_t OperationalBaud = 3200000;
constexpr size_t MaxACLPayload = 4096; // bounded host policy, not a controller capability
constexpr size_t MaxH4Frame = 1 + 4 + MaxACLPayload;

enum class Result : uint32_t {
    Success, Invalid, NotReady, Busy, Unsupported, Timeout, Fault, Pending, Ignored
};
enum class State : uint32_t { Detached, Off, Starting, WaitingForReset, Ready, Stopping, Fault };
enum Resource : uint32_t {
    Rails = 1U << 0, ReferenceClock = 1U << 1, Pins = 1U << 2,
    UART = 1U << 3, BootROM = 1U << 4, Firmware = 1U << 5
};
constexpr unsigned ResourceCount = 6;
constexpr uint32_t AllResources = Rails | ReferenceClock | Pins | UART | BootROM | Firmware;
enum Missing : uint32_t {
    PlatformOwnership = 1U << 0, UARTBackend = 1U << 1,
    FirmwareAssets = 1U << 2, BoardNVM = 1U << 3, NativeStackABI = 1U << 4
};
constexpr uint32_t InitialMissing = PlatformOwnership | UARTBackend | FirmwareAssets | BoardNVM | NativeStackABI;
enum Selector : uint32_t { GetInfo, GetStatus, SetPower, GetBootstrap, BootstrapSetPower, SelectorCount };
constexpr unsigned InfoScalars = 8;
constexpr unsigned StatusScalars = 8;
constexpr unsigned BootstrapScalars = 12;
// One gated snapshot: metadata is validated DT policy, not an HCI response.
enum BootstrapIndex : unsigned {
    BootstrapVersion, BootstrapGeneration, BootstrapState, BootstrapResources,
    BootstrapFault, BootstrapFlags, BootstrapProduct, BootstrapSoC, BootstrapROM,
    BootstrapPatch, BootstrapAddress, BootstrapResult
};
enum BootstrapFlag : uint32_t {
    BootstrapConfigured = 1, BootstrapEndpoints = 2, BootstrapPortOpen = 4,
    BootstrapPowerRequested = 8, BootstrapDrained = 16
};

inline bool validCall(uint32_t selector, unsigned inputs, unsigned outputs, uint64_t value = 0) {
    switch (selector) {
    case GetInfo: return inputs == 0 && outputs == InfoScalars;
    case GetStatus: return inputs == 0 && outputs == StatusScalars;
    case SetPower: return inputs == 1 && outputs == 0 && value <= 1;
    case GetBootstrap: return inputs == 0 && outputs == BootstrapScalars;
    case BootstrapSetPower: return inputs == 2 && outputs == 1 && value <= 1;
    default: return false;
    }
}
inline uint16_t read16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t read32(const uint8_t *p) { return uint32_t(read16(p)) | uint32_t(read16(p + 2)) << 16; }
inline void write16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void write32(uint8_t *p, uint32_t v) { write16(p,uint16_t(v)); write16(p + 2,uint16_t(v >> 16)); }
inline bool deadlineAfter(uint64_t now, uint64_t delay, uint64_t &deadline) {
    if (!delay || delay > UINT64_MAX - now) return false;
    deadline = now + delay; return true;
}
inline const char *stateName(State state) {
    switch (state) {
    case State::Detached: return "detached";
    case State::Off: return "off";
    case State::Starting: return "starting";
    case State::WaitingForReset: return "waiting-for-reset";
    case State::Ready: return "transport-ready";
    case State::Stopping: return "stopping";
    case State::Fault: return "fault";
    }
    return "invalid";
}
}
