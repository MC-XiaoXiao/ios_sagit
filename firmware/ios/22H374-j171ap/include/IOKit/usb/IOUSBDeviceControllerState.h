// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

// Read-only state projection for the hash-locked iOS 18 arm64 Family. Keep
// policy decoding shared with the offline ABI tests; never write this state.
namespace IOUSBDeviceFamilyABI {
constexpr unsigned ControllerBytes=560,CapabilityListOffset=392,USB3PolicyOffset=544;
struct USB3LinkPolicy {
    unsigned u1SEL=0,u1PEL=0,u2SEL=0,u2PEL=0;
    bool u1Enabled=false,u2Enabled=false;
};
inline bool decodeUSB3LinkPolicy(const void *bytes,size_t size,USB3LinkPolicy &out) {
    if (!bytes || size!=8) return false;
    const auto *p=static_cast<const uint8_t *>(bytes);
    if (p[6]>1 || p[7]>1) return false;
    out={p[0],p[1],unsigned(p[2])|unsigned(p[3])<<8,unsigned(p[4])|unsigned(p[5])<<8,p[6]!=0,p[7]!=0};
    return true;
}
}
