/* SPDX-License-Identifier: BSD-2-Clause */
#include "QcomDisplaySurface.h"
#include <IOKit/IOLib.h>
#include <kern/task.h>

OSDefineMetaClassAndStructors(QcomDisplaySurface, OSObject);

bool QcomDisplaySurface::initWithSurface(IOSurface *surface, IOMapper *mapper, const Display::Mode &mode) {
    if (!OSObject::init() || !surface || !mapper) return false;
    auto *memory = surface->nativeMemoryDescriptor();
    UInt64 row = surface->nativeRowBytes(), allocation = surface->nativeAllocationSize();
    Display::Surface description{surface->nativeWidth(), surface->nativeHeight(), row, allocation,
        memory ? memory->getLength() : 0, surface->nativePixelFormat(), surface->nativeTiled(), surface->nativeTileFormat()};
    if (!description.valid(mode) || surface->nativeProtection() || surface->nativeBaseOffset() ||
        surface->nativePlaneCount() || surface->nativeBytesPerElement() != 4 ||
        surface->nativeElementWidth() != 1 || surface->nativeElementHeight() != 1 ||
        !memory || allocation != memory->getLength() || memory->getDMAMapLength() != allocation ||
        !allocation || allocation > 16 * 1024 * 1024 || (allocation & 4095) || row > 65535 || (row & 63)) return false;
    fSurface = surface; surface->retain(); surface->nativeIncrementUseCount();
    fMemory = memory; memory->retain(); fMapper = mapper; mapper->retain();
    fRowBytes = UInt32(row); fImageBytes = row * mode.height;
    auto status = memory->prepare(kIODirectionOut);
    if (status != kIOReturnSuccess) return fail(status);
    fPrepared = true;
    fDMA = IODMACommand::withSpecification(IODMACommand::OutputHost64, 32,
        allocation, IODMACommand::kMapped, allocation, 64, mapper);
    if (!fDMA) return fail(kIOReturnNoMemory);
    status = fDMA->setMemoryDescriptor(memory, false);
    if (status != kIOReturnSuccess) return fail(status);
    if (fDMA->getIOMemoryDescriptor() != memory) return fail(kIOReturnUnsupported);
    fDMAAttempted = true;
    // No bounce-buffer synchronization: only a mapping of the original
    // descriptor may be scanned out. Cache maintenance is explicit below.
    status = fDMA->prepare(0, allocation, false, false);
    if (status != kIOReturnSuccess) return fail(status);
    fDMAPrepared = true;
    if (fDMA->getIOMemoryDescriptor() != memory) return fail(kIOReturnUnsupported);
    UInt64 offset = 0; UInt32 count = 1; IODMACommand::Segment64 segment{};
    status = fDMA->gen64IOVMSegments(&offset, &segment, &count);
    if (status != kIOReturnSuccess) return fail(status);
    if (fDMA->getIOMemoryDescriptor() != memory || count != 1 || offset != allocation || segment.fLength != allocation ||
        !Display::mappedRaster(mode, segment.fIOVMAddr, fRowBytes, allocation)) return fail(kIOReturnUnsupported);
    fAddress = segment.fIOVMAddr;
    return synchronize();
}
bool QcomDisplaySurface::synchronize() {
    if (!fPrepared || !fDMAPrepared || !fMemory) return fail(kIOReturnNotReady);
    auto status = fMemory->performOperation(kIOMemoryIncoherentIOFlush, 0, fImageBytes);
    if (status != kIOReturnSuccess) return fail(status);
    fError = kIOReturnSuccess; return true;
}
OSData *QcomDisplaySurface::copyPixels() {
    if (!fHardwareReading || !synchronize()) return nullptr;
    auto *mapping = fMemory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly, 0, fImageBytes);
    if (!mapping) return nullptr;
    auto *pixels = mapping->getLength() >= fImageBytes ?
        OSData::withBytes(reinterpret_cast<const void *>(mapping->getVirtualAddress()), unsigned(fImageBytes)) : nullptr;
    mapping->release(); return pixels;
}
bool QcomDisplaySurface::retire() {
    fHardwareReading = false; return releaseDMA();
}
bool QcomDisplaySurface::releaseDMA() {
    if (fHardwareReading || fCleanupFailed) return false;
    if (fDMAAttempted && !fDMAPrepared) { fCleanupFailed = true; return false; }
    if (fDMA) {
        if (fDMAPrepared) {
            auto status = fDMA->complete(false, false); fDMAPrepared = false;
            if (status != kIOReturnSuccess) {
                fCleanupFailed = true; return fail(status);
            }
            fDMAAttempted = false;
        }
        auto status = fDMA->clearMemoryDescriptor(false);
        if (status != kIOReturnSuccess) { fCleanupFailed = true; return fail(status); }
        OSSafeReleaseNULL(fDMA);
    }
    if (fPrepared) {
        auto status = fMemory->complete(kIODirectionOut);
        if (status != kIOReturnSuccess) { fCleanupFailed = true; return fail(status); }
        fPrepared = false;
    }
    OSSafeReleaseNULL(fMemory); OSSafeReleaseNULL(fMapper);
    return true;
}
void QcomDisplaySurface::free() {
    // Backlight-off and interrupt masking do not stop the DPU reading. If a
    // real replacement/stop was not observed, keep the whole DMA graph wired.
    if (!releaseDMA()) {
        IOLog("QcomDisplay: scanout DMA lifetime/cleanup uncertain (%x); mapping quarantined\n", fError); return;
    }
    if (fSurface) { fSurface->nativeDecrementUseCount(); OSSafeReleaseNULL(fSurface); }
    OSObject::free();
}
