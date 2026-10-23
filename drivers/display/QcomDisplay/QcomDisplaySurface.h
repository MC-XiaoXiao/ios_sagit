/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <IOKit/graphics/IOSurface.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <libkern/c++/OSData.h>
#include "DisplayProtocol.h"

// A display-device DMA mapping of the original IOSurface allocation. The
// front mapping outlives the Family request which first presented it.
class QcomDisplaySurface : public OSObject {
    OSDeclareDefaultStructors(QcomDisplaySurface);
    IOSurface *fSurface = nullptr;
    IOMemoryDescriptor *fMemory = nullptr;
    IOMapper *fMapper = nullptr;
    IODMACommand *fDMA = nullptr;
    UInt64 fAddress = 0, fImageBytes = 0;
    UInt32 fRowBytes = 0;
    IOReturn fError = kIOReturnBadArgument;
    bool fPrepared = false, fDMAAttempted = false, fDMAPrepared = false;
    bool fHardwareReading = false;
    bool fCleanupFailed = false;
    bool fail(IOReturn error) { fError = error; return false; }
    bool releaseDMA();
public:
    bool initWithSurface(IOSurface *, IOMapper *, const Display::Mode &);
    bool synchronize();
    void exposed() { fHardwareReading = true; }
    // Command-mode PP_DONE with auto-refresh disabled ends this read, but
    // the prepared address may be reused under a later native read fence.
    void transferCompleted() { fHardwareReading = false; }
    bool retire(); // Only after the old hardware fetch has actually ended.
    bool hardwareReading() const { return fHardwareReading; }
    bool matches(UInt64 address, UInt32 rowBytes) const { return fAddress == address && fRowBytes == rowBytes; }
    IOSurface *surface() const { return fSurface; }
    UInt64 address() const { return fAddress; }
    UInt32 rowBytes() const { return fRowBytes; }
    IOReturn error() const { return fError; }
    OSData *copyPixels(); // Explicit capture only, never the presentation path.
    void free() APPLE_KEXT_OVERRIDE;
};
