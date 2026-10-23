/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _QCOM_MIPI_DSI_PROTOCOL_H
#define _QCOM_MIPI_DSI_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

namespace MIPIDSI {
enum : uint32_t {
    Control = 0x004, Status = 0x008, DMAControl = 0x03c,
    DMABase = 0x048, DMALength = 0x04c, Readback = 0x06c,
    TriggerControl = 0x084, TriggerDMA = 0x090,
    LaneControl = 0x0ac, InterruptControl = 0x110, Reset = 0x118,
    ClockControl = 0x11c, HardwareVersion = 0x000, ReadbackControl = 0x1d4,
    LegacyVersion = 0x1f0, ShiftedVersion = 0x1f4,
    Enable = 1, VideoEnable = 2, CommandEnable = 4,
    DMAActive = 2, DMAComplete = 1, DMAMask = 2,
    BTAComplete = 1u << 20, BTAMask = 1u << 21,
    Error = 1u << 24, ErrorMask = 1u << 25,
    LowPower = 1u << 26, FromMemory = 1u << 28,
    MaxPayload = 4092, CommandBufferSize = 4096
};
inline bool controller6G(uint32_t legacyVersion, uint32_t version, uint32_t hardware) {
    return !legacyVersion && (version >> 24) == 3 && (hardware >> 28) == 2;
}

// MSM8998 uses the 6G register layout: legacy XML offsets plus 4 bytes.
// DMA consumes a Qualcomm packet descriptor, not the MIPI on-wire ECC/CRC.
inline size_t packet(uint8_t *out, size_t capacity, uint8_t type,
    const uint8_t *data, size_t size, bool read = false)
{
    if (!out || (size && !data) || type > 0x3f) return 0;
    bool isLong = type == 0x29 || type == 0x39;
    unsigned expected = 0;
    switch (type) {
        case 0x03: case 0x04: expected = 0; break;
        case 0x05: case 0x06: case 0x13: case 0x14: expected = 1; break;
        case 0x15: case 0x23: case 0x24: case 0x37: expected = 2; break;
        case 0x29: case 0x39: break;
        default: return 0;
    }
    if ((!isLong && size != expected) || (isLong && (!size || size > MaxPayload))) return 0;
    const size_t bytes = isLong ? (size + 7) & ~size_t(3) : 4;
    if (bytes > capacity) return 0;
    out[0] = isLong ? uint8_t(size) : size ? data[0] : 0;
    out[1] = isLong ? uint8_t(size >> 8) : size > 1 ? data[1] : 0;
    out[2] = type;
    out[3] = 0x80 | (isLong ? 0x40 : 0) | (read ? 0x20 : 0);
    if (isLong) {
        for (size_t i = 0; i < size; ++i) out[i + 4] = data[i];
        for (size_t i = size + 4; i < bytes; ++i) out[i] = 0xff;
    }
    return bytes;
}

struct PlatformContract {
    uint32_t version, flags, lanes, byteClockHz, escapeClockHz;
    enum : uint32_t { ClocksPinned = 1, PowerPinned = 2, PHYReady = 4,
        ResetReleased = 8, FirmwareQuiesced = 16, DMABypass = 32 };
    bool valid() const {
        return version == 1 && (flags & 31) == 31 && !(flags & ~63u) &&
            lanes >= 1 && lanes <= 4 && byteClockHz >= 1000000 &&
            byteClockHz <= 250000000 && escapeClockHz >= 1000000 &&
            escapeClockHz <= 20000000;
    }
};

enum class Result { Success, Busy, Timeout, HardwareError, BadArgument };
inline Result response(const uint8_t raw[16], unsigned capacity, uint8_t *out, unsigned &actual) {
    actual = 0;
    if (!out || !capacity || capacity > 10) return Result::BadArgument;
    // Long responses shorter than ten bytes are right aligned in the 16-byte
    // readback shift register. Short packets occupy the final register.
    const uint8_t *p = raw + (capacity > 2 ? 10 - capacity : 12);
    if (p[0] == 0x02) return Result::HardwareError;
    unsigned count = 0, offset = 1;
    if (p[0] == 0x11 || p[0] == 0x21) count = 1;
    else if (p[0] == 0x12 || p[0] == 0x22) count = 2;
    else if (p[0] == 0x1a || p[0] == 0x1c) { count = p[1] | unsigned(p[2]) << 8; offset = 4; }
    else if (capacity > 2) {
        // A panel can answer a large requested maximum with a short response.
        p = raw + 12;
        if (p[0] == 0x11 || p[0] == 0x21) count = 1;
        else if (p[0] == 0x12 || p[0] == 0x22) count = 2;
    }
    if (!count || count > capacity || p + offset + count > raw + 16) return Result::HardwareError;
    for (unsigned i = 0; i < count; ++i) out[i] = p[offset + i];
    actual = count;
    return Result::Success;
}
template <class Registers> class Host {
    Registers &r;
public:
    explicit Host(Registers &registers) : r(registers) {}
    Result transfer(uint64_t address, unsigned bytes, bool read, bool lowPower) {
        if (!bytes || bytes > CommandBufferSize || (bytes & 3) ||
            (address & 3) || address > UINT32_MAX || bytes - 1 > UINT32_MAX - address)
            return Result::BadArgument;
        if (r.read(Status) & DMAActive) return Result::Busy;
        uint32_t control = r.read(Control), dma = r.read(DMAControl);
        uint32_t trigger = r.read(TriggerControl), irq = r.read(InterruptControl);
        const uint32_t masks = irq & (DMAMask | BTAMask | ErrorMask | (1u << 9) | (1u << 17));
        const uint32_t owned = DMAComplete | BTAComplete | Error;
        // Commands are polled. Mask their IRQs while preserving video/MDP
        // masks on the shared MDSS line, then restore the caller's masks.
        r.write(InterruptControl, (masks & ~(DMAMask | BTAMask | ErrorMask)) | owned);
        r.write(Control, control | Enable | CommandEnable | (1u << 20) | (1u << 24));
        r.write(DMAControl, FromMemory | (lowPower ? uint32_t(LowPower) : 0u));
        r.write(TriggerControl, (trigger & ~7u) | 4u);
        r.write(DMABase, uint32_t(address));
        r.write(DMALength, bytes);
        r.write(TriggerDMA, 1);
        Result result = Result::Timeout;
        for (unsigned i = 0; i < 10000; ++i) {
            uint32_t state = r.read(InterruptControl);
            if (state & Error) { result = Result::HardwareError; break; }
            if ((state & (DMAComplete | (read ? uint32_t(BTAComplete) : 0u))) ==
                    (DMAComplete | (read ? uint32_t(BTAComplete) : 0u))) {
                result = Result::Success; break;
            }
            r.delay(10);
        }
        // A timed out DMA can still fetch this buffer. Keep ownership and all
        // mappings until the controller is demonstrably idle; don't recycle it.
        if (r.read(Status) & DMAActive) return Result::Timeout;
        r.write(InterruptControl, masks | owned);
        r.write(TriggerControl, trigger);
        r.write(DMAControl, dma);
        r.write(Control, control);
        return result;
    }
};
}
#endif
