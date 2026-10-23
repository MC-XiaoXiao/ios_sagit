// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <ClockControllerProtocol.h>
namespace PowerUser {
enum Selector : uint32_t { GetInfo, ReadResource, SetLimit, SetCPUFrequency, SetAutomatic, SelectorCount };
inline bool readable(uint32_t op) {
    using namespace ClockController;
    return op == GetRate || op == GetVoltage || op == GetDomainState || op == GetMinimumRate ||
        op == GetMaximumRate || op == GetPerformanceLimit || op == GetResourceCount || op == GetResourceID || op == GetDomainLevel || op == GetPerformanceStateCount || op == GetPerformanceStateFrequency || op == GetPerformanceStateVoltage ||
        op == GetAutomaticPerformance || op == GetPerformanceUtilization || op == GetPerformanceDemand ||
        op == GetPerformanceTransitions || op == GetPerformanceStatus || op == GetPerformanceDroppedSamples;
}
enum Validation { Valid, BadArguments, NotPrivileged, UnsupportedSelector };
inline Validation validate(uint32_t selector, uint32_t inputs, uint32_t outputs,
    uint64_t a, uint64_t b, bool admin, uint32_t controller) {
    if (selector == GetInfo) return !inputs && outputs == 3 ? Valid : BadArguments;
    if (selector >= SelectorCount) return UnsupportedSelector;
    if (inputs != 2 || outputs != 1) return BadArguments;
    if (selector == ReadResource)
        return a <= UINT32_MAX && readable(static_cast<uint32_t>(a)) && b <= UINT32_MAX ? Valid : BadArguments;
    if (!admin) return NotPrivileged;
    if (controller != ClockController::OSM) return UnsupportedSelector;
    return a <= 1 && (selector != SetCPUFrequency || b != 0) && (selector != SetAutomatic || b <= 1) ? Valid : BadArguments;
}
// IOConnectCallScalarMethod only: no pointers, MMIO mappings, raw register
// addresses, arbitrary regulator writes or Apple-private selector aliases.
}
