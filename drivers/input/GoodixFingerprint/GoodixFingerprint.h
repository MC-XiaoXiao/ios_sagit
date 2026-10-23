// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <FingerprintHID.h>
class GoodixFingerprint : public FingerprintHID {
    OSDeclareDefaultStructors(GoodixFingerprint);
public:
    bool handleStart(IOService *) APPLE_KEXT_OVERRIDE;
};
