// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace USB {
// The Native Family owns allocation and release. These are mA, not USB
// descriptor units or the global AppleUSBHostResources power supply in mW.
// A fresh controller and pool are created for each connection/cold-PM epoch.
struct HostCurrent {
    uint32_t wake=0,maximumSpeed=0;
    Result configure(const PlatformContract &resources) {
        wake=maximumSpeed=0;
        if(!resources.valid())return Result::Invalid;
        const uint32_t minimum=resources.maximumSpeed==4 ? 900000:500000;
        if(resources.sourceMicroamps<minimum)return Result::Unsupported;
        wake=resources.sourceMicroamps/1000;maximumSpeed=resources.maximumSpeed;return Result::Success;
    }
    template<class Set> bool controller(Set set) const {
        return wake && set("kUSBWakePowerSupply",wake) && set("kUSBSleepPowerSupply",0);
    }
    template<class Set> bool port(bool super,Set set) const {
        const uint32_t limit=super ? 900:500;
        // This platform's leased connector is Type-C. QMP switches the SS
        // lanes for orientation while keeping the same logical root port.
        // These are physical connector codes, distinct from getPortType().
        const uint32_t connector=maximumSpeed==4 ? 9:8;
        // The shared pool can grant less than a port's protocol ceiling.
        return wake && (!super || maximumSpeed==4) && set("UsbConnector",connector) &&
            set("kUSBWakePortCurrentLimit",limit) && set("kUSBSleepPortCurrentLimit",0);
    }
    static uint32_t enumeration(bool super) { return super ? 150:100; }
};
}
