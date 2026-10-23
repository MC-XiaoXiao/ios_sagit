// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
class OSData;
class IOMemoryDescriptor;

// Kernel-only IOKit platform function. No user client accepts this structure.
namespace SecureFirmware {
constexpr const char *Function = "SecureFirmwareRequest";
constexpr uint32_t GPUPeripheral = 13;
enum class Action : uint32_t {
    Acquire = 1, Resume, Validate, Release, InitImage, MemSetup, Authenticate, Shutdown
};
enum Capability : uint64_t { RemoteState = 1, PeripheralAuthentication = 2 };
enum Flag : uint32_t { Quarantined = 1 };
enum State : uint32_t { Empty, Metadata, Region, Authenticated, Warm, Faulted };
struct Request {
    uint32_t version = 1, size = sizeof(Request);
    Action action = Action::Acquire;
    uint32_t peripheral = GPUPeripheral;
    uint64_t token = 0;
    OSData *metadata = nullptr;
    IOMemoryDescriptor *memory = nullptr;
    uint64_t capabilities = 0; // output
    uint32_t state = 0, flags = 0; // output
    uint64_t reserved = 0;
};
static_assert(sizeof(Request) == 64, "native secure firmware request ABI");
inline bool valid(const Request &r) {
    return r.version == 1 && r.size == sizeof(r) && r.peripheral == GPUPeripheral &&
        r.action >= Action::Acquire && r.action <= Action::Shutdown && !r.reserved &&
        !r.capabilities && !r.state && !r.flags &&
        (r.action == Action::Acquire ? !r.token : r.token != 0) &&
        (r.action == Action::InitImage ? r.metadata && !r.memory :
         r.action == Action::MemSetup ? r.memory && !r.metadata : !r.memory && !r.metadata);
}
} // namespace SecureFirmware
