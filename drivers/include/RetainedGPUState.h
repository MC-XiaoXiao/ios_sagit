// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

namespace GPUPlatform {
// Serialized by the owning clock controller's command gate. Acquisition and
// release only read registers: the boot owner still owns the physical pins.
// A changed/unknown configuration permanently holds the lease until reset.
class RetainedState {
    struct Register { uint32_t bank, offset, mask, value; } registers[80]{};
    unsigned count = 0;
    uint64_t serial = 0, token = 0;
    bool failed = false;
public:
    bool held() const { return token != 0; }
    bool begin() {
        if (held() || failed || serial == UINT64_MAX) return false;
        count = 0; return true;
    }
    bool watch(uint32_t bank, uint32_t offset, uint32_t mask, uint32_t value) {
        if (held() || count == 80 || !mask || value == UINT32_MAX) return false;
        registers[count++] = {bank,offset,mask,value&mask}; return true;
    }
    template<class Read> bool unchanged(Read read) const {
        if (!count) return false;
        for (unsigned i=0;i<count;++i) {
            const auto &r=registers[i]; const auto value=read(r.bank,r.offset);
            if (value == UINT32_MAX || (value&r.mask) != r.value) return false;
        }
        return true;
    }
    template<class Read> bool acquire(uint64_t &out, Read read) {
        if (held() || failed || serial == UINT64_MAX || !unchanged(read)) return false;
        out=token=++serial; return true;
    }
    template<class Read> bool validate(uint64_t value, Read read) {
        if (!value || value != token || failed) return false;
        if (!unchanged(read)) { failed=true; return false; }
        return true;
    }
    template<class Read> bool release(uint64_t value, Read read) {
        if (!validate(value,read)) return false;
        token=0; count=0; return true;
    }
};
}
