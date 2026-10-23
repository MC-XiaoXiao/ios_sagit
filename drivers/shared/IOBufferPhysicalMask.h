// SPDX-License-Identifier: BSD-2-Clause
#pragma once

namespace MI6IOBuffer {
// Private allocation option shared by the guest kernel and platform drivers.
// Keep a supplied physical mask as a host physical bound even if a system
// IOMapper exists. It is consumed by allocation, not forwarded to the MD.
constexpr unsigned HostPhysicalMask = 0x01000000;
}
