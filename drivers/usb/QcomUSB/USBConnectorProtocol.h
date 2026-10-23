// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBRole.h"

namespace USBConnector {
constexpr const char *Function = "USBConnectorRequest";
constexpr const char *Resource = "USBConnectorResources";
constexpr uint32_t Version = 2;
constexpr uint32_t Changed = 0x5554;
enum class Operation : uint32_t { Acquire, Release, Sample, Source, SafePower };
struct Request {
    uint32_t version = Version, size = sizeof(Request);
    Operation operation = Operation::Sample;
    uint32_t reserved = 0;
    uint64_t lease = 0;
    USB::ConnectorState connector{};
    USB::Role next = USB::Role::None;
    bool source = false;
    uint8_t padding[6]{};
    uint32_t sourceMicroamps=0, budgetReserved=0;
};
static_assert(sizeof(Request) == 56, "connector RPC ABI");
struct Notice {
    uint32_t version = Version, size = sizeof(Notice);
    uint64_t lease = 0, connection = 0;
};
static_assert(sizeof(Notice) == 24, "connector notice ABI");
// Kernel-only, sleepable RPC: param1=Request*, param2=opening IOService*.
// Acquire retains the owner. A failed Acquire may still return a lease if
// electrical cleanup is unproven; only successful Release drops that hold.
// Every input budget field is zero. Successful Acquire alone returns the
// configured Source ceiling in microamps (zero means no Source capability).
// It is immutable until Release and does not authorize enabling VBUS.
// Source/SafePower preserve the input route, including on Aborted. Shutdown
// never requires a trustworthy route. Acquire/Sample write connector.
// Notifications use iokit_vendor_specific_msg(Changed), provider=this service.
// connection=0 immediately invalidates admission until a fresh Sample. Release
// drains notifications; reentrant Release during a notice returns Busy.
inline bool valid(const Request &r) {
    if (r.version != Version || r.size != sizeof(r) || r.reserved || r.budgetReserved || r.sourceMicroamps ||
        unsigned(r.operation) > unsigned(Operation::SafePower) ||
        unsigned(r.next) > unsigned(USB::Role::Device) || !r.connector.valid()) return false;
    for (auto b : r.padding) if (b) return false;
    if ((r.operation == Operation::Acquire) != !r.lease) return false;
    const bool target = r.next != USB::Role::None && r.connector.desired() == r.next;
    if (r.operation == Operation::Source)
        return r.source ? target && r.connector.power == USB::PowerRole::Source : r.next == USB::Role::None;
    if (r.operation == Operation::SafePower) return !r.source && (r.next == USB::Role::None || target);
    return !r.source && r.next == USB::Role::None;
}
inline bool validBudget(uint32_t microamps) {
    return !microamps || (microamps>=500000 && microamps<=1500000 && !(microamps%250000));
}
}
