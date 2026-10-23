// SPDX-License-Identifier: BSD-2-Clause
#include <DriverKit/DriverKit.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSBoolean.h>
#include "QcomWLANDriver.h"
#include <WLANDriverPolicy.h>

kern_return_t QcomWLANDriver::Start_Impl(IOService *provider) {
    kern_return_t result = Start(provider,SUPERDISPATCH);
    if (result != kIOReturnSuccess) return result;
    OSDictionary *properties = nullptr;
    result = provider->CopyProperties(&properties);
    if (result == kIOReturnSuccess && properties) {
        auto *version = OSDynamicCast(OSNumber,properties->getObject("WLANTransportVersion"));
        auto *missing = OSDynamicCast(OSNumber,properties->getObject("WLANMissingRequirements"));
        auto *ready = OSDynamicCast(OSBoolean,properties->getObject("WLANTransportReady"));
        // The nonvirtual native registration bridge is compiled separately.
        // Full IO80211 scan/join/key RPCs and firmware-backed queues remain
        // prerequisites. Provider properties cannot waive those requirements.
        const auto admission = version && missing && ready ? WLAN::driverAdmission(
            version->unsigned64BitValue(),ready == kOSBooleanTrue,missing->unsigned64BitValue(),false) : WLAN::Result::NotReady;
        result = admission == WLAN::Result::Unsupported ? kIOReturnUnsupported : kIOReturnNotReady;
    } else if (result == kIOReturnSuccess) result = kIOReturnNotReady;
    OSSafeReleaseNULL(properties);
    // No RegisterService()/Create() until both transport and native WLAN
    // queues/control plane are implemented. Failure pairs the superclass Start.
    Stop(provider,SUPERDISPATCH);
    return result;
}

kern_return_t QcomWLANDriver::Stop_Impl(IOService *provider) { return Stop(provider,SUPERDISPATCH); }
