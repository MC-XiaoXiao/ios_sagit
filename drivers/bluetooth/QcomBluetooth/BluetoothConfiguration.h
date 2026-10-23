// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "BluetoothLoader.h"
namespace Bluetooth {
struct Configuration {
    uint32_t version = 1, bytes = sizeof(Configuration), product = 0, soc = 0, rom = 0, railCount = 0;
    struct Rail {
        uint32_t id, voltage;
    } rails[6] { };
    uint32_t pinConfiguration[4] { };
    uint32_t patchBytes = 0, nvmBytes = 0, reserved[2] { };
};
static_assert(sizeof(Configuration) == 104, "Bluetooth DT configuration ABI");
inline bool validConfiguration(const Configuration& c)
{
    if (c.version != 1 || c.bytes != sizeof(c) || !c.soc || !c.rom || c.rom > 0xffff || !c.railCount
        || c.railCount > 6 || c.patchBytes < 28 || c.patchBytes > 0x100000 || c.nvmBytes < 16
        || c.nvmBytes > 0x10000 || c.reserved[0] || c.reserved[1])
        return false;
    for (unsigned i = 0; i < 6; ++i) {
        if (i >= c.railCount) {
            if (c.rails[i].id || c.rails[i].voltage)
                return false;
            continue;
        }
        if (!c.rails[i].id || c.rails[i].id >= 0x1000 || c.rails[i].voltage < 300000
            || c.rails[i].voltage > 4000000)
            return false;
        for (unsigned j = 0; j < i; ++j)
            if (c.rails[i].id == c.rails[j].id)
                return false;
    }
    // BLSP1 UART3 GPIO45..48, mux selector 2. Pull/drive are board policy.
    for (auto cfg : c.pinConfiguration)
        if ((cfg & ~0x1ffU) || ((cfg >> 2) & 15) != 2)
            return false;
    return true;
}
template <size_t Capacity> class ByteRing {
    uint8_t buffer[Capacity] { };
    size_t first = 0, used = 0;

public:
    size_t size() const
    {
        return used;
    }
    size_t available() const
    {
        return Capacity - used;
    }
    void clear()
    {
        first = used = 0;
    }
    uint8_t peek(size_t i) const
    {
        return i < used ? buffer[(first + i) % Capacity] : 0;
    }
    size_t push(const uint8_t* p, size_t n)
    {
        if (!p)
            return 0;
        if (n > available())
            n = available();
        for (size_t i = 0; i < n; ++i)
            buffer[(first + used + i) % Capacity] = p[i];
        used += n;
        return n;
    }
    size_t pop(uint8_t* p, size_t n)
    {
        if (n > used)
            n = used;
        if (p)
            for (size_t i = 0; i < n; ++i)
                p[i] = peek(i);
        first = (first + n) % Capacity;
        used -= n;
        return n;
    }
};
}
