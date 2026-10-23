// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace SPI {
constexpr unsigned MaxBytes = 64;
enum class Result { Success, Invalid, Unsupported, NotReady, Timeout, IO };
struct Configuration {
    uint32_t chipSelect = 0, frequency = 0, mode = 0;
};
inline bool valid(const Configuration &c) {
    return c.chipSelect < 4 && c.frequency >= 100000 &&
           c.frequency <= 1000000 && (c.mode == 0 || c.mode == 3);
}
// One full-duplex transaction, MSB first, 8 bits per word. CS remains asserted
// across the complete command + payload. Null tx clocks zeroes; null rx discards.
// Input is committed only on success. The device nub supplies the configuration.
struct Transaction {
    Configuration config;
    const uint8_t *tx;
    uint8_t *rx;
    size_t length;
    uint32_t timeoutUS;
};
inline bool valid(const Transaction &t) {
    return valid(t.config) && (t.tx || t.rx) && t.length &&
           t.length <= MaxBytes && t.timeoutUS >= 100 && t.timeoutUS <= 20000;
}
}
