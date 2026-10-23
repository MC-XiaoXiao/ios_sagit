// SPDX-License-Identifier: GPL-2.0-only
// MSM8998 sequence derived from Linux v6.12 qcom_q6v5_mss.c.
#pragma once
#include <BasebandProtocol.h>

namespace Baseband { namespace Q6 {
constexpr uint64_t Base = 0x04080000, RMBBase = 0x04180000;
// STRAP_ACC is at 0x110. A mapping of the upstream DT's 0x100 reg length is
// insufficient even though Linux ioremap happens to round it to a page.
constexpr uint32_t Aperture = 0x1000, RMBAperture = 0x1000;
constexpr uint32_t Reset = 0x14, GFMux = 0x20, Power = 0x30, Memory = 0x34,
    XO = 0x38, Strap = 0x110;
constexpr uint32_t MBAImage = 0, PBLStatus = 4, Command = 8, MBAStatus = 12,
    Metadata = 16, CodeStart = 20, CodeLength = 24;

// Caller holds clocks/CX/MX and has already acquired memory ownership and
// deasserted GCC MSS_RESTART. This routine does not use the GPU's PAS ID 13.
// Backend: read/write(space, offset), delayUS(), nowUS(). Errors are explicit.
template<class IO> class Core {
    IO &io;
    Result modify(unsigned space, uint32_t reg, uint32_t clear, uint32_t set) {
        uint32_t v;
        if (!io.read(space, reg, v) || !io.write(space, reg, (v & ~clear) | set))
            return Result::IO;
        return Result::OK;
    }
public:
    explicit Core(IO &backend) : io(backend) {}
    Result wait(unsigned space, uint32_t reg, uint32_t mask, uint32_t expected,
                uint32_t timeoutUS, bool signedStatus = false) {
        const uint64_t start = io.nowUS();
        // The bounded poll count also terminates if a broken backend clock
        // stops advancing. Real firmware delays must advance the timer.
        for (uint32_t i = 0; i <= timeoutUS; ++i) {
            uint32_t v;
            if (!io.read(space, reg, v)) return Result::IO;
            if (signedStatus && int32_t(v) < 0) return Result::Authentication;
            if ((v & mask) == expected) return Result::OK;
            if (io.nowUS() - start >= timeoutUS) break;
            io.delayUS(1);
        }
        return Result::Timeout;
    }
    Result start(uint64_t mba) {
        if (!mba || (mba & 4095) || mba > UINT32_MAX) return Result::Invalid;
        if (!io.write(1, MBAImage, uint32_t(mba)) || !io.write(0, Strap, 0x20))
            return Result::IO;
        Result r = modify(0, Reset, 0, 7);
        if (r != Result::OK) return r;
        if ((r = modify(0, XO, 0, 1)) != Result::OK) return r;
        if ((r = wait(0, XO, 1u << 31, 0, 200)) != Result::OK) return r;
        if ((r = modify(0, Power, 0, 1u << 24)) != Result::OK) return r;
        uint32_t value;
        if (!io.read(0, Power, value)) return Result::IO;
        io.delayUS(1);
        if ((r = modify(0, Power, 0, 1u << 25)) != Result::OK ||
            (r = modify(0, Power, 1u << 22, 0)) != Result::OK ||
            (r = modify(0, Power, 0, (1u << 18) | (1u << 19))) != Result::OK)
            return r;
        if (!io.read(0, Memory, value)) return Result::IO;
        for (int bit = 28; bit >= 0; --bit) {
            value |= 1u << bit;
            if (!io.write(0, Memory, value) || !io.read(0, Memory, value))
                return Result::IO;
            io.delayUS(1);
        }
        if ((r = modify(0, Power, 1u << 21, 0)) != Result::OK ||
            (r = modify(0, Power, 1u << 20, 0)) != Result::OK ||
            (r = modify(0, Reset, 2, 0)) != Result::OK ||
            (r = modify(0, GFMux, 0, 2)) != Result::OK ||
            (r = modify(0, Reset, 1, 0)) != Result::OK) return r;
        // PBL returns 1 on success; any other nonzero status is a failure.
        const uint64_t start = io.nowUS();
        for (unsigned i = 0; i <= 1000000; ++i) {
            if (!io.read(1, PBLStatus, value)) return Result::IO;
            if (value) return value == 1 ? Result::OK : Result::Authentication;
            if (io.nowUS() - start >= 1000000) break;
            io.delayUS(1);
        }
        return Result::Timeout;
    }
    Result waitMBA() {
        const uint64_t start = io.nowUS();
        for (unsigned i = 0; i <= 5000000; ++i) {
            uint32_t v;
            if (!io.read(1, MBAStatus, v)) return Result::IO;
            if (int32_t(v) < 0) return Result::Authentication;
            if (v == 1 || v == 2) return Result::OK;
            if (v) return Result::Invalid;
            if (io.nowUS() - start >= 5000000) break;
            io.delayUS(1);
        }
        return Result::Timeout;
    }
    Result authenticateMetadata(uint64_t address) {
        if (!address || (address & 4095) || address > UINT32_MAX) return Result::Invalid;
        if (!io.write(1, CodeLength, 0) || !io.write(1, Metadata, uint32_t(address)) ||
            !io.write(1, Command, 1)) return Result::IO;
        return wait(1, MBAStatus, UINT32_MAX, 3, 1000000, true);
    }
    Result beginLoad(uint64_t address) {
        if (!address || address > UINT32_MAX) return Result::Invalid;
        return io.write(1, CodeStart, uint32_t(address)) && io.write(1, Command, 2)
            ? Result::OK : Result::IO;
    }
    Result loaded(uint32_t cumulativeBytes) {
        if (!cumulativeBytes) return Result::Invalid;
        if (!io.write(1, CodeLength, cumulativeBytes)) return Result::IO;
        uint32_t status;
        if (!io.read(1, MBAStatus, status)) return Result::IO;
        return int32_t(status) < 0 ? Result::Authentication : Result::OK;
    }
    Result waitAuthentication() {
        return wait(1, MBAStatus, UINT32_MAX, 4, 10000000, true);
    }
};
}} // namespace Baseband::Q6
