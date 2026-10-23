// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

namespace QcomWatchdogHardware {
constexpr uint32_t Frequency = 32765, MaximumSeconds = 31;
// MSM8998 watchdog_v2: reset=0x04, enable=0x08, bark=0x10, bite=0x14.
// Thresholds are 20 bits. Bark follows bite to avoid an unhandled bark IRQ.
template<class IO> bool arm(IO &io, uint32_t seconds) {
    if (!seconds || seconds > MaximumSeconds) return false;
    io.write(0x08, 0);
    io.write(0x04, 1);
    io.write(0x10, 0xfffff);
    io.write(0x14, Frequency * seconds);
    io.write(0x08, 3);
    io.write(0x04, 1);
    return true;
}
template<class IO> void feed(IO &io) { io.write(0x04, 1); }
template<class IO> void disable(IO &io) { io.write(0x08, 0); }
}
