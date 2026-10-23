// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace USBPHY {
constexpr const char *Function = "USBPhyRequest";
constexpr uint32_t Version = 2;
enum class Operation : uint32_t { Enable, Disable, Check };
// Kernel-only callPlatformFunction: param1=Request*, param2=owning IOService*,
// param3/4=null. The owner must open this PHY before Enable and keep it open
// until Disable succeeds. Enable is valid only while controller DMA is stopped.
// A failed Enable can return a nonzero lease when rollback retains resources;
// the owner must preserve the service/open and retry Disable with that lease.
// Check observes the real PLL/PCS status. It never creates a resource hold.
// Orientation is Normal=1/Reverse=2 for USB3, Normal=1 for USB2. To change it,
// stop controller DMA, Disable, then Enable. The PHY never resets live DMA.
// Data role chooses QUSB2 tuning independently from Type-C power role.
// Initial firmware adoption additionally requires actual controller BCR.
struct Request {
    uint32_t version = Version, size = sizeof(Request);
    Operation operation = Operation::Check;
    uint32_t reserved = 0;
    uint64_t lease = 0;
    uint32_t orientation = 1, role = unsigned(USB::Role::Device);
};
static_assert(sizeof(Request) == 32, "PHY request ABI");
inline bool valid(const Request &r) {
    return r.version == Version && r.size == sizeof(r) && !r.reserved &&
        (r.role==unsigned(USB::Role::Host) || r.role==unsigned(USB::Role::Device)) &&
        unsigned(r.operation) <= unsigned(Operation::Check) && r.orientation >= 1 && r.orientation <= 2 &&
        (r.operation == Operation::Enable ? !r.lease : r.lease != 0);
}
}
