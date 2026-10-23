// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

namespace GPU { namespace NativePoster {
// CodeDirectory identity of the 22H374 Mercury binary in this firmware.
constexpr uint8_t MercuryHash[20]={0x1f,0x43,0x8c,0xab,0x47,0x8a,0x18,0xf1,0x0e,0x84,
    0xe8,0xf3,0x3e,0x1c,0x79,0x48,0x7b,0x51,0x79,0xd9};
inline bool allowed(bool provider,bool secure,const uint8_t *cdhash) {
    if(!provider || !secure || !cdhash)return false;
    for(unsigned i=0;i<sizeof(MercuryHash);++i)if(cdhash[i]!=MercuryHash[i])return false;
    return true;
}
} }
