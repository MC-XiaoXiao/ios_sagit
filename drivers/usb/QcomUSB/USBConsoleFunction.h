// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

namespace USBConsoleFunction {
// The configurator puts control immediately before data. Interface numbers
// are supplied by the Family descriptor callback, never fixed globally.
inline unsigned descriptors(uint8_t *out, unsigned capacity, unsigned control) {
    if (!out || capacity < 19 || control >= 255) return 0;
    const uint8_t bytes[] = {5,0x24,0,0x10,1, 5,0x24,1,0,uint8_t(control+1),
        4,0x24,2,2, 5,0x24,6,uint8_t(control),uint8_t(control+1)};
    for (unsigned n=0;n<sizeof(bytes);++n) out[n]=bytes[n];
    return sizeof(bytes);
}
inline bool validLineCoding(const uint8_t *p, unsigned size) {
    if (!p || size != 7) return false;
    const uint32_t baud = uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
    return baud && p[4] <= 2 && p[5] <= 4 && (p[6] == 5 || p[6] == 6 || p[6] == 7 || p[6] == 8 || p[6] == 16);
}
inline void notification(uint8_t *p, unsigned interface, bool open) {
    const uint8_t bytes[] = {0xa1,0x20,0,0,uint8_t(interface),0,2,0,uint8_t(open?3:0),0};
    for(unsigned n=0;n<sizeof(bytes);++n) p[n]=bytes[n];
}
}
