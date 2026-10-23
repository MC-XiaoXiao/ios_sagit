// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <ClockControllerProtocol.h>
class OSSymbol;

// A warm GPU owns no PLL calibration/reset sequence. These native platform
// leases preserve the acknowledged boot configuration until DMA is retired.
class QcomGPUPlatform {
    IOService *fProvider=nullptr, *fBus=nullptr;
    IOService *fControllers[4]{};
    const OSSymbol *fFunction=nullptr;
    IOLock *fLock=nullptr;
    uint64_t fTokens[4]{};
    unsigned fControllerCount=3;
    unsigned fLastController=UINT32_MAX,fLastOperation=UINT32_MAX;
    IOReturn fLastResult=kIOReturnSuccess;
    bool fReady=false, fFault=false;
    bool fTiming=false, fTimingReported=false;
    IOReturn request(unsigned,ClockController::Operation,uint64_t &);
    IOReturn resolveController(unsigned);
    IOReturn validate(bool bootstrap=false);
public:
    explicit QcomGPUPlatform(IOService *);
    ~QcomGPUPlatform(); // only destroy after release() succeeds
    IOReturn acquire();
    static bool retryable(IOReturn status) {
        return status==kIOReturnNotReady || status==kIOReturnBusy || status==kIOReturnOffline || status==kIOReturnNoMemory;
    }
    bool healthy();
    bool release(); // caller has already drained CP/AXI and destroyed its mapper
};
