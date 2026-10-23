// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <SecureFirmwareProtocol.h>
class OSSymbol;

// Owns the native SCM session for the retained, authenticated PAS-13 image.
// Destroy only after release() confirms that the session has been relinquished.
class QcomGPUSecure {
    IOService *fOwner = nullptr, *fService = nullptr;
    const OSSymbol *fFunction = nullptr;
    uint64_t fToken = 0;
    bool fOpen = false, fResumed = false, fFault = false;
    IOReturn request(SecureFirmware::Action);
public:
    static QcomGPUSecure *create(IOService *owner);
    ~QcomGPUSecure();
    IOReturn resume();
    IOReturn restore(); // repeat the real BOOT_SET_REMOTE_STATE after a GPU reset
    bool healthy();
    bool quarantined() const { return fFault; }
    bool release(); // CP/AXI isolation and mapper shutdown have already succeeded
};
