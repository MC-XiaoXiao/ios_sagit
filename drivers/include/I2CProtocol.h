// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

// Kernel-only, synchronous API. A transaction owns the bus through every
// repeated START and the final STOP. Buffers remain owned by the caller.
namespace I2C {
constexpr unsigned MaxMessages = 8, MaxMessageBytes = 256, MaxBytes = 512;
enum class Direction : uint8_t { Write, Read };
enum class Result { Success, Invalid, Unsupported, NotReady, Nack, Timeout, BusError };
struct Message {
    Direction direction;
    uint8_t *bytes;
    size_t length;
};
struct Transaction {
    uint16_t address;
    Message *messages;
    unsigned count;
    uint32_t timeoutUS = 20000;
};
inline bool valid(const Transaction &t) {
    if (t.address < 8 || t.address > 0x77 || !t.messages || !t.count ||
        t.count > MaxMessages || t.timeoutUS < 1000 || t.timeoutUS > 100000) return false;
    size_t total = 0;
    for (unsigned i = 0; i < t.count; ++i) {
        const auto &m = t.messages[i];
        if ((m.direction != Direction::Write && m.direction != Direction::Read) ||
            !m.bytes || !m.length || m.length > MaxMessageBytes) return false;
        total += m.length;
    }
    return total <= MaxBytes;
}
}
