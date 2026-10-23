// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"
#include "USBRole.h"

namespace USB {
// Kernel-only platform function on an IOResources-published IOService. The
// platform implementation owns clocks, rails, calibrated PHYs, NoC votes,
// connector/PMIC wiring and the declared DMA path. Acquire must obtain real resource
// holds; a DT property claiming readiness is not an implementation of Acquire.
// Calls may sleep and must not be made while the USB work loop is gated.
constexpr const char *PlatformFunction = "USBPlatformRequest";
constexpr const char *PlatformResource = "USBPlatformResources";
constexpr uint32_t ConnectorChanged = 0x5553; // iokit_vendor_specific_msg(ConnectorChanged)
constexpr uint32_t PlatformVersion = 5;
enum class PlatformAction : uint32_t { Acquire, Release, Sample, Source, SafePower, Prepare, Validate, Suspend, Resume, Isolate };
struct PlatformRequest {
    uint32_t version = PlatformVersion, size = sizeof(PlatformRequest);
    PlatformAction action = PlatformAction::Sample;
    uint32_t reserved = 0;
    uint64_t lease = 0;
    PlatformContract resources{};
    ConnectorState connector{};
    Role next = Role::None;
    bool source = false;
    uint8_t padding[6]{};
};
static_assert(sizeof(PlatformRequest) == 80, "USB platform function request ABI");
static_assert(offsetof(PlatformRequest, resources) == 24, "USB resource contract offset");
static_assert(offsetof(PlatformRequest, connector) == 56, "USB connector sample offset");
struct ConnectorNotice {
    uint32_t version = PlatformVersion, size = sizeof(ConnectorNotice);
    uint64_t lease = 0;
    uint64_t connection = 0;
};
static_assert(sizeof(ConnectorNotice) == 24, "USB connector notification ABI");
// The ready provider publishes itself with IOService::publishResource using
// PlatformResource. A controller's IOResourceMatch waits for that resource.
// param1 = PlatformRequest *, param2 = requesting IOService *, others null;
// owner->getProvider() is the AppleARMIODevice describing the controller.
// Acquire returns a nonzero, owner-bound lease and the resource contract;
// its Source ceiling comes from the current connector lease. Resume obtains
// a fresh ceiling; registry diagnostics cannot grant current or Source power.
// Release is legal only after all controller clients and DMA have stopped.
// Beginning Release transfers register ownership to the provider. A partial
// teardown can leave reset asserted or clocks off even on error; retain the
// lease/objects, but do not access controller registers again. Busy from an
// in-flight notification can be retried outside the controller workloop gate.
// Source(false) disables our source without discharging external upstream VBUS.
// SafePower verifies the connector's electrical requirements for next; the
// platform must reject source enable if it would backfeed an external supply.
// Sample reports current hardware state, never a cached desired role from DT.
// Prepare may reset/reorient PHYs only after the caller has synchronously
// halted/drained the old controller; Validate is read-only before publication.
// Prepare/SafePower(next)/Validate/Source(true) must compare the requested
// connector route with a fresh sample. kIOReturnAborted means that route went
// stale, with the input fields/lease preserved. The caller rolls back source
// and controller before retrying. No platform call may reset live DMA.
// Source(true) follows connector.power, independently of Host/Device data role.
// The provider records connection edges even if several precede one Sample.
// Counter0 notices report an untrusted observation and close admission.
// For mapped DMA, Acquire installs the device's resolved IOMapper object in
// its AppleARMIODevice's iommu-parent property. This avoids an unbounded wait
// for an unresolved mapper ID during driver start. Bypass removes that property
// only after the platform has independently established stream bypass.
// The provider retains the owner until Release, and sends ConnectorNotice via
// owner->message(iokit_vendor_specific_msg(ConnectorChanged), providerService, &notice).
// Release must drain notices before dropping the owner and resource holds.
// Hardware must cut off a source on overcurrent independently of software.
// Suspend follows synchronous Native client drain, DMA unmapping and source
// removal. It keeps the owner-bound platform lease, returns all hardware
// resource holds and closes connector notices. MMIO ownership transfers to
// the provider as soon as Suspend begins, including an unsuccessful teardown.
// Resume reacquires the actual resources and returns a fresh contract/mapper;
// the caller must probe a new controller view and sample a new attachment.
// Neither operation promises retained USB addresses or remote wake.
// Isolate follows actual controller halt, outside the workloop gate and before
// synchronous Native termination. An owned SMMU detaches its root under BCR,
// so consumed native unmap failures can be acknowledged while clients drain.
// The owner must not access MMIO again until Prepare or Resume restores it.
inline bool validPlatformHeader(const PlatformRequest &r) {
    if (r.version != PlatformVersion || r.size != sizeof(r) || r.reserved ||
        unsigned(r.action) > unsigned(PlatformAction::Isolate) || unsigned(r.next) > unsigned(Role::Device)) return false;
    for (auto byte : r.padding) if (byte) return false;
    return r.connector.valid();
}
inline bool validPlatformRequest(const PlatformRequest &r) {
    if (!validPlatformHeader(r) || (r.action == PlatformAction::Acquire ? r.lease != 0 : r.lease == 0)) return false;
    const bool target = r.next != Role::None && r.connector.desired() == r.next;
    switch (r.action) {
    case PlatformAction::Source: return r.source ? target && r.connector.power == PowerRole::Source : r.next == Role::None;
    case PlatformAction::SafePower: return !r.source && (r.next == Role::None || target);
    case PlatformAction::Prepare: case PlatformAction::Validate: return !r.source && target;
    default: return !r.source && r.next == Role::None;
    }
}
// Provider-side freshness test, after checking the requesting owner/lease.
// Shutdown never depends on a trustworthy current sample. Other transitions
// need the current electrical state as well as the immutable route identity.
inline bool platformTargetCurrent(const PlatformRequest &r, const ConnectorState &current) {
    if (!validPlatformRequest(r)) return false;
    const bool targeted = r.action == PlatformAction::Prepare || r.action == PlatformAction::Validate ||
        (r.action == PlatformAction::SafePower && r.next != Role::None) ||
        (r.action == PlatformAction::Source && r.source);
    if (!targeted) return true;
    return current.valid() && current.sameRoute(r.connector) && current.desired() == r.next;
}
inline bool validPlatformResponse(const PlatformRequest &r, PlatformAction action, uint64_t lease, bool success = true) {
    return validPlatformHeader(r) && r.action == action &&
        (action == PlatformAction::Acquire ? !lease && (!success || r.lease != 0) : r.lease != 0 && r.lease == lease);
}
}
