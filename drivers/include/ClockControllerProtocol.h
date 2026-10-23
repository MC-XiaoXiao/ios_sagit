// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

namespace ClockController {
constexpr const char *Function = "ClockControllerRequest";
constexpr const char *Marker = "GenericClockController";
constexpr const char *GCCReadyResource="GCCControllerReady";
constexpr const char *RPMReadyResource="RPMControllerReady";
constexpr const char *GPUCCReadyResource="GPUCCControllerReady";
enum Operation : uint32_t {
    GetName, GetRate, SetRate, RetainClock, ReleaseClock,
    RetainDomain, ReleaseDomain, GetDomainState, AssertReset, DeassertReset, FindClock,
    GetVoltage, SetVoltage, RetainRegulator, ReleaseRegulator,
    GetMinimumRate, GetMaximumRate, SetPerformanceLimit, GetPerformanceLimit,
    GetCapabilities, GetResourceCount, GetResourceID, SetDomainLevel, GetDomainLevel,
    GetPerformanceStateCount, GetPerformanceStateFrequency, GetPerformanceStateVoltage,
    AdoptClock, GetResetState, GetBandwidth, SetBandwidth, GetMaximumBandwidth, GetMinimumBandwidth,
    RetainGPUState, ValidateGPUState, ReleaseGPUState,
    SetAutomaticPerformance, GetAutomaticPerformance, GetPerformanceUtilization,
    GetPerformanceDemand, GetPerformanceTransitions, GetPerformanceStatus, GetPerformanceDroppedSamples,
    RestoreClock, PrepareUSBResources, GetModemClockVote, SetModemClockVote,
    RetainModemState, ValidateModemState, ReleaseModemState
};
// Kernel-only synchronous protocol. All pointers are kernel addresses; pointers never cross the scalar user-client ABI.
// IDs belong to the matched controller, never to an Apple SoC or a Linux phandle.
struct Request {
    uint32_t version, size;
    Operation operation;
    uint32_t id;
    uint64_t value;
    const char *name;
};
enum Controller : uint32_t { GCC = 1, RPM, SPMI, MMCC, GPUCC, OSM };
// Existing clock backends use their controller kind (1..6) as voltage
// consumer. Reserve distinct USB PHY voters so releasing one shared rail
// never removes the other PHY's vote. Slot0 is not a USB write consumer.
constexpr uint32_t USB2PHYConsumer = 7, USB3PHYConsumer = 8;
constexpr uint32_t USBControllerConsumer = 9;
constexpr uint32_t USBInterconnectConsumer = 10;
constexpr uint32_t FingerprintConsumer = 11;
constexpr uint32_t GPUConsumer = 12;
constexpr uint32_t BluetoothConsumer = 13;
constexpr uint32_t I2CConsumer = 14;
constexpr uint32_t BasebandConsumer = 15;
enum Capability : uint64_t {
    Clocks = 1, Domains = 2, Resets = 4, Voltage = 8, Performance = 16,
    Reporting = 32, UserAccess = 64, ClockHandoff = 128, Interconnect = 256, RetainedGPUState = 512, AutomaticPerformance = 1024,
    ResourceHandoff = 2048
};
struct TargetedRequest {
    Request request;
    Controller controller;
    uint32_t consumer; // 0..15, allocated by the in-kernel consumer, not userspace.
    uint32_t flags;    // bit 0: sleep-set regulator vote, otherwise active-set.
    uint32_t reserved;
};
inline bool gpuOperation(Operation op) { return op >= RetainGPUState && op <= ReleaseGPUState; }
inline bool valid(const Request &r) {
    if (r.operation > ReleaseModemState) return false;
    if (r.version == 1) return r.size == sizeof(Request) && r.operation <= FindClock;
    if (r.version != 2 || r.size != sizeof(TargetedRequest)) return false;
    const auto &t = reinterpret_cast<const TargetedRequest &>(r);
    if (t.controller < GCC || t.controller > OSM || t.consumer >= 16 || t.reserved || (t.flags & ~1U)) return false;
    // One kernel consumer holds an opaque, nonwrapping lease per backend.
    // Acquire takes zero; validate/release take the returned token in value.
    if (gpuOperation(r.operation)) return t.consumer == GPUConsumer && !t.flags && !r.id &&
        (t.controller == GCC || t.controller == GPUCC || t.controller == RPM || t.controller == SPMI);
    if (r.operation==RestoreClock) return t.controller==GCC && t.consumer==BluetoothConsumer &&
        !t.flags && r.id==53;
    // id is the USB core frequency to cover during takeover, unchanged on
    // reply. The caller covers the greater of observed and intended rates.
    // RPM must reject an insufficient persistent CX floor before sending.
    if (r.operation==PrepareUSBResources) return t.controller==RPM && t.consumer==USBControllerConsumer &&
        !t.flags && (r.id==60000000 || r.id==120000000 || r.id==150000000) && !r.value;
    if (r.operation==GetModemClockVote || r.operation==SetModemClockVote)
        return t.controller==RPM && t.consumer==BasebandConsumer && (r.id==0x3000 || r.id==0x3001) &&
            (r.operation==GetModemClockVote || r.value<=(r.id==0x3000 ? 1u:4u));
    if (r.operation>=RetainModemState && r.operation<=ReleaseModemState)
        return t.controller==GCC && t.consumer==BasebandConsumer && !t.flags && !r.id &&
            (r.operation==RetainModemState || r.value!=0); // Acquire reply carries its token.
    return true;
}
inline TargetedRequest targeted(Controller controller, Operation operation, uint32_t id,
    uint64_t value = 0, uint32_t consumer = 0, uint32_t flags = 0) {
    return {{2, sizeof(TargetedRequest), operation, id, value, nullptr}, controller, consumer, flags, 0};
}
}
