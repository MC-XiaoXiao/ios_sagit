// SPDX-License-Identifier: BSD-2-Clause
#pragma once

namespace InputIRQ {
// Masking/acking a child can leave one late parent delivery. An unserved
// source is a fault immediately; repeated empty deliveries must be bounded.
class SummaryGuard {
    unsigned empty=0;
public:
    bool fault(bool handled,bool foreign) {
        if (foreign) return true;
        if (handled) { empty=0; return false; }
        if (empty<8) ++empty;
        return empty==8;
    }
};
}
