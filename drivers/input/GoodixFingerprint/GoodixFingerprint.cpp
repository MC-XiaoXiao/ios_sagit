// SPDX-License-Identifier: GPL-2.0-only
#include "GoodixFingerprint.h"
OSDefineMetaClassAndStructors(GoodixFingerprint,FingerprintHID);
bool GoodixFingerprint::handleStart(IOService *provider) {
    fVariant=QcomHomeProtocol::Variant::Goodix;
    return FingerprintHID::handleStart(provider);
}
