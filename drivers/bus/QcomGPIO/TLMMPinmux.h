// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "TLMMPins.h"
#include <PinmuxProtocol.h>

namespace QcomTLMM {
enum class MuxResult { Success, Invalid, Denied, Busy, Exhausted, IOFailure, NotReady };

// The controller lock serializes this ledger with IRQ registration and legacy
// GPIO operations. Its caller retains the owner when a token is first issued
// and releases it only after a successful Release, outside the spin lock.
class PinmuxLeases {
    uint64_t serial = 0, tokens[PinCount] { };
    struct Lease {
        IOService* owner = nullptr;
        uint64_t token = 0;
        unsigned count = 0;
        uint32_t pins[4] { }, saved[4] { }, configuration[4] { };
        bool restoring = false;
    } leases[8];

public:
    bool busy(unsigned pin) const
    {
        return pin < PinCount && tokens[pin];
    }

    template <class Registers, class Check> MuxResult apply(Pinmux::Request& r, Registers& io, Check check)
    {
        if (!Pinmux::valid(r))
            return MuxResult::Invalid;
        Lease* lease = nullptr;
        if (r.action == Pinmux::Action::Acquire) {
            for (auto& candidate : leases)
                if (!candidate.owner) {
                    lease = &candidate;
                    break;
                }
            if (!lease || serial == UINT64_MAX)
                return MuxResult::Exhausted;
            for (unsigned i = 0; i < r.count; ++i) {
                const auto result = check(r.pins[i], (r.configuration[i] >> 2) & 15);
                if (result != MuxResult::Success)
                    return result;
                if (tokens[r.pins[i]])
                    return MuxResult::Busy;
            }
            lease->owner = r.owner;
            lease->token = ++serial;
            lease->count = r.count;
            lease->restoring = false;
            for (unsigned i = 0; i < r.count; ++i) {
                const auto pin = r.pins[i];
                lease->pins[i] = pin;
                lease->saved[i] = io.read32(PinBase[pin]);
                lease->configuration[i] = (lease->saved[i] & ~0x3ffU) | r.configuration[i];
                tokens[pin] = lease->token;
                io.write32(PinBase[pin], lease->configuration[i]);
            }
            r.token = lease->token;
            for (unsigned i = 0; i < lease->count; ++i)
                if (io.read32(PinBase[lease->pins[i]]) != lease->configuration[i]) {
                    lease->restoring = true;
                    return MuxResult::IOFailure;
                }
            return MuxResult::Success;
        }
        for (auto& candidate : leases)
            if (candidate.owner == r.owner && candidate.token == r.token) {
                lease = &candidate;
                break;
            }
        if (!lease || lease->count != r.count)
            return MuxResult::Denied;
        if (lease->restoring && r.action != Pinmux::Action::Release)
            return MuxResult::NotReady;
        for (unsigned i = 0; i < r.count; ++i) {
            const auto current = io.read32(PinBase[lease->pins[i]]);
            if (lease->pins[i] != r.pins[i] || (lease->configuration[i] & 0x3ff) != r.configuration[i]
                || (current != lease->configuration[i] && !(lease->restoring && current == lease->saved[i])))
                return MuxResult::IOFailure;
        }
        if (r.action != Pinmux::Action::Release)
            return MuxResult::Success;
        lease->restoring = true;
        for (unsigned i = 0; i < lease->count; ++i)
            io.write32(PinBase[lease->pins[i]], lease->saved[i]);
        for (unsigned i = 0; i < lease->count; ++i)
            if (io.read32(PinBase[lease->pins[i]]) != lease->saved[i])
                return MuxResult::IOFailure;
        for (unsigned i = 0; i < lease->count; ++i)
            tokens[lease->pins[i]] = 0;
        lease->owner = nullptr;
        lease->token = 0;
        r.token = 0;
        return MuxResult::Success;
    }
};
}
