// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
namespace USBInterconnect {
// Kernel-only bandwidth lease on the MSM8998 USB3 -> memory path. The producer
// must own the NoC QoS/clock path and obtain acknowledged RPM bandwidth votes;
// retaining only the controller's AXI leaf clocks does not grant bandwidth.
constexpr const char *Resource="USBInterconnectResources";
constexpr const char *Function="USBInterconnectRequest";
enum class Action:uint32_t { Query, Acquire, Release, Check };
struct Request {
    uint32_t version=1, size=sizeof(Request);
    Action action=Action::Query;
    uint32_t reserved=0;
    uint64_t lease=0, averageBytesPerSecond=0, peakBytesPerSecond=0;
    uint32_t flags=0, padding=0;
};
static_assert(sizeof(Request)==48,"USB NoC lease ABI");
constexpr uint32_t Supported=1, Granted=2;
// param1=Request*, param2=opening IOService*. Failed Acquire can retain a
// nonzero lease. Release must not lower a vote with an unacknowledged update.
inline bool valid(const Request &r) {
    if (r.version!=1 || r.size!=sizeof(r) || r.reserved || r.padding || r.flags || unsigned(r.action)>3) return false;
    if (r.action==Action::Query) return !r.lease && !r.averageBytesPerSecond && !r.peakBytesPerSecond;
    if (r.action==Action::Acquire) return !r.lease && r.averageBytesPerSecond &&
        r.averageBytesPerSecond<=r.peakBytesPerSecond;
    return r.lease && !r.averageBytesPerSecond && !r.peakBytesPerSecond;
}
}
