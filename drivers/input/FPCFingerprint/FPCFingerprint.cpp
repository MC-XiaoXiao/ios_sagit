// SPDX-License-Identifier: GPL-2.0-only
#include "FPCFingerprint.h"
OSDefineMetaClassAndStructors(FPCFingerprint,FingerprintHID);
bool FPCFingerprint::handleStart(IOService *provider) {
    fVariant=QcomHomeProtocol::Variant::FPC;
    return FingerprintHID::handleStart(provider);
}
