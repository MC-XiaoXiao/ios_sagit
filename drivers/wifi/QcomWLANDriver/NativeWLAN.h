// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <DriverKit/IOService.h>
#include <NetworkingDriverKit/IOUserNetworkPacketBufferPool.h>
#include <NetworkingDriverKit/IOUserNetworkPacketQueue.h>

namespace WLAN {
// Framework-owned objects remain opaque. No private class layout, virtual
// table, or made-up IO80211 header is projected into this driver.
struct NativeQueues {
    OSObject *logicalLink = nullptr;
    IOUserNetworkPacketBufferPool *txPool = nullptr, *rxPool = nullptr;
};
class NativeWLAN {
    IOService *interface = nullptr;
public:
    NativeWLAN() = default;
    NativeWLAN(const NativeWLAN &) = delete;
    NativeWLAN &operator=(const NativeWLAN &) = delete;
    bool attached() const { return interface != nullptr; }
    kern_return_t attach(IOService *,const uint8_t *,const NativeQueues &);
    kern_return_t detach();
    static kern_return_t createLogicalLink(IOUserNetworkPacketQueue **,uint32_t,OSObject **);
};
}
