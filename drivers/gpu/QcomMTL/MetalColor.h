// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <string.h>

namespace GPU { namespace MetalColor {
// Keep signed/HDR components and round directly from MTL's double to binary16.
// The blitter repeats the two packed words without interpreting their channels.
inline void packRGBA16Float(const double (&rgba)[4],uint32_t &low,uint32_t &high) {
    static_assert(sizeof(_Float16)==2,"binary16 clear storage");
    uint16_t bits[4];
    for(unsigned i=0;i<4;++i) {
        _Float16 value=static_cast<_Float16>(rgba[i]);
        memcpy(&bits[i],&value,sizeof(value));
    }
    low=uint32_t(bits[0])|(uint32_t(bits[1])<<16);
    high=uint32_t(bits[2])|(uint32_t(bits[3])<<16);
}
} }
