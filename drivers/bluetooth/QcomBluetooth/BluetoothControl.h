// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <BluetoothProtocol.h>

namespace Bluetooth {
constexpr const char* NativeControlName = "com.apple.uart.bluetooth";
constexpr uint32_t NativeGetTermios = 0x40487413, NativeSetTermios = 0x80487414;
// Private extension of our endpoint, used only by the entitled QCA backend.
// A successful proof seals the bootstrap session before close; an unsealed
// bootstrap close/crash must power down so late HCI replies cannot be reused.
constexpr uint32_t BootstrapHandoff = 0x42540001;
constexpr int NativeControlProtocol = 2;
constexpr uint32_t NativeControlGetInfo = 0xc0644e03;
struct NativeControlInfo { uint32_t id = 0; char name[96] {}; };
struct NativeControlAddress {
    uint8_t length = 32, family = 32;
    uint16_t system = 2;
    uint32_t id = 0, unit = 0, reserved[5] {};
};
static_assert(sizeof(NativeControlInfo) == 100 && sizeof(NativeControlAddress) == 32, "Darwin control socket ABI");
// arm64 Darwin termios, verified against the kernel SDK in BluetoothControl.cpp.
struct NativeTermios {
    uint64_t input = 0, output = 0, control = 0x38b00, local = 0;
    uint8_t characters[20] { };
    uint32_t padding = 0;
    uint64_t inputSpeed = OperationalBaud, outputSpeed = OperationalBaud;
};
static_assert(sizeof(NativeTermios) == 72, "iOS 18 UART socket termios ABI");
inline bool validNativeTermios(const NativeTermios& t)
{
    return (t.control & 0x300) == 0x300 && !(t.control & 0x1400) && // 8 data bits, no parity, one stop bit
        !(t.input & 0xe00) && !(t.output & 1) && !t.local && t.inputSpeed == t.outputSpeed && t.outputSpeed
        && t.outputSpeed <= 4000000 && !t.padding;
}
class ControlSession {
    uint32_t connection = 0;
    uint64_t generation = 0;
    bool bootstrap = false, handedOff = false;

public:
    uint32_t unit() const
    {
        return connection;
    }
    bool connect(uint32_t unit, uint64_t epoch, bool bootstrapClient = false)
    {
        if (connection || !unit || !epoch)
            return false;
        connection = unit;
        generation = epoch;
        bootstrap = bootstrapClient;
        handedOff = false;
        return true;
    }
    bool matches(uint32_t unit, uint64_t epoch) const
    {
        return connection && connection == unit && generation == epoch;
    }
    bool requiresReset() const { return connection && bootstrap && !handedOff; }
    bool sealed() const { return handedOff; }
    bool handoff(uint32_t unit, uint64_t epoch) {
        if (!matches(unit,epoch) || !bootstrap || handedOff) return false;
        handedOff = true; return true;
    }
    bool disconnect(uint32_t unit)
    {
        if (!connection || connection != unit)
            return false;
        connection = 0;
        generation = 0;
        bootstrap = handedOff = false;
        return true;
    }
};
}
