// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <FingerprintHID.h>
class FPCFingerprint : public FingerprintHID {
    OSDeclareDefaultStructors(FPCFingerprint);
public:
    bool handleStart(IOService *) APPLE_KEXT_OVERRIDE;
};
