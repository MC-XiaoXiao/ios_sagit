// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace QcomPower {
enum Status { OK, Invalid, Unsupported, NotReady, Busy, TimedOut, Denied, Failed, Stale };
// MSM8998 GCC USB30 master FMAX: LOWER/SVS2 through 66.67 MHz,
// LOW/SVS through 133.33 MHz, NOMINAL through 200 MHz. These are
// minimum CX levels for the supported rates, not an AP boot policy.
constexpr uint32_t usbCoreCXLevel(uint32_t hz) {
    return hz==60000000 ? 64u:hz==120000000 ? 128u:hz==150000000 ? 256u:0u;
}
constexpr uint32_t usbCoreCoveredClock(uint32_t targetHz,uint64_t inheritedHz) {
    if(!usbCoreCXLevel(targetHz) || (inheritedHz!=19200000 && inheritedHz!=60000000 &&
        inheritedHz!=120000000 && inheritedHz!=150000000))return 0;
    return inheritedHz>targetHz ? uint32_t(inheritedHz):targetHz;
}
struct IO {
    void *context;
    uint32_t (*read)(void *, unsigned bank, uint32_t offset);
    void (*write)(void *, unsigned bank, uint32_t offset, uint32_t value);
    void (*wait)(void *, unsigned usec);
    void (*kick)(void *);
};
inline uint32_t le32(const void *bytes) {
    auto *p = static_cast<const uint8_t *>(bytes);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void put32(void *bytes, uint32_t v) {
    auto *p = static_cast<uint8_t *>(bytes);
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8*i));
}
inline uint32_t align8(uint32_t n) { return (n + 7) & ~7U; }
class Registers {
protected:
    IO io{};
    uint32_t rd(uint32_t offset, unsigned bank = 0) const { return io.read(io.context, bank, offset); }
    void wr(uint32_t offset, uint32_t v, unsigned bank = 0) { io.write(io.context, bank, offset, v); }
    void wait(unsigned us) { io.wait(io.context, us); }
    void bits(uint32_t offset, uint32_t mask, uint32_t value, unsigned bank = 0) {
        wr(offset, (rd(offset, bank) & ~mask) | (value & mask), bank);
    }
    bool poll(uint32_t offset, uint32_t mask, uint32_t value, unsigned timeout, unsigned bank = 0) {
        for (unsigned n = 0; n <= timeout; ++n) {
            if ((rd(offset, bank) & mask) == value) return true;
            if (n != timeout) wait(1);
        }
        return false;
    }
public:
    void setIO(IO access) { io = access; }
};
}
