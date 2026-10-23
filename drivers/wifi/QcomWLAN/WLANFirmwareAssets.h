// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "WLANWLFW.h"
namespace WLAN {
class FirmwareAssets {
    const uint8_t *board = nullptr;
    uint32_t boardBytes = 0, chip = 0, boardID = 0;
public:
    Result bind(const void *configuration,size_t bytes,const void *data,size_t length) {
        board = nullptr; boardBytes = chip = boardID = 0;
        if (!configuration || bytes != 24 || !data || !length || length > 1024 * 1024) return Result::Invalid;
        const auto *p = static_cast<const uint8_t *>(configuration);
        if (read32(p) != 1 || read32(p + 4) != 24 || read32(p + 16) != length || read32(p + 20)) return Result::Invalid;
        chip = read32(p + 8); boardID = read32(p + 12); board = static_cast<const uint8_t *>(data); boardBytes = uint32_t(length);
        return Result::Success;
    }
    bool valid() const { return board && boardBytes; }
    FirmwareConfiguration forMSA(uint64_t address,uint32_t size,bool quirk8,bool noMSAIndicator) const {
        FirmwareConfiguration configuration;
        configuration.msaAddress = address; configuration.msaBytes = size;
        configuration.chipID = chip; configuration.boardID = boardID;
        configuration.board = board; configuration.boardBytes = boardBytes;
        configuration.hostCapability8Bit = quirk8; configuration.noMSAReadyIndicator = noMSAIndicator;
        return configuration;
    }
};
}
