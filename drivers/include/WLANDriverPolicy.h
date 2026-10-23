// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "WLANProtocol.h"

namespace WLAN {
// Shared by the DriverKit lifecycle and its offline tests. This is admission
// policy, not an IO80211DriverKit ABI declaration or a networking substitute.
inline Result driverAdmission(uint64_t version, bool transportReady, uint64_t missing,
    bool nativeABIAudited) {
    if (version != ProtocolVersion) return Result::Unsupported;
    if (!transportReady || (missing & ~uint64_t(NativeInterfaceABI))) return Result::NotReady;
    if (!nativeABIAudited || (missing & NativeInterfaceABI)) return Result::Unsupported;
    return Result::Success;
}
}
