// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

namespace PMI8998Backlight {
// iOS 18 brightness-nits uses signed 16.16 nits; IODisplayParameters
// BrightnessMilliNits uses integer millinits. The board supplies a nominal
// linear DAC model, not an optical measurement or a change to LED current.
struct NativeBrightness {
    uint32_t maximumMilliNits = 0;
    bool valid() const { return maximumMilliNits && maximumMilliNits <= 1000000; }
    uint32_t milliNits(uint32_t dac) const {
        return uint32_t((uint64_t(dac) * maximumMilliNits + 2047) / 4095);
    }
    bool dac(uint64_t fixedNits, uint32_t &result) const {
        if (!valid() || fixedNits > INT32_MAX) return false;
        const uint64_t scale = uint64_t(maximumMilliNits) * 65536;
        uint64_t value = (fixedNits * 1000 * 4095 + scale / 2) / scale;
        result = value > 4095 ? 4095 : uint32_t(value);
        return true;
    }
};
}
