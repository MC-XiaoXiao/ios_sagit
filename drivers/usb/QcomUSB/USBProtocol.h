// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace USB {
enum class Role : uint8_t { None, Host, Device };
enum class Orientation : uint8_t { Unknown, Normal, Reverse };
enum class PowerRole : uint8_t { None, Source, Sink };
enum class Result : uint8_t {
    Success, Invalid, Unsupported, Busy, Timeout, HardwareError, NotReady, Pending,
    Aborted, Overflow, SetupPending, BusExpired, MissedService
};

// The platform, rather than the USB controller, owns connector wiring,
// regulators, PHY calibration, NoC votes and the DMA address space. This versioned
// contract describes resources that remain pinned throughout controller use.
struct PlatformContract {
    enum : uint32_t {
        Clocks = 1, Rails = 2, PHY = 4, Interconnect = 8,
        Exclusive = 16, DMABypass = 32, Coherent = 64 // hardware CPU-cache snooping
    };
    uint32_t version, flags, coreHz, referenceHz, addressBits;
    uint32_t maximumSpeed; // USB::Speed; routing may limit an SS-capable SoC.
    uint32_t sourceMicroamps=0, reserved=0;
    bool valid() const {
        return version == 2 && !reserved && (flags & 31) == 31 && !(flags & ~127u) &&
            coreHz >= 60000000 && coreHz <= 150000000 &&
            referenceHz == 19200000 && addressBits >= 32 && addressBits <= 40 &&
            (maximumSpeed == 3 || maximumSpeed == 4) &&
            (!sourceMicroamps || (sourceMicroamps>=500000 && sourceMicroamps<=1500000 && !(sourceMicroamps%250000)));
    }
};
static_assert(sizeof(PlatformContract)==32,"USB resource and connector budget contract");
enum class Speed : uint8_t { Unknown, Low, Full, High, Super };

inline bool dmaRange(uint64_t address, uint64_t length, unsigned bits,
                     unsigned alignment = 1) {
    if (!length || bits < 32 || bits > 64 || !alignment ||
        (alignment & (alignment - 1)) || (address & (alignment - 1)) ||
        length - 1 > UINT64_MAX - address) return false;
    return bits == 64 || address + length - 1 < (uint64_t(1) << bits);
}

struct SetupPacket {
    uint8_t requestType, request;
    uint16_t value, index, length;
    bool in() const { return (requestType & 0x80) != 0; }
    static SetupPacket decode(const uint8_t bytes[8]) {
        return {bytes[0], bytes[1], uint16_t(bytes[2] | uint16_t(bytes[3]) << 8),
            uint16_t(bytes[4] | uint16_t(bytes[5]) << 8),
            uint16_t(bytes[6] | uint16_t(bytes[7]) << 8)};
    }
};
static_assert(sizeof(SetupPacket) == 8, "USB setup packet layout");
}
