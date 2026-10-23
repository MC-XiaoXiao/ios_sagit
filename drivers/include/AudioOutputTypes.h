// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace AudioOutput {
constexpr uint32_t StreamID = 1;
constexpr uint32_t PCM = 0x6c70636d; // 'lpcm'
constexpr uint32_t PCMFlags = 0x4c; // signed integer, packed, native-endian
constexpr uint64_t Rate = uint64_t(48000) << 32; // IOAudio2 uses 32.32, not double
struct Format {
    uint64_t rate;
    uint32_t format, flags, bytesPerPacket, framesPerPacket;
    uint32_t bytesPerFrame, channels, bits, reserved;
};
constexpr Format OutputFormat{Rate, PCM, PCMFlags, 4, 1, 4, 2, 16, 0};
inline bool validFormat(const Format &f) {
    return f.rate == Rate && f.format == PCM &&
        (f.flags == PCMFlags || f.flags == (PCMFlags & ~0x40u)) &&
        f.bytesPerPacket == 4 && f.framesPerPacket == 1 &&
        f.bytesPerFrame == 4 && f.channels == 2 && f.bits == 16 && !f.reserved;
}
struct Status {
    uint64_t sampleTime, hostTime, seed, clientInputPosition, clientOutputPosition;
};
struct ConfigChange {
    uint32_t objectID, selector, action, streamID;
    uint64_t value, context;
};
struct Configuration {
    uint32_t version, rate, channels, bits, ringFrames, periodFrames;
    uint32_t latencyFrames, safetyFrames;
};
inline bool validConfiguration(const Configuration &c) {
    return c.version == 1 && c.rate == 48000 && c.channels == 2 && c.bits == 16 &&
        c.ringFrames >= 1024 && c.ringFrames <= 65536 &&
        (c.ringFrames & (c.ringFrames - 1)) == 0 &&
        c.periodFrames >= 64 && c.periodFrames <= c.ringFrames / 4 &&
        c.ringFrames % c.periodFrames == 0 && c.safetyFrames >= c.periodFrames &&
        c.safetyFrames <= c.ringFrames / 2 && c.latencyFrames <= 48000;
}
struct Position {
    // Absolute completed frames since startOutput, timestamped by the backend
    // in mach_absolute_time units. A timer invocation is NOT a completion.
    uint64_t generation, frames, hostTime;
};
static_assert(sizeof(Format) == 40, "IOAudio2 stream format");
static_assert(sizeof(Status) == 40, "IOAudio2 shared status");
static_assert(sizeof(ConfigChange) == 32, "IOAudio2 config notification");
static_assert(offsetof(ConfigChange, action) == 8, "native config action");
}
