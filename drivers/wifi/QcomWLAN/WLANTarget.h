// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "WLANWLFW.h"
namespace WLAN {
// QMI target configuration is distinct from host CE allocation. In particular
// target CE5 is OUT and CE6 is autonomous; neither is inferred from host rings.
inline Result targetConfiguration(void *buffer,size_t capacity,size_t &length) {
    length = 0;
    static constexpr uint32_t target[12][5] = {
        {0,2,32,2048,0},{1,1,32,2048,0},{2,1,64,2048,0},{3,2,32,2048,0},
        {4,2,256,256,8},{5,2,1024,64,8},{6,3,32,16384,0},{7,4,0,0,8},
        {8,1,32,2048,0},{9,1,32,2048,0},{10,1,32,2048,0},{11,1,32,2048,0}
    };
    static constexpr uint32_t services[][3] = {
        {0x104,2,3},{0x104,1,2},{0x102,2,3},{0x102,1,2},{0x101,2,3},{0x101,1,2},
        {0x103,2,3},{0x103,1,2},{0x100,2,3},{0x100,1,2},{1,2,0},{1,1,2},
        {0xfe00,2,0},{0xfe00,1,2},{0x300,2,4},{0x300,1,1},{0xfe00,2,5},
        {0x301,1,9},{0x302,1,10},{0x600,1,11},{0,0,0}
    };
    static constexpr uint16_t shadow[][2] = {
        {0,0x3c},{3,0x3c},{4,0x3c},{5,0x3c},{7,0x3c},{1,0x40},
        {2,0x40},{7,0x40},{8,0x40},{9,0x40},{10,0x40},{11,0x40}
    };
    TLVWriter writer(buffer,capacity);
    constexpr char version[] = "QcomWLAN/1";
    writer.append(0x10,version,sizeof(version) - 1);
    // Encode explicitly, including counts; never copy native C++ structures.
    uint8_t data[sizeof(services) > sizeof(target) ? sizeof(services) : sizeof(target)];
    for (unsigned i = 0; i < 12; ++i) for (unsigned n = 0; n < 5; ++n) write32(data + (i * 5 + n) * 4,target[i][n]);
    writer.array(0x11,data,sizeof(target),1,12);
    for (unsigned i = 0; i < sizeof(services)/sizeof(services[0]); ++i)
        for (unsigned n = 0; n < 3; ++n) write32(data + (i * 3 + n) * 4,services[i][n]);
    writer.array(0x12,data,sizeof(services),1,sizeof(services)/sizeof(services[0]));
    for (unsigned i = 0; i < 12; ++i) for (unsigned n = 0; n < 2; ++n) write16(data + (i * 2 + n) * 2,shadow[i][n]);
    writer.array(0x13,data,sizeof(shadow),1,12);
    if (!writer.valid()) return Result::Invalid;
    length = writer.length(); return Result::Success;
}
}
