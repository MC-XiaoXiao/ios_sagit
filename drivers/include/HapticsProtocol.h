// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

// Project diagnostic ABI, not Apple's private AOP / Core Haptics ABI.
namespace Haptics {
constexpr uint32_t Version = 1, DiagnosticClient = 0;
constexpr uint32_t InfoScalars = 8, StatusScalars = 12;
enum Selector : uint32_t { GetInfo, GetStatus, Play, Stop, SelectorCount };
enum class State : uint32_t { Uninitialized, Idle, Playing, Braking, Suspended, Fault };
enum class Result : uint32_t {
    Success, Invalid, NotReady, Busy, IOFailure, WrongChip,
    ShortCircuit, ResonanceError, Faulted
};
inline bool validCall(uint32_t selector, uint32_t inputs, uint32_t outputs,
                      const uint64_t *values) {
    switch (selector) {
    case GetInfo: return inputs == 0 && outputs == InfoScalars;
    case GetStatus: return inputs == 0 && outputs == StatusScalars;
    case Play: return inputs == 2 && outputs == 0 && values &&
                      values[0] >= 5 && values[0] <= 15000 &&
                      values[1] >= 1 && values[1] <= 1000;
    case Stop: return inputs == 0 && outputs == 0;
    default: return false;
    }
}
}
