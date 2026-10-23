// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <BluetoothProtocol.h>

namespace Bluetooth {
// Apple's published serial state and event values. The native family owns the
// BSD tty, line discipline, termios and open permissions.
constexpr uint32_t Acquired = 0x80000000, Active = 0x40000000, TXEnable = 0x20000000, TXBusy = 0x10000000;
constexpr uint32_t TXEmpty = 0x04000000, TXLow = 0x02000000, TXHigh = 0x01000000, TXFull = 0x00800000;
constexpr uint32_t RXEnable = TXEnable >> 7, RXEmpty = TXEmpty >> 7, RXLow = TXLow >> 7, RXHigh = TXHigh >> 7,
                   RXFull = TXFull >> 7;
constexpr uint32_t ModemReady = 0x166; // DTR/RTS/CTS/CAR/DSR on a virtual HCI link
constexpr uint32_t SerialCapacity = 32768;
enum SerialEvent : uint32_t {
    PortActive = 5,
    TXSize = 11,
    RXSize = 15,
    TXLowWater = 19,
    RXLowWater = 23,
    TXHighWater = 27,
    RXHighWater = 31,
    TXAvailable = 35,
    RXAvailable = 39,
    TXFlush = 40,
    RXFlush = 47,
    DataRate = 51,
    RXDataRate = 55,
    DataSize = 59,
    RXDataSize = 63,
    DataIntegrity = 67,
    RXDataIntegrity = 71,
    Delay = 75,
    DataLatency = 79,
    FlowControl = 83,
    MinLatency = 229,
    XoffByte = 233,
    XonByte = 237,
    StopBits = 243,
    RXStopBits = 247,
    LineBreak = 249
};

struct SerialSettings {
    uint32_t logicalBaud = OperationalBaud, flow = 0;
    uint32_t rxLow = 1, txLow = 1, rxHigh = SerialCapacity - 1, txHigh = SerialCapacity - 1;
    uint32_t latency = 0, xon = 0x11, xoff = 0x13, minimumLatency = 0;

    Result event(uint32_t type, bool query, uint32_t& value)
    {
        // This stream is downstream of firmware loading. Termios rates are
        // metadata; physical rate and CTS/RTS are owned by the platform backend.
        switch (type) {
        case TXSize:
        case RXSize:
            if (!query && value != SerialCapacity)
                return Result::Unsupported;
            value = SerialCapacity;
            break;
        case TXLowWater:
        case RXLowWater: {
            auto& low = type == TXLowWater ? txLow : rxLow;
            const auto high = type == TXLowWater ? txHigh : rxHigh;
            if (query)
                value = low;
            else if (value < high)
                low = value;
            else
                return Result::Invalid;
            break;
        }
        case TXHighWater:
        case RXHighWater: {
            auto& high = type == TXHighWater ? txHigh : rxHigh;
            const auto low = type == TXHighWater ? txLow : rxLow;
            if (query)
                value = high;
            else if (value > low && value <= SerialCapacity)
                high = value;
            else
                return Result::Invalid;
            break;
        }
        case DataRate:
        case RXDataRate:
            if (query)
                value = logicalBaud * 2;
            else if (value && !(value & 1) && value <= 8000000)
                logicalBaud = value / 2;
            else
                return Result::Invalid;
            break;
        case DataSize:
        case RXDataSize:
            if (!query && value != 16)
                return Result::Unsupported;
            value = 16;
            break;
        case DataIntegrity:
        case RXDataIntegrity:
            if (!query && value != 1 && !(type == RXDataIntegrity && value == 0))
                return Result::Unsupported;
            value = 1;
            break;
        case StopBits:
        case RXStopBits:
            if (!query && value != 2)
                return Result::Unsupported;
            value = 2;
            break;
        case FlowControl:
            if (query)
                value = flow;
            // Software flow control would eat arbitrary HCI payload bytes.
            else if (value & ~0x24U)
                return Result::Unsupported;
            else
                flow = value;
            break;
        case Delay:
            if (!query && value)
                return Result::Unsupported;
            value = 0;
            break;
        case DataLatency:
            if (query)
                value = latency;
            else
                latency = value;
            break;
        case MinLatency:
            if (query)
                value = minimumLatency;
            else if (value <= 1)
                minimumLatency = value;
            else
                return Result::Invalid;
            break;
        case XonByte:
        case XoffByte: {
            auto& byte = type == XonByte ? xon : xoff;
            if (query)
                value = byte;
            else if (value <= 255)
                byte = value;
            else
                return Result::Invalid;
            break;
        }
        case LineBreak:
            if (!query && value)
                return Result::Unsupported;
            value = 0;
            break;
        default:
            return Result::Unsupported;
        }
        return Result::Success;
    }
};
}
