// SPDX-License-Identifier: BSD-2-Clause
#include "NativeWLAN.h"
#include <DriverKit/OSMetaClass.h>

// These are narrow, nonvirtual calls audited against NetworkingDriverKit
// 521.140.3.700.1 in the project's DriverKit 24.6 cache. The asm names are the
// actual Apple exports. All three opaque object pointers are framework-owned.
// See drivers/abi-22H374/driverkit-abi.json for binary hashes and function evidence.
extern OSMetaClass *gIOUserNetworkWLANMetaClass;
extern OSMetaClass *gIOUserNetworkLogicalLinkMetaClass;
struct ether_addr { uint8_t octet[6]; };
extern "C" kern_return_t registerWLAN(IOService *,ether_addr,OSObject *,
    IOUserNetworkPacketBufferPool *,IOUserNetworkPacketBufferPool *)
    __asm("__ZN17IOUserNetworkWLAN21registerWlanInterfaceE10ether_addrP24IOUserNetworkLogicalLinkP29IOUserNetworkPacketBufferPoolS4_");
extern "C" kern_return_t createLink(OSObject **,uint32_t,OSObject **)
    __asm("__ZN24IOUserNetworkLogicalLink6CreateEPP21IOUserNetworkQueueSetjPPS_");
extern "C" kern_return_t createQueueSet(IOUserNetworkPacketQueue **,uint32_t,uint32_t,OSObject **)
    __asm("__ZN21IOUserNetworkQueueSet6CreateEPP24IOUserNetworkPacketQueuejjPPS_");

namespace WLAN {
kern_return_t NativeWLAN::createLogicalLink(IOUserNetworkPacketQueue **queues,uint32_t count,OSObject **result) {
    if (!result) return kIOReturnBadArgument;
    *result = nullptr;
    if (!queues || count != 4) return kIOReturnBadArgument;
    for (unsigned i = 0; i < count; ++i) {
        if (!queues[i]) return kIOReturnBadArgument;
        for (unsigned n = 0; n < i; ++n) if (queues[n] == queues[i]) return kIOReturnBadArgument;
    }
    OSObject *set = nullptr;
    auto status = createQueueSet(queues,count,0,&set);
    if (status == kIOReturnSuccess) {
        if (!set) return kIOReturnError;
        status = createLink(&set,1,result);
    }
    if (set) set->release();
    if (status == kIOReturnSuccess && !*result) return kIOReturnError;
    return status;
}
kern_return_t NativeWLAN::attach(IOService *owner,const uint8_t *mac,const NativeQueues &queues) {
    if (interface) return kIOReturnBusy;
    if (!owner || !mac || (mac[0] & 1) || !queues.logicalLink || !queues.txPool || !queues.rxPool ||
        queues.txPool == queues.rxPool) return kIOReturnBadArgument;
    uint8_t any = 0; for (unsigned i = 0; i < 6; ++i) any |= mac[i];
    if (!any) return kIOReturnBadArgument;
    if (!OSMetaClassBase::safeMetaCast(queues.logicalLink,gIOUserNetworkLogicalLinkMetaClass)) return kIOReturnBadArgument;
    IOService *created = nullptr;
    auto status = owner->Create(owner,"WLANInterfaceProperties",&created);
    if (status != kIOReturnSuccess) return status;
    if (!created) return kIOReturnError;
    interface = created; // retain on every uncertain termination path
    if (!OSMetaClassBase::safeMetaCast(created,gIOUserNetworkWLANMetaClass)) status = kIOReturnUnsupported;
    if (status == kIOReturnSuccess) {
        ether_addr address{}; for (unsigned i = 0; i < 6; ++i) address.octet[i] = mac[i];
        status = registerWLAN(created,address,queues.logicalLink,queues.txPool,queues.rxPool);
    }
    if (status != kIOReturnSuccess) {
        const auto cleanup = detach();
        return cleanup == kIOReturnSuccess ? status : cleanup;
    }
    return kIOReturnSuccess;
}
kern_return_t NativeWLAN::detach() {
    if (!interface) return kIOReturnSuccess;
    const auto status = interface->Terminate(0);
    if (status != kIOReturnSuccess) return status;
    interface->release(); interface = nullptr;
    return kIOReturnSuccess;
}
}
