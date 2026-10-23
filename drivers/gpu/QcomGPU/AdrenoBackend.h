// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "GPUResources.h"

namespace Adreno {
// Hardware facts from the pinned upstream MSM8998/A540 references. Register
// ownership and an isolated SMMU must precede any MMIO or command processor DMA.
constexpr uint64_t RegisterBase = 0x05000000, RegisterBytes = 0x40000;
constexpr uint64_t SMMUBase = 0x05040000, SMMUBytes = 0x10000;
constexpr uint32_t InterruptSPI = 300;
constexpr uint32_t AddressBits = 49;
constexpr uint64_t MinimumVA = 16 * 1024 * 1024; // Leave low VA for GMEM/reserved ranges.
enum class FirmwareKind { PM4, PFP, GPMU, Zap };
inline const char *firmwareName(FirmwareKind kind) {
    switch (kind) {
        case FirmwareKind::PM4: return "a530_pm4.fw";
        case FirmwareKind::PFP: return "a530_pfp.fw";
        case FirmwareKind::GPMU: return "a540_gpmu.fw2";
        case FirmwareKind::Zap: return "a540_zap.mdt";
    }
    return nullptr;
}

}
