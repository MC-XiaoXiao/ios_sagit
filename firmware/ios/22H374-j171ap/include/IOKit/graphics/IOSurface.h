/* Minimal iOS 18 kernel interface; bindings are audited in apple-surface-abi.json. */
#ifndef _IO_SURFACE_KERNEL_H
#define _IO_SURFACE_KERNEL_H
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>
#include <GPUSurfaceAccess.h>
class IOFence : public OSObject {
protected:
    UInt8 nativeFenceState[216-sizeof(OSObject)];
public:
    // IOSurface's ready worker stores this byte before invoking its callback.
    // This is readiness to access the surface, never GPU completion.
    bool nativeReady() const {
        return (__atomic_load_n(reinterpret_cast<const UInt8 *>(this)+80,__ATOMIC_ACQUIRE)&1)!=0;
    }
    bool nativeCompleted() const {
        return __atomic_load_n(reinterpret_cast<const UInt32 *>(reinterpret_cast<const UInt8 *>(this)+76),__ATOMIC_ACQUIRE)!=0;
    }
    GPU::Native::SurfaceAccessObservation nativeAccessObservation(bool initiallyReady) const {
        const auto *bytes=reinterpret_cast<const UInt8 *>(this);
        const bool ready=initiallyReady || nativeReady();
        const bool completed=nativeCompleted();
        const auto status=completed?__atomic_load_n(reinterpret_cast<const UInt32 *>(bytes+72),__ATOMIC_ACQUIRE):0;
        return GPU::Native::surfaceAccessObservation(ready,completed,status);
    }
};
static_assert(sizeof(IOFence)==216,"pinned IOSurface IOFence ABI");
class IOSurface;
using QcomSurfaceFenceReady = void (*)(void *,UInt64,IOSurface *,IOReturn);
// Audited aliases for the pinned native functions, not Apple export names.
extern "C" bool QcomIOSurfaceCreateAccessFence(IOSurface *,IOFence **,UInt32,UInt32,
    QcomSurfaceFenceReady,void *,UInt64,UInt8);
extern "C" IOReturn QcomIOFenceSignal(IOFence *,IOReturn);
class IOSurface : public OSObject {
public:
    UInt32 nativeID();
    UInt64 nativeWidth();
    UInt64 nativeHeight();
    UInt64 nativeRowBytes();
    UInt32 nativePixelFormat();
    bool nativeTiled();
    UInt32 nativeTileFormat();
    IOMemoryDescriptor *nativeMemoryDescriptor();
    UInt64 nativeAllocationSize();
    UInt32 nativePlaneCount();
    UInt32 nativeBytesPerElement();
    UInt32 nativeElementWidth();
    UInt32 nativeElementHeight();
    UInt64 nativeBaseOffset();
    UInt64 nativePlaneOffset(UInt32);
    UInt64 nativeProtection();
    void nativeIncrementUseCount();
    void nativeDecrementUseCount();
    UInt32 nativeMarkGPUWrite(UInt32 needsCPUInvalidation, UInt32 incrementSeed);
};
class IOSurfaceRoot : public IOService {
public:
    IOSurface *createSurface(task_t, OSDictionary *);
};
#endif
