// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
class IOService;
namespace Pinmux {
constexpr const char* Function = "GPIOPinmuxLease";
enum class Action : uint32_t { Acquire = 1, Validate, Release };
struct Request {
    uint32_t version = 1, size = sizeof(Request);
    Action action = Action::Acquire;
    uint32_t count = 0;
    IOService* owner = nullptr;
    uint64_t token = 0;
    uint32_t pins[4] { }, configuration[4] { };
};
inline bool valid(const Request& r)
{
    if (r.version != 1 || r.size != sizeof(Request) || !r.owner || !r.count || r.count > 4
        || r.action < Action::Acquire || r.action > Action::Release
        || (r.action == Action::Acquire ? r.token != 0 : r.token == 0))
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        if (i >= r.count) {
            if (r.pins[i] || r.configuration[i])
                return false;
            continue;
        }
        if (r.pins[i] >= 150 || (r.configuration[i] & ~0x3ffU))
            return false;
        for (unsigned j = 0; j < i; ++j)
            if (r.pins[j] == r.pins[i])
                return false;
    }
    return true;
}
}
