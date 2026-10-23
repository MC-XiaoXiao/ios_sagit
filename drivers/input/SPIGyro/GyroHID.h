// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "GyroSensor.h"

namespace GyroHID {
constexpr uint8_t InputID = 1, IntervalID = 2;
constexpr unsigned InputBytes = 13, FeatureBytes = 5;
constexpr uint32_t MinIntervalUS = 5000, MaxIntervalUS = 1000000, DefaultIntervalUS = 10000;
// Apple IOHIDEventDriver::handleGyroReport uses exponent-scaled values. Report
// microrad/s with exponent -6 so native IOFixed events carry rad/s.
constexpr uint8_t Descriptor[] = {
    0x05,0x20, 0x09,0x76, 0xa1,0x01, // Sensor / Gyrometer3D application
    0x85,InputID,
    0x0a,0x57,0x04, 0x0a,0x58,0x04, 0x0a,0x59,0x04,
    0x17,0xc0,0x63,0x8e,0xfd, // logical min -41000000 (beyond full raw ST range)
    0x27,0x40,0x9c,0x71,0x02, // logical max +41000000
    0x37,0xc0,0x63,0x8e,0xfd, // physical range equals logical, before exponent
    0x47,0x40,0x9c,0x71,0x02,
    0x55,0x0a,               // unit exponent -6
    0x66,0x12,0xf0,          // SI rotation: radian / second
    0x75,0x20, 0x95,0x03, 0x81,0x02,
    0x85,IntervalID,
    0x0a,0x0e,0x03,         // Sensor Property ReportInterval
    0x17,0x88,0x13,0x00,0x00, // minimum 5000 us
    0x27,0x40,0x42,0x0f,0x00, // maximum 1000000 us
    0x37,0x88,0x13,0x00,0x00,
    0x47,0x40,0x42,0x0f,0x00,
    0x66,0x01,0x10,         // SI linear seconds, exponent remains -6
    0x75,0x20, 0x95,0x01, 0xb1,0x02,
    0xc0
};
inline void put32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = uint8_t(value >> (8*i));
}
inline uint32_t get32(const uint8_t *in) {
    return uint32_t(in[0]) | (uint32_t(in[1]) << 8) |
           (uint32_t(in[2]) << 16) | (uint32_t(in[3]) << 24);
}
inline bool interval(const uint8_t *bytes, size_t length, uint32_t &out) {
    if (!bytes || length != FeatureBytes || bytes[0] != IntervalID) return false;
    const uint32_t value = get32(bytes + 1);
    if (value < MinIntervalUS || value > MaxIntervalUS) return false;
    out = value; return true;
}
inline void feature(uint32_t us, uint8_t *out) { out[0] = IntervalID; put32(out + 1, us); }
inline void report(const Gyro::Sample &sample, uint8_t *out) {
    out[0] = InputID;
    for (unsigned i = 0; i < 3; ++i) put32(out + 1 + i*4, uint32_t(sample.axis[i]));
}
}
