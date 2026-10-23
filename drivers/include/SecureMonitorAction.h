// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

namespace SecureMonitorAction {
// Kernel-only synchronous action under QcomSCM's monitor lock. The action
// includes the complete transaction and its continuation/retry sequence.
// No user client or scalar method exposes callback or context pointers.
constexpr const char *Function="SecureMonitorRunAction";
using Action=int32_t (*)(void *);
struct Request {
    uint32_t version=1,size=sizeof(Request);
    Action action=nullptr;
    void *context=nullptr;
    uint64_t reserved=0;
};
inline bool valid(const Request &r) {
    return r.version==1 && r.size==sizeof(r) && r.action && r.context && !r.reserved;
}
}
