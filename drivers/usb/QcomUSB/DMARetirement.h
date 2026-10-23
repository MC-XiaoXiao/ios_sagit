// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
namespace USB {
constexpr const char *DMARetirementFunction="USBDMARetirement";
// Kernel-only acknowledgement of an exact descriptor/command pair after
// actual root detachment. Unsupported mappers retain the sticky failure.
// A zero/zero token pair queries that proof without acknowledging a record.
// A mixed zero/nonzero pair is invalid.
struct DMARetirement {
    uint32_t version=1,size=sizeof(DMARetirement);
    uintptr_t memory=0,command=0;
};
static_assert(sizeof(DMARetirement)==24,"DMA retirement request ABI");
}
